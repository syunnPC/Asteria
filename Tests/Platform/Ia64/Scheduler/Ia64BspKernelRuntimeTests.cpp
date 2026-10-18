// SPDX-License-Identifier: Apache-2.0
#include <sys/wait.h>
#include <unistd.h>
#include "Platform/Synchronization/ProcessorContext.hpp"
#include <csetjmp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>

#include "Boot/BootInfo.hpp"
#include "Kernel/Diagnostics/DiagnosticChannel.hpp"
#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Ipc/Notification.hpp"
#include "Kernel/Ipc/WaitSet.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Scheduler/ProcessorRunController.hpp"
#include "Kernel/Time/Timer.hpp"
#include "Kernel/Time/TimerNotificationDelivery.hpp"
#include "Kernel/Time/TimerQueue.hpp"
#include "Platform/Ia64/Diagnostics/Ia64EvidenceRecord.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"
#include "Platform/Ia64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Ia64/Memory/Ia64BspRuntimeMemory.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Ia64/Executable/Ia64InitialUserImageOwner.hpp"
#include "Platform/Ia64/Scheduler/Ia64BspKernelRuntime.hpp"
#include "Platform/Ia64/Scheduler/Ia64UserExecutionProbePayload.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/Ia64HostedBootGeometry.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace
{
constexpr Asteria::Foundation::Size InitialImageSourceSizeBytes{0x5000U};
constexpr Asteria::Foundation::Size InitialImageBackingPageCount{136U};
constexpr Asteria::Foundation::Size InitialImageBackingSizeBytes{InitialImageBackingPageCount * 4096U};
} // namespace

extern "C"
{
extern Asteria::Foundation::UInt8 AsteriaIa64BspKernelRuntimeInitialUserImageStorage[InitialImageSourceSizeBytes];
}

asm(".pushsection .bss\n"
    ".balign 4096\n"
    ".global AsteriaIa64BspKernelRuntimeInitialUserImageStorage\n"
    ".type AsteriaIa64BspKernelRuntimeInitialUserImageStorage, @object\n"
    "AsteriaIa64BspKernelRuntimeInitialUserImageStorage:\n"
    ".zero 20480\n"
    ".size AsteriaIa64BspKernelRuntimeInitialUserImageStorage, "
    ". - AsteriaIa64BspKernelRuntimeInitialUserImageStorage\n"
    ".popsection\n");

namespace Asteria::Test
{
Foundation::Size g_timerInitializeCalls{0U};
Foundation::Size g_timerProgramCalls{0U};
Foundation::Size g_timerCancelCalls{0U};
Foundation::Size g_localPurgeCalls{0U};
Foundation::Size g_userLaunchCalls{0U};
Foundation::Size g_regionActivationCalls{0U};
bool g_workerSwitchActive{false};
bool g_workerSwitchWasActiveAtLaunch{false};
bool g_userLaunchPlanCaptured{false};
Foundation::UInt64 g_installedContextAddress{0U};
Foundation::UInt64 g_programmedTimerDeadline{0U};
Foundation::UInt64 g_programmedTimerVector{0U};
Foundation::UInt64 g_programmedTimerRollbackDeadline{0U};
Foundation::UInt64 g_programmedTimerRollbackVector{0U};
Foundation::UInt64 g_programmedTimerMaskedVector{0U};
Foundation::UInt64 g_cancelledTimerVector{0U};
Foundation::UInt64 g_lastRegionOneActivation{0U};
Foundation::UInt64 g_regionOneAtUserLaunch{0U};
Foundation::UInt64 g_intervalTimeCounter{1U};
Foundation::UInt64 g_intervalTimeCounterStep{0U};
Platform::Ia64::Scheduler::Ia64UserModeLaunchPlan g_userLaunchPlan{};
std::jmp_buf g_idleResumeEnvironment;
alignas(Platform::Ia64::Memory::Ia64BspRuntimeMemory) std::byte
    g_runtimeMemoryStorage[sizeof(Platform::Ia64::Memory::Ia64BspRuntimeMemory)]{};
alignas(Platform::Ia64::Scheduler::Ia64BspKernelRuntime) std::byte
    g_kernelRuntimeStorage[sizeof(Platform::Ia64::Scheduler::Ia64BspKernelRuntime)]{};
} // namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64
AsteriaIa64InstallRuntimeInterruptVectorTable(const Asteria::Foundation::UInt64 contextAddress,
                                              Asteria::Foundation::UInt64) noexcept
{
    Asteria::Test::g_installedContextAddress = contextAddress;
    return 0U;
}

extern "C" [[noreturn]] void AsteriaIa64FailStopRuntimeInterruptVectorTableInstall() noexcept
{
    __builtin_trap();
}

extern "C" void AsteriaIa64PurgeLocalTranslation(Asteria::Foundation::UInt64, Asteria::Foundation::UInt64) noexcept
{
    ++Asteria::Test::g_localPurgeCalls;
}

extern "C" void AsteriaIa64SerializeLocalTranslationPurge() noexcept
{
}

extern "C" Asteria::Foundation::UInt64
AsteriaIa64LaunchUserMode(const Asteria::Platform::Ia64::Scheduler::Ia64UserModeLaunchPlan* const plan) noexcept
{
    ++Asteria::Test::g_userLaunchCalls;
    Asteria::Test::g_regionOneAtUserLaunch = Asteria::Test::g_lastRegionOneActivation;
    Asteria::Test::g_workerSwitchWasActiveAtLaunch = Asteria::Test::g_workerSwitchActive;
    if (plan != nullptr)
    {
        Asteria::Test::g_userLaunchPlan = *plan;
        Asteria::Test::g_userLaunchPlanCaptured = true;
    }
    return 3U;
}

extern "C" Asteria::Foundation::UInt64 AsteriaIa64ReadIntervalTimeCounter() noexcept
{
    // Constant unless a test lets it advance by g_intervalTimeCounterStep
    // on every read.
    Asteria::Test::g_intervalTimeCounter += Asteria::Test::g_intervalTimeCounterStep;
    return Asteria::Test::g_intervalTimeCounter;
}

extern "C" Asteria::Foundation::UInt64 AsteriaIa64InitializeLocalTimer(Asteria::Foundation::UInt64) noexcept
{
    ++Asteria::Test::g_timerInitializeCalls;
    return 0U;
}

extern "C" Asteria::Foundation::UInt64
AsteriaIa64ProgramLocalTimer(const Asteria::Foundation::UInt64 deadline, const Asteria::Foundation::UInt64 vector,
                             const Asteria::Foundation::UInt64 rollbackDeadline,
                             const Asteria::Foundation::UInt64 rollbackVector,
                             const Asteria::Foundation::UInt64 maskedVector) noexcept
{
    ++Asteria::Test::g_timerProgramCalls;
    Asteria::Test::g_programmedTimerDeadline = deadline;
    Asteria::Test::g_programmedTimerVector = vector;
    Asteria::Test::g_programmedTimerRollbackDeadline = rollbackDeadline;
    Asteria::Test::g_programmedTimerRollbackVector = rollbackVector;
    Asteria::Test::g_programmedTimerMaskedVector = maskedVector;
    return 0U;
}

extern "C" Asteria::Foundation::UInt64
AsteriaIa64CancelLocalTimer(const Asteria::Foundation::UInt64 maskedVector) noexcept
{
    ++Asteria::Test::g_timerCancelCalls;
    Asteria::Test::g_cancelledTimerVector = maskedVector;
    return 0U;
}

extern "C" void
AsteriaIa64SwitchKernelContext(Asteria::Platform::Ia64::Scheduler::Ia64KernelRegisterState*,
                               const Asteria::Platform::Ia64::Scheduler::Ia64KernelRegisterState* const next) noexcept
{
    if (Asteria::Test::g_workerSwitchActive)
    {
        std::longjmp(Asteria::Test::g_idleResumeEnvironment, 1);
    }
    if ((next == nullptr) || (next->R4 == 0U) || (next->R6 == 0U))
    {
        return;
    }

    Asteria::Test::g_workerSwitchActive = true;
    if (setjmp(Asteria::Test::g_idleResumeEnvironment) == 0)
    {
        using WorkerEntry = void (*)(void*) noexcept;
        const auto entry = reinterpret_cast<WorkerEntry>(static_cast<Asteria::Foundation::UIntPointer>(next->R4));
        auto* const argument = reinterpret_cast<void*>(static_cast<Asteria::Foundation::UIntPointer>(next->R6));
        entry(argument);
    }
    Asteria::Test::g_workerSwitchActive = false;
}

extern "C" Asteria::Foundation::UInt64
AsteriaIa64ActivateRuntimeRegions(const Asteria::Foundation::UInt64 regionOne, Asteria::Foundation::UInt64,
                                  Asteria::Foundation::UInt64, Asteria::Foundation::UInt64, Asteria::Foundation::UInt64,
                                  Asteria::Foundation::UInt64, Asteria::Foundation::UInt64) noexcept
{
    ++Asteria::Test::g_regionActivationCalls;
    Asteria::Test::g_lastRegionOneActivation = regionOne;
    return 0U;
}

namespace Asteria::Test
{
namespace
{
[[nodiscard]] Boot::BootFileRegion SourceRegion() noexcept
{
    Boot::BootFileRegion region{};
    region.PhysicalStart = reinterpret_cast<Foundation::UInt64>(AsteriaIa64BspKernelRuntimeInitialUserImageStorage);
    region.PageCount = (20480U + 4095U) / 4096U;
    return region;
}

// A fixed key stands in for the boot-derived BootstrapSpace key.
constexpr Kernel::Object::CapabilityValidationKey TestCapabilityValidationKey{
    .Word0 = 0x4941'3634'4341'5001ULL,
    .Word1 = 0x4941'3634'4341'5002ULL,
};

using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectPin;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectType;
using Kernel::Object::MonotonicTime;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityOperation;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
using Kernel::Resource::ResourceDomainUsage;
using Kernel::Resource::ResourceReservation;
using Kernel::Protection::ProtectionDomain;
using Kernel::Protection::ProtectionDomainId;
using Kernel::Protection::ProtectionDomainKind;
using Kernel::Protection::ProtectionDomainState;
using Kernel::Diagnostics::DiagnosticChannel;
using Kernel::Ipc::Endpoint;
using Kernel::Ipc::Notification;
using Kernel::Ipc::NotificationId;
using Kernel::Ipc::NotificationState;
using Kernel::Ipc::WaitSet;
using Kernel::Ipc::WaitSetState;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::ProcessorRunController;
using Kernel::Scheduler::ProcessorRunDispatchMode;
using Kernel::Scheduler::SchedulingContext;
using Kernel::Scheduler::SchedulingContextError;
using Kernel::Scheduler::SchedulingContextState;
using Kernel::Scheduler::Thread;
using Kernel::Scheduler::ThreadError;
using Kernel::Scheduler::ThreadId;
using Kernel::Time::Timer;
using Kernel::Time::TimerId;
using Kernel::Time::TimerNotificationDelivery;
using Kernel::Time::TimerNotificationDeliveryError;
using Kernel::Time::TimerQueue;
using Kernel::Time::TimerState;
using Boot::BootInfo;
using Boot::PhysicalMemoryKind;
using Boot::PhysicalMemoryRegion;
using Kernel::Memory::BootstrapPageAllocator;
using Kernel::Memory::BootstrapPageAllocatorError;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PhysicalPageRange;
using Kernel::Memory::TlbShootdownBatch;
using Kernel::Memory::TlbShootdownDispatcher;
using Kernel::Memory::UserMemoryAccessError;
using Kernel::Memory::UserMemoryRange;
using Kernel::Memory::WritableKernelBuffer;
using Platform::Ia64::Firmware::SalSystemTableInformation;
using Platform::Ia64::Executable::Ia64InitialUserImageMappingOwner;
using Platform::Ia64::Executable::Ia64InitialUserImageMemoryOwner;
using Platform::Ia64::Executable::Ia64InitialUserImageOwner;
using Platform::Ia64::Memory::BootstrapReservedPageCount;
using Platform::Ia64::Memory::Ia64BspRuntimeMemory;
using Platform::Ia64::Memory::Ia64BspRuntimeRootBorrowError;
using Platform::Ia64::Memory::Ia64BspRuntimeSystemCallPublicationError;
using Platform::Ia64::Memory::Ia64BspRuntimeExternalInterruptAcknowledgementError;
using Platform::Ia64::Memory::Ia64BspRuntimeExternalInterruptPublicationError;
using Platform::Ia64::Memory::Ia64RuntimeExternalInterruptPublicationError;
using Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTableContext;
using Platform::Ia64::Memory::InitialAddressSpacePreparation;
using Platform::Ia64::Memory::InitialInterruptVectorTableSizeBytes;
using Platform::Ia64::Memory::InitialRseBackingStoreSizeBytes;
using Platform::Ia64::Memory::InitialTranslationRegionSet;
using Platform::Ia64::Memory::PalVirtualMemoryCapabilities;
using Platform::Ia64::Memory::RegionConfiguration;
using Platform::Ia64::Memory::TranslationCapabilities;
using Platform::Ia64::Memory::WritableInterruptVectorTableView;
using Platform::Ia64::Memory::WritableRseBackingStoreView;
using Platform::Ia64::Scheduler::Ia64BspKernelRuntime;
using Platform::Ia64::Scheduler::Ia64BspKernelRuntimeError;
using Platform::Ia64::Scheduler::Ia64BspKernelRuntimeState;
using Platform::Ia64::Scheduler::Ia64BspSharedRootShootdownError;
using Platform::Ia64::Scheduler::Ia64BspSharedRootShootdownState;
using Platform::Ia64::Scheduler::Ia64BspKernelRuntimeLocalTimerProbeError;
using Platform::Ia64::Scheduler::Ia64BspKernelRuntimeLocalTimerProbeState;
using Platform::Ia64::Scheduler::Ia64BspKernelRuntimeTimerQueueEvidence;
using Platform::Ia64::Scheduler::Ia64BspKernelRuntimeUserProbeError;
using Platform::Ia64::Scheduler::Ia64BspKernelRuntimeUserProbeState;
using Platform::Ia64::Scheduler::Ia64BspKernelRuntimeInitialUserImageError;
using Platform::Ia64::Scheduler::Ia64BspKernelRuntimeInitialUserImageState;
using Platform::Ia64::Scheduler::Ia64InitialUserImageLaunchAccessError;
using Platform::Ia64::Scheduler::Ia64InitialUserImageLaunchOwnerState;
using Platform::Ia64::Scheduler::Ia64BspNativePreemptionLeadTicks;
using Platform::Ia64::Scheduler::Ia64BspNativePreemptionPeriodTicks;
using Platform::Ia64::Scheduler::MakeIa64BspNativePreemptionSchedulingConfiguration;
using Platform::Ia64::Scheduler::Ia64ProcessorRunLoop;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbe;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeCodeSizeBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeCodeView;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeMemoryStackSizeBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeRseBackingStoreSizeBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeStorage;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeStorageRegion;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeStageError;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeState;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeWritableAlias;
using Platform::Ia64::Scheduler::Ia64UserModeLaunchError;
using Platform::Ia64::Scheduler::Ia64UserModeLaunch;
using Platform::Ia64::Scheduler::Ia64UserModeSupervisorContinuationOwner;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptPollStatus;
using Platform::Ia64::System::Ia64SystemCallContextValidationError;
using Platform::Ia64::System::Ia64SystemCallBridgeContext;
using Platform::Ia64::System::ValidateIa64SystemCallTrustedContext;
using Platform::Synchronization::AtomicUInt64Reference;
using Platform::Synchronization::SpinLock;

constexpr Foundation::UInt64 PageBytes{4096U};
constexpr Foundation::Size InitialImageProgramHeaderTableOffset{64U};
constexpr Foundation::Size InitialImageProgramHeaderSizeBytes{56U};
constexpr Foundation::UInt64 InitialImageRegionOneBase{0x2000'0000'0000'0000ULL};
constexpr Foundation::UInt64 InitialImageCodeVirtualStart{InitialImageRegionOneBase + 0x0400'1100U};
constexpr Foundation::UInt64 InitialImageReadOnlyVirtualStart{InitialImageRegionOneBase + 0x0400'3100U};
constexpr Foundation::UInt64 InitialImageDataVirtualStart{InitialImageRegionOneBase + 0x0400'8100U};
constexpr Foundation::UInt64 InitialImageCodeFileOffset{0x1100U};
constexpr Foundation::UInt64 InitialImageReadOnlyFileOffset{0x2100U};
constexpr Foundation::UInt64 InitialImageDataFileOffset{0x3100U};
constexpr Foundation::UInt64 InitialImageDescriptorAddress{InitialImageReadOnlyVirtualStart + 0x20U};
constexpr Foundation::UInt64 InitialImageEntryBundleAddress{InitialImageCodeVirtualStart + 0x20U};
constexpr Foundation::UInt64 InitialImageGlobalPointer{InitialImageDataVirtualStart + 0x80U};
constexpr Foundation::UInt64 InitialImageFirmwareWriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 ReadyReservedBase{0x0100'0000U};
constexpr Foundation::UInt64 ReadyProbePoolBase{0x0400'0000U};
constexpr Foundation::UInt64 ReadyProbePoolPages{24U};
constexpr Foundation::UInt64 ReadyChargeOwner{0x7A64U};
constexpr Foundation::UInt16 ReadyDataTranslationRegisterCount{32U};
constexpr Foundation::UInt8 OverlapCodeFill{0x31U};
constexpr Foundation::UInt8 OverlapMemoryStackFill{0x52U};
constexpr Foundation::UInt8 OverlapRseFill{0x73U};
constexpr Foundation::UInt8 HostedBootstrapGuardFill{0xA6U};
constexpr Foundation::Size HostedBootstrapOffsetBytes{
    Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcBootstrapVirtualAddress -
    Platform::Ia64::Scheduler::Ia64UserExecutionProbeMemoryStackVirtualStart};
constexpr Foundation::UInt8 ExpectedEndpointSelectorBytes[8U]{
    0x01U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};
constexpr Foundation::UInt8 ExpectedWaitSetSelectorBytes[8U]{
    0x02U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};
constexpr Foundation::UInt8
    ExpectedUserProbeCanonicalFrame[Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcBootstrapFrameSizeBytes]{
        0x41U, 0x49U, 0x50U, 0x43U, 0x01U, 0x00U, 0x58U, 0x00U, 0x58U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    };

static_assert(HostedBootstrapOffsetBytes +
                  Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcBootstrapRecordSizeBytes <=
              Ia64UserExecutionProbeMemoryStackSizeBytes);
static_assert(sizeof(ExpectedEndpointSelectorBytes) == 8U);
static_assert(sizeof(ExpectedWaitSetSelectorBytes) == 8U);
static_assert(sizeof(ExpectedUserProbeCanonicalFrame) == 88U);

template <Foundation::Size DataSize> struct alignas(PageBytes) ProbeStorage
{
    Foundation::UInt8 Data[DataSize];
};

struct alignas(PageBytes) ReadyRuntimeArena
{
    Ia64BspRuntimeMemory RuntimeMemory;
    alignas(Ia64BspKernelRuntime) std::byte KernelRuntimeStorage[sizeof(Ia64BspKernelRuntime)]{};
    Ia64BspRuntimeMemory InitialUserImageRuntimeMemory;
    alignas(Ia64BspKernelRuntime) std::byte InitialUserImageKernelRuntimeStorage[sizeof(Ia64BspKernelRuntime)]{};
    Ia64BspRuntimeMemory ConflictRuntimeMemory;
    alignas(Ia64BspKernelRuntime) std::byte ConflictKernelRuntimeStorage[sizeof(Ia64BspKernelRuntime)]{};
    alignas(Ia64UserExecutionProbe) std::byte UserProbeStorage[sizeof(Ia64UserExecutionProbe)]{};
    Ia64BspRuntimeMemory SingleProcessorRuntimeMemory;
    alignas(Ia64BspKernelRuntime) std::byte SingleProcessorKernelRuntimeStorage[sizeof(Ia64BspKernelRuntime)]{};
    Ia64BspRuntimeMemory PreparationFailureRuntimeMemory;
    alignas(Ia64BspKernelRuntime) std::byte PreparationFailureKernelRuntimeStorage[sizeof(Ia64BspKernelRuntime)]{};
    alignas(Ia64UserExecutionProbe) std::byte PreparationFailureUserProbeStorage[sizeof(Ia64UserExecutionProbe)]{};
    struct alignas(PageBytes) ShootdownFailureSlot
    {
        Ia64BspRuntimeMemory RuntimeMemory;
        alignas(Ia64BspKernelRuntime) std::byte KernelRuntimeStorage[sizeof(Ia64BspKernelRuntime)]{};
    };
    ShootdownFailureSlot ShootdownFailures[4U];
    Kernel::Memory::BootFrame VirtualOverlapFrame;
    Kernel::Memory::MemoryObject VirtualOverlapMemoryObject;
    Kernel::Memory::PinnedFrameMappingPreparation VirtualOverlapPreparation;
};

// The arena is the run-time memory reservation: it fills its own architected
// page, so one DTR maps it wherever the host loads the test and no other host
// object shares that page (Ia64HostedBootGeometry).
constinit TestSupport::Ia64HostedRuntimeMemoryArena<ReadyRuntimeArena> g_readyRuntimeArenaStorage{};
ReadyRuntimeArena& g_readyRuntimeArena = g_readyRuntimeArenaStorage.Construct();

constinit SpinLock g_readyRegistryLock;
Asteria::TestSupport::TestObjectRegistry g_readyRegistry{g_readyRegistryLock};
constinit RootCapabilityIssuer g_readyIssuer{g_readyRegistry};
constinit BootstrapPageAllocator g_readyAllocator;
constinit PhysicalPageRange g_readySharedRootShootdownPage{};
constinit PhysicalPageRange g_singleProcessorSharedRootShootdownPage{};
constinit PhysicalPageRange g_sharedRootShootdownFailurePages[5U]{};
constinit InitialAddressSpacePreparation g_readyAddressSpacePreparation;
alignas(32U *
        1024U) constinit Foundation::UInt8 g_readyInitialInterruptVectorTable[InitialInterruptVectorTableSizeBytes]{};
alignas(PageBytes) constinit Foundation::UInt8 g_readyInitialRseBackingStore[InitialRseBackingStoreSizeBytes]{};
constinit ResourceDomainLedger g_readyResourceLedger;
constinit ResourceDomain g_readyResourceDomain{g_readyResourceLedger};
constinit ResourceDomainObjectId g_readyResourceDomainIdentity{};
constinit BootstrapPageAllocator g_initialImageAllocator;
constinit Ia64InitialUserImageOwner g_initialImageStagedOwner;
constinit Ia64InitialUserImageMemoryOwner g_initialImageMemoryOwner;
constinit Ia64InitialUserImageMemoryOwner g_initialImageUnreadyMemoryOwner;
constinit Ia64InitialUserImageMappingOwner g_initialImageDispatchMappingOwner;
alignas(PageBytes) constinit Foundation::UInt8 g_initialImageMaterializedArena[InitialImageBackingSizeBytes]{};
constinit ProbeStorage<Ia64UserExecutionProbeCodeSizeBytes> g_readyProbeCode{};
constinit ProbeStorage<Ia64UserExecutionProbeMemoryStackSizeBytes> g_readyProbeMemoryStack{};
constinit ProbeStorage<Ia64UserExecutionProbeRseBackingStoreSizeBytes> g_readyProbeRse{};
alignas(8U) constinit Foundation::UInt64 g_conflictBridgeContextToken[3U]{};

static_assert((sizeof(ReadyRuntimeArena) % PageBytes) == 0U);
static_assert(TestSupport::Ia64HostedRuntimeMemoryPageBytes(sizeof(ReadyRuntimeArena)) == 0x400'0000U);
static_assert(alignof(ReadyRuntimeArena) == PageBytes);

class TestLocalTlbInvalidator final : public Hal::Memory::ILocalTlbInvalidator
{
public:
    [[nodiscard]] Hal::Memory::LocalTlbInvalidationError
    Invalidate(const Kernel::Memory::VirtualAddress start, const Kernel::Memory::PageCount pages) noexcept override
    {
        ++m_callCount;
        m_lastStart = start;
        m_lastPages = pages;
        return Hal::Memory::LocalTlbInvalidationError::None;
    }

    [[nodiscard]] constexpr Foundation::Size GetCallCount() const noexcept
    {
        return m_callCount;
    }

    [[nodiscard]] constexpr Kernel::Memory::VirtualAddress GetLastStart() const noexcept
    {
        return m_lastStart;
    }

    [[nodiscard]] constexpr Kernel::Memory::PageCount GetLastPages() const noexcept
    {
        return m_lastPages;
    }

private:
    Foundation::Size m_callCount{0U};
    Kernel::Memory::VirtualAddress m_lastStart{};
    Kernel::Memory::PageCount m_lastPages{};
};

class CooperativeShootdownTransport final : public Hal::Memory::ITlbShootdownTransport
{
public:
    constexpr CooperativeShootdownTransport() noexcept
        : m_processorOne{CpuId{1U}, m_invalidatorOne}, m_processorTwo{CpuId{2U}, m_invalidatorTwo},
          m_processorThree{CpuId{3U}, m_invalidatorThree}
    {
    }

    [[nodiscard]] Hal::Memory::TlbShootdownTransportError Publish(const TlbShootdownBatch& batch) noexcept override
    {
        ++m_publishCount;
        for (Foundation::UInt8 index{0U}; index < batch.GetCount(); ++index)
        {
            const auto& work = batch.GetWork(index);
            const Foundation::UInt8 processor = work.GetTargetProcessor().GetValue();
            TlbShootdownDispatcher* dispatcher{nullptr};
            if (processor == 1U)
            {
                dispatcher = &m_processorOne;
            }
            else if (processor == 2U)
            {
                dispatcher = &m_processorTwo;
            }
            else if (processor == 3U)
            {
                dispatcher = &m_processorThree;
            }
            if ((dispatcher == nullptr) || !dispatcher->HandleRemote(work).HasValue())
            {
                return Hal::Memory::TlbShootdownTransportError::Unavailable;
            }
            m_observedProcessorMask = static_cast<Foundation::UInt8>(
                m_observedProcessorMask | static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << processor));
        }
        return Hal::Memory::TlbShootdownTransportError::None;
    }

    [[nodiscard]] constexpr Foundation::Size GetPublishCount() const noexcept
    {
        return m_publishCount;
    }

    [[nodiscard]] constexpr Foundation::UInt8 GetObservedProcessorMask() const noexcept
    {
        return m_observedProcessorMask;
    }

    [[nodiscard]] constexpr bool HasExactRemoteInvalidations() const noexcept
    {
        return (m_invalidatorOne.GetCallCount() == 1U) && (m_invalidatorTwo.GetCallCount() == 1U) &&
               (m_invalidatorThree.GetCallCount() == 1U) &&
               (m_invalidatorOne.GetLastStart().GetValue() ==
                Platform::Ia64::Scheduler::Ia64BspSharedRootShootdownVirtualStart) &&
               (m_invalidatorTwo.GetLastStart().GetValue() ==
                Platform::Ia64::Scheduler::Ia64BspSharedRootShootdownVirtualStart) &&
               (m_invalidatorThree.GetLastStart().GetValue() ==
                Platform::Ia64::Scheduler::Ia64BspSharedRootShootdownVirtualStart) &&
               (m_invalidatorOne.GetLastPages().GetValue() == 1U) &&
               (m_invalidatorTwo.GetLastPages().GetValue() == 1U) &&
               (m_invalidatorThree.GetLastPages().GetValue() == 1U);
    }

private:
    TestLocalTlbInvalidator m_invalidatorOne;
    TestLocalTlbInvalidator m_invalidatorTwo;
    TestLocalTlbInvalidator m_invalidatorThree;
    TlbShootdownDispatcher m_processorOne;
    TlbShootdownDispatcher m_processorTwo;
    TlbShootdownDispatcher m_processorThree;
    Foundation::Size m_publishCount{0U};
    Foundation::UInt8 m_observedProcessorMask{0U};
};

enum class ScriptedShootdownTransportMode : Foundation::UInt8
{
    RejectPublish,
    RejectRemote,
    RetainWithoutAcknowledgement,
};

class ScriptedShootdownTransport final : public Hal::Memory::ITlbShootdownTransport
{
public:
    explicit constexpr ScriptedShootdownTransport(const ScriptedShootdownTransportMode mode) noexcept : m_mode{mode}
    {
    }

    [[nodiscard]] Hal::Memory::TlbShootdownTransportError Publish(const TlbShootdownBatch& batch) noexcept override
    {
        ++m_publishCount;
        if (m_mode == ScriptedShootdownTransportMode::RejectPublish)
        {
            return Hal::Memory::TlbShootdownTransportError::Unavailable;
        }
        m_retainedBatch = batch;
        m_hasRetainedBatch = true;
        if (m_mode == ScriptedShootdownTransportMode::RetainWithoutAcknowledgement)
        {
            return Hal::Memory::TlbShootdownTransportError::None;
        }
        for (Foundation::UInt8 index{0U}; index < batch.GetCount(); ++index)
        {
            const auto& work = batch.GetWork(index);
            if ((work.GetOwner() == nullptr) ||
                (work.GetOwner()->RejectShootdown(work.GetToken(), work.GetTargetProcessor()) !=
                 Kernel::Memory::AddressSpaceError::None))
            {
                return Hal::Memory::TlbShootdownTransportError::Unavailable;
            }
        }
        m_hasRetainedBatch = false;
        m_retainedBatch = {};
        return Hal::Memory::TlbShootdownTransportError::None;
    }

    [[nodiscard]] constexpr Foundation::Size GetPublishCount() const noexcept
    {
        return m_publishCount;
    }

    [[nodiscard]] constexpr bool HasRetainedBatch() const noexcept
    {
        return m_hasRetainedBatch;
    }

    [[nodiscard]] constexpr bool RetainsTransaction(const Foundation::UInt64 sequence) const noexcept
    {
        if (!m_hasRetainedBatch || (m_retainedBatch.GetCount() != 1U))
        {
            return false;
        }
        const auto& work = m_retainedBatch.GetWork(0U);
        return (work.GetOwner() != nullptr) && (work.GetToken().GetSequence() == sequence) &&
               (work.GetInvalidationStart().GetValue() ==
                Platform::Ia64::Scheduler::Ia64BspSharedRootShootdownVirtualStart) &&
               (work.GetInvalidationPages().GetValue() == 1U) && (work.GetTargetProcessor().GetValue() == 1U);
    }

private:
    TlbShootdownBatch m_retainedBatch{};
    Foundation::Size m_publishCount{0U};
    ScriptedShootdownTransportMode m_mode;
    bool m_hasRetainedBatch{false};
};

void TestNativePreemptionAdmissionMargin(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    Thread thread;
    const bool threadInitialized = thread.Initialize(ResourceDomainId{0x6401U}) == ThreadError::None;
    const auto registered = issuer.CreateObject(thread);
    const ThreadId threadIdentity = registered.HasValue() ? ThreadId{registry, registered.GetValue()} : ThreadId{};

    constexpr Foundation::UInt64 initialTicks{1'000U};
    constexpr Foundation::UInt64 admissionDelayTicks{Ia64BspNativePreemptionLeadTicks / 4U};
    const Foundation::UInt64 startTicks = initialTicks + admissionDelayTicks;
    bool exactWindow{false};
    {
        SchedulingContext context{registry};
        const auto configuration = MakeIa64BspNativePreemptionSchedulingConfiguration(0x01U);
        const SchedulingContextError initializeError =
            context.Initialize(configuration, ResourceDomainId{0x6401U}, MonotonicTime{initialTicks});
        const SchedulingContextError bindError = context.Bind(threadIdentity);
        const auto admitted = context.StartRun(threadIdentity, CpuId{0U}, MonotonicTime{startTicks});
        const auto completed = context.EndRun(CpuId{0U}, MonotonicTime{startTicks + Ia64BspNativePreemptionLeadTicks});
        const auto snapshot = context.GetSnapshot();
        exactWindow = (initializeError == SchedulingContextError::None) &&
                      (bindError == SchedulingContextError::None) && admitted.HasValue() &&
                      (admitted.GetValue().MaximumRunTicks == Ia64BspNativePreemptionLeadTicks) &&
                      completed.HasValue() && (completed.GetValue().ElapsedTicks == Ia64BspNativePreemptionLeadTicks) &&
                      (completed.GetValue().RemainingBudgetTicks == 0U) &&
                      (completed.GetValue().BudgetExhausted == 1U) && (completed.GetValue().DeadlineMissed == 0U) &&
                      (snapshot.State == SchedulingContextState::BudgetExhausted) &&
                      (snapshot.NextReplenishmentTicks == initialTicks + Ia64BspNativePreemptionPeriodTicks);
    }

    const bool destructionStarted = registered.HasValue() && (issuer.DestroyObject(registered.GetValue()) ==
                                                              Kernel::Object::ObjectRegistryError::Ok);
    const auto collected = issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner,
                   "native-preemption admission delay does not consume the admitted "
                   "run budget",
                   threadInitialized, registered.HasValue(), exactWindow, destructionStarted, collected.HasValue(),
                   collected.GetValue().Instance == &thread);
}

class ModelLocalTimer final : public Hal::Time::ILocalTimer
{
public:
    [[nodiscard]] Foundation::UInt64 GetTicks() const noexcept override
    {
        return m_ticks;
    }

    [[nodiscard]] Hal::Time::LocalTimerError ProgramDeadline(const Foundation::UInt64 deadlineTicks) noexcept override
    {
        if ((deadlineTicks == 0U) || (deadlineTicks == ~Foundation::UInt64{0U}))
        {
            return Hal::Time::LocalTimerError::InvalidDeadline;
        }
        if (deadlineTicks <= m_ticks)
        {
            return Hal::Time::LocalTimerError::DeadlineReached;
        }
        m_deadlineTicks = deadlineTicks;
        m_armed = true;
        ++m_programCalls;
        return Hal::Time::LocalTimerError::None;
    }

    [[nodiscard]] Hal::Time::LocalTimerError CancelDeadline() noexcept override
    {
        m_deadlineTicks = 0U;
        m_armed = false;
        ++m_cancelCalls;
        return Hal::Time::LocalTimerError::None;
    }

    [[nodiscard]] Hal::Time::LocalTimerError
    AcknowledgeExternallyMaskedDeadline(const Foundation::UInt64 deadlineTicks) noexcept override
    {
        if ((deadlineTicks == 0U) || (deadlineTicks != m_deadlineTicks))
        {
            return Hal::Time::LocalTimerError::InvalidDeadline;
        }
        m_deadlineTicks = 0U;
        m_armed = false;
        return Hal::Time::LocalTimerError::None;
    }

    void SetTicks(const Foundation::UInt64 ticks) noexcept
    {
        m_ticks = ticks;
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetDeadlineTicks() const noexcept
    {
        return m_deadlineTicks;
    }

    [[nodiscard]] constexpr bool IsArmed() const noexcept
    {
        return m_armed;
    }

    [[nodiscard]] constexpr Foundation::Size GetProgramCalls() const noexcept
    {
        return m_programCalls;
    }

    [[nodiscard]] constexpr Foundation::Size GetCancelCalls() const noexcept
    {
        return m_cancelCalls;
    }

private:
    Foundation::UInt64 m_ticks{10U};
    Foundation::UInt64 m_deadlineTicks{0U};
    Foundation::Size m_programCalls{0U};
    Foundation::Size m_cancelCalls{0U};
    bool m_armed{false};
};

[[nodiscard]] constexpr ResourceDomainLimits ResourceLimits() noexcept
{
    return ResourceDomainLimits{
        .CommittedMemoryPages = 1024U,
        .ResidentMemoryPages = 1024U,
        .KernelMetadataBytes = 1024U * 1024U,
        .CapabilityHandles = 256U,
        .IpcQueueBytes = 64U * 1024U,
        .Threads = 16U,
        .ChildDomains = 4U,
    };
}

[[nodiscard]] constexpr PalVirtualMemoryCapabilities Capabilities() noexcept
{
    return PalVirtualMemoryCapabilities{
        .Translation =
            TranslationCapabilities{
                .InsertablePageSizeMask = Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask,
                .PhysicalAddressBits = 50U,
                .ImplementedVirtualAddressMostSignificantBit = 60U,
                .RegionIdentifierBits = 24U,
                .ProtectionKeyBits = 24U,
                .MemoryAttributeMask = 0x71U,
                .HardwareWalkerImplemented = true,
                .Reserved16 = 0U,
                .Reserved64 = 0U,
            },
        .PurgePageSizeMask = Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask,
        .InstructionTranslationRegisterCount = 32U,
        .DataTranslationRegisterCount = ReadyDataTranslationRegisterCount,
        .ProtectionKeyRegisterCount = 16U,
        .MaximumConcurrentPurgesEncoding = 1U,
        .HashTagIdentifier = 0U,
        .UniqueTranslationCacheCount = 1U,
        .TranslationCacheLevelCount = 1U,
        .Reserved8 = 0U,
        .Reserved32 = 0U,
    };
}

[[nodiscard]] Foundation::UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
}

void WriteInitialImageLittleEndian16(Foundation::UInt8* const bytes, const Foundation::UInt16 value) noexcept
{
    bytes[0U] = static_cast<Foundation::UInt8>(value);
    bytes[1U] = static_cast<Foundation::UInt8>(value >> 8U);
}

void WriteInitialImageLittleEndian32(Foundation::UInt8* const bytes, const Foundation::UInt32 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

void WriteInitialImageLittleEndian64(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

void SetInitialImageProgramHeader(const Foundation::Size index, const Foundation::UInt32 flags,
                                  const Foundation::UInt64 fileOffset, const Foundation::UInt64 virtualStart,
                                  const Foundation::UInt64 fileSizeBytes,
                                  const Foundation::UInt64 memorySizeBytes) noexcept
{
    Foundation::UInt8* const header = AsteriaIa64BspKernelRuntimeInitialUserImageStorage +
                                      InitialImageProgramHeaderTableOffset +
                                      (index * InitialImageProgramHeaderSizeBytes);
    WriteInitialImageLittleEndian32(header, 1U);
    WriteInitialImageLittleEndian32(header + 4U, flags);
    WriteInitialImageLittleEndian64(header + 8U, fileOffset);
    WriteInitialImageLittleEndian64(header + 16U, virtualStart);
    WriteInitialImageLittleEndian64(header + 24U, virtualStart);
    WriteInitialImageLittleEndian64(header + 32U, fileSizeBytes);
    WriteInitialImageLittleEndian64(header + 40U, memorySizeBytes);
    WriteInitialImageLittleEndian64(header + 48U, PageBytes);
}

void FillInitialImagePayload(const Foundation::UInt64 fileOffset, const Foundation::UInt64 sizeBytes,
                             const Foundation::UInt8 seed) noexcept
{
    for (Foundation::UInt64 index{0U}; index < sizeBytes; ++index)
    {
        AsteriaIa64BspKernelRuntimeInitialUserImageStorage[static_cast<Foundation::Size>(fileOffset + index)] =
            static_cast<Foundation::UInt8>(seed + static_cast<Foundation::UInt8>(index));
    }
}

void MakeInitialUserImage() noexcept
{
    static_cast<void>(std::memset(AsteriaIa64BspKernelRuntimeInitialUserImageStorage, 0, InitialImageSourceSizeBytes));
    Foundation::UInt8* const image = AsteriaIa64BspKernelRuntimeInitialUserImageStorage;
    image[0U] = 0x7FU;
    image[1U] = static_cast<Foundation::UInt8>('E');
    image[2U] = static_cast<Foundation::UInt8>('L');
    image[3U] = static_cast<Foundation::UInt8>('F');
    image[4U] = 2U;
    image[5U] = 1U;
    image[6U] = 1U;
    WriteInitialImageLittleEndian16(image + 16U, 2U);
    WriteInitialImageLittleEndian16(image + 18U, Platform::Ia64::Executable::Ia64ElfMachine);
    WriteInitialImageLittleEndian32(image + 20U, 1U);
    WriteInitialImageLittleEndian64(image + 24U, InitialImageDescriptorAddress);
    WriteInitialImageLittleEndian64(image + 32U, InitialImageProgramHeaderTableOffset);
    WriteInitialImageLittleEndian32(image + 48U, Platform::Ia64::Executable::Ia64P64ElfFlags);
    WriteInitialImageLittleEndian16(image + 52U, 64U);
    WriteInitialImageLittleEndian16(image + 54U, InitialImageProgramHeaderSizeBytes);
    WriteInitialImageLittleEndian16(image + 56U, 3U);
    SetInitialImageProgramHeader(0U, 5U, InitialImageCodeFileOffset, InitialImageCodeVirtualStart, 0x180U, 0x300U);
    SetInitialImageProgramHeader(1U, 4U, InitialImageReadOnlyFileOffset, InitialImageReadOnlyVirtualStart, 0x300U,
                                 0x1100U);
    SetInitialImageProgramHeader(2U, 6U, InitialImageDataFileOffset, InitialImageDataVirtualStart, 0x900U, 0x3F00U);
    FillInitialImagePayload(InitialImageCodeFileOffset, 0x180U, 0x31U);
    FillInitialImagePayload(InitialImageReadOnlyFileOffset, 0x300U, 0x52U);
    FillInitialImagePayload(InitialImageDataFileOffset, 0x900U, 0x73U);
    const Foundation::Size descriptorOffset = static_cast<Foundation::Size>(
        InitialImageReadOnlyFileOffset + (InitialImageDescriptorAddress - InitialImageReadOnlyVirtualStart));
    WriteInitialImageLittleEndian64(image + descriptorOffset, InitialImageEntryBundleAddress);
    WriteInitialImageLittleEndian64(image + descriptorOffset + 8U, InitialImageGlobalPointer);
}

[[nodiscard]] bool PrepareInitialImageOwners(const ResourceDomainObjectId resourceDomainIdentity) noexcept
{
    MakeInitialUserImage();
    BootInfo bootInfo{
        .Signature = Boot::BootInfoSignature,
        .Version = Boot::BootInfoVersion,
        .SizeBytes = sizeof(BootInfo),
        .MemoryRegionCount = 1U,
        .FirmwareDescriptorVersion = 1U,
        .FirmwareDescriptorSize = 40U,
        .Reserved = 0U,
        .FirmwareMemoryMapSizeBytes = 40U,
        .MemoryRegions = {},
        .Acpi = {},
        .Entropy = {},
        .Display = {},
        .Inventory = {},
        .Files = {},
        .Crc32c = 0U,
        .ReservedAbi = 0U,
        .Kernel = {},
        .InventoryRecord = {},
        .DebugPort = {},
        .Trust = {},
        .DmaExclusions = {},
        .LowMemoryBlocks = 0U,
        .WatchdogSeconds = Asteria::Abi::WatchdogDefaultSeconds,
        .Power = {},
        .SessionMode = 0U,
        .LoginEnabled = 0U,
    };
    bootInfo.MemoryRegions[0U] = PhysicalMemoryRegion{
        .PhysicalStart = AddressOf(g_initialImageMaterializedArena),
        .VirtualStart = 0U,
        .PageCount = InitialImageBackingPageCount,
        .Attributes = InitialImageFirmwareWriteBackAttribute,
        .FirmwareType = 7U,
        .Kind = PhysicalMemoryKind::Conventional,
    };
    if (g_initialImageAllocator.Initialize(bootInfo) != BootstrapPageAllocatorError::None)
    {
        return false;
    }
    const auto staged = g_initialImageStagedOwner.Stage(
        g_initialImageAllocator, 48U, Capabilities().Translation.ImplementedVirtualAddressMostSignificantBit,
        {AsteriaIa64BspKernelRuntimeInitialUserImageStorage, 20480U}, SourceRegion());
    if (!staged.HasValue())
    {
        return false;
    }
    const auto materialized = g_initialImageMemoryOwner.Materialize(
        g_initialImageStagedOwner, g_readyRegistry, g_readyIssuer, resourceDomainIdentity, g_readyResourceDomain);
    if (!materialized.HasValue())
    {
        return false;
    }
    return g_initialImageDispatchMappingOwner
        .Install(g_initialImageMemoryOwner, g_readyRuntimeArena.InitialUserImageRuntimeMemory, g_readyRegistry)
        .HasValue();
}

void ResetHostedLaunchObservations() noexcept
{
    g_timerProgramCalls = 0U;
    g_timerCancelCalls = 0U;
    g_localPurgeCalls = 0U;
    g_userLaunchCalls = 0U;
    g_programmedTimerDeadline = 0U;
    g_programmedTimerVector = 0U;
    g_programmedTimerRollbackDeadline = 0U;
    g_programmedTimerRollbackVector = 0U;
    g_programmedTimerMaskedVector = 0U;
    g_cancelledTimerVector = 0U;
    g_workerSwitchWasActiveAtLaunch = false;
    g_userLaunchPlanCaptured = false;
    g_userLaunchPlan = {};
    g_regionActivationCalls = 0U;
    g_lastRegionOneActivation = 0U;
    g_regionOneAtUserLaunch = 0U;
}

[[nodiscard]] constexpr bool ResourceUsageIsEqual(const ResourceDomainUsage& first,
                                                  const ResourceDomainUsage& second) noexcept
{
    return (first.CommittedMemoryPages == second.CommittedMemoryPages) &&
           (first.ResidentMemoryPages == second.ResidentMemoryPages) &&
           (first.KernelMetadataBytes == second.KernelMetadataBytes) &&
           (first.CapabilityHandles == second.CapabilityHandles) && (first.IpcQueueBytes == second.IpcQueueBytes) &&
           (first.Threads == second.Threads) && (first.ChildDomains == second.ChildDomains);
}

[[nodiscard]] constexpr bool ResourceUsageHasExactInitialUserImageIncrease(const ResourceDomainUsage& before,
                                                                           const ResourceDomainUsage& after) noexcept
{
    return (after.CommittedMemoryPages == before.CommittedMemoryPages) &&
           (after.ResidentMemoryPages == before.ResidentMemoryPages) &&
           (after.KernelMetadataBytes == before.KernelMetadataBytes + sizeof(Endpoint) + sizeof(DiagnosticChannel)) &&
           (after.CapabilityHandles == before.CapabilityHandles + 2U) &&
           (after.IpcQueueBytes == before.IpcQueueBytes + 88U) && (after.Threads == before.Threads) &&
           (after.ChildDomains == before.ChildDomains);
}

[[nodiscard]] constexpr bool
LaunchPlansAreEqual(const Platform::Ia64::Scheduler::Ia64UserModeLaunchPlan& first,
                    const Platform::Ia64::Scheduler::Ia64UserModeLaunchPlan& second) noexcept
{
    return (first.EntryBundleAddress == second.EntryBundleAddress) &&
           (first.UserStackPointer == second.UserStackPointer) &&
           (first.UserRseBackingStorePointer == second.UserRseBackingStorePointer) &&
           (first.UserProcessorStatus == second.UserProcessorStatus) &&
           (first.UserRseConfiguration == second.UserRseConfiguration) &&
           (first.CurrentProcessorStatusMask == second.CurrentProcessorStatusMask) &&
           (first.CurrentProcessorStatusValue == second.CurrentProcessorStatusValue) &&
           (first.SupervisorContinuationAddress == second.SupervisorContinuationAddress) &&
           (first.InitialArguments.Register4 == second.InitialArguments.Register4) &&
           (first.InitialArguments.Register5 == second.InitialArguments.Register5) &&
           (first.InitialArguments.Register6 == second.InitialArguments.Register6) &&
           (first.InitialArguments.Register7 == second.InitialArguments.Register7) &&
           (first.InitialGlobalPointer == second.InitialGlobalPointer);
}

/// The r6/r7 capability of the prepared image: an Inspect|Write, infinite,
/// unsealed DiagnosticChannel capability charged to the root domain, in a
/// revocation root distinct from the r4/r5 Endpoint capability's.
[[nodiscard, gnu::noinline]] bool
HasExactInitialUserImageDiagnosticChannel(const Ia64BspKernelRuntime& runtime,
                                          Kernel::Object::CapabilitySpace& capabilitySpace) noexcept
{
    const auto summary = runtime.GetInitialUserImageLaunchOwner().GetSummary();
    const auto request = runtime.GetInitialUserImageLaunchOwner().GetLaunchRequest();
    if (!summary.HasValue() || !request.HasValue())
    {
        return false;
    }
    const CapabilityHandle handle{
        request.GetValue().InitialArguments.Register6,
        request.GetValue().InitialArguments.Register7,
    };
    const auto descriptor = capabilitySpace.Query(handle);
    const auto endpointDescriptor = capabilitySpace.Query(summary.GetValue().BootstrapCapability);
    if (!descriptor.HasValue() || !endpointDescriptor.HasValue())
    {
        return false;
    }
    ObjectPin channelPin;
    const bool channelMatches = (g_readyRegistry.PinObject(descriptor.GetValue().Object, ObjectType::DiagnosticChannel,
                                                           channelPin) == ObjectPinError::Ok) &&
                                (channelPin.GetObject().GetChargeOwner().GetValue() == ReadyChargeOwner);
    channelPin.Reset();
    constexpr CapabilityRights diagnosticRights{
        CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Write)};
    return channelMatches && handle.IsValid() &&
           (handle.GetSelector() == summary.GetValue().DiagnosticCapability.GetSelector()) &&
           (handle.GetAuthenticator() == summary.GetValue().DiagnosticCapability.GetAuthenticator()) &&
           (descriptor.GetValue().Type == ObjectType::DiagnosticChannel) &&
           descriptor.GetValue().Rights.IsEqualTo(diagnosticRights) &&
           descriptor.GetValue().RevocationDomain.IsValid() &&
           (descriptor.GetValue().RevocationDomain.GetValue() !=
            endpointDescriptor.GetValue().RevocationDomain.GetValue()) &&
           descriptor.GetValue().LeaseDeadline.IsInfinite() && (descriptor.GetValue().SealedObjectSelector == 0U) &&
           (descriptor.GetValue().ChargeOwner.GetValue() == ReadyChargeOwner);
}

[[nodiscard, gnu::noinline]] bool
HasExactInitialUserImageAuthority(const Ia64BspKernelRuntime& runtime,
                                  const ProtectionDomainId protectionDomainIdentity) noexcept
{
    const auto& owner = runtime.GetInitialUserImageLaunchOwner();
    const auto summary = owner.GetSummary();
    const auto request = owner.GetLaunchRequest();
    const auto memorySummary = g_initialImageMemoryOwner.GetSummary();
    const auto mappingSummary = g_initialImageDispatchMappingOwner.GetSummary();
    if (!summary.HasValue() || !request.HasValue() || !memorySummary.HasValue() || !mappingSummary.HasValue())
    {
        return false;
    }

    ObjectPin protectionDomainPin;
    if (g_readyRegistry.PinObject(protectionDomainIdentity.GetObjectId(), ObjectType::ProtectionDomain,
                                  protectionDomainPin) != ObjectPinError::Ok)
    {
        return false;
    }
    auto& protectionDomain = static_cast<ProtectionDomain&>(protectionDomainPin.GetObject());
    const auto binding = protectionDomain.GetBinding();
    if (!binding.HasValue() || (binding.GetValue().Capabilities == nullptr))
    {
        return false;
    }
    Kernel::Object::CapabilitySpace& capabilitySpace = *binding.GetValue().Capabilities;
    const CapabilityHandle handle{
        request.GetValue().InitialArguments.Register4,
        request.GetValue().InitialArguments.Register5,
    };
    const auto descriptor = capabilitySpace.Query(handle);
    const auto launchPlan = Ia64UserModeLaunch::Validate(request.GetValue());
    constexpr CapabilityRights bootstrapRights{
        CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Invoke).With(CapabilityRight::Read)};
    ObjectPin endpointPin;
    const ObjectPinError endpointPinError =
        descriptor.HasValue()
            ? g_readyRegistry.PinObject(descriptor.GetValue().Object, ObjectType::Endpoint, endpointPin)
            : ObjectPinError::InvalidArgument;
    bool endpointMatches{false};
    if (endpointPinError == ObjectPinError::Ok)
    {
        auto& endpoint = static_cast<Endpoint&>(endpointPin.GetObject());
        const Kernel::Ipc::EndpointQueueState queue = endpoint.GetQueueState();
        endpointMatches = endpointPin.IsValid() && endpoint.HasReceiverCapabilitySpace(capabilitySpace) &&
                          endpoint.IsReady() && (endpoint.GetChargeOwner().GetValue() == ReadyChargeOwner) &&
                          (queue.QueuedBytes == 0U) && (queue.NextSequence == 1U) && (queue.QueuedMessages == 0U) &&
                          (queue.QueuedAttachments == 0U);
    }
    endpointPin.Reset();
    const Foundation::UInt64 runtimeStart = AddressOf(&runtime);
    const Foundation::UInt64 runtimeEnd = runtimeStart + sizeof(runtime);
    const Foundation::UInt64 continuationAddress = AddressOf(request.GetValue().SupervisorContinuation);
    const Foundation::UInt64 codePageStart = InitialImageCodeVirtualStart & ~(PageBytes - 1U);
    return (owner.GetState() == Ia64InitialUserImageLaunchOwnerState::Ready) &&
           summary.GetValue().AddressSpaceIdentity.IsEqualTo(mappingSummary.GetValue().AddressSpaceIdentity) &&
           summary.GetValue().ProtectionDomainIdentity.IsEqualTo(protectionDomainIdentity) &&
           summary.GetValue().ResourceDomainIdentity.IsEqualTo(g_readyResourceDomainIdentity) &&
           (summary.GetValue().CapabilitySpaceIdentity.GetValue() == capabilitySpace.GetIdentifier().GetValue()) &&
           (summary.GetValue().RangeCount == 5U) && (summary.GetValue().EntryMappingIndex == 0U) &&
           (summary.GetValue().GlobalPointerMappingIndex == 2U) && (summary.GetValue().MemoryStackMappingIndex == 3U) &&
           (summary.GetValue().RseBackingStoreMappingIndex == 4U) &&
           (summary.GetValue().FirstMappingSequence == mappingSummary.GetValue().FirstMappingSequence) &&
           (summary.GetValue().LastMappingSequence == mappingSummary.GetValue().LastMappingSequence) &&
           (memorySummary.GetValue().Image.EntryBundleAddress == InitialImageEntryBundleAddress) &&
           (memorySummary.GetValue().Image.GlobalPointer == InitialImageGlobalPointer) &&
           (InitialImageGlobalPointer != 0U) && (request.GetValue().Code.VirtualStart == codePageStart) &&
           (request.GetValue().Code.SizeBytes == PageBytes) &&
           (request.GetValue().EntryBundleAddress == InitialImageEntryBundleAddress) &&
           (request.GetValue().InitialGlobalPointer == InitialImageGlobalPointer) &&
           (request.GetValue().MemoryStack.VirtualStart == Platform::Ia64::Ia64InitialUserMemoryStackVirtualStart) &&
           (request.GetValue().MemoryStack.SizeBytes == Platform::Ia64::Ia64InitialUserMemoryStackSizeBytes) &&
           (request.GetValue().RseBackingStore.VirtualStart ==
            Platform::Ia64::Ia64InitialUserRseBackingStoreVirtualStart) &&
           (request.GetValue().RseBackingStore.SizeBytes == Platform::Ia64::Ia64InitialUserRseBackingStoreSizeBytes) &&
           handle.IsValid() && (handle.GetSelector() == summary.GetValue().BootstrapCapability.GetSelector()) &&
           (handle.GetAuthenticator() == summary.GetValue().BootstrapCapability.GetAuthenticator()) &&
           (capabilitySpace.GetActiveCount() == 2U) &&
           HasExactInitialUserImageDiagnosticChannel(runtime, capabilitySpace) && descriptor.HasValue() &&
           (descriptor.GetValue().Object.GetValue() != protectionDomainIdentity.GetObjectId().GetValue()) &&
           (descriptor.GetValue().Type == ObjectType::Endpoint) &&
           descriptor.GetValue().Rights.IsEqualTo(bootstrapRights) &&
           descriptor.GetValue().RevocationDomain.IsValid() && descriptor.GetValue().LeaseDeadline.IsInfinite() &&
           (descriptor.GetValue().SealedObjectSelector == 0U) &&
           (descriptor.GetValue().ChargeOwner.GetValue() == ReadyChargeOwner) && endpointMatches &&
           launchPlan.HasValue() && (continuationAddress >= runtimeStart) &&
           (continuationAddress <= runtimeEnd - sizeof(*request.GetValue().SupervisorContinuation)) &&
           (request.GetValue().SupervisorContinuation->MemoryStack.SizeBytes ==
            Platform::Ia64::Scheduler::MinimumIa64KernelMemoryStackSizeBytes) &&
           (request.GetValue().SupervisorContinuation->RseBackingStore.SizeBytes ==
            Platform::Ia64::Scheduler::MinimumIa64KernelRseBackingStoreSizeBytes) &&
           (request.GetValue().SupervisorContinuation->State ==
            Platform::Ia64::Scheduler::Ia64UserModeSupervisorContinuationState::Fresh) &&
           (request.GetValue().SupervisorContinuation->Owner == Ia64UserModeSupervisorContinuationOwner::None);
}

struct InitialUserImagePreparedFixture
{
    Ia64BspKernelRuntime* Runtime;
    ProtectionDomainId ProtectionDomainIdentity;
    ThreadId ThreadIdentity;
};

[[nodiscard]] bool HasExactPreparedCapabilityAuthority(ObjectRegistry& registry,
                                                       const ProtectionDomainId protectionDomainIdentity,
                                                       const Ia64BspKernelRuntime& runtime,
                                                       const Foundation::Size expectedActiveCount) noexcept
{
    ObjectPin protectionDomainPin;
    if (registry.PinObject(protectionDomainIdentity.GetObjectId(), ObjectType::ProtectionDomain, protectionDomainPin) !=
        ObjectPinError::Ok)
    {
        return false;
    }

    auto& protectionDomain = static_cast<ProtectionDomain&>(protectionDomainPin.GetObject());
    const auto binding = protectionDomain.GetBinding();
    if (!binding.HasValue() || (binding.GetValue().Capabilities == nullptr))
    {
        return false;
    }

    const auto snapshot = protectionDomain.GetSnapshot();
    const Kernel::Object::CapabilitySpace* const capabilitySpace = binding.GetValue().Capabilities;
    const Kernel::Object::CapabilitySpaceIdentifier capabilitySpaceIdentity = capabilitySpace->GetIdentifier();
    const Foundation::UInt64 runtimeStart = AddressOf(&runtime);
    const Foundation::UInt64 runtimeEnd = runtimeStart + sizeof(runtime);
    const Foundation::UInt64 capabilitySpaceStart = AddressOf(capabilitySpace);
    const Foundation::UInt64 capabilitySpaceEnd = capabilitySpaceStart + sizeof(*capabilitySpace);
    return snapshot.Identity.IsEqualTo(protectionDomainIdentity) &&
           (snapshot.Kind == ProtectionDomainKind::UserCapable) && (snapshot.State == ProtectionDomainState::Ready) &&
           capabilitySpace->IsReady() && capabilitySpace->IsFromRegistry(registry) &&
           (capabilitySpace->GetActiveCount() == expectedActiveCount) && capabilitySpaceIdentity.IsValid() &&
           (snapshot.CapabilitySpace.GetValue() == capabilitySpaceIdentity.GetValue()) &&
           (binding.GetValue().CapabilitySpace.GetValue() == capabilitySpaceIdentity.GetValue()) &&
           (capabilitySpaceStart >= runtimeStart) && (capabilitySpaceEnd <= runtimeEnd);
}

[[nodiscard]] bool HasExactLocalTimerProbeObjects(ObjectRegistry& registry, const Ia64BspKernelRuntime& runtime,
                                                  const Foundation::UInt64 expectedLastNotificationClaim) noexcept
{
    const TimerId timerIdentity = runtime.GetLocalTimerInterruptTimerIdentity();
    const NotificationId notificationIdentity = runtime.GetLocalTimerInterruptNotificationIdentity();
    if (!timerIdentity.IsValid() || !timerIdentity.IsFromRegistry(registry) || !notificationIdentity.IsValid() ||
        !notificationIdentity.IsFromRegistry(registry) || (timerIdentity.GetValue() == notificationIdentity.GetValue()))
    {
        return false;
    }

    ObjectPin timerPin;
    ObjectPin notificationPin;
    if ((registry.PinObject(timerIdentity.GetObjectId(), ObjectType::Timer, timerPin) != ObjectPinError::Ok) ||
        (registry.PinObject(notificationIdentity.GetObjectId(), ObjectType::Notification, notificationPin) !=
         ObjectPinError::Ok))
    {
        return false;
    }

    auto& timer = static_cast<Timer&>(timerPin.GetObject());
    auto& notification = static_cast<Notification&>(notificationPin.GetObject());
    const Kernel::Time::TimerSnapshot timerSnapshot = timer.GetSnapshot();
    const Kernel::Ipc::NotificationSnapshot notificationSnapshot = notification.GetSnapshot();
    const Foundation::UInt64 runtimeStart = AddressOf(&runtime);
    const Foundation::UInt64 runtimeEnd = runtimeStart + sizeof(runtime);
    const Foundation::UInt64 timerAddress = AddressOf(&timer);
    const Foundation::UInt64 notificationAddress = AddressOf(&notification);
    const bool matches =
        (timerPin.GetObjectId().GetValue() == timerIdentity.GetValue()) &&
        (notificationPin.GetObjectId().GetValue() == notificationIdentity.GetValue()) &&
        (timer.GetObjectType() == ObjectType::Timer) && (notification.GetObjectType() == ObjectType::Notification) &&
        (timer.GetChargeOwner().GetValue() == ReadyChargeOwner) &&
        (notification.GetChargeOwner().GetValue() == ReadyChargeOwner) &&
        (timerSnapshot.State == TimerState::Disarmed) && !timerSnapshot.Token.IsValid() &&
        (timerSnapshot.DeadlineTicks == 0U) && (notificationSnapshot.State == NotificationState::Ready) &&
        (notificationSnapshot.Pending == 0U) &&
        (notificationSnapshot.LastClaimSequence == expectedLastNotificationClaim) && (timerAddress >= runtimeStart) &&
        (timerAddress <= (runtimeEnd - sizeof(timer))) && (notificationAddress >= runtimeStart) &&
        (notificationAddress <= (runtimeEnd - sizeof(notification)));
    notificationPin.Reset();
    timerPin.Reset();
    return matches;
}

[[nodiscard]] bool HasExactUserProbeCapability(ObjectRegistry& registry,
                                               const ProtectionDomainId protectionDomainIdentity) noexcept
{
    if (!g_userLaunchPlanCaptured)
    {
        return false;
    }

    ObjectPin protectionDomainPin;
    if (registry.PinObject(protectionDomainIdentity.GetObjectId(), ObjectType::ProtectionDomain, protectionDomainPin) !=
        ObjectPinError::Ok)
    {
        return false;
    }

    auto& protectionDomain = static_cast<ProtectionDomain&>(protectionDomainPin.GetObject());
    const auto binding = protectionDomain.GetBinding();
    if (!binding.HasValue() || (binding.GetValue().Capabilities == nullptr))
    {
        return false;
    }

    Kernel::Object::CapabilitySpace& capabilitySpace = *binding.GetValue().Capabilities;
    const CapabilityHandle handle{
        g_userLaunchPlan.InitialArguments.Register4,
        g_userLaunchPlan.InitialArguments.Register5,
    };
    const auto descriptor = capabilitySpace.Query(handle);
    constexpr CapabilityRights inspectRights{CapabilityRight::Inspect};
    // The probe receives no DiagnosticChannel: r6/r7 stay zero.
    return (g_userLaunchPlan.InitialGlobalPointer == 0U) && (g_userLaunchPlan.InitialArguments.Register6 == 0U) &&
           (g_userLaunchPlan.InitialArguments.Register7 == 0U) && handle.IsValid() &&
           (handle.GetSelector() ==
            Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedProtectionDomainCapabilitySelector) &&
           (capabilitySpace.GetActiveCount() == 3U) && descriptor.HasValue() &&
           (descriptor.GetValue().Object.GetValue() == protectionDomainIdentity.GetObjectId().GetValue()) &&
           (descriptor.GetValue().Type == ObjectType::ProtectionDomain) &&
           descriptor.GetValue().Rights.IsEqualTo(inspectRights) && descriptor.GetValue().RevocationDomain.IsValid() &&
           descriptor.GetValue().LeaseDeadline.IsInfinite() && (descriptor.GetValue().SealedObjectSelector == 0U) &&
           (descriptor.GetValue().ChargeOwner.GetValue() == ReadyChargeOwner);
}

[[nodiscard]] bool HasExactUserProbeEndpoint(ObjectRegistry& registry,
                                             const ProtectionDomainId protectionDomainIdentity,
                                             const Ia64BspKernelRuntime& runtime) noexcept
{
    if (!g_userLaunchPlanCaptured)
    {
        return false;
    }

    ObjectPin protectionDomainPin;
    if (registry.PinObject(protectionDomainIdentity.GetObjectId(), ObjectType::ProtectionDomain, protectionDomainPin) !=
        ObjectPinError::Ok)
    {
        return false;
    }
    auto& protectionDomain = static_cast<ProtectionDomain&>(protectionDomainPin.GetObject());
    const auto binding = protectionDomain.GetBinding();
    if (!binding.HasValue() || (binding.GetValue().Capabilities == nullptr))
    {
        return false;
    }
    Kernel::Object::CapabilitySpace& capabilitySpace = *binding.GetValue().Capabilities;

    const CapabilityHandle protectionDomainHandle{
        g_userLaunchPlan.InitialArguments.Register4,
        g_userLaunchPlan.InitialArguments.Register5,
    };
    const auto protectionDomainDescriptor = capabilitySpace.Query(protectionDomainHandle);
    const CapabilityHandle& endpointHandle = runtime.GetUserExecutionProbeEndpointCapability();
    auto endpointLookup = capabilitySpace.Lookup(endpointHandle, ObjectType::Endpoint, CapabilityOperation::Invoke);
    if (!protectionDomainDescriptor.HasValue() || !endpointLookup.HasValue())
    {
        return false;
    }

    const Kernel::Object::CapabilityDescriptor& protectionDomainAuthority = protectionDomainDescriptor.GetValue();
    const Kernel::Object::CapabilityDescriptor& endpointAuthority = endpointLookup.GetValue().GetDescriptor();
    Kernel::Object::IKernelObject& endpointObject = endpointLookup.GetValue().GetObject();

    Foundation::Size endpointCount{0U};
    bool registeredStorageMatches{false};
    for (Foundation::Size slot{1U}; slot <= Asteria::TestSupport::TestObjectRegistry::EntryCapacity; ++slot)
    {
        const Foundation::UInt64 rawObject = 0x1'0000U | static_cast<Foundation::UInt64>(slot);
        ObjectPin endpointPin;
        if (registry.PinObject(ObjectId{rawObject}, ObjectType::Endpoint, endpointPin) != ObjectPinError::Ok)
        {
            continue;
        }
        ++endpointCount;
        registeredStorageMatches =
            registeredStorageMatches || ((&endpointPin.GetObject() == &endpointObject) &&
                                         (endpointPin.GetObjectId().GetValue() == endpointAuthority.Object.GetValue()));
        endpointPin.Reset();
    }

    auto& endpoint = static_cast<Endpoint&>(endpointObject);
    const auto queue = endpoint.GetQueueState();
    const Foundation::UInt64 endpointAddress = AddressOf(&endpoint);
    const Foundation::UInt64 runtimeStart = AddressOf(&runtime);
    const Foundation::UInt64 runtimeEnd = runtimeStart + sizeof(runtime);
    constexpr CapabilityRights inspectRights{CapabilityRight::Inspect};
    constexpr CapabilityRights endpointRights{CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Read)};
    return protectionDomainHandle.IsValid() &&
           (protectionDomainHandle.GetSelector() ==
            Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedProtectionDomainCapabilitySelector) &&
           (protectionDomainAuthority.Object.GetValue() == protectionDomainIdentity.GetObjectId().GetValue()) &&
           (protectionDomainAuthority.Type == ObjectType::ProtectionDomain) &&
           protectionDomainAuthority.Rights.IsEqualTo(inspectRights) &&
           protectionDomainAuthority.RevocationDomain.IsValid() && endpointHandle.IsValid() &&
           (endpointHandle.GetSelector() ==
            Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedEndpointCapabilitySelector) &&
           (capabilitySpace.GetActiveCount() == 3U) && (endpointObject.GetObjectType() == ObjectType::Endpoint) &&
           (endpointAuthority.Type == ObjectType::Endpoint) && endpointAuthority.Rights.IsEqualTo(endpointRights) &&
           (endpointAuthority.Rights.GetRawValue() == 6U) && endpointAuthority.RevocationDomain.IsValid() &&
           (endpointAuthority.RevocationDomain.GetValue() == protectionDomainAuthority.RevocationDomain.GetValue()) &&
           endpointAuthority.LeaseDeadline.IsInfinite() && (endpointAuthority.SealedObjectSelector == 0U) &&
           (endpointAuthority.ChargeOwner.GetValue() == ReadyChargeOwner) && (endpointCount == 1U) &&
           registeredStorageMatches && endpoint.IsReady() && endpoint.HasReceiverCapabilitySpace(capabilitySpace) &&
           (endpoint.GetChargeOwner().GetValue() == ReadyChargeOwner) && (endpointAddress >= runtimeStart) &&
           (endpointAddress <= (runtimeEnd - sizeof(endpoint))) && (queue.QueuedBytes == 0U) &&
           (queue.NextSequence == 1U) && (queue.QueuedMessages == 0U) && (queue.QueuedAttachments == 0U);
}

[[nodiscard]] bool HasExactUserProbeWaitSet(ObjectRegistry& registry, const ProtectionDomainId protectionDomainIdentity,
                                            const Ia64BspKernelRuntime& runtime) noexcept
{
    ObjectPin protectionDomainPin;
    if (registry.PinObject(protectionDomainIdentity.GetObjectId(), ObjectType::ProtectionDomain, protectionDomainPin) !=
        ObjectPinError::Ok)
    {
        return false;
    }
    auto& protectionDomain = static_cast<ProtectionDomain&>(protectionDomainPin.GetObject());
    const auto binding = protectionDomain.GetBinding();
    if (!binding.HasValue() || (binding.GetValue().Capabilities == nullptr))
    {
        return false;
    }
    Kernel::Object::CapabilitySpace& capabilitySpace = *binding.GetValue().Capabilities;
    const CapabilityHandle& handle = runtime.GetUserExecutionProbeWaitSetCapability();
    auto authorized = capabilitySpace.Lookup(handle, ObjectType::WaitSet, CapabilityOperation::Wait);
    if (!authorized.HasValue())
    {
        return false;
    }

    const Kernel::Object::CapabilityDescriptor& descriptor = authorized.GetValue().GetDescriptor();
    auto& waitSet = static_cast<WaitSet&>(authorized.GetValue().GetObject());
    const auto snapshot = waitSet.GetSnapshot();
    constexpr CapabilityRights waitRights{CapabilityRight::Wait};
    const Foundation::UInt64 waitSetAddress = AddressOf(&waitSet);
    const Foundation::UInt64 runtimeStart = AddressOf(&runtime);
    const Foundation::UInt64 runtimeEnd = runtimeStart + sizeof(runtime);
    return handle.IsValid() &&
           (handle.GetSelector() ==
            Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedWaitSetCapabilitySelector) &&
           (capabilitySpace.GetActiveCount() == 3U) && (descriptor.Type == ObjectType::WaitSet) &&
           descriptor.Rights.IsEqualTo(waitRights) && (descriptor.Rights.GetRawValue() == 32U) &&
           descriptor.RevocationDomain.IsValid() && descriptor.LeaseDeadline.IsInfinite() &&
           (descriptor.SealedObjectSelector == 0U) && (descriptor.ChargeOwner.GetValue() == ReadyChargeOwner) &&
           (waitSet.GetObjectType() == ObjectType::WaitSet) &&
           (waitSet.GetChargeOwner().GetValue() == ReadyChargeOwner) && (snapshot.State == WaitSetState::Ready) &&
           (snapshot.RegisteredEntries == 0U) && (snapshot.ClaimedEntries == 0U) && (waitSetAddress >= runtimeStart) &&
           (waitSetAddress <= (runtimeEnd - sizeof(waitSet)));
}

[[nodiscard]] Foundation::UInt64 AcquireSystemCallPublication() noexcept
{
    if (g_installedContextAddress == 0U)
    {
        return 0U;
    }
    auto* const context = reinterpret_cast<Ia64RuntimeInterruptVectorTableContext*>(
        static_cast<Foundation::UIntPointer>(g_installedContextAddress));
    AtomicUInt64Reference publication{context->SystemCallEntryContextAddress};
    return publication.LoadAcquire();
}

[[nodiscard]] bool BytesHaveValue(const Foundation::UInt8* const data, const Foundation::Size sizeBytes,
                                  const Foundation::UInt8 expected) noexcept
{
    for (Foundation::Size index{0U}; index < sizeBytes; ++index)
    {
        if (data[index] != expected)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool BytesAreEqual(const Foundation::UInt8* const first, const Foundation::UInt8* const second,
                                 const Foundation::Size sizeBytes) noexcept
{
    for (Foundation::Size index{0U}; index < sizeBytes; ++index)
    {
        if (first[index] != second[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Foundation::UInt64 ReadLittleEndian64(const Foundation::UInt8* const encoded) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < sizeof(value); ++index)
    {
        value |= static_cast<Foundation::UInt64>(encoded[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] bool HasExactRetainedUserProbeBootstrap(const Ia64BspKernelRuntime& runtime) noexcept
{
    const CapabilityHandle& endpointHandle = runtime.GetUserExecutionProbeEndpointCapability();
    const CapabilityHandle& waitSetHandle = runtime.GetUserExecutionProbeWaitSetCapability();
    const auto& record = runtime.GetUserExecutionProbeIpcBootstrapRecord();
    return endpointHandle.IsValid() &&
           BytesAreEqual(record.EndpointCapabilitySelector, ExpectedEndpointSelectorBytes,
                         sizeof(ExpectedEndpointSelectorBytes)) &&
           (ReadLittleEndian64(record.EndpointCapabilitySelector) == endpointHandle.GetSelector()) &&
           (ReadLittleEndian64(record.EndpointCapabilityAuthenticator) == endpointHandle.GetAuthenticator()) &&
           waitSetHandle.IsValid() &&
           BytesAreEqual(record.WaitSetCapabilitySelector, ExpectedWaitSetSelectorBytes,
                         sizeof(ExpectedWaitSetSelectorBytes)) &&
           (ReadLittleEndian64(record.WaitSetCapabilitySelector) == waitSetHandle.GetSelector()) &&
           (ReadLittleEndian64(record.WaitSetCapabilityAuthenticator) == waitSetHandle.GetAuthenticator()) &&
           BytesAreEqual(record.Frame, ExpectedUserProbeCanonicalFrame, sizeof(ExpectedUserProbeCanonicalFrame));
}

[[nodiscard]] bool HasClearSystemCallScratch(Kernel::System::SystemCallScratch& scratch) noexcept
{
    const auto wire = scratch.GetWireBuffer(Kernel::Ipc::IpcDeliveryCodec::MaximumWireSizeBytes);
    const Kernel::Ipc::MessageEnvelope& message = scratch.GetMessage();
    bool attachmentsAreClear{true};
    for (const Kernel::Ipc::CapabilityAttachment& attachment : message.Attachments)
    {
        attachmentsAreClear =
            attachmentsAreClear && (attachment.Handle.GetSelector() == 0U) &&
            (attachment.Handle.GetAuthenticator() == 0U) && (attachment.RequestedDeadline.GetTicks() == 0U) &&
            (attachment.RequestedRights.GetRawValue() == 0U) && (attachment.ExpectedType == ObjectType::Invalid) &&
            (attachment.TransferMode == Kernel::Ipc::CapabilityTransferMode::Invalid) && (attachment.Reserved == 0U);
    }
    bool variablePayloadsAreClear{true};
    for (const Kernel::Ipc::VariablePayloadDescriptor& variable : message.VariablePayloads)
    {
        variablePayloadsAreClear = variablePayloadsAreClear && (variable.OffsetBytes == 0U) &&
                                   (variable.SizeBytes == 0U) && (variable.Reserved == 0U);
    }
    return (wire.Data != nullptr) && (wire.SizeBytes == Kernel::Ipc::IpcDeliveryCodec::MaximumWireSizeBytes) &&
           BytesHaveValue(wire.Data, wire.SizeBytes, 0U) && (message.Interface.GetLow() == 0U) &&
           (message.Interface.GetHigh() == 0U) && (message.TransactionIdentifier == 0U) &&
           (message.Deadline.GetTicks() == 0U) && (message.Trace.TraceIdentifierHigh == 0U) &&
           (message.Trace.TraceIdentifierLow == 0U) && (message.Trace.SpanIdentifier == 0U) &&
           (message.MethodIdentifier == 0U) && (message.InterfaceVersionMajor == 0U) &&
           (message.InterfaceVersionMinor == 0U) && (message.FixedPayloadSizeBytes == 0U) &&
           (message.PayloadSizeBytes == 0U) && (message.Kind == Kernel::Ipc::MessageKind::Invalid) &&
           (message.Cancellation == Kernel::Ipc::CancellationState::None) && (message.VariablePayloadCount == 0U) &&
           (message.AttachmentCount == 0U) && (message.Reserved == 0U) && (message.ReservedAlignment == 0U) &&
           attachmentsAreClear && variablePayloadsAreClear &&
           BytesHaveValue(message.Payload, sizeof(message.Payload), 0U);
}

[[nodiscard]] constexpr SalSystemTableInformation Firmware() noexcept
{
    return SalSystemTableInformation{
        .PalProcedurePhysicalAddress = 0x0010'0000U,
        .SalProcedurePhysicalAddress = 0x0020'0000U,
        .SalGlobalDataPhysicalAddress = 0x0030'0000U,
        .TranslationRegisterVirtualAddress = 0U,
        .TranslationRegisterEncodedPageSize = 22U << 2U,
        .CoherenceDomainInformationPhysicalAddress = 0U,
        .ApWakeVector = 0xFFU,
        .TotalTableLength = 192U,
        .CoherenceDomainCount = 0U,
        .SalRevision = 0x0340U,
        .EntryCount = 3U,
        .SalAVersion = 0x0100U,
        .SalBVersion = 0x0100U,
        .MemoryDescriptorCount = 0U,
        .PlatformFeatures = 0U,
        .HasPlatformFeatures = true,
        .HasTranslationRegister = true,
        .HasPurgeCoherenceInformation = false,
        .HasApWakeVector = true,
        .Reserved8 = 0U,
    };
}

[[nodiscard]] constexpr InitialTranslationRegionSet Regions() noexcept
{
    InitialTranslationRegionSet regions{};
    for (Foundation::Size index{0U}; index < Platform::Ia64::Memory::InitialTranslationRegionCount; ++index)
    {
        regions.Regions[index] = RegionConfiguration{
            .RegionIdentifier = static_cast<Foundation::UInt32>(0x4000U + index),
            .ProtectionKey = 0U,
            .VhptWalkerEnabled = false,
            .SharedAcrossAddressSpaces = index == 0U,
            .Reserved16 = 0U,
            .Reserved32 = 0U,
        };
    }
    return regions;
}

[[nodiscard]] bool PrepareReadyFixture(Ia64UserExecutionProbeStorage& storage) noexcept
{
    const PhysicalPageRange bootstrapReservation{
        .PhysicalStart = ReadyReservedBase,
        .PageCount = BootstrapReservedPageCount,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Reserved = 0U,
    };
    TestSupport::Ia64HostedBootGeometry geometry{};
    if (!TestSupport::MakeIa64HostedBootGeometry(
            AddressOf(&Platform::Ia64::Memory::AsteriaIa64RuntimeInterruptVectorTableImage),
            AddressOf(&g_readyRuntimeArena), sizeof(g_readyRuntimeArena), bootstrapReservation, geometry))
    {
        return false;
    }
    BootInfo bootInfo{
        .Signature = Boot::BootInfoSignature,
        .Version = Boot::BootInfoVersion,
        .SizeBytes = sizeof(BootInfo),
        .MemoryRegionCount = 4U,
        .FirmwareDescriptorVersion = 1U,
        .FirmwareDescriptorSize = 40U,
        .Reserved = 0U,
        .FirmwareMemoryMapSizeBytes = 0U,
        .MemoryRegions = {},
        .Acpi = {},
        .Entropy = {},
        .Display = {},
        .Inventory = {},
        .Files = {},
        .Crc32c = 0U,
        .ReservedAbi = 0U,
        .Kernel = {},
        .InventoryRecord = {},
        .DebugPort = {},
        .Trust = {},
        .DmaExclusions = {},
        .LowMemoryBlocks = 0U,
        .WatchdogSeconds = Asteria::Abi::WatchdogDefaultSeconds,
        .Power = {},
        .SessionMode = 0U,
        .LoginEnabled = 0U,
    };
    bootInfo.MemoryRegions[0U] = PhysicalMemoryRegion{
        .PhysicalStart = ReadyReservedBase,
        .VirtualStart = 0U,
        .PageCount = BootstrapReservedPageCount,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Kind = PhysicalMemoryKind::Conventional,
    };
    bootInfo.MemoryRegions[1U] = PhysicalMemoryRegion{
        .PhysicalStart = ReadyProbePoolBase,
        .VirtualStart = 0U,
        .PageCount = ReadyProbePoolPages,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Kind = PhysicalMemoryKind::Conventional,
    };
    bootInfo.MemoryRegions[2U] = geometry.ImageRegion;
    bootInfo.MemoryRegions[3U] = geometry.RuntimeMemoryRegion;
    if (!TestSupport::SortIa64HostedBootRegions(bootInfo) || (geometry.Image.ImageBase <= ReadyProbePoolBase) ||
        (g_readyAllocator.Initialize(bootInfo) != BootstrapPageAllocatorError::None))
    {
        return false;
    }
    const auto reserved = g_readyAllocator.Allocate(BootstrapReservedPageCount, 1U);
    // The boot allocates the run-time memory reservation right after the
    // bootstrap one, aligned to its size; here that is the arena's page.
    const auto runtimeReservation =
        g_readyAllocator.Allocate(geometry.RuntimeMemoryPages.PageCount, geometry.RuntimeMemoryPages.PageCount);
    const auto code = g_readyAllocator.Allocate(1U, 1U);
    const auto memoryStack = g_readyAllocator.Allocate(4U, 1U);
    const auto rse = g_readyAllocator.Allocate(4U, 1U);
    const auto sharedRootShootdownPage = g_readyAllocator.Allocate(1U, 1U);
    const auto singleProcessorSharedRootShootdownPage = g_readyAllocator.Allocate(1U, 1U);
    if (!reserved.HasValue() || !runtimeReservation.HasValue() ||
        !TestSupport::Ia64HostedPagesEqual(runtimeReservation.GetValue(), geometry.RuntimeMemoryPages) ||
        !code.HasValue() || !memoryStack.HasValue() || !rse.HasValue() || !sharedRootShootdownPage.HasValue() ||
        !singleProcessorSharedRootShootdownPage.HasValue())
    {
        return false;
    }
    g_readySharedRootShootdownPage = sharedRootShootdownPage.GetValue();
    g_singleProcessorSharedRootShootdownPage = singleProcessorSharedRootShootdownPage.GetValue();
    for (Foundation::Size index{0U}; index < 5U; ++index)
    {
        const auto failurePage = g_readyAllocator.Allocate(1U, 1U);
        if (!failurePage.HasValue())
        {
            return false;
        }
        g_sharedRootShootdownFailurePages[index] = failurePage.GetValue();
    }
    storage = Ia64UserExecutionProbeStorage{
        .Code =
            Ia64UserExecutionProbeStorageRegion{
                .Allocation = code.GetValue(),
                .WritableAlias =
                    Ia64UserExecutionProbeWritableAlias{
                        .Data = g_readyProbeCode.Data,
                        .SizeBytes = sizeof(g_readyProbeCode.Data),
                        .PhysicalStart = code.GetValue().PhysicalStart,
                    },
            },
        .MemoryStack =
            Ia64UserExecutionProbeStorageRegion{
                .Allocation = memoryStack.GetValue(),
                .WritableAlias =
                    Ia64UserExecutionProbeWritableAlias{
                        .Data = g_readyProbeMemoryStack.Data,
                        .SizeBytes = sizeof(g_readyProbeMemoryStack.Data),
                        .PhysicalStart = memoryStack.GetValue().PhysicalStart,
                    },
            },
        .RseBackingStore =
            Ia64UserExecutionProbeStorageRegion{
                .Allocation = rse.GetValue(),
                .WritableAlias =
                    Ia64UserExecutionProbeWritableAlias{
                        .Data = g_readyProbeRse.Data,
                        .SizeBytes = sizeof(g_readyProbeRse.Data),
                        .PhysicalStart = rse.GetValue().PhysicalStart,
                    },
            },
    };
    if (!g_readyAddressSpacePreparation
             .Prepare(geometry.Image, bootInfo, reserved.GetValue(), runtimeReservation.GetValue(), Capabilities(),
                      Firmware(), Regions(),
                      WritableInterruptVectorTableView{
                          .Data = g_readyInitialInterruptVectorTable,
                          .SizeBytes = InitialInterruptVectorTableSizeBytes,
                          .PhysicalStart = ReadyReservedBase + 0x1'0000U,
                      },
                      WritableRseBackingStoreView{
                          .Data = g_readyInitialRseBackingStore,
                          .SizeBytes = InitialRseBackingStoreSizeBytes,
                          .PhysicalStart = ReadyReservedBase + 0x1'8000U,
                      })
             .HasValue())
    {
        return false;
    }
    // The hosted geometry's plan is the same at every host layout.
    const auto summary = g_readyAddressSpacePreparation.TryGetSummary();
    return summary.HasValue() &&
           (summary.GetValue()->Translation.DataRegisterCount == TestSupport::Ia64HostedDataTranslationRegisters) &&
           (summary.GetValue()->Translation.InstructionRegisterCount ==
            TestSupport::Ia64HostedInstructionTranslationRegisters);
}

[[nodiscard, gnu::noinline]] bool
PrepareAndDispatchInitialUserImageIntegration(TestContext& runner, const ResourceDomainObjectId rootDomainIdentity,
                                              Ia64UserExecutionProbe& readyUserProbe,
                                              InitialUserImagePreparedFixture& fixture) noexcept
{
    const auto runtimeMemoryPrepared = g_readyRuntimeArena.InitialUserImageRuntimeMemory.PrepareAndAdmit(
        CpuId{0U}, Capabilities(), g_readyAddressSpacePreparation, ResourceDomainId{ReadyChargeOwner}, g_readyRegistry,
        g_readyIssuer);
    const bool ownersPrepared = runtimeMemoryPrepared.HasValue() && PrepareInitialImageOwners(rootDomainIdentity);
    auto* const runtime = ::new (static_cast<void*>(g_readyRuntimeArena.InitialUserImageKernelRuntimeStorage))
        Ia64BspKernelRuntime{g_readyRegistry, g_readyRegistryLock, TestCapabilityValidationKey};
    fixture = InitialUserImagePreparedFixture{
        .Runtime = runtime,
        .ProtectionDomainIdentity = {},
        .ThreadIdentity = {},
    };

    const auto defaultOwnerSummary = runtime->GetInitialUserImageLaunchOwner().GetSummary();
    const auto defaultOwnerRequest = runtime->GetInitialUserImageLaunchOwner().GetLaunchRequest();
    const auto defaultOwnerFailure = runtime->GetInitialUserImageLaunchOwner().GetFailure();
    ASTERIA_EXPECT(runner, "the real-image runtime starts Fresh with inaccessible owner output and no failure",
                   runtime->GetInitialUserImageState() == Ia64BspKernelRuntimeInitialUserImageState::Fresh,
                   runtime->GetInitialUserImageFailure().Error == Ia64BspKernelRuntimeInitialUserImageError::None,
                   runtime->GetInitialUserImageLaunchOwner().GetState() == Ia64InitialUserImageLaunchOwnerState::Fresh,
                   !defaultOwnerSummary.HasValue(),
                   defaultOwnerSummary.GetError() == Ia64InitialUserImageLaunchAccessError::NotReady,
                   !defaultOwnerRequest.HasValue(),
                   defaultOwnerRequest.GetError() == Ia64InitialUserImageLaunchAccessError::NotReady,
                   !defaultOwnerFailure.HasValue(),
                   defaultOwnerFailure.GetError() == Ia64InitialUserImageLaunchAccessError::NoFailure);

    const auto dispatchBeforePreparation = runtime->DispatchInitialUserImage();
    ASTERIA_EXPECT(runner, "initial-image dispatch before preparation is side-effect-free and retryable",
                   !dispatchBeforePreparation.HasValue(),
                   dispatchBeforePreparation.GetError() ==
                       Ia64BspKernelRuntimeInitialUserImageError::DispatchNotPrepared,
                   runtime->GetInitialUserImageState() == Ia64BspKernelRuntimeInitialUserImageState::Fresh,
                   runtime->GetInitialUserImageFailure().Error == Ia64BspKernelRuntimeInitialUserImageError::None);

    const auto runtimePrepared =
        runtime->Prepare(g_readyRuntimeArena.InitialUserImageRuntimeMemory, rootDomainIdentity, g_readyResourceDomain,
                         g_readyIssuer, Capabilities(), Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector);
    if (runtimePrepared.HasValue())
    {
        fixture.ProtectionDomainIdentity = runtimePrepared.GetValue().ProtectionDomainIdentity;
        fixture.ThreadIdentity = runtimePrepared.GetValue().ThreadIdentity;
    }
    const auto rejectedPreparation =
        runtime->PrepareInitialUserImage(g_initialImageUnreadyMemoryOwner, g_initialImageDispatchMappingOwner);
    const auto rejectedOwnerFailure = runtime->GetInitialUserImageLaunchOwner().GetFailure();
    ASTERIA_EXPECT(
        runner, "an unready image owner is rejected before endpoint, user-memory, or launch authority commit",
        runtimeMemoryPrepared.HasValue(), ownersPrepared, runtimePrepared.HasValue(), !rejectedPreparation.HasValue(),
        rejectedPreparation.GetError() == Ia64BspKernelRuntimeInitialUserImageError::UserMemoryInitializationRejected,
        runtime->GetInitialUserImageState() == Ia64BspKernelRuntimeInitialUserImageState::Fresh,
        runtime->GetInitialUserImageLaunchOwner().GetState() == Ia64InitialUserImageLaunchOwnerState::Fresh,
        !rejectedOwnerFailure.HasValue(),
        rejectedOwnerFailure.GetError() == Ia64InitialUserImageLaunchAccessError::NoFailure,
        HasExactPreparedCapabilityAuthority(g_readyRegistry, fixture.ProtectionDomainIdentity, *runtime, 0U));
    if (!runtimePrepared.HasValue() || !ownersPrepared)
    {
        return false;
    }

    const Kernel::Resource::ResourceDomainSnapshot quotaSnapshot = g_readyResourceDomain.GetSnapshot();
    const ResourceReservation quotaFill{
        .CommittedMemoryPages = 0U,
        .ResidentMemoryPages = 0U,
        .KernelMetadataBytes = 0U,
        .CapabilityHandles = 0U,
        .IpcQueueBytes = quotaSnapshot.Limits.IpcQueueBytes - quotaSnapshot.Usage.IpcQueueBytes,
        .Threads = 0U,
    };
    const bool quotaFilled =
        quotaFill.IpcQueueBytes != 0U && (g_readyResourceDomain.TryReserve(quotaFill) == ResourceDomainError::None);
    const ResourceDomainUsage filledUsage = g_readyResourceDomain.GetSnapshot().Usage;
    const Foundation::Size objectsBeforeQuotaRejection = g_readyRegistry.GetActiveCount();
    const auto quotaRejected =
        runtime->PrepareInitialUserImage(g_initialImageMemoryOwner, g_initialImageDispatchMappingOwner);
    const auto quotaOwnerFailure = runtime->GetInitialUserImageLaunchOwner().GetFailure();
    ASTERIA_EXPECT(
        runner, "exhausted IPC quota rejects the boot Endpoint before its commit cut", quotaFilled,
        !quotaRejected.HasValue(),
        quotaRejected.GetError() == Ia64BspKernelRuntimeInitialUserImageError::ResourceReservationRejected,
        runtime->GetInitialUserImageState() == Ia64BspKernelRuntimeInitialUserImageState::Fresh,
        runtime->GetInitialUserImageFailure().ResourceDomain == ResourceDomainError::LimitExceeded,
        runtime->GetInitialUserImageLaunchOwner().GetState() == Ia64InitialUserImageLaunchOwnerState::Fresh,
        !quotaOwnerFailure.HasValue(), quotaOwnerFailure.GetError() == Ia64InitialUserImageLaunchAccessError::NoFailure,
        g_readyRegistry.GetActiveCount() == objectsBeforeQuotaRejection,
        ResourceUsageIsEqual(filledUsage, g_readyResourceDomain.GetSnapshot().Usage),
        HasExactPreparedCapabilityAuthority(g_readyRegistry, fixture.ProtectionDomainIdentity, *runtime, 0U));
    const ResourceDomainError quotaRelease =
        quotaFilled ? g_readyResourceDomain.Release(quotaFill) : ResourceDomainError::InvalidReservation;
    ASTERIA_EXPECT(runner, "a retryable boot Endpoint quota rejection retains no reservation",
                   quotaRelease == ResourceDomainError::None,
                   ResourceUsageIsEqual(quotaSnapshot.Usage, g_readyResourceDomain.GetSnapshot().Usage));
    if (quotaRelease != ResourceDomainError::None)
    {
        return false;
    }

    const Foundation::Size objectCountBeforeInitialImage = g_readyRegistry.GetActiveCount();
    const ResourceDomainUsage resourceUsageBeforeInitialImage = g_readyResourceDomain.GetSnapshot().Usage;
    const auto prepared =
        runtime->PrepareInitialUserImage(g_initialImageMemoryOwner, g_initialImageDispatchMappingOwner);
    const ResourceDomainUsage resourceUsageAfterInitialImage = g_readyResourceDomain.GetSnapshot().Usage;
    const auto probeConflict = runtime->PrepareUserExecutionProbe(readyUserProbe);
    const bool exactPrepared =
        prepared.HasValue() && (prepared.GetValue().ThreadIdentity.IsEqualTo(fixture.ThreadIdentity)) &&
        (prepared.GetValue().Processor.GetValue() == 0U) &&
        (prepared.GetValue().State == Ia64BspKernelRuntimeInitialUserImageState::Prepared) &&
        (runtime->GetInitialUserImageState() == Ia64BspKernelRuntimeInitialUserImageState::Prepared) &&
        (runtime->GetInitialUserImageFailure().Error == Ia64BspKernelRuntimeInitialUserImageError::None) &&
        (g_readyRegistry.GetActiveCount() == objectCountBeforeInitialImage + 2U) &&
        ResourceUsageHasExactInitialUserImageIncrease(resourceUsageBeforeInitialImage,
                                                      resourceUsageAfterInitialImage) &&
        HasExactInitialUserImageAuthority(*runtime, fixture.ProtectionDomainIdentity);
    ASTERIA_EXPECT(runner,
                   "valid preparation retains the exact real-image owner, request, "
                   "nonzero GP, user memory, and Endpoint and DiagnosticChannel "
                   "capabilities",
                   exactPrepared);
    ASTERIA_EXPECT(runner,
                   "preparing the real image excludes the synthetic probe without consuming either prepared owner",
                   !probeConflict.HasValue(),
                   probeConflict.GetError() == Ia64BspKernelRuntimeUserProbeError::ConfigurationConflict,
                   runtime->GetUserExecutionProbeState() == Ia64BspKernelRuntimeUserProbeState::Fresh,
                   runtime->GetInitialUserImageState() == Ia64BspKernelRuntimeInitialUserImageState::Prepared);
    if (!exactPrepared)
    {
        return false;
    }

    const auto commonRoot = g_readyRuntimeArena.InitialUserImageRuntimeMemory.BorrowReadyRoot();
    const auto privateRoot = g_readyRuntimeArena.InitialUserImageRuntimeMemory.BorrowReadyInitialComponentRoot();
    const Foundation::Size objectCountBeforeDispatch = g_readyRegistry.GetActiveCount();
    const ResourceDomainUsage resourceUsageBeforeDispatch = g_readyResourceDomain.GetSnapshot().Usage;
    const Foundation::Size commonMappingCountBeforeDispatch =
        commonRoot.HasValue() ? commonRoot.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount : 0U;
    const Foundation::Size privateMappingCountBeforeDispatch =
        privateRoot.HasValue() ? privateRoot.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount : 0U;
    const auto publication = g_readyRuntimeArena.InitialUserImageRuntimeMemory.GetSystemCallPublicationSnapshot();
    const auto* const bridgeContext = reinterpret_cast<const Ia64SystemCallBridgeContext*>(
        static_cast<Foundation::UIntPointer>(publication.Context.BridgeContextAddress));
    const Ia64ProcessorRunLoop* const runLoop = bridgeContext != nullptr ? bridgeContext->RunLoop : nullptr;
    const auto retainedRequest = runtime->GetInitialUserImageLaunchOwner().GetLaunchRequest();
    const auto expectedLaunchPlan =
        retainedRequest.HasValue()
            ? Ia64UserModeLaunch::Validate(retainedRequest.GetValue())
            : Foundation::Result<Platform::Ia64::Scheduler::Ia64UserModeLaunchPlan, Ia64UserModeLaunchError>::Failure(
                  Ia64UserModeLaunchError::InvalidCodeRange);
    const auto readOnlyRange = UserMemoryRange::Create(InitialImageCodeVirtualStart & ~(PageBytes - 1U), 1U);
    const auto writableRange = UserMemoryRange::Create(InitialImageDataVirtualStart & ~(PageBytes - 1U), 1U);
    UserMemoryAccessError readOnlyWriteError{UserMemoryAccessError::BackendProtocolViolation};
    bool writableReservationAccepted{false};
    UserMemoryAccessError writableCancelError{UserMemoryAccessError::BackendProtocolViolation};
    if ((bridgeContext != nullptr) && (bridgeContext->UserMemory != nullptr) && readOnlyRange.HasValue() &&
        writableRange.HasValue())
    {
        const auto readOnlyWrite = bridgeContext->UserMemory->PrepareCopyToUser(readOnlyRange.GetValue());
        readOnlyWriteError = readOnlyWrite.HasValue() ? UserMemoryAccessError::None : readOnlyWrite.GetError();
        const auto writableWrite = bridgeContext->UserMemory->PrepareCopyToUser(writableRange.GetValue());
        writableReservationAccepted = writableWrite.HasValue();
        if (writableWrite.HasValue())
        {
            writableCancelError = bridgeContext->UserMemory->CancelCopyToUser(writableWrite.GetValue());
        }
    }
    ASTERIA_EXPECT(
        runner,
        "default image preparation publishes all five mappings, denies "
        "writes to code, accepts writable data, and leaves Wait disabled",
        bridgeContext != nullptr, bridgeContext->UserMemory != nullptr, bridgeContext->UserMemory->IsReady(),
        privateRoot.HasValue(),
        bridgeContext->UserMemory->GetAddressSpaceId().IsEqualTo(privateRoot.GetValue().AddressSpaceIdentity),
        bridgeContext->Scratch != nullptr, bridgeContext->WaitContinuation == nullptr,
        bridgeContext->WaitSetDispatcher == nullptr, readOnlyRange.HasValue(), writableRange.HasValue(),
        readOnlyWriteError == UserMemoryAccessError::AccessDenied, writableReservationAccepted,
        writableCancelError == UserMemoryAccessError::None);
    const Foundation::Size activationsBeforeDispatch = g_regionActivationCalls;
    const auto dispatched = runtime->DispatchInitialUserImage();
    const auto repeatedDispatch = runtime->DispatchInitialUserImage();
    const auto& failure = runtime->GetInitialUserImageFailure();

    ObjectPin threadPin;
    const ObjectPinError threadPinError =
        g_readyRegistry.PinObject(fixture.ThreadIdentity.GetObjectId(), ObjectType::Thread, threadPin);
    const Kernel::Scheduler::ThreadSnapshot threadSnapshot =
        threadPinError == ObjectPinError::Ok ? static_cast<Thread&>(threadPin.GetObject()).GetSnapshot()
                                             : Kernel::Scheduler::ThreadSnapshot{};
    const ResourceDomainUsage resourceUsageAfterDispatch = g_readyResourceDomain.GetSnapshot().Usage;
    ASTERIA_EXPECT(runner, "hosted real-image dispatch reaches the exact native-unavailable launch and exits once",
                   !dispatched.HasValue(),
                   dispatched.GetError() == Ia64BspKernelRuntimeInitialUserImageError::LaunchRejected,
                   runtime->GetState() == Ia64BspKernelRuntimeState::Failed,
                   runtime->GetInitialUserImageState() == Ia64BspKernelRuntimeInitialUserImageState::DispatchFailed,
                   failure.Error == Ia64BspKernelRuntimeInitialUserImageError::LaunchRejected,
                   failure.Launch == Ia64UserModeLaunchError::NativeTransitionUnavailable,
                   failure.ExitCode == Platform::Ia64::Scheduler::Ia64BspInitialUserImageLaunchFailureExitCode,
                   expectedLaunchPlan.HasValue(), g_userLaunchPlanCaptured,
                   LaunchPlansAreEqual(g_userLaunchPlan, expectedLaunchPlan.GetValue()), g_userLaunchCalls == 1U,
                   g_workerSwitchWasActiveAtLaunch, !g_workerSwitchActive,
                   g_regionActivationCalls == activationsBeforeDispatch + 2U, g_regionOneAtUserLaunch != 0U,
                   g_regionOneAtUserLaunch != g_lastRegionOneActivation);
    ASTERIA_EXPECT(runner, "terminal real-image dispatch is not repeated and retains its exact launch owner",
                   !repeatedDispatch.HasValue(),
                   repeatedDispatch.GetError() == Ia64BspKernelRuntimeInitialUserImageError::DispatchAlreadyAttempted,
                   g_userLaunchCalls == 1U,
                   runtime->GetInitialUserImageLaunchOwner().GetState() == Ia64InitialUserImageLaunchOwnerState::Ready,
                   HasExactInitialUserImageAuthority(*runtime, fixture.ProtectionDomainIdentity));
    ASTERIA_EXPECT(
        runner,
        "real-image dispatch restores the common root, idle run loop, exited thread, mappings, registry, and resource usage",
        commonRoot.HasValue(), privateRoot.HasValue(),
        commonRoot.GetValue().ControllerStorage->GetSnapshot().ActiveStorage ==
            commonRoot.GetValue().AddressSpaceStorage,
        commonRoot.GetValue().AddressSpaceStorage->GetSnapshot().ResidentProcessorMask == 0x01U,
        privateRoot.GetValue().AddressSpaceStorage->GetSnapshot().ResidentProcessorMask == 0U,
        commonRoot.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount == commonMappingCountBeforeDispatch,
        privateRoot.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount == privateMappingCountBeforeDispatch,
        g_readyRegistry.GetActiveCount() == objectCountBeforeDispatch,
        ResourceUsageIsEqual(resourceUsageAfterDispatch, resourceUsageBeforeDispatch), runLoop != nullptr,
        runLoop->GetSnapshot().State == Kernel::Scheduler::ProcessorKernelRunLoopState::Idle,
        threadPinError == ObjectPinError::Ok, threadSnapshot.State == Kernel::Scheduler::ThreadState::Exited,
        threadSnapshot.ExitCode == Platform::Ia64::Scheduler::Ia64BspInitialUserImageLaunchFailureExitCode);
    return !dispatched.HasValue() &&
           (dispatched.GetError() == Ia64BspKernelRuntimeInitialUserImageError::LaunchRejected) &&
           !repeatedDispatch.HasValue() &&
           (repeatedDispatch.GetError() == Ia64BspKernelRuntimeInitialUserImageError::DispatchAlreadyAttempted);
}

void TestTimerQueueNotificationSuccessModel(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ModelLocalTimer localTimer;
    TimerQueue timerQueue{registry, localTimer};
    ProcessorRunController controller{registry, timerQueue, localTimer, CpuId{0U},
                                      ProcessorRunDispatchMode::CooperativeBootstrap};
    Timer timer{localTimer};
    Notification notification;
    constexpr ResourceDomainId chargeOwner{0xD064U};
    const Kernel::Time::TimerError timerInitialization = timer.Initialize(chargeOwner);
    const Kernel::Ipc::NotificationError notificationInitialization = notification.Initialize(chargeOwner);
    const auto timerObject = issuer.CreateObject(timer);
    const auto notificationObject = issuer.CreateObject(notification);
    const TimerId timerIdentity = timerObject.HasValue() ? TimerId{registry, timerObject.GetValue()} : TimerId{};
    const NotificationId notificationIdentity =
        notificationObject.HasValue() ? NotificationId{registry, notificationObject.GetValue()} : NotificationId{};

    const auto armed = timerQueue.Arm(timerIdentity, timer, Kernel::Object::MonotonicTime{100U});
    const auto initialDeadline = controller.RefreshTimerQueueDeadline(10U);
    TimerNotificationDelivery delivery{registry, timerQueue};
    const auto earlyDelivery = delivery.Deliver(timerIdentity, timer, notificationIdentity, notification);
    const Kernel::Time::TimerQueueState afterEarlyDelivery = timerQueue.GetState();
    const Kernel::Time::TimerSnapshot afterEarlyTimer = timer.GetSnapshot();
    const Kernel::Ipc::NotificationSnapshot afterEarlyNotification = notification.GetSnapshot();

    localTimer.SetTicks(100U);
    const auto delivered = delivery.Deliver(timerIdentity, timer, notificationIdentity, notification);
    const auto notificationClaim = notification.Claim();
    Kernel::Ipc::NotificationSnapshot finalNotification{};
    bool notificationCommitted{false};
    if (notificationClaim.HasValue())
    {
        const auto committed = notification.CommitClaim(notificationClaim.GetValue());
        notificationCommitted = committed.HasValue();
        if (committed.HasValue())
        {
            finalNotification = committed.GetValue();
        }
    }
    const auto finalDeadline = controller.RefreshTimerQueueDeadline(100U);
    const Kernel::Time::TimerSnapshot finalTimer = timer.GetSnapshot();
    const Kernel::Time::TimerQueueState finalQueue = timerQueue.GetState();
    const Kernel::Scheduler::ProcessorRunSnapshot finalController = controller.GetSnapshot();

    Ia64BspKernelRuntimeTimerQueueEvidence evidence{};
    if (armed.HasValue() && initialDeadline.HasValue() && delivered.HasValue() && notificationClaim.HasValue() &&
        notificationCommitted && finalDeadline.HasValue())
    {
        evidence.TimerObjectIdentity = timerIdentity.GetValue();
        evidence.NotificationObjectIdentity = notificationIdentity.GetValue();
        evidence.TimerArmSequence = armed.GetValue().Timer.Token.GetSequence();
        evidence.DeadlineTicks = delivered.GetValue().Expiration.DeadlineTicks;
        evidence.ObservedTicks = delivered.GetValue().Expiration.ObservedTicks;
        evidence.QueueClaimSequence = delivered.GetValue().ClaimSequence;
        evidence.NotificationClaimSequence = notificationClaim.GetValue().GetSequence();
        evidence.InitialDeadlineState = static_cast<Foundation::UInt8>(initialDeadline.GetValue().State);
        evidence.SelectedDeadlineSources = initialDeadline.GetValue().SelectedSources;
        evidence.InitialDeadlineReprogrammed = initialDeadline.GetValue().Reprogrammed;
        evidence.ClaimedTimerState = static_cast<Foundation::UInt8>(TimerState::Expired);
        evidence.NotificationPendingAfterSignal = delivered.GetValue().NotificationState.Pending;
        evidence.FinalTimerState = static_cast<Foundation::UInt8>(finalTimer.State);
        evidence.FinalNotificationState = static_cast<Foundation::UInt8>(finalNotification.State);
        evidence.FinalNotificationPending = finalNotification.Pending;
        evidence.FinalDeadlineState = static_cast<Foundation::UInt8>(finalController.Deadline.State);
        evidence.FinalLocalTimerState = 1U;
        evidence.FinalLocalTimerArmed = static_cast<Foundation::UInt8>(localTimer.IsArmed() ? 1U : 0U);
        evidence.FinalScheduledEntries = finalQueue.ScheduledEntries;
        evidence.FinalPendingEntries = finalQueue.PendingEntries;
        evidence.FinalClaimedEntries = finalQueue.ClaimedEntries;
    }
    const bool evidenceWasUncommitted = evidence.Sequence == 0U;
    if (evidenceWasUncommitted)
    {
        evidence.Sequence = 1U;
    }

    ASTERIA_EXPECT(
        runner, "the hosted Phase D model registers, arms, and delegates the sole hardware deadline to the controller",
        timerInitialization == Kernel::Time::TimerError::None,
        notificationInitialization == Kernel::Ipc::NotificationError::None, timerObject.HasValue(),
        notificationObject.HasValue(), timerIdentity.IsValid(), notificationIdentity.IsValid(),
        timerIdentity.GetValue() != notificationIdentity.GetValue(), armed.HasValue(),
        armed.GetValue().Timer.Token.GetSequence() == 1U, armed.GetValue().Timer.DeadlineTicks == 100U,
        armed.GetValue().NextDeadlineTicks == 100U, initialDeadline.HasValue(),
        initialDeadline.GetValue().SelectedDeadlineTicks == 100U,
        initialDeadline.GetValue().State == Kernel::Time::ProcessorDeadlineState::Armed,
        initialDeadline.GetValue().SelectedSources ==
            static_cast<Foundation::UInt8>(Kernel::Time::ProcessorDeadlineSource::TimerQueue),
        initialDeadline.GetValue().Reprogrammed == 1U, localTimer.GetProgramCalls() == 1U,
        localTimer.GetDeadlineTicks() == 0U);
    ASTERIA_EXPECT(
        runner,
        "an early normal-context delivery is retryable and preserves the exact queued generation without publication",
        !earlyDelivery.HasValue(), earlyDelivery.GetError().Error == TimerNotificationDeliveryError::QueueClaimRejected,
        earlyDelivery.GetError().Queue.Error == Kernel::Time::TimerQueueError::NotDue,
        earlyDelivery.GetError().NotificationPublished == 0U, earlyDelivery.GetError().QueueCommitted == 0U,
        afterEarlyDelivery.NextDeadlineTicks == 100U, afterEarlyDelivery.ScheduledEntries == 1U,
        afterEarlyDelivery.PendingEntries == 0U, afterEarlyDelivery.ClaimedEntries == 0U,
        afterEarlyTimer.State == TimerState::Armed, afterEarlyNotification.State == NotificationState::Ready,
        afterEarlyNotification.Pending == 0U);
    ASTERIA_EXPECT(
        runner,
        "the hosted Phase D model claims the exact due Timer, signals and proves the exact Notification generation",
        delivered.HasValue(), delivered.GetValue().TimerIdentity.IsEqualTo(timerIdentity),
        delivered.GetValue().NotificationIdentity.IsEqualTo(notificationIdentity),
        delivered.GetValue().Expiration.Token.GetSequence() == 1U,
        delivered.GetValue().Expiration.DeadlineTicks == 100U, delivered.GetValue().Expiration.ObservedTicks == 100U,
        delivered.GetValue().ClaimSequence == 1U, delivered.GetValue().NotificationPublished == 1U,
        delivered.GetValue().QueueCommitted == 1U,
        delivered.GetValue().NotificationState.State == NotificationState::Ready,
        delivered.GetValue().NotificationState.Pending == 1U, notificationClaim.HasValue(),
        notificationClaim.GetValue().GetSequence() == 1U, notificationCommitted,
        finalNotification.LastClaimSequence == 1U, finalNotification.State == NotificationState::Ready,
        finalNotification.Pending == 0U);
    ASTERIA_EXPECT(
        runner,
        "the hosted Phase D model commits and acknowledges delivery before the controller disarms the empty queue",
        finalDeadline.HasValue(), finalDeadline.GetValue().SelectedDeadlineTicks == 0U,
        finalDeadline.GetValue().State == Kernel::Time::ProcessorDeadlineState::Disarmed,
        finalDeadline.GetValue().SelectedSources == 0U, finalDeadline.GetValue().Reprogrammed == 1U,
        finalTimer.State == TimerState::Disarmed, !finalTimer.Token.IsValid(), finalTimer.DeadlineTicks == 0U,
        finalQueue.NextDeadlineTicks == 0U, finalQueue.ScheduledEntries == 0U, finalQueue.PendingEntries == 0U,
        finalQueue.ClaimedEntries == 0U, finalController.State == Kernel::Scheduler::ProcessorRunState::Idle,
        finalController.Deadline.State == Kernel::Time::ProcessorDeadlineState::Disarmed,
        localTimer.GetCancelCalls() == 1U, !localTimer.IsArmed());
    ASTERIA_EXPECT(
        runner, "the success model produces the exact 80-byte ABI and rejects an overflowing publication range",
        evidenceWasUncommitted, evidence.Sequence == 1U, evidence.TimerObjectIdentity == timerIdentity.GetValue(),
        evidence.NotificationObjectIdentity == notificationIdentity.GetValue(), evidence.TimerArmSequence == 1U,
        evidence.DeadlineTicks == 100U, evidence.ObservedTicks == 100U, evidence.QueueClaimSequence == 1U,
        evidence.NotificationClaimSequence == 1U, evidence.InitialDeadlineState == 1U,
        evidence.SelectedDeadlineSources == 2U, evidence.InitialDeadlineReprogrammed == 1U,
        evidence.ClaimedTimerState == 3U, evidence.NotificationPendingAfterSignal == 1U, evidence.FinalTimerState == 1U,
        evidence.FinalNotificationState == 1U, evidence.FinalNotificationPending == 0U,
        evidence.FinalDeadlineState == 0U, evidence.FinalLocalTimerState == 1U, evidence.FinalLocalTimerArmed == 0U,
        evidence.FinalScheduledEntries == 0U, evidence.FinalPendingEntries == 0U, evidence.FinalClaimedEntries == 0U,
        evidence.Reserved[0U] == 0U, evidence.Reserved[1U] == 0U);

    const bool timerDestructionStarted = timerObject.HasValue() && (issuer.DestroyObject(timerObject.GetValue()) ==
                                                                    Kernel::Object::ObjectRegistryError::Ok);
    const bool notificationDestructionStarted =
        notificationObject.HasValue() &&
        (issuer.DestroyObject(notificationObject.GetValue()) == Kernel::Object::ObjectRegistryError::Ok);
    const auto firstCollected = issuer.CollectDestroyedObject();
    const auto secondCollected = issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "the model releases both queue and delivery pins after the completed transaction",
                   timerDestructionStarted, notificationDestructionStarted, firstCollected.HasValue(),
                   secondCollected.HasValue(), registry.GetActiveCount() == 0U);
}

void TestUserProbeWaitDeadlinePolicy(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 maximumTicks{~Foundation::UInt64{0U}};
    constexpr Foundation::UInt64 leadTicks{Platform::Ia64::Scheduler::Ia64BspUserProbeWaitTimerLeadTicks};
    constexpr Foundation::UInt64 graceTicks{Platform::Ia64::Scheduler::Ia64BspLocalTimerProbeGraceTicks};
    constexpr Foundation::UInt64 boundaryTicks{maximumTicks - leadTicks - graceTicks};

    ASTERIA_EXPECT(runner, "user WaitSet probe lead is distinct, conservative, and bounded", leadTicks == 100'000'000U,
                   leadTicks > Platform::Ia64::Scheduler::Ia64BspLocalTimerProbeLeadTicks,
                   leadTicks == (graceTicks / 10U));

    Foundation::UInt64 acceptedDeadline{0U};
    Foundation::UInt64 acceptedTimeout{0U};
    Foundation::UInt64 rejectedDeadline{0U};
    Foundation::UInt64 rejectedTimeout{0U};
    const bool accepted = Platform::Ia64::Scheduler::Detail::TryComputeUserProbeWaitDeadlines(
        boundaryTicks - 1U, acceptedDeadline, acceptedTimeout);
    const bool rejected = Platform::Ia64::Scheduler::Detail::TryComputeUserProbeWaitDeadlines(
        boundaryTicks, rejectedDeadline, rejectedTimeout);
    ASTERIA_EXPECT(runner, "user WaitSet probe deadlines reject the first overflowing window", accepted,
                   acceptedDeadline == (maximumTicks - graceTicks - 1U), acceptedTimeout == (maximumTicks - 1U),
                   !rejected, rejectedDeadline == 0U, rejectedTimeout == 0U);
}

void TestUnreadyRootFailsBeforeRunnableMutation(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ResourceDomainLedger resourceLedger;
    ResourceDomain resourceDomain{resourceLedger};
    const bool rootInitialized =
        resourceDomain.InitializeRoot(ResourceDomainId{9U}, ResourceLimits()).Error == ResourceDomainError::None;
    const auto resourceObject = issuer.CreateObject(resourceDomain);
    const ResourceDomainObjectId resourceIdentity = resourceObject.HasValue()
                                                        ? ResourceDomainObjectId{registry, resourceObject.GetValue()}
                                                        : ResourceDomainObjectId{};
    auto* const runtimeMemory = ::new (static_cast<void*>(g_runtimeMemoryStorage)) Ia64BspRuntimeMemory{};
    auto* const kernelRuntime = ::new (static_cast<void*>(g_kernelRuntimeStorage))
        Ia64BspKernelRuntime{registry, registryLock, TestCapabilityValidationKey};

    const auto prepared =
        kernelRuntime->Prepare(*runtimeMemory, resourceIdentity, resourceDomain, issuer, Capabilities(), 0xD0U);
    const auto repeated =
        kernelRuntime->Prepare(*runtimeMemory, resourceIdentity, resourceDomain, issuer, Capabilities(), 0xD0U);
    const auto dispatch = kernelRuntime->DispatchBootstrapWorker();

    ASTERIA_EXPECT(runner, "an unready real BSP owner produces one typed terminal preflight failure", rootInitialized,
                   resourceObject.HasValue(), !prepared.HasValue(),
                   prepared.GetError() == Ia64BspKernelRuntimeError::RuntimeRootBorrowRejected,
                   kernelRuntime->GetState() == Ia64BspKernelRuntimeState::Failed,
                   kernelRuntime->GetFailure().Error == Ia64BspKernelRuntimeError::RuntimeRootBorrowRejected,
                   kernelRuntime->GetFailure().RootBorrowError == Ia64BspRuntimeRootBorrowError::NotReady);
    const auto unreadyRuntimeSnapshot = runtimeMemory->GetSnapshot();
    ASTERIA_EXPECT(
        runner, "preflight failure registers no worker and publishes no break transport", !repeated.HasValue(),
        repeated.GetError() == Ia64BspKernelRuntimeError::PreviousPreparationFailed, !dispatch.HasValue(),
        dispatch.GetError() == Ia64BspKernelRuntimeError::NotPrepared, g_timerInitializeCalls == 0U,
        registry.GetActiveCount() == 1U, !unreadyRuntimeSnapshot.SystemCallPublication.Published,
        unreadyRuntimeSnapshot.SystemCallPublication.LastError == Ia64BspRuntimeSystemCallPublicationError::None,
        AcquireSystemCallPublication() == 0U);
    ASTERIA_EXPECT(runner, "the boot-lifetime owner retains page-aligned storage geometry",
                   (sizeof(Ia64BspKernelRuntime) % Kernel::Memory::MappingPageSizeBytes) == 0U,
                   alignof(Ia64BspKernelRuntime) == Kernel::Memory::MappingPageSizeBytes);

    kernelRuntime->~Ia64BspKernelRuntime();
    runtimeMemory->~Ia64BspRuntimeMemory();
    const bool resourceQuiesced = resourceDomain.Quiesce() == ResourceDomainError::None;
    const bool destructionStarted = resourceObject.HasValue() && (issuer.DestroyObject(resourceObject.GetValue()) ==
                                                                  Kernel::Object::ObjectRegistryError::Ok);
    const auto collected = issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "the side-effect-free rejection leaves the real resource root reclaimable", resourceQuiesced,
                   destructionStarted, collected.HasValue(), collected.GetValue().Instance == &resourceDomain);
}

void TestUserProbePreparationAndHostedLaunchFailure(TestContext& runner) noexcept
{
    g_timerInitializeCalls = 0U;
    ResetHostedLaunchObservations();
    auto* const readyUserProbe =
        ::new (static_cast<void*>(g_readyRuntimeArena.UserProbeStorage)) Ia64UserExecutionProbe{};
    Ia64UserExecutionProbeStorage probeStorage{};
    const bool fixturePrepared = PrepareReadyFixture(probeStorage);
    const auto payload = Platform::Ia64::Scheduler::GetIa64UserExecutionProbePayload();
    Ia64UserExecutionProbe overlapProbe;
    Ia64UserExecutionProbeStorage overlappingStorage = probeStorage;
    overlappingStorage.RseBackingStore.Allocation = overlappingStorage.MemoryStack.Allocation;
    overlappingStorage.RseBackingStore.WritableAlias.PhysicalStart =
        overlappingStorage.MemoryStack.Allocation.PhysicalStart;
    static_cast<void>(std::memset(g_readyProbeCode.Data, OverlapCodeFill, sizeof(g_readyProbeCode.Data)));
    static_cast<void>(
        std::memset(g_readyProbeMemoryStack.Data, OverlapMemoryStackFill, sizeof(g_readyProbeMemoryStack.Data)));
    static_cast<void>(std::memset(g_readyProbeRse.Data, OverlapRseFill, sizeof(g_readyProbeRse.Data)));
    const auto overlappingStage = overlapProbe.StageStorage(overlappingStorage, Ia64UserExecutionProbeCodeView{
                                                                                    .Data = payload.Data,
                                                                                    .SizeBytes = payload.SizeBytes,
                                                                                });
    ASTERIA_EXPECT(
        runner, "StageStorage rejects duplicate backing without mutating any alias", !overlappingStage.HasValue(),
        overlappingStage.GetError() == Ia64UserExecutionProbeStageError::PhysicalAllocationsOverlap,
        overlapProbe.GetState() == Ia64UserExecutionProbeState::Fresh,
        BytesHaveValue(g_readyProbeCode.Data, sizeof(g_readyProbeCode.Data), OverlapCodeFill),
        BytesHaveValue(g_readyProbeMemoryStack.Data, sizeof(g_readyProbeMemoryStack.Data), OverlapMemoryStackFill),
        BytesHaveValue(g_readyProbeRse.Data, sizeof(g_readyProbeRse.Data), OverlapRseFill));
    const auto staged = readyUserProbe->StageStorage(probeStorage, Ia64UserExecutionProbeCodeView{
                                                                       .Data = payload.Data,
                                                                       .SizeBytes = payload.SizeBytes,
                                                                   });
    static_cast<void>(std::memset(g_readyProbeMemoryStack.Data + HostedBootstrapOffsetBytes, HostedBootstrapGuardFill,
                                  Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcBootstrapRecordSizeBytes));
    const bool rootDomainInitialized =
        g_readyResourceDomain.InitializeRoot(ResourceDomainId{ReadyChargeOwner}, ResourceLimits()).Error ==
        ResourceDomainError::None;
    const auto rootDomainObject = g_readyIssuer.CreateObject(g_readyResourceDomain);
    const ResourceDomainObjectId rootDomainIdentity =
        rootDomainObject.HasValue() ? ResourceDomainObjectId{g_readyRegistry, rootDomainObject.GetValue()}
                                    : ResourceDomainObjectId{};
    g_readyResourceDomainIdentity = rootDomainIdentity;
    const auto conflictRuntimeMemoryPrepared = g_readyRuntimeArena.ConflictRuntimeMemory.PrepareAndAdmit(
        CpuId{0U}, Capabilities(), g_readyAddressSpacePreparation, ResourceDomainId{ReadyChargeOwner}, g_readyRegistry,
        g_readyIssuer);
    const auto conflictPublication = g_readyRuntimeArena.ConflictRuntimeMemory.PublishSystemCallEntryContext(
        CpuId{0U}, AddressOf(&g_conflictBridgeContextToken));
    auto* const conflictKernelRuntime = ::new (static_cast<void*>(g_readyRuntimeArena.ConflictKernelRuntimeStorage))
        Ia64BspKernelRuntime{g_readyRegistry, g_readyRegistryLock, TestCapabilityValidationKey};
    const auto conflictKernelPreparation =
        conflictKernelRuntime->Prepare(g_readyRuntimeArena.ConflictRuntimeMemory, rootDomainIdentity,
                                       g_readyResourceDomain, g_readyIssuer, Capabilities(), 0xD2U);
    const auto conflictAfterScheduler = g_readyRuntimeArena.ConflictRuntimeMemory.GetSystemCallPublicationSnapshot();
    ASTERIA_EXPECT(runner, "the hosted address-space fixture is ready for BSP admission", fixturePrepared);
    ASTERIA_EXPECT(runner, "a separate ready BSP owner is admitted before syscall publication",
                   conflictRuntimeMemoryPrepared.HasValue());
    ASTERIA_EXPECT(runner, "the admitted BSP owner accepts its initial syscall context publication",
                   conflictPublication == Ia64BspRuntimeSystemCallPublicationError::None);
    ASTERIA_EXPECT(runner, "scheduler retains the typed AlreadyPublished publication cause",
                   !conflictKernelPreparation.HasValue(),
                   conflictKernelPreparation.GetError() == Ia64BspKernelRuntimeError::SystemCallPublicationRejected,
                   conflictKernelRuntime->GetFailure().SystemCallPublicationError ==
                       Ia64BspRuntimeSystemCallPublicationError::AlreadyPublished);
    ASTERIA_EXPECT(runner, "the rejected scheduler publication does not replace the prior context",
                   conflictAfterScheduler.Published,
                   conflictAfterScheduler.LastError == Ia64BspRuntimeSystemCallPublicationError::AlreadyPublished,
                   conflictAfterScheduler.Context.BridgeContextAddress == AddressOf(&g_conflictBridgeContextToken));
    const PalVirtualMemoryCapabilities processorCapabilities[Kernel::Scheduler::MaximumProcessorCount]{
        Capabilities(),
        Capabilities(),
        Capabilities(),
        Capabilities(),
    };
    const auto runtimeMemoryPrepared = g_readyRuntimeArena.RuntimeMemory.PrepareAndAdmit(
        CpuId{0U}, 0x0FU, processorCapabilities, g_readyAddressSpacePreparation, ResourceDomainId{ReadyChargeOwner},
        g_readyRegistry, g_readyIssuer);
    auto* const kernelRuntime = ::new (static_cast<void*>(g_readyRuntimeArena.KernelRuntimeStorage))
        Ia64BspKernelRuntime{g_readyRegistry, g_readyRegistryLock, TestCapabilityValidationKey};

    const auto beforeRuntimePreparation = kernelRuntime->PrepareUserExecutionProbe(*readyUserProbe);
    const auto timerBeforeRuntimePreparation = kernelRuntime->RunLocalTimerInterruptProbe();
    const bool setupReady =
        fixturePrepared && staged.HasValue() && rootDomainInitialized && rootDomainObject.HasValue() &&
        conflictRuntimeMemoryPrepared.HasValue() &&
        (conflictPublication == Ia64BspRuntimeSystemCallPublicationError::None) &&
        !conflictKernelPreparation.HasValue() &&
        (conflictKernelPreparation.GetError() == Ia64BspKernelRuntimeError::SystemCallPublicationRejected) &&
        (conflictKernelRuntime->GetState() == Ia64BspKernelRuntimeState::Failed) &&
        (conflictKernelRuntime->GetFailure().Error == Ia64BspKernelRuntimeError::SystemCallPublicationRejected) &&
        (conflictKernelRuntime->GetFailure().SystemCallPublicationError ==
         Ia64BspRuntimeSystemCallPublicationError::AlreadyPublished) &&
        runtimeMemoryPrepared.HasValue();
    ASTERIA_EXPECT(
        runner, "the hosted fixture propagates an exact late-publication conflict and admits a separate real BSP root",
        setupReady);
    ASTERIA_EXPECT(runner, "the staged owner remains untouched until the runtime root and exact worker exist",
                   !beforeRuntimePreparation.HasValue(),
                   beforeRuntimePreparation.GetError() == Ia64BspKernelRuntimeUserProbeError::RuntimeNotPrepared,
                   kernelRuntime->GetUserExecutionProbeState() == Ia64BspKernelRuntimeUserProbeState::Fresh);
    ASTERIA_EXPECT(runner, "the timer proof rejects an unprepared runtime without consuming its one attempt",
                   !timerBeforeRuntimePreparation.HasValue(),
                   timerBeforeRuntimePreparation.GetError() ==
                       Ia64BspKernelRuntimeLocalTimerProbeError::RuntimeNotPrepared,
                   kernelRuntime->GetLocalTimerInterruptProbeState() == Ia64BspKernelRuntimeLocalTimerProbeState::Fresh,
                   g_timerProgramCalls == 0U, g_timerCancelCalls == 0U);
    if (!setupReady)
    {
        return;
    }

    const auto runtimePrepared = kernelRuntime->Prepare(g_readyRuntimeArena.RuntimeMemory, rootDomainIdentity,
                                                        g_readyResourceDomain, g_readyIssuer, Capabilities(),
                                                        Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector);
    const auto timerBeforeUserMemoryPreparation = kernelRuntime->RunLocalTimerInterruptProbe();
    const auto publishedRuntime = g_readyRuntimeArena.RuntimeMemory.GetSnapshot();
    const Foundation::UInt64 bridgeContextAddress = publishedRuntime.SystemCallPublication.Context.BridgeContextAddress;
    const auto* const publishedBridgeContext = reinterpret_cast<const Ia64SystemCallBridgeContext*>(
        static_cast<Foundation::UIntPointer>(bridgeContextAddress));
    const Ia64ProcessorRunLoop* const publishedRunLoop =
        publishedBridgeContext != nullptr ? publishedBridgeContext->RunLoop : nullptr;
    const Foundation::UInt64 kernelRuntimeStart = AddressOf(kernelRuntime);
    const Foundation::UInt64 kernelRuntimeEnd = kernelRuntimeStart + sizeof(*kernelRuntime);
    const auto readyRoot = g_readyRuntimeArena.RuntimeMemory.BorrowReadyRoot();
    const auto initialComponentRoot = g_readyRuntimeArena.RuntimeMemory.BorrowReadyInitialComponentRoot();
    const Foundation::Size mappingCountBefore =
        initialComponentRoot.HasValue()
            ? initialComponentRoot.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount
            : 0U;
    const Foundation::Size commonMappingCountBefore =
        readyRoot.HasValue() ? readyRoot.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount : 0U;
    const Foundation::Size objectCountBefore = g_readyRegistry.GetActiveCount();
    const Foundation::Size activationsBeforeInvalidPlacement = g_regionActivationCalls;
    ASTERIA_EXPECT(runner, "base Prepare retains an exact empty capability space", runtimePrepared.HasValue(),
                   HasExactPreparedCapabilityAuthority(
                       g_readyRegistry, runtimePrepared.GetValue().ProtectionDomainIdentity, *kernelRuntime, 0U));
    ASTERIA_EXPECT(runner,
                   "base Prepare registers the exact retained Timer and Notification generations under the root domain",
                   runtimePrepared.HasValue(), HasExactLocalTimerProbeObjects(g_readyRegistry, *kernelRuntime, 0U));
    ASTERIA_EXPECT(runner, "the timer proof requires prepared direct user memory before publishing delivery state",
                   !timerBeforeUserMemoryPreparation.HasValue(),
                   timerBeforeUserMemoryPreparation.GetError() ==
                       Ia64BspKernelRuntimeLocalTimerProbeError::UserMemoryNotPrepared,
                   kernelRuntime->GetLocalTimerInterruptProbeState() == Ia64BspKernelRuntimeLocalTimerProbeState::Fresh,
                   !publishedRuntime.ExternalInterruptPublication.Published,
                   publishedRuntime.ExternalInterruptPublication.LastError ==
                       Ia64BspRuntimeExternalInterruptPublicationError::None,
                   g_timerProgramCalls == 0U, g_timerCancelCalls == 0U);
    ASTERIA_EXPECT(
        runner, "base Prepare publishes nonblocking collaborators with user memory unready and Wait disabled",
        runtimePrepared.HasValue(), publishedBridgeContext != nullptr,
        publishedBridgeContext->RunLoop == publishedRunLoop, publishedBridgeContext->UserMemory != nullptr,
        !publishedBridgeContext->UserMemory->IsReady(),
        !publishedBridgeContext->UserMemory->GetAddressSpaceId().IsValid(), publishedBridgeContext->Scratch != nullptr,
        publishedBridgeContext->WaitContinuation == nullptr, publishedBridgeContext->WaitSetDispatcher == nullptr);
    Ia64UserExecutionProbe unmappedProbe;
    const auto invalidProbePlacement = kernelRuntime->PrepareUserExecutionProbe(unmappedProbe);
    ASTERIA_EXPECT(
        runner, "an unmapped external probe owner is rejected before dispatcher construction",
        !invalidProbePlacement.HasValue(),
        invalidProbePlacement.GetError() == Ia64BspKernelRuntimeUserProbeError::InvalidProbePlacement,
        kernelRuntime->GetUserExecutionProbeState() == Ia64BspKernelRuntimeUserProbeState::Fresh,
        g_readyRegistry.GetActiveCount() == objectCountBefore, readyRoot.HasValue(), initialComponentRoot.HasValue(),
        readyRoot.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount == commonMappingCountBefore,
        initialComponentRoot.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount == mappingCountBefore,
        g_regionActivationCalls == activationsBeforeInvalidPlacement, g_localPurgeCalls == 0U);
    const Foundation::Size activationsBeforeProbePreparation = g_regionActivationCalls;
    const auto probePrepared = kernelRuntime->PrepareUserExecutionProbe(*readyUserProbe);
    const auto repeatedPreparation = kernelRuntime->PrepareUserExecutionProbe(*readyUserProbe);
    const auto initialImageAfterProbe =
        kernelRuntime->PrepareInitialUserImage(g_initialImageMemoryOwner, g_initialImageDispatchMappingOwner);
    const Foundation::Size mappingCountAfter =
        initialComponentRoot.HasValue()
            ? initialComponentRoot.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount
            : 0U;
    const auto outsideStack =
        UserMemoryRange::Create(Platform::Ia64::Scheduler::Ia64UserExecutionProbeMemoryStackVirtualStart +
                                    Ia64UserExecutionProbeMemoryStackSizeBytes,
                                1U);
    Foundation::UInt8 outsideDestination{0xA5U};
    UserMemoryAccessError outsideStackError{UserMemoryAccessError::BackendProtocolViolation};
    if (probePrepared.HasValue() && outsideStack.HasValue() && (publishedBridgeContext != nullptr) &&
        (publishedBridgeContext->UserMemory != nullptr))
    {
        outsideStackError =
            publishedBridgeContext->UserMemory->CopyFromUser(outsideStack.GetValue(), WritableKernelBuffer{
                                                                                          .Data = &outsideDestination,
                                                                                          .SizeBytes = 1U,
                                                                                      });
    }
    ASTERIA_EXPECT(runner, "probe preparation retains the probe, endpoint, and WaitSet capabilities",
                   runtimePrepared.HasValue(),
                   HasExactPreparedCapabilityAuthority(
                       g_readyRegistry, runtimePrepared.GetValue().ProtectionDomainIdentity, *kernelRuntime, 3U));
    ASTERIA_EXPECT(runner, "probe preparation retains endpoint selector 0x101 after the r4/r5 selector 0x100",
                   probePrepared.HasValue(),
                   kernelRuntime->GetUserExecutionProbeEndpointCapability().GetSelector() ==
                       Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedEndpointCapabilitySelector);
    ASTERIA_EXPECT(
        runner, "probe preparation retains Wait authority for the exact runtime WaitSet at selector 0x102",
        probePrepared.HasValue(),
        HasExactUserProbeWaitSet(g_readyRegistry, runtimePrepared.GetValue().ProtectionDomainIdentity, *kernelRuntime));
    ASTERIA_EXPECT(runner,
                   "probe preparation retains the exact authenticated handle and canonical 88-byte bootstrap frame",
                   probePrepared.HasValue(), HasExactRetainedUserProbeBootstrap(*kernelRuntime));
    ASTERIA_EXPECT(runner, "bootstrap preparation clears the complete published syscall scratch state",
                   probePrepared.HasValue(), publishedBridgeContext != nullptr,
                   publishedBridgeContext->Scratch != nullptr,
                   HasClearSystemCallScratch(*publishedBridgeContext->Scratch));
    ASTERIA_EXPECT(
        runner,
        "probe preparation binds the exact stack-only backend and rejects "
        "an outside range without touching host memory",
        probePrepared.HasValue(), readyRoot.HasValue(), initialComponentRoot.HasValue(),
        publishedBridgeContext != nullptr, publishedBridgeContext->UserMemory != nullptr,
        publishedBridgeContext->UserMemory->IsReady(),
        publishedBridgeContext->UserMemory->GetAddressSpaceId().IsEqualTo(
            initialComponentRoot.GetValue().AddressSpaceIdentity),
        readyRoot.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount == commonMappingCountBefore,
        readyRoot.GetValue().AddressSpaceStorage->GetSnapshot().ResidentProcessorMask == 0x01U,
        initialComponentRoot.GetValue().AddressSpaceStorage->GetSnapshot().ResidentProcessorMask == 0U,
        readyRoot.GetValue().ControllerStorage->GetSnapshot().ActiveStorage == readyRoot.GetValue().AddressSpaceStorage,
        g_regionActivationCalls == activationsBeforeProbePreparation + 2U, g_lastRegionOneActivation == 0x0020'0930U,
        outsideStack.HasValue(), outsideStackError == UserMemoryAccessError::AccessDenied, outsideDestination == 0xA5U);
    ASTERIA_EXPECT(runner,
                   "host preparation validates bootstrap bytes without dereferencing the region-one direct alias",
                   probePrepared.HasValue(),
                   BytesHaveValue(g_readyProbeMemoryStack.Data + HostedBootstrapOffsetBytes,
                                  Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcBootstrapRecordSizeBytes,
                                  HostedBootstrapGuardFill));
    ASTERIA_EXPECT(
        runner, "Prepare publishes the exact retained bridge collaborators before probe preparation and Prepared state",
        runtimePrepared.HasValue(), publishedRuntime.SystemCallPublication.Published,
        publishedRuntime.SystemCallPublication.LastError == Ia64BspRuntimeSystemCallPublicationError::None,
        publishedRuntime.SystemCallPublication.LastContextValidationError == Ia64SystemCallContextValidationError::None,
        ValidateIa64SystemCallTrustedContext(publishedRuntime.SystemCallPublication.Context) ==
            Ia64SystemCallContextValidationError::None,
        publishedRuntime.SystemCallPublication.ContextAddress == AcquireSystemCallPublication(),
        publishedRuntime.SystemCallPublication.Context.ProcessorIndex == 0U, bridgeContextAddress >= kernelRuntimeStart,
        bridgeContextAddress <= (kernelRuntimeEnd - sizeof(Ia64SystemCallBridgeContext)),
        (bridgeContextAddress & (alignof(Ia64SystemCallBridgeContext) - 1U)) == 0U, publishedBridgeContext != nullptr,
        publishedRunLoop != nullptr, publishedBridgeContext->UserMemory != nullptr,
        publishedBridgeContext->Scratch != nullptr, publishedBridgeContext->WaitContinuation != nullptr,
        publishedBridgeContext->WaitSetDispatcher != nullptr,
        publishedRunLoop->GetSnapshot().State == Kernel::Scheduler::ProcessorKernelRunLoopState::Idle,
        kernelRuntime->GetFailure().SystemCallPublicationError == Ia64BspRuntimeSystemCallPublicationError::None,
        readyRoot.HasValue(), probePrepared.HasValue(),
        probePrepared.GetValue().ThreadIdentity.IsEqualTo(runtimePrepared.GetValue().ThreadIdentity),
        probePrepared.GetValue().Processor.GetValue() == 0U,
        probePrepared.GetValue().State == Ia64BspKernelRuntimeUserProbeState::Prepared,
        kernelRuntime->GetState() == Ia64BspKernelRuntimeState::Prepared,
        kernelRuntime->GetUserExecutionProbeState() == Ia64BspKernelRuntimeUserProbeState::Prepared,
        mappingCountAfter == mappingCountBefore + 3U, g_localPurgeCalls == 9U);
    ASTERIA_EXPECT(runner, "the distinct prepared probe path is one-shot without changing legacy state",
                   !repeatedPreparation.HasValue(),
                   repeatedPreparation.GetError() == Ia64BspKernelRuntimeUserProbeError::AlreadyPrepared,
                   kernelRuntime->GetUserExecutionProbeState() == Ia64BspKernelRuntimeUserProbeState::Prepared);
    ASTERIA_EXPECT(
        runner, "preparing the synthetic probe excludes the real image before its owner is inspected",
        !initialImageAfterProbe.HasValue(),
        initialImageAfterProbe.GetError() == Ia64BspKernelRuntimeInitialUserImageError::ConfigurationConflict,
        kernelRuntime->GetInitialUserImageState() == Ia64BspKernelRuntimeInitialUserImageState::Fresh,
        kernelRuntime->GetInitialUserImageFailure().Error == Ia64BspKernelRuntimeInitialUserImageError::None,
        kernelRuntime->GetInitialUserImageLaunchOwner().GetState() == Ia64InitialUserImageLaunchOwnerState::Fresh,
        kernelRuntime->GetUserExecutionProbeState() == Ia64BspKernelRuntimeUserProbeState::Prepared);
    if (!runtimePrepared.HasValue() || !probePrepared.HasValue())
    {
        return;
    }

    CooperativeShootdownTransport cooperativeTransport;
    PhysicalPageRange forgedPage = g_readySharedRootShootdownPage;
    forgedPage.PageCount = 2U;
    const Foundation::Size objectCountBeforeShootdown = g_readyRegistry.GetActiveCount();
    const auto rejectedPreflight = kernelRuntime->MapSharedRootShootdownPage(forgedPage, 0x0FU, cooperativeTransport);
    ASTERIA_EXPECT(
        runner, "shared-root mapping preflight is side-effect-free and retryable", !rejectedPreflight.HasValue(),
        rejectedPreflight.GetError() == Ia64BspSharedRootShootdownError::InvalidPhysicalPage,
        kernelRuntime->GetSharedRootShootdownState() == Ia64BspSharedRootShootdownState::Fresh,
        kernelRuntime->GetSharedRootShootdownSummary().State == Ia64BspSharedRootShootdownState::Fresh,
        g_readyRegistry.GetActiveCount() == objectCountBeforeShootdown, cooperativeTransport.GetPublishCount() == 0U);

    const auto admittedOne = g_readyRuntimeArena.RuntimeMemory.AdmitApplicationProcessorPhysicalRoot(CpuId{1U});
    const auto admittedTwo = g_readyRuntimeArena.RuntimeMemory.AdmitApplicationProcessorPhysicalRoot(CpuId{2U});
    const auto admittedThree = g_readyRuntimeArena.RuntimeMemory.AdmitApplicationProcessorPhysicalRoot(CpuId{3U});
    const auto sharedRootMapped =
        kernelRuntime->MapSharedRootShootdownPage(g_readySharedRootShootdownPage, 0x0FU, cooperativeTransport);
    const auto repeatedSharedRootMapping =
        kernelRuntime->MapSharedRootShootdownPage(g_readySharedRootShootdownPage, 0x0FU, cooperativeTransport);
    const auto rootAfterSharedMapping = readyRoot.GetValue().AddressSpaceStorage->GetSnapshot();
    const auto sharedMappingRecord = readyRoot.GetValue().AddressSpaceStorage->TryGetMapping(commonMappingCountBefore);
    ASTERIA_EXPECT(
        runner, "one cooperative transaction maps the exact pinned page only after every profiled CPU is resident",
        admittedOne.HasValue(), admittedTwo.HasValue(), admittedThree.HasValue(), sharedRootMapped.HasValue(),
        sharedRootMapped.GetValue().State == Ia64BspSharedRootShootdownState::Complete,
        sharedRootMapped.GetValue().TargetProcessorMask == 0x0FU,
        sharedRootMapped.GetValue().QueuedProcessorMask == 0x0EU,
        sharedRootMapped.GetValue().AcknowledgedProcessorMask == 0x0FU,
        sharedRootMapped.GetValue().Mapping.VirtualStart.GetValue() ==
            Platform::Ia64::Scheduler::Ia64BspSharedRootShootdownVirtualStart,
        sharedRootMapped.GetValue().Mapping.Pages.GetValue() == 1U,
        kernelRuntime->GetSharedRootShootdownState() == Ia64BspSharedRootShootdownState::Complete,
        rootAfterSharedMapping.State == Kernel::Memory::AddressSpaceState::Ready,
        rootAfterSharedMapping.ResidentProcessorMask == 0x0FU,
        rootAfterSharedMapping.MappingCount == commonMappingCountBefore + 1U, sharedMappingRecord.HasValue(),
        sharedMappingRecord.GetValue().VirtualStart.GetValue() ==
            Platform::Ia64::Scheduler::Ia64BspSharedRootShootdownVirtualStart,
        sharedMappingRecord.GetValue().PhysicalStart.GetValue() == g_readySharedRootShootdownPage.PhysicalStart,
        sharedMappingRecord.GetValue().Pages.GetValue() == 1U,
        sharedMappingRecord.GetValue().Permissions.GetBits() == 0x03U,
        !sharedMappingRecord.GetValue().Permissions.Contains(Kernel::Memory::PagePermission::Execute),
        !sharedMappingRecord.GetValue().Permissions.Contains(Kernel::Memory::PagePermission::User),
        sharedMappingRecord.GetValue().CachePolicy == Kernel::Memory::MemoryCachePolicy::WriteBack,
        sharedMappingRecord.GetValue().Commitment == Kernel::Memory::MemoryCommitment::Pinned,
        g_readyRegistry.GetActiveCount() == objectCountBeforeShootdown + 2U,
        cooperativeTransport.GetPublishCount() == 1U, cooperativeTransport.GetObservedProcessorMask() == 0x0EU,
        cooperativeTransport.HasExactRemoteInvalidations());
    ASTERIA_EXPECT(runner, "the completed shared-root proof is one-shot and retains its mapping",
                   !repeatedSharedRootMapping.HasValue(),
                   repeatedSharedRootMapping.GetError() == Ia64BspSharedRootShootdownError::AlreadyAttempted,
                   kernelRuntime->GetSharedRootShootdownState() == Ia64BspSharedRootShootdownState::Complete,
                   cooperativeTransport.GetPublishCount() == 1U);

    const auto singleProcessorMemoryPrepared = g_readyRuntimeArena.SingleProcessorRuntimeMemory.PrepareAndAdmit(
        CpuId{0U}, Capabilities(), g_readyAddressSpacePreparation, ResourceDomainId{ReadyChargeOwner}, g_readyRegistry,
        g_readyIssuer);
    auto* const singleProcessorRuntime =
        ::new (static_cast<void*>(g_readyRuntimeArena.SingleProcessorKernelRuntimeStorage))
            Ia64BspKernelRuntime{g_readyRegistry, g_readyRegistryLock, TestCapabilityValidationKey};
    const auto singleProcessorPrepared = singleProcessorRuntime->Prepare(
        g_readyRuntimeArena.SingleProcessorRuntimeMemory, rootDomainIdentity, g_readyResourceDomain, g_readyIssuer,
        Capabilities(), Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector);
    CooperativeShootdownTransport singleProcessorTransport;
    const auto singleProcessorMapped = singleProcessorRuntime->MapSharedRootShootdownPage(
        g_singleProcessorSharedRootShootdownPage, 0x01U, singleProcessorTransport);
    const auto singleProcessorRoot = g_readyRuntimeArena.SingleProcessorRuntimeMemory.BorrowReadyRoot();
    ASTERIA_EXPECT(runner, "an SMP1 shared-root transaction completes locally without transport publication",
                   singleProcessorMemoryPrepared.HasValue(), singleProcessorPrepared.HasValue(),
                   singleProcessorMapped.HasValue(), singleProcessorMapped.GetValue().TargetProcessorMask == 0x01U,
                   singleProcessorMapped.GetValue().QueuedProcessorMask == 0U,
                   singleProcessorMapped.GetValue().AcknowledgedProcessorMask == 0x01U,
                   singleProcessorMapped.GetValue().State == Ia64BspSharedRootShootdownState::Complete,
                   singleProcessorRoot.HasValue(),
                   singleProcessorRoot.GetValue().AddressSpaceStorage->GetSnapshot().State ==
                       Kernel::Memory::AddressSpaceState::Ready,
                   singleProcessorRoot.GetValue().AddressSpaceStorage->GetSnapshot().ResidentProcessorMask == 0x01U,
                   singleProcessorTransport.GetPublishCount() == 0U,
                   singleProcessorTransport.GetObservedProcessorMask() == 0U);

    const auto localTimerProbe = kernelRuntime->RunLocalTimerInterruptProbe();
    const auto repeatedLocalTimerProbe = kernelRuntime->RunLocalTimerInterruptProbe();
    const auto externalPublication = g_readyRuntimeArena.RuntimeMemory.GetExternalInterruptPublicationSnapshot();
    const auto& localTimerFailure = kernelRuntime->GetLocalTimerInterruptProbeFailure();
    const auto& retainedTimerEvidence = kernelRuntime->GetLocalTimerInterruptEvidence();
    const auto& retainedTimerQueueEvidence = kernelRuntime->GetLocalTimerInterruptTimerQueueEvidence();
    const Foundation::UInt64 expectedTimerDeadline = 1U + Platform::Ia64::Scheduler::Ia64BspLocalTimerProbeLeadTicks;
    const Foundation::UInt64 expectedTimerTimeout =
        expectedTimerDeadline + Platform::Ia64::Scheduler::Ia64BspLocalTimerProbeGraceTicks;
    ASTERIA_EXPECT(
        runner, "hosted native unavailability is retained after the timer is reconciled", !localTimerProbe.HasValue(),
        localTimerProbe.GetError() == Ia64BspKernelRuntimeLocalTimerProbeError::PollRejected,
        kernelRuntime->GetLocalTimerInterruptProbeState() == Ia64BspKernelRuntimeLocalTimerProbeState::Failed,
        localTimerFailure.Error == Ia64BspKernelRuntimeLocalTimerProbeError::PollRejected,
        localTimerFailure.PollStatus == Ia64ExternalInterruptPollStatus::NativeUnavailable,
        localTimerFailure.InitialTicks == 1U, localTimerFailure.ProgrammedDeadlineTicks == expectedTimerDeadline,
        localTimerFailure.AbsoluteTimeoutTicks == expectedTimerTimeout,
        localTimerFailure.Publication == Ia64BspRuntimeExternalInterruptPublicationError::None,
        localTimerFailure.InterruptVectorTable == Ia64RuntimeExternalInterruptPublicationError::None,
        localTimerFailure.TimerQueue.Error == Kernel::Time::TimerQueueError::None,
        localTimerFailure.Controller.Error == Kernel::Scheduler::ProcessorRunError::None,
        localTimerFailure.Delivery.Error == TimerNotificationDeliveryError::None,
        localTimerFailure.Notification.Error == Kernel::Ipc::NotificationError::None,
        localTimerFailure.EvidenceValidation ==
            Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidenceValidationError::None,
        localTimerFailure.Acknowledgement == Ia64BspRuntimeExternalInterruptAcknowledgementError::None);
    ASTERIA_EXPECT(
        runner, "the runtime owner publishes its exact BSP context and zero evidence corridor",
        externalPublication.Published,
        externalPublication.LastError == Ia64BspRuntimeExternalInterruptPublicationError::None,
        externalPublication.LastInterruptVectorTableError == Ia64RuntimeExternalInterruptPublicationError::None,
        externalPublication.LastAcknowledgementError == Ia64BspRuntimeExternalInterruptAcknowledgementError::None,
        externalPublication.Context.EvidenceAddress == externalPublication.EvidenceAddress,
        externalPublication.Context.ProcessorIndex == 0U,
        externalPublication.Context.TimerVector == Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector,
        externalPublication.Context.ExternalPreemptionControlAddress == 0U, externalPublication.Evidence.Sequence == 0U,
        externalPublication.Evidence.AcknowledgedSequence == 0U,
        externalPublication.ContextAddress >= publishedRuntime.Summary.RuntimeObjects.VirtualStart,
        externalPublication.EvidenceAddress >= publishedRuntime.Summary.RuntimeObjects.VirtualStart,
        externalPublication.ContextAddress + sizeof(externalPublication.Context) <=
            publishedRuntime.Summary.RuntimeObjects.VirtualStart + publishedRuntime.Summary.RuntimeObjects.SizeBytes,
        externalPublication.EvidenceAddress + sizeof(externalPublication.Evidence) <=
            publishedRuntime.Summary.RuntimeObjects.VirtualStart + publishedRuntime.Summary.RuntimeObjects.SizeBytes);
    ASTERIA_EXPECT(runner,
                   "one hosted attempt programs, masks, and cancels without publishing "
                   "partial evidence or blocking dispatch",
                   g_timerProgramCalls == 1U, g_timerCancelCalls == 1U,
                   g_programmedTimerDeadline == expectedTimerDeadline,
                   g_programmedTimerVector == Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector,
                   g_programmedTimerRollbackDeadline == 0U,
                   g_programmedTimerRollbackVector == Platform::Ia64::Interrupt::Ia64ExternalInterruptMaskedTimerVector,
                   g_programmedTimerMaskedVector == Platform::Ia64::Interrupt::Ia64ExternalInterruptMaskedTimerVector,
                   g_cancelledTimerVector == Platform::Ia64::Interrupt::Ia64ExternalInterruptMaskedTimerVector,
                   !repeatedLocalTimerProbe.HasValue(),
                   repeatedLocalTimerProbe.GetError() == Ia64BspKernelRuntimeLocalTimerProbeError::AlreadyAttempted,
                   retainedTimerEvidence.Sequence == 0U, retainedTimerEvidence.AcknowledgedSequence == 0U,
                   retainedTimerQueueEvidence.Sequence == 0U,
                   Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().UserProbe.LocalTimerPublished == 0U,
                   Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().UserProbe.WaitTimerPublished == 0U,
                   kernelRuntime->GetState() == Ia64BspKernelRuntimeState::Prepared,
                   kernelRuntime->GetUserExecutionProbeState() == Ia64BspKernelRuntimeUserProbeState::Prepared,
                   HasExactLocalTimerProbeObjects(g_readyRegistry, *kernelRuntime, 0U));

    const Foundation::Size activationsBeforeDispatch = g_regionActivationCalls;
    const auto dispatched = kernelRuntime->DispatchUserExecutionProbe();
    const auto repeatedDispatch = kernelRuntime->DispatchUserExecutionProbe();
    const auto& failure = kernelRuntime->GetUserExecutionProbeFailure();
    const Foundation::UInt64 expectedActiveRunDeadline = ~Foundation::UInt64{0U} - 1U;
    ASTERIA_EXPECT(
        runner,
        "a hosted native-unavailable return programs the exact active-run "
        "deadline, exits the selected worker, and disarms the timer",
        !dispatched.HasValue(), dispatched.GetError() == Ia64BspKernelRuntimeUserProbeError::LaunchRejected,
        kernelRuntime->GetState() == Ia64BspKernelRuntimeState::Failed,
        kernelRuntime->GetUserExecutionProbeState() == Ia64BspKernelRuntimeUserProbeState::DispatchFailed,
        failure.Error == Ia64BspKernelRuntimeUserProbeError::LaunchRejected,
        failure.ExitCode == Platform::Ia64::Scheduler::Ia64BspUserProbeLaunchFailureExitCode,
        failure.Launch == Ia64UserModeLaunchError::NativeTransitionUnavailable,
        failure.LocalInvalidator == Platform::Ia64::Memory::Ia64LocalTlbInvalidatorInitializationError::None,
        failure.Revocation == Kernel::Object::RevocationError::Ok,
        failure.Capability == Kernel::Object::CapabilityError::Ok,
        failure.UserMemory == Kernel::Memory::DirectUserMemoryAccessError::None,
        failure.EndpointRegistry == Kernel::Object::ObjectRegistryError::Ok,
        failure.EndpointPin == Kernel::Object::ObjectPinError::Ok,
        failure.BootstrapCodec == Kernel::Ipc::MessageCodecError::None,
        failure.BootstrapRange == Kernel::Memory::UserMemoryRangeError::None,
        failure.BootstrapAccess == Kernel::Memory::UserMemoryAccessError::None, runtimePrepared.HasValue(),
        HasExactUserProbeCapability(g_readyRegistry, runtimePrepared.GetValue().ProtectionDomainIdentity),
        HasExactUserProbeEndpoint(g_readyRegistry, runtimePrepared.GetValue().ProtectionDomainIdentity, *kernelRuntime),
        HasExactUserProbeWaitSet(g_readyRegistry, runtimePrepared.GetValue().ProtectionDomainIdentity, *kernelRuntime),
        g_userLaunchCalls == 1U, g_workerSwitchWasActiveAtLaunch, !g_workerSwitchActive,
        g_regionOneAtUserLaunch == 0x0020'1130U, g_regionActivationCalls == activationsBeforeDispatch + 2U,
        g_lastRegionOneActivation == 0x0020'0930U, readyRoot.HasValue(), initialComponentRoot.HasValue(),
        readyRoot.GetValue().ControllerStorage->GetSnapshot().ActiveStorage == readyRoot.GetValue().AddressSpaceStorage,
        readyRoot.GetValue().AddressSpaceStorage->GetSnapshot().ResidentProcessorMask == 0x0FU,
        initialComponentRoot.GetValue().AddressSpaceStorage->GetSnapshot().ResidentProcessorMask == 0U,
        g_timerProgramCalls == 2U, g_timerCancelCalls == 2U, g_programmedTimerDeadline == expectedActiveRunDeadline,
        g_programmedTimerVector == Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector,
        g_cancelledTimerVector == Platform::Ia64::Interrupt::Ia64ExternalInterruptMaskedTimerVector);
    ASTERIA_EXPECT(runner, "terminal probe dispatch cannot enqueue or launch the worker again",
                   !repeatedDispatch.HasValue(),
                   repeatedDispatch.GetError() == Ia64BspKernelRuntimeUserProbeError::DispatchAlreadyAttempted,
                   g_userLaunchCalls == 1U);

    const auto cleanupMemoryPrepared = g_readyRuntimeArena.PreparationFailureRuntimeMemory.PrepareAndAdmit(
        CpuId{0U}, Capabilities(), g_readyAddressSpacePreparation, ResourceDomainId{ReadyChargeOwner}, g_readyRegistry,
        g_readyIssuer);
    auto* const cleanupRuntime = ::new (static_cast<void*>(g_readyRuntimeArena.PreparationFailureKernelRuntimeStorage))
        Ia64BspKernelRuntime{g_readyRegistry, g_readyRegistryLock, TestCapabilityValidationKey};
    const auto cleanupRuntimePrepared = cleanupRuntime->Prepare(
        g_readyRuntimeArena.PreparationFailureRuntimeMemory, rootDomainIdentity, g_readyResourceDomain, g_readyIssuer,
        Capabilities(), Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector);
    auto* const freshMappedProbe =
        ::new (static_cast<void*>(g_readyRuntimeArena.PreparationFailureUserProbeStorage)) Ia64UserExecutionProbe{};
    const auto cleanupCommonRoot = g_readyRuntimeArena.PreparationFailureRuntimeMemory.BorrowReadyRoot();
    const auto cleanupPrivateRoot =
        g_readyRuntimeArena.PreparationFailureRuntimeMemory.BorrowReadyInitialComponentRoot();
    const Foundation::Size activationsBeforeCleanupFailure = g_regionActivationCalls;
    const auto cleanupFailure = cleanupRuntime->PrepareUserExecutionProbe(*freshMappedProbe);
    ASTERIA_EXPECT(
        runner,
        "a post-activation probe failure restores the common root and retains the original preparation diagnostic",
        cleanupMemoryPrepared.HasValue(), cleanupRuntimePrepared.HasValue(), cleanupCommonRoot.HasValue(),
        cleanupPrivateRoot.HasValue(), !cleanupFailure.HasValue(),
        cleanupFailure.GetError() == Ia64BspKernelRuntimeUserProbeError::ProbePreparationRejected,
        cleanupRuntime->GetUserExecutionProbeState() == Ia64BspKernelRuntimeUserProbeState::PreparationFailed,
        freshMappedProbe->GetState() == Platform::Ia64::Scheduler::Ia64UserExecutionProbeState::Fresh,
        g_regionActivationCalls == activationsBeforeCleanupFailure + 2U, g_lastRegionOneActivation == 0x0020'0930U,
        cleanupCommonRoot.GetValue().ControllerStorage->GetSnapshot().ActiveStorage ==
            cleanupCommonRoot.GetValue().AddressSpaceStorage,
        cleanupCommonRoot.GetValue().AddressSpaceStorage->GetSnapshot().ResidentProcessorMask == 0x01U,
        cleanupPrivateRoot.GetValue().AddressSpaceStorage->GetSnapshot().ResidentProcessorMask == 0U);

    ResetHostedLaunchObservations();
    InitialUserImagePreparedFixture initialImageFixture{};
    const bool initialImageIntegrated =
        PrepareAndDispatchInitialUserImageIntegration(runner, rootDomainIdentity, *readyUserProbe, initialImageFixture);
    ASTERIA_EXPECT(runner, "the independent hosted runtime completes the real initial-user-image integration path",
                   initialImageIntegrated);
}

[[nodiscard]] bool PrepareSharedRootShootdownFailureFixture(const Foundation::Size index,
                                                            const Foundation::UInt8 profiledProcessorMask,
                                                            const bool admitApplicationProcessor,
                                                            Ia64BspKernelRuntime*& runtime) noexcept
{
    if ((index >= 4U) || !g_readyResourceDomainIdentity.IsValid())
    {
        return false;
    }
    PalVirtualMemoryCapabilities processorCapabilities[Kernel::Scheduler::MaximumProcessorCount]{
        Capabilities(),
        Capabilities(),
        Capabilities(),
        Capabilities(),
    };
    auto& slot = g_readyRuntimeArena.ShootdownFailures[index];
    const auto memoryPrepared = slot.RuntimeMemory.PrepareAndAdmit(
        CpuId{0U}, profiledProcessorMask, processorCapabilities, g_readyAddressSpacePreparation,
        ResourceDomainId{ReadyChargeOwner}, g_readyRegistry, g_readyIssuer);
    runtime = ::new (static_cast<void*>(slot.KernelRuntimeStorage))
        Ia64BspKernelRuntime{g_readyRegistry, g_readyRegistryLock, TestCapabilityValidationKey};
    const auto prepared =
        runtime->Prepare(slot.RuntimeMemory, g_readyResourceDomainIdentity, g_readyResourceDomain, g_readyIssuer,
                         Capabilities(), Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector);
    bool admitted{true};
    if (admitApplicationProcessor)
    {
        admitted = slot.RuntimeMemory.AdmitApplicationProcessorPhysicalRoot(CpuId{1U}).HasValue();
    }
    return memoryPrepared.HasValue() && prepared.HasValue() && admitted;
}

[[nodiscard]] bool RetainsExactSharedRootShootdownPage(const Ia64BspKernelRuntime& runtime,
                                                       const PhysicalPageRange& page) noexcept
{
    const auto& summary = runtime.GetSharedRootShootdownSummary();
    ObjectPin framePin;
    ObjectPin memoryObjectPin;
    if (!summary.FrameIdentity.IsValid() || !summary.MemoryObjectIdentity.IsValid() ||
        (g_readyRegistry.PinObject(summary.FrameIdentity.GetObjectId(), ObjectType::Frame, framePin) !=
         ObjectPinError::Ok) ||
        (g_readyRegistry.PinObject(summary.MemoryObjectIdentity.GetObjectId(), ObjectType::MemoryObject,
                                   memoryObjectPin) != ObjectPinError::Ok))
    {
        return false;
    }
    auto& frame = static_cast<Kernel::Memory::Frame&>(framePin.GetObject());
    auto& memoryObject = static_cast<Kernel::Memory::MemoryObject&>(memoryObjectPin.GetObject());
    const auto frameSnapshot = frame.GetSnapshot();
    const auto memoryObjectSnapshot = memoryObject.GetSnapshot();
    return (frameSnapshot.State == Kernel::Memory::FrameState::Ready) &&
           (frameSnapshot.Descriptor.PhysicalStart.GetValue() == page.PhysicalStart) &&
           (frameSnapshot.Descriptor.Pages.GetValue() == 1U) &&
           (memoryObjectSnapshot.State == Kernel::Memory::MemoryObjectState::Ready) &&
           (memoryObjectSnapshot.Descriptor.Pages.GetValue() == 1U) &&
           (memoryObjectSnapshot.Descriptor.Commitment == Kernel::Memory::MemoryCommitment::Pinned);
}

[[nodiscard]] bool InstallVirtualOverlapMapping(Ia64BspRuntimeMemory& runtimeMemory,
                                                const PhysicalPageRange& page) noexcept
{
    const auto root = runtimeMemory.BorrowReadyRoot();
    if (!root.HasValue())
    {
        return false;
    }
    auto& frame = g_readyRuntimeArena.VirtualOverlapFrame;
    auto& memoryObject = g_readyRuntimeArena.VirtualOverlapMemoryObject;
    auto& preparation = g_readyRuntimeArena.VirtualOverlapPreparation;
    if (frame.Initialize(g_readyResourceDomain.GetIdentifier(),
                         Kernel::Memory::FrameDescriptor{
                             .PhysicalStart = Kernel::Memory::PhysicalAddress{page.PhysicalStart},
                             .Pages = Kernel::Memory::PageCount{1U},
                             .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                             .Kind = Kernel::Memory::FrameKind::Ram,
                             .Reserved = {},
                         }) != Kernel::Memory::FrameError::None)
    {
        return false;
    }
    const auto memoryInitialization = memoryObject.Initialize(
        g_readyRegistry, g_readyResourceDomainIdentity, g_readyResourceDomain,
        Kernel::Memory::MemoryObjectDescriptor{
            .Pages = Kernel::Memory::PageCount{1U},
            .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
            .MaximumPermissions = Kernel::Memory::PagePermissions{Kernel::Memory::PagePermission::Read}.With(
                Kernel::Memory::PagePermission::Write),
            .Commitment = Kernel::Memory::MemoryCommitment::Pinned,
            .Reserved = {},
        });
    const auto frameObject = g_readyIssuer.CreateObject(frame);
    const auto memoryObjectIdentity = g_readyIssuer.CreateObject(memoryObject);
    if ((memoryInitialization.Error != Kernel::Memory::MemoryObjectError::None) || !frameObject.HasValue() ||
        !memoryObjectIdentity.HasValue())
    {
        return false;
    }
    const Kernel::Memory::FrameId frameIdentity{g_readyRegistry, frameObject.GetValue()};
    const Kernel::Memory::MemoryObjectId memoryIdentity{g_readyRegistry, memoryObjectIdentity.GetValue()};
    if (preparation.Prepare(
            g_readyRegistry, memoryIdentity, memoryObject, frameIdentity, frame,
            Kernel::Memory::PinnedFrameMappingRequest{
                .VirtualStart =
                    Kernel::Memory::VirtualAddress{Platform::Ia64::Scheduler::Ia64BspSharedRootShootdownVirtualStart},
                .Pages = Kernel::Memory::PageCount{1U},
                .MemoryObjectOffsetPages = 0U,
                .FrameOffsetPages = 0U,
                .Permissions = Kernel::Memory::PagePermissions{Kernel::Memory::PagePermission::Read}.With(
                    Kernel::Memory::PagePermission::Write),
                .Reserved = {},
            }) != Kernel::Memory::PinnedFrameMappingError::None)
    {
        return false;
    }
    const auto begin =
        root.GetValue().AddressSpaceStorage->BeginPinnedFrameMapping(root.GetValue().AddressSpaceIdentity, preparation);
    if (!begin.HasValue())
    {
        return false;
    }
    TestLocalTlbInvalidator invalidator;
    TlbShootdownDispatcher dispatcher{CpuId{0U}, invalidator};
    ScriptedShootdownTransport transport{ScriptedShootdownTransportMode::RejectPublish};
    const auto dispatched = dispatcher.Dispatch(*root.GetValue().AddressSpaceStorage, begin.GetValue(), transport);
    return dispatched.HasValue() &&
           root.GetValue().AddressSpaceStorage->CompleteMapping(begin.GetValue().GetToken()).HasValue() &&
           (transport.GetPublishCount() == 0U);
}

void TestSharedRootShootdownFailureStateMachine(TestContext& runner) noexcept
{
    Ia64BspKernelRuntime* publishRuntime{nullptr};
    const bool publishFixturePrepared = PrepareSharedRootShootdownFailureFixture(0U, 0x03U, false, publishRuntime);
    if (!publishFixturePrepared || (publishRuntime == nullptr))
    {
        ASTERIA_EXPECT(runner, "publish shootdown fixture preparation succeeds", false);
        return;
    }
    CooperativeShootdownTransport unusedTransport;
    const Foundation::Size preflightObjectCount = g_readyRegistry.GetActiveCount();
    const auto wrongResidentProfile =
        publishRuntime != nullptr
            ? publishRuntime->MapSharedRootShootdownPage(g_sharedRootShootdownFailurePages[0U], 0x03U, unusedTransport)
            : Foundation::Result<
                  Platform::Ia64::Scheduler::Ia64BspSharedRootShootdownSummary,
                  Ia64BspSharedRootShootdownError>::Failure(Ia64BspSharedRootShootdownError::RuntimeNotPrepared);
    const auto publishRoot = g_readyRuntimeArena.ShootdownFailures[0U].RuntimeMemory.BorrowReadyRoot();
    if (!publishRoot.HasValue())
    {
        ASTERIA_EXPECT(runner, "publish shootdown fixture retains its ready root", false);
        return;
    }
    PhysicalPageRange overlappingPhysicalPage = g_sharedRootShootdownFailurePages[0U];
    if (publishRoot.HasValue())
    {
        const auto existing = publishRoot.GetValue().AddressSpaceStorage->TryGetMapping(0U);
        if (existing.HasValue())
        {
            overlappingPhysicalPage.PhysicalStart = existing.GetValue().PhysicalStart.GetValue();
        }
    }
    const auto physicalOverlap =
        publishRuntime != nullptr
            ? publishRuntime->MapSharedRootShootdownPage(overlappingPhysicalPage, 0x01U, unusedTransport)
            : Foundation::Result<
                  Platform::Ia64::Scheduler::Ia64BspSharedRootShootdownSummary,
                  Ia64BspSharedRootShootdownError>::Failure(Ia64BspSharedRootShootdownError::RuntimeNotPrepared);
    ASTERIA_EXPECT(runner, "resident-mask and physical-overlap preflights retain a fresh retryable owner",
                   publishFixturePrepared, !wrongResidentProfile.HasValue(),
                   wrongResidentProfile.GetError() == Ia64BspSharedRootShootdownError::RootRejected,
                   !physicalOverlap.HasValue(),
                   physicalOverlap.GetError() == Ia64BspSharedRootShootdownError::PhysicalStorageAlreadyMapped,
                   publishRuntime->GetSharedRootShootdownState() == Ia64BspSharedRootShootdownState::Fresh,
                   g_readyRegistry.GetActiveCount() == preflightObjectCount, unusedTransport.GetPublishCount() == 0U);

    const bool publishAdmitted = g_readyRuntimeArena.ShootdownFailures[0U]
                                     .RuntimeMemory.AdmitApplicationProcessorPhysicalRoot(CpuId{1U})
                                     .HasValue();
    ScriptedShootdownTransport rejectedPublish{ScriptedShootdownTransportMode::RejectPublish};
    const Foundation::Size publishObjectsBefore = g_readyRegistry.GetActiveCount();
    const auto publishRejected =
        publishRuntime->MapSharedRootShootdownPage(g_sharedRootShootdownFailurePages[0U], 0x03U, rejectedPublish);
    const auto repeatedPublish =
        publishRuntime->MapSharedRootShootdownPage(g_sharedRootShootdownFailurePages[0U], 0x03U, rejectedPublish);
    const auto publishFaultedRoot = publishRoot.GetValue().AddressSpaceStorage->GetSnapshot();
    const auto& publishFailure = publishRuntime->GetSharedRootShootdownFailure();
    ASTERIA_EXPECT(
        runner, "transport rejection uses the dispatcher's single settlement and retains the terminal transaction",
        publishAdmitted, !publishRejected.HasValue(),
        publishRejected.GetError() == Ia64BspSharedRootShootdownError::ShootdownRejected,
        publishFailure.Shootdown.Error == Kernel::Memory::TlbShootdownError::TransportRejected,
        publishFailure.Shootdown.AddressSpaceErrorCode == Kernel::Memory::AddressSpaceError::None,
        publishFailure.Settlement == Kernel::Memory::AddressSpaceError::None,
        publishFaultedRoot.State == Kernel::Memory::AddressSpaceState::Faulted,
        publishRuntime->GetSharedRootShootdownState() == Ia64BspSharedRootShootdownState::Failed,
        publishRuntime->GetSharedRootShootdownSummary().State == Ia64BspSharedRootShootdownState::Failed,
        publishRuntime->GetSharedRootShootdownSummary().ShootdownSequence != 0U,
        RetainsExactSharedRootShootdownPage(*publishRuntime, g_sharedRootShootdownFailurePages[0U]),
        g_readyRegistry.GetActiveCount() == publishObjectsBefore + 2U, rejectedPublish.GetPublishCount() == 1U,
        !repeatedPublish.HasValue(), repeatedPublish.GetError() == Ia64BspSharedRootShootdownError::AlreadyAttempted);

    Ia64BspKernelRuntime* remoteRuntime{nullptr};
    const bool remoteFixturePrepared = PrepareSharedRootShootdownFailureFixture(1U, 0x03U, true, remoteRuntime);
    if (!remoteFixturePrepared || (remoteRuntime == nullptr))
    {
        ASTERIA_EXPECT(runner, "remote shootdown fixture preparation succeeds", false);
        return;
    }
    ScriptedShootdownTransport remoteRejection{ScriptedShootdownTransportMode::RejectRemote};
    const auto remoteRoot = g_readyRuntimeArena.ShootdownFailures[1U].RuntimeMemory.BorrowReadyRoot();
    const auto remoteRejected =
        remoteRuntime->MapSharedRootShootdownPage(g_sharedRootShootdownFailurePages[1U], 0x03U, remoteRejection);
    ASTERIA_EXPECT(runner, "remote rejection is decided by the one authoritative CompleteMapping call",
                   remoteFixturePrepared, !remoteRejected.HasValue(),
                   remoteRejected.GetError() == Ia64BspSharedRootShootdownError::MappingCompletionRejected,
                   remoteRuntime->GetSharedRootShootdownFailure().AddressSpace.Error ==
                       Kernel::Memory::AddressSpaceError::ShootdownFailed,
                   remoteRuntime->GetSharedRootShootdownFailure().Settlement == Kernel::Memory::AddressSpaceError::None,
                   remoteRuntime->GetSharedRootShootdownSummary().State == Ia64BspSharedRootShootdownState::Failed,
                   remoteRuntime->GetSharedRootShootdownSummary().ShootdownSequence != 0U, remoteRoot.HasValue(),
                   remoteRoot.GetValue().AddressSpaceStorage->GetSnapshot().State ==
                       Kernel::Memory::AddressSpaceState::Faulted,
                   remoteRejection.GetPublishCount() == 1U, !remoteRejection.HasRetainedBatch(),
                   RetainsExactSharedRootShootdownPage(*remoteRuntime, g_sharedRootShootdownFailurePages[1U]));

    Ia64BspKernelRuntime* timeoutRuntime{nullptr};
    const bool timeoutFixturePrepared = PrepareSharedRootShootdownFailureFixture(2U, 0x03U, true, timeoutRuntime);
    if (!timeoutFixturePrepared || (timeoutRuntime == nullptr))
    {
        ASTERIA_EXPECT(runner, "timeout shootdown fixture preparation succeeds", false);
        return;
    }
    static constinit ScriptedShootdownTransport timeoutTransport{
        ScriptedShootdownTransportMode::RetainWithoutAcknowledgement};
    const auto timeoutRoot = g_readyRuntimeArena.ShootdownFailures[2U].RuntimeMemory.BorrowReadyRoot();
    // The acknowledgement wait gives up only once both its minimum polls and
    // its minimum interval have passed (Ia64CrossProcessorWaitBudgetTests):
    // ITC standing still would keep it waiting, so here each poll's reading
    // advances ITC just enough for the interval to pass with the polls.
    const Foundation::UInt64 stillIntervalTimeCounter{g_intervalTimeCounter};
    g_intervalTimeCounterStep = (Platform::Ia64::Scheduler::Ia64BspSharedRootShootdownMinimumTicks /
                                 Platform::Ia64::Scheduler::Ia64BspSharedRootShootdownMinimumPolls) +
                                1U;
    const auto child = fork();
    if (child == 0)
    {
        Platform::Synchronization::BindProcessorProgress(
            Kernel::Scheduler::CpuId{0U},
            Platform::Synchronization::ProcessorProgressHooks{
                nullptr, nullptr, nullptr, nullptr,
                +[](const Kernel::Diagnostics::StopCode code, const Foundation::UInt64 mask) noexcept
                {
                    _exit(code == Kernel::Diagnostics::StopCode::ShootdownTimeout && mask == 2U ? 0 : 1);
                }});
        static_cast<void>(
            timeoutRuntime->MapSharedRootShootdownPage(g_sharedRootShootdownFailurePages[2U], 0x03U, timeoutTransport));
        _exit(2);
    }
    int status{0};
    const bool stopped =
        child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    g_intervalTimeCounterStep = 0U;
    g_intervalTimeCounter = stillIntervalTimeCounter;
    ASTERIA_EXPECT(runner, "a silent online processor stops with ShootdownTimeout and its mask", stopped,
                   timeoutRoot.HasValue());

    Ia64BspKernelRuntime* virtualRuntime{nullptr};
    const bool virtualFixturePrepared = PrepareSharedRootShootdownFailureFixture(3U, 0x01U, false, virtualRuntime);
    if (!virtualFixturePrepared || (virtualRuntime == nullptr))
    {
        ASTERIA_EXPECT(runner, "virtual shootdown fixture preparation succeeds", false);
        return;
    }
    const bool virtualMappingInstalled = InstallVirtualOverlapMapping(
        g_readyRuntimeArena.ShootdownFailures[3U].RuntimeMemory, g_sharedRootShootdownFailurePages[4U]);
    CooperativeShootdownTransport virtualTransport;
    const Foundation::Size virtualObjectCount = g_readyRegistry.GetActiveCount();
    const auto virtualOverlap =
        virtualRuntime->MapSharedRootShootdownPage(g_sharedRootShootdownFailurePages[3U], 0x01U, virtualTransport);
    ASTERIA_EXPECT(runner, "fixed-VA overlap is rejected before accepting the one-shot mapping attempt",
                   virtualFixturePrepared, virtualMappingInstalled, !virtualOverlap.HasValue(),
                   virtualOverlap.GetError() == Ia64BspSharedRootShootdownError::VirtualRangeOccupied,
                   virtualRuntime->GetSharedRootShootdownState() == Ia64BspSharedRootShootdownState::Fresh,
                   virtualRuntime->GetSharedRootShootdownSummary().State == Ia64BspSharedRootShootdownState::Fresh,
                   g_readyRegistry.GetActiveCount() == virtualObjectCount, virtualTransport.GetPublishCount() == 0U);
}

} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64BspKernelRuntimeTests"};
    Asteria::Test::TestNativePreemptionAdmissionMargin(runner);
    Asteria::Test::TestTimerQueueNotificationSuccessModel(runner);
    Asteria::Test::TestUserProbeWaitDeadlinePolicy(runner);
    Asteria::Test::TestUnreadyRootFailsBeforeRunnableMutation(runner);
    Asteria::Test::TestUserProbePreparationAndHostedLaunchFailure(runner);
    Asteria::Test::TestSharedRootShootdownFailureStateMachine(runner);
    return runner.Finish();
}
