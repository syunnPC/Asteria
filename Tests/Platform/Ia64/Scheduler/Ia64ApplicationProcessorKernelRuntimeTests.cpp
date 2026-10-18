// SPDX-License-Identifier: Apache-2.0
#include "Platform/Ia64/Scheduler/Ia64ApplicationProcessorKernelRuntime.hpp"

#include <atomic>
#include <cstdio>
#include <new>
#include <thread>

#include "Boot/BootInfo.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Memory/Frame.hpp"
#include "Kernel/Memory/MemoryObject.hpp"
#include "Kernel/Memory/PinnedFrameMapping.hpp"
#include "Kernel/Memory/TlbShootdownDrain.hpp"
#include "Kernel/Memory/TlbShootdownDispatcher.hpp"
#include "Platform/Ia64/Firmware/Ia64LoadedImageLayout.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"
#include "Platform/Ia64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/Ia64HostedBootGeometry.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
Foundation::UInt64 g_timerTicks[4U]{};
Foundation::Size g_timerTickCount{0U};
Foundation::Size g_timerTickIndex{0U};
Foundation::UInt64 g_timerInitializeStatus{0U};
Foundation::UInt64 g_timerProgramStatus{0U};
// Calls after this many return success instead of g_timerProgramStatus.
Foundation::Size g_timerProgramStatusCalls{~Foundation::Size{0U}};
Foundation::UInt64 g_timerCancelStatus{0U};
Foundation::UInt64 g_taskPriorityStatus{0U};
Foundation::UInt64 g_programmedDeadline{0U};
Foundation::UInt64 g_programmedVector{0U};
Foundation::Size g_timerInitializeCalls{0U};
Foundation::Size g_timerProgramCalls{0U};
Foundation::Size g_timerCancelCalls{0U};
Foundation::Size g_taskPriorityCalls{0U};
Foundation::Size g_runtimeInterruptTableInstallCalls{0U};
Foundation::Size g_runtimeRegionActivationCalls{0U};
Foundation::Size g_tlbPurgeCalls{0U};
Foundation::Size g_tlbSerializationCalls{0U};
Foundation::UInt64 g_lastTlbPurgeAddress{0U};
std::atomic<Foundation::Size> g_switchCount{0U};
std::atomic<bool> g_workerSwitchEnabled{true};
std::atomic<bool> g_holdWorkerResume{false};
std::atomic<bool> g_workerResumeObserved{false};

struct HostedWorkerSwitchSlot
{
    std::atomic<const Platform::Ia64::Scheduler::Ia64KernelRegisterState*> Registers{nullptr};
    std::atomic<Foundation::UInt64> ResumeSequence{0U};
    std::atomic<Foundation::UInt64> YieldSequence{0U};
    std::atomic<bool> Returned{false};
};

thread_local HostedWorkerSwitchSlot* t_hostedWorkerSwitchSlot{nullptr};

constexpr Foundation::Size MaximumHostedWorkerSwitchSlots{20U};

void RunHostedWorkerSwitchSlot(HostedWorkerSwitchSlot* slot) noexcept;

class HostedSwitchAdapter final
{
public:
    [[nodiscard]] HostedWorkerSwitchSlot*
    GetOrStart(const Platform::Ia64::Scheduler::Ia64KernelRegisterState& registers) noexcept
    {
        for (HostedWorkerSwitchSlot& slot : m_slots)
        {
            const auto* registered = slot.Registers.load(std::memory_order_acquire);
            if (registered == &registers)
            {
                return &slot;
            }
            if (registered != nullptr)
            {
                continue;
            }

            const Platform::Ia64::Scheduler::Ia64KernelRegisterState* expected{nullptr};
            if (slot.Registers.compare_exchange_strong(expected, &registers, std::memory_order_acq_rel,
                                                       std::memory_order_acquire))
            {
                // A slot is bound for the complete test process. It is never
                // cleared or reused, so the monotonic resume generations
                // cannot acquire an ABA interpretation.
                std::thread{RunHostedWorkerSwitchSlot, &slot}.detach();
                return &slot;
            }
            if (expected == &registers)
            {
                return &slot;
            }
        }
        return nullptr;
    }

private:
    HostedWorkerSwitchSlot m_slots[MaximumHostedWorkerSwitchSlots]{};
};

// Placement construction deliberately has no matching destructor. The raw
// storage and every bound slot remain valid while detached suspended workers
// exist, including during ordinary static-object teardown.
alignas(HostedSwitchAdapter) Foundation::UInt8 g_hostedSwitchAdapterStorage[sizeof(HostedSwitchAdapter)]{};
HostedSwitchAdapter* const g_hostedSwitchAdapter{::new (static_cast<void*>(g_hostedSwitchAdapterStorage))
                                                     HostedSwitchAdapter{}};

void RunHostedWorkerSwitchSlot(HostedWorkerSwitchSlot* const slot) noexcept
{
    while (slot->ResumeSequence.load(std::memory_order_acquire) == 0U)
    {
        slot->ResumeSequence.wait(0U, std::memory_order_acquire);
    }

    t_hostedWorkerSwitchSlot = slot;
    const auto* const registers = slot->Registers.load(std::memory_order_acquire);
    if (registers == nullptr)
    {
        slot->Returned.store(true, std::memory_order_release);
        return;
    }
    using WorkerEntry = void (*)(void*) noexcept;
    const auto entry = reinterpret_cast<WorkerEntry>(static_cast<Foundation::UIntPointer>(registers->R4));
    auto* const argument = reinterpret_cast<void*>(static_cast<Foundation::UIntPointer>(registers->R6));
    entry(argument);

    slot->Returned.store(true, std::memory_order_release);
}
} // Namespace Asteria::Test

extern "C" void
AsteriaIa64SwitchKernelContext(Asteria::Platform::Ia64::Scheduler::Ia64KernelRegisterState*,
                               const Asteria::Platform::Ia64::Scheduler::Ia64KernelRegisterState*) noexcept;
extern "C" Asteria::Foundation::UInt64 AsteriaIa64ReadIntervalTimeCounter() noexcept;
extern "C" Asteria::Foundation::UInt64 AsteriaIa64InitializeLocalTimer(Asteria::Foundation::UInt64) noexcept;
extern "C" Asteria::Foundation::UInt64
    AsteriaIa64ProgramLocalTimer(Asteria::Foundation::UInt64, Asteria::Foundation::UInt64, Asteria::Foundation::UInt64,
                                 Asteria::Foundation::UInt64, Asteria::Foundation::UInt64) noexcept;
extern "C" Asteria::Foundation::UInt64 AsteriaIa64CancelLocalTimer(Asteria::Foundation::UInt64) noexcept;
extern "C" Asteria::Foundation::UInt64 AsteriaIa64InstallApplicationProcessorLocalTimerTaskPriority() noexcept;
extern "C" Asteria::Foundation::UInt64
    AsteriaIa64InstallRuntimeInterruptVectorTable(Asteria::Foundation::UInt64, Asteria::Foundation::UInt64) noexcept;
extern "C" [[noreturn]] void AsteriaIa64FailStopRuntimeInterruptVectorTableInstall() noexcept;
extern "C" Asteria::Foundation::UInt64 AsteriaIa64ActivateRuntimeRegions(
    Asteria::Foundation::UInt64, Asteria::Foundation::UInt64, Asteria::Foundation::UInt64, Asteria::Foundation::UInt64,
    Asteria::Foundation::UInt64, Asteria::Foundation::UInt64, Asteria::Foundation::UInt64) noexcept;
extern "C" void AsteriaIa64PurgeLocalTranslation(Asteria::Foundation::UInt64, Asteria::Foundation::UInt64) noexcept;
extern "C" void AsteriaIa64SerializeLocalTranslationPurge() noexcept;

extern "C" void
AsteriaIa64SwitchKernelContext(Asteria::Platform::Ia64::Scheduler::Ia64KernelRegisterState*,
                               const Asteria::Platform::Ia64::Scheduler::Ia64KernelRegisterState* const next) noexcept
{
    Asteria::Test::g_switchCount.fetch_add(1U, std::memory_order_relaxed);
    Asteria::Test::HostedWorkerSwitchSlot* const active = Asteria::Test::t_hostedWorkerSwitchSlot;
    if (active != nullptr)
    {
        const Asteria::Foundation::UInt64 yieldedSequence = active->ResumeSequence.load(std::memory_order_acquire);
        active->YieldSequence.store(yieldedSequence, std::memory_order_release);
        while (active->ResumeSequence.load(std::memory_order_acquire) == yieldedSequence)
        {
            active->ResumeSequence.wait(yieldedSequence, std::memory_order_acquire);
        }
        return;
    }
    if (!Asteria::Test::g_workerSwitchEnabled.load(std::memory_order_acquire) || (next == nullptr) ||
        (next->R4 == 0U) || (next->R6 == 0U))
    {
        return;
    }

    Asteria::Test::HostedWorkerSwitchSlot* const slot = Asteria::Test::g_hostedSwitchAdapter->GetOrStart(*next);
    if (slot == nullptr)
    {
        return;
    }
    if (Asteria::Test::g_holdWorkerResume.load(std::memory_order_acquire))
    {
        Asteria::Test::g_workerResumeObserved.store(true, std::memory_order_release);
        while (Asteria::Test::g_holdWorkerResume.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
    }
    Asteria::Foundation::UInt64 priorResumeSequence = slot->ResumeSequence.load(std::memory_order_acquire);
    while (priorResumeSequence != ~Asteria::Foundation::UInt64{0U})
    {
        if (slot->ResumeSequence.compare_exchange_weak(priorResumeSequence, priorResumeSequence + 1U,
                                                       std::memory_order_acq_rel, std::memory_order_acquire))
        {
            break;
        }
    }
    if (priorResumeSequence == ~Asteria::Foundation::UInt64{0U})
    {
        return;
    }
    slot->ResumeSequence.notify_all();
    const Asteria::Foundation::UInt64 resumeSequence = priorResumeSequence + 1U;
    while ((slot->YieldSequence.load(std::memory_order_acquire) < resumeSequence) &&
           !slot->Returned.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
}

extern "C" Asteria::Foundation::UInt64 AsteriaIa64ReadIntervalTimeCounter() noexcept
{
    if (Asteria::Test::g_timerTickCount == 0U)
    {
        return 0U;
    }
    const Asteria::Foundation::Size index = Asteria::Test::g_timerTickIndex < Asteria::Test::g_timerTickCount
                                                ? Asteria::Test::g_timerTickIndex
                                                : Asteria::Test::g_timerTickCount - 1U;
    ++Asteria::Test::g_timerTickIndex;
    return Asteria::Test::g_timerTicks[index];
}

extern "C" Asteria::Foundation::UInt64 AsteriaIa64InitializeLocalTimer(const Asteria::Foundation::UInt64) noexcept
{
    ++Asteria::Test::g_timerInitializeCalls;
    return Asteria::Test::g_timerInitializeStatus;
}

extern "C" Asteria::Foundation::UInt64 AsteriaIa64ProgramLocalTimer(const Asteria::Foundation::UInt64 deadlineTicks,
                                                                    const Asteria::Foundation::UInt64 interruptVector,
                                                                    const Asteria::Foundation::UInt64,
                                                                    const Asteria::Foundation::UInt64,
                                                                    const Asteria::Foundation::UInt64) noexcept
{
    ++Asteria::Test::g_timerProgramCalls;
    Asteria::Test::g_programmedDeadline = deadlineTicks;
    Asteria::Test::g_programmedVector = interruptVector;
    return (Asteria::Test::g_timerProgramCalls <= Asteria::Test::g_timerProgramStatusCalls)
               ? Asteria::Test::g_timerProgramStatus
               : 0U;
}

extern "C" Asteria::Foundation::UInt64 AsteriaIa64CancelLocalTimer(const Asteria::Foundation::UInt64) noexcept
{
    ++Asteria::Test::g_timerCancelCalls;
    return Asteria::Test::g_timerCancelStatus;
}

extern "C" Asteria::Foundation::UInt64 AsteriaIa64InstallApplicationProcessorLocalTimerTaskPriority() noexcept
{
    ++Asteria::Test::g_taskPriorityCalls;
    return Asteria::Test::g_taskPriorityStatus;
}

extern "C" Asteria::Foundation::UInt64
AsteriaIa64InstallRuntimeInterruptVectorTable(const Asteria::Foundation::UInt64,
                                              const Asteria::Foundation::UInt64) noexcept
{
    ++Asteria::Test::g_runtimeInterruptTableInstallCalls;
    return 0U;
}

extern "C" [[noreturn]] void AsteriaIa64FailStopRuntimeInterruptVectorTableInstall() noexcept
{
    __builtin_trap();
}

extern "C" Asteria::Foundation::UInt64
AsteriaIa64ActivateRuntimeRegions(const Asteria::Foundation::UInt64, const Asteria::Foundation::UInt64,
                                  const Asteria::Foundation::UInt64, const Asteria::Foundation::UInt64,
                                  const Asteria::Foundation::UInt64, const Asteria::Foundation::UInt64,
                                  const Asteria::Foundation::UInt64) noexcept
{
    ++Asteria::Test::g_runtimeRegionActivationCalls;
    return 0U;
}

extern "C" void AsteriaIa64PurgeLocalTranslation(const Asteria::Foundation::UInt64 virtualAddress,
                                                 const Asteria::Foundation::UInt64) noexcept
{
    ++Asteria::Test::g_tlbPurgeCalls;
    Asteria::Test::g_lastTlbPurgeAddress = virtualAddress;
}

extern "C" void AsteriaIa64SerializeLocalTranslationPurge() noexcept
{
    ++Asteria::Test::g_tlbSerializationCalls;
}

namespace Asteria::Test
{
namespace
{
using Coordinator = Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeReleaseCoordinator;
using CoordinatorInitializationError = Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeInitializationError;
using CoordinatorState = Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeReleaseCoordinatorState;
using CoordinatorTransitionError = Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeTransitionError;
using KernelContextState = Platform::Ia64::Scheduler::Ia64KernelContextState;
using Layout = Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeProcessorLayout;
using LayoutPlan = Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeLayoutPlan;
using LayoutPlanner = Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner;
using Runtime = Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntime;
using RuntimeAccessError = Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeAccessError;
using RuntimeError = Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeError;
using RuntimeEvidence = Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeRegisterEvidence;
using RuntimeFailure = Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeFailure;
using RuntimeFailurePoll = Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeFailurePoll;
using RuntimeOperation = Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeOperation;
using RuntimeProofPhase = Platform::Ia64::Scheduler::Ia64ApplicationProcessorLocalTimerProofPhase;
using RuntimePortablePhase = Platform::Ia64::Scheduler::Ia64ApplicationProcessorPortableSchedulerPhase;
using RuntimePortableFailure = Platform::Ia64::Scheduler::Ia64ApplicationProcessorPortableSchedulerFailure;
using RuntimePortableRecord = Platform::Ia64::Scheduler::Ia64ApplicationProcessorPortableSchedulerRecord;
using RuntimeServiceResult = Platform::Ia64::Scheduler::Ia64ApplicationProcessorPortableSchedulerServiceResult;
using RuntimeServiceState = Platform::Ia64::Scheduler::Ia64ApplicationProcessorPortableSchedulerServiceState;
using RuntimeTimerDeliveryResult = Platform::Ia64::Scheduler::Ia64ApplicationProcessorTimerDeliveryResult;
using RuntimeTimerDeliveryState = Platform::Ia64::Scheduler::Ia64ApplicationProcessorTimerDeliveryState;
using RuntimePhysicalFailure = Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimePhysicalFailure;
using RuntimeProcessorSnapshot = Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeProcessorSnapshot;
using RuntimeProcessorState = Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeProcessorState;
using RuntimeState = Platform::Ia64::Scheduler::Ia64ApplicationProcessorKernelRuntimeState;
using RuntimeTimerDeliveryRecord = Platform::Ia64::Scheduler::Ia64ApplicationProcessorTimerDeliveryRecord;
using Platform::Ia64::Scheduler::Detail::ConsumeTimerDeliveryNotification;
using Platform::Ia64::Scheduler::Detail::FixedRootControllerMatches;
using Platform::Ia64::Scheduler::Detail::TimerDeliveryPublicationIsCoherent;
using Platform::Ia64::Scheduler::Detail::TryAdvancePortableSchedulerServiceSequence;
using Platform::Ia64::Scheduler::Detail::TryComputeTimerDeliveryDeadlines;
using Platform::Ia64::Scheduler::Detail::ValidateTlbShootdownDrainSummary;
using Kernel::Memory::AddressSpaceError;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::BootFrame;
using Kernel::Memory::Frame;
using Kernel::Memory::FrameDescriptor;
using Kernel::Memory::FrameError;
using Kernel::Memory::FrameId;
using Kernel::Memory::FrameKind;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::MemoryCommitment;
using Kernel::Memory::MemoryObject;
using Kernel::Memory::MemoryObjectDescriptor;
using Kernel::Memory::MemoryObjectError;
using Kernel::Memory::MemoryObjectId;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Memory::PinnedFrameMappingError;
using Kernel::Memory::PinnedFrameMappingPreparation;
using Kernel::Memory::PinnedFrameMappingRequest;
using Kernel::Memory::TlbShootdownDrain;
using Kernel::Memory::TlbShootdownDrainSummary;
using Kernel::Memory::TlbShootdownDispatcher;
using Kernel::Memory::TlbShootdownError;
using Kernel::Memory::TlbShootdownMailboxTransport;
using Kernel::Memory::TlbShootdownReceiveError;
using Kernel::Memory::VirtualAddress;
using InterruptEvidence = Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidence;
using PollRange = Platform::Ia64::Interrupt::Ia64ExternalInterruptPollCodeRange;
using PollStatus = Platform::Ia64::Interrupt::Ia64ExternalInterruptPollStatus;
using ValidationError = Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidenceValidationError;
using TakeoverLifecycle = Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverLifecycle;
using TakeoverSnapshot = Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverProgressSnapshot;
using Boot::BootInfo;
using Boot::PhysicalMemoryKind;
using Boot::PhysicalMemoryRegion;
using Kernel::Memory::PhysicalPageRange;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
using Platform::Ia64::Firmware::Ia64LoadedImageLayout;
using Platform::Ia64::Firmware::Ia64LoadedImageSection;
using Platform::Ia64::Firmware::Ia64LoadedImageSectionPurpose;
using Platform::Ia64::Firmware::SalSystemTableInformation;
using Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask;
using Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask;
using Platform::Ia64::Memory::BootstrapReservedPageCount;
using Platform::Ia64::Memory::Ia64BspRuntimeMemory;
using Platform::Ia64::Memory::Ia64LocalTlbInvalidator;
using Platform::Ia64::Memory::Ia64LocalTlbInvalidatorInitializationError;
using Platform::Ia64::Memory::Ia64PolledTlbShootdownNotifier;
using Platform::Ia64::Memory::Ia64PolledTlbShootdownPollResult;
using Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTableSizeBytes;
using Platform::Ia64::Memory::InitialAddressSpacePreparation;
using Platform::Ia64::Memory::InitialInterruptVectorTableSizeBytes;
using Platform::Ia64::Memory::InitialRseBackingStoreSizeBytes;
using Platform::Ia64::Memory::InitialTranslationRegionSet;
using Platform::Ia64::Memory::PalVirtualMemoryCapabilities;
using Platform::Ia64::Memory::RegionConfiguration;
using Platform::Ia64::Memory::TranslationCapabilities;
using Platform::Ia64::Memory::WritableInterruptVectorTableView;
using Platform::Ia64::Memory::WritableRseBackingStoreView;
using Platform::Synchronization::SpinLock;

constexpr Foundation::UInt8 PhysicalAddressBits{50U};
constexpr Foundation::UInt8 VirtualAddressMostSignificantBit{53U};
constexpr Foundation::UInt64 IdlePhysicalBase{0x0020'0000U};
constexpr Foundation::UInt64 WorkerPhysicalBase{0x0040'0000U};
constexpr Foundation::UInt64 PageBytes{4096U};
constexpr Foundation::UInt64 PortableReservedBase{0x0100'0000U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 PortableResourceDomainValue{0xA640U};
constexpr Foundation::UInt64 TranslationStateMask{(Foundation::UInt64{1U} << 36U) | (Foundation::UInt64{1U} << 27U) |
                                                  (Foundation::UInt64{1U} << 17U)};
constexpr Foundation::UInt64 InstructionCollectionMask{Foundation::UInt64{1U} << 13U};

enum class EvidencePlacement : Foundation::UInt8
{
    Idle,
    Worker,
};

struct PortableRuntimeArena
{
    Ia64BspRuntimeMemory RuntimeMemory;
};

// The arena is the run-time memory reservation: it fills its own architected
// page, so one DTR maps it wherever the host loads the test and no other host
// object shares that page (Ia64HostedBootGeometry).
constinit TestSupport::Ia64HostedRuntimeMemoryArena<PortableRuntimeArena> g_portableRuntimeArenaStorage{};
PortableRuntimeArena& g_portableRuntimeArena = g_portableRuntimeArenaStorage.Construct();
constinit SpinLock g_portableRegistryLock;
Asteria::TestSupport::TestObjectRegistry g_portableRegistry{g_portableRegistryLock};
constinit RootCapabilityIssuer g_portableIssuer{g_portableRegistry};
constinit ResourceDomainLedger g_portableResourceLedger;
constinit ResourceDomain g_portableResourceDomain{g_portableResourceLedger};
constinit InitialAddressSpacePreparation g_portableInitialPreparation;
constinit BootFrame g_tlbShootdownFrame;
constinit MemoryObject g_tlbShootdownMemoryObject;
alignas(32U * 1024U) constinit Foundation::UInt8
    g_portableInitialInterruptVectorTable[InitialInterruptVectorTableSizeBytes]{};
alignas(PageBytes) constinit Foundation::UInt8 g_portableInitialRseBackingStore[InitialRseBackingStoreSizeBytes]{};
ObjectId g_portableResourceObject{};
ResourceDomainObjectId g_portableResourceIdentity{};
ObjectId g_tlbShootdownFrameObject{};
ObjectId g_tlbShootdownMemoryObjectId{};

static_assert(alignof(PortableRuntimeArena) == PageBytes);
static_assert((sizeof(PortableRuntimeArena) & (PageBytes - 1U)) == 0U);

[[nodiscard]] constexpr Foundation::UInt8 ExpectedMask(const Foundation::UInt8 processorCount) noexcept
{
    return static_cast<Foundation::UInt8>((Foundation::UInt32{1U} << processorCount) - 1U);
}

[[nodiscard]] Foundation::UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
}

[[nodiscard]] constexpr PalVirtualMemoryCapabilities PortableCapabilities() noexcept
{
    return PalVirtualMemoryCapabilities{
        .Translation =
            TranslationCapabilities{
                .InsertablePageSizeMask = ArchitectedMinimumInsertablePageSizeMask,
                .PhysicalAddressBits = 50U,
                .ImplementedVirtualAddressMostSignificantBit = 60U,
                .RegionIdentifierBits = 24U,
                .ProtectionKeyBits = 24U,
                .MemoryAttributeMask = 0x71U,
                .HardwareWalkerImplemented = true,
                .Reserved16 = 0U,
                .Reserved64 = 0U,
            },
        .PurgePageSizeMask = ArchitectedMinimumPurgePageSizeMask,
        .InstructionTranslationRegisterCount = 32U,
        .DataTranslationRegisterCount = 32U,
        .ProtectionKeyRegisterCount = 16U,
        .MaximumConcurrentPurgesEncoding = 1U,
        .HashTagIdentifier = 0U,
        .UniqueTranslationCacheCount = 1U,
        .TranslationCacheLevelCount = 1U,
        .Reserved8 = 0U,
        .Reserved32 = 0U,
    };
}

[[nodiscard]] constexpr SalSystemTableInformation PortableFirmware() noexcept
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

[[nodiscard]] constexpr InitialTranslationRegionSet PortableRegions() noexcept
{
    InitialTranslationRegionSet regions{};
    for (Foundation::Size index{0U}; index < Platform::Ia64::Memory::InitialTranslationRegionCount; ++index)
    {
        regions.Regions[index] = RegionConfiguration{
            .RegionIdentifier = static_cast<Foundation::UInt32>(0x2000U + index),
            .ProtectionKey = 0U,
            .VhptWalkerEnabled = false,
            .SharedAcrossAddressSpaces = index == 0U,
            .Reserved16 = 0U,
            .Reserved32 = 0U,
        };
    }
    return regions;
}

[[nodiscard]] constexpr ResourceDomainLimits PortableResourceLimits() noexcept
{
    ResourceDomainLimits limits{
        .CommittedMemoryPages = 4096U,
        .ResidentMemoryPages = 4096U,
        .KernelMetadataBytes = 64U * 1024U,
        .CapabilityHandles = 4096U,
        .IpcQueueBytes = 64U * 1024U,
        .Threads = 256U,
        .ChildDomains = 16U,
    };
    // These hosted runtimes retain each suspended scheduler incarnation for
    // the entire test process. They do not allocate through native pools.
    limits.Objects.Counts[static_cast<Foundation::Size>(Abi::LimitedObject::ProtectionDomain)] = 256U;
    return limits;
}

[[nodiscard]] bool PreparePortableEnvironment() noexcept
{
    const PhysicalPageRange reserved{
        .PhysicalStart = PortableReservedBase,
        .PageCount = BootstrapReservedPageCount,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Reserved = 0U,
    };
    TestSupport::Ia64HostedBootGeometry geometry{};
    if (!TestSupport::MakeIa64HostedBootGeometry(
            AddressOf(&Platform::Ia64::Memory::AsteriaIa64RuntimeInterruptVectorTableImage),
            AddressOf(&g_portableRuntimeArena), sizeof(g_portableRuntimeArena), reserved, geometry))
    {
        return false;
    }
    BootInfo bootInfo{
        .Signature = Boot::BootInfoSignature,
        .Version = Boot::BootInfoVersion,
        .SizeBytes = sizeof(BootInfo),
        .MemoryRegionCount = 3U,
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
        .PhysicalStart = PortableReservedBase,
        .VirtualStart = 0U,
        .PageCount = BootstrapReservedPageCount,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Kind = PhysicalMemoryKind::Conventional,
    };
    bootInfo.MemoryRegions[1U] = geometry.ImageRegion;
    bootInfo.MemoryRegions[2U] = geometry.RuntimeMemoryRegion;
    if (!TestSupport::SortIa64HostedBootRegions(bootInfo))
    {
        return false;
    }
    if (!g_portableInitialPreparation
             .Prepare(geometry.Image, bootInfo, reserved, geometry.RuntimeMemoryPages, PortableCapabilities(),
                      PortableFirmware(), PortableRegions(),
                      WritableInterruptVectorTableView{
                          .Data = g_portableInitialInterruptVectorTable,
                          .SizeBytes = InitialInterruptVectorTableSizeBytes,
                          .PhysicalStart = PortableReservedBase + 0x1'0000U,
                      },
                      WritableRseBackingStoreView{
                          .Data = g_portableInitialRseBackingStore,
                          .SizeBytes = InitialRseBackingStoreSizeBytes,
                          .PhysicalStart = PortableReservedBase + 0x1'8000U,
                      })
             .HasValue())
    {
        return false;
    }
    // The hosted geometry's plan is the same at every host layout.
    const auto summary = g_portableInitialPreparation.TryGetSummary();
    if (!summary.HasValue() ||
        (summary.GetValue()->Translation.DataRegisterCount != TestSupport::Ia64HostedDataTranslationRegisters) ||
        (summary.GetValue()->Translation.InstructionRegisterCount !=
         TestSupport::Ia64HostedInstructionTranslationRegisters))
    {
        return false;
    }
    if (g_portableResourceDomain.InitializeRoot(ResourceDomainId{PortableResourceDomainValue}, PortableResourceLimits())
            .Error != ResourceDomainError::None)
    {
        return false;
    }
    const auto resource = g_portableIssuer.CreateObject(g_portableResourceDomain);
    if (!resource.HasValue())
    {
        return false;
    }
    g_portableResourceObject = resource.GetValue();
    g_portableResourceIdentity = ResourceDomainObjectId{g_portableRegistry, g_portableResourceObject};

    if ((g_tlbShootdownFrame.Initialize(g_portableResourceDomain.GetIdentifier(),
                                        FrameDescriptor{
                                            .PhysicalStart = Kernel::Memory::PhysicalAddress{0x0800'0000U},
                                            .Pages = PageCount{2U},
                                            .CachePolicy = MemoryCachePolicy::WriteBack,
                                            .Kind = FrameKind::Ram,
                                            .Reserved = {},
                                        }) != FrameError::None) ||
        (g_tlbShootdownMemoryObject
             .Initialize(g_portableRegistry, g_portableResourceIdentity, g_portableResourceDomain,
                         MemoryObjectDescriptor{
                             .Pages = PageCount{2U},
                             .CachePolicy = MemoryCachePolicy::WriteBack,
                             .MaximumPermissions = PagePermissions{PagePermission::Read}.With(PagePermission::Write),
                             .Commitment = MemoryCommitment::Pinned,
                             .Reserved = {},
                         })
             .Error != MemoryObjectError::None))
    {
        return false;
    }
    const auto frameObject = g_portableIssuer.CreateObject(g_tlbShootdownFrame);
    const auto memoryObject = g_portableIssuer.CreateObject(g_tlbShootdownMemoryObject);
    if (!frameObject.HasValue() || !memoryObject.HasValue())
    {
        return false;
    }
    g_tlbShootdownFrameObject = frameObject.GetValue();
    g_tlbShootdownMemoryObjectId = memoryObject.GetValue();

    PalVirtualMemoryCapabilities capabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    for (Foundation::Size index{0U}; index < Kernel::Scheduler::MaximumProcessorCount; ++index)
    {
        capabilities[index] = PortableCapabilities();
    }
    const auto prepared = g_portableRuntimeArena.RuntimeMemory.PrepareAndAdmit(
        Kernel::Scheduler::CpuId{0U}, 0x07U, capabilities, g_portableInitialPreparation,
        g_portableResourceDomain.GetIdentifier(), g_portableRegistry, g_portableIssuer);
    return prepared.HasValue() &&
           g_portableRuntimeArena.RuntimeMemory.AdmitApplicationProcessorPhysicalRoot(Kernel::Scheduler::CpuId{1U})
               .HasValue();
}

[[nodiscard]] bool CompletePortableEnvironmentAdmission() noexcept
{
    return g_portableRuntimeArena.RuntimeMemory.AdmitApplicationProcessorPhysicalRoot(Kernel::Scheduler::CpuId{2U})
        .HasValue();
}

[[nodiscard]] LayoutPlan MakePlan(const Foundation::UInt8 processorCount) noexcept
{
    LayoutPlan plan{};
    plan.ProcessorCount = processorCount;
    plan.EnabledProcessorMask = ExpectedMask(processorCount);
    plan.LayoutCount = static_cast<Foundation::UInt8>(processorCount - 1U);
    for (Foundation::UInt8 processor{1U}; processor < processorCount; ++processor)
    {
        const Foundation::Size index = static_cast<Foundation::Size>(processor - 1U);
        plan.Processors[index] = Layout{
            .SlotVirtualBase = Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeRegionOneBase +
                               (static_cast<Foundation::UInt64>(index) *
                                Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeProcessorStride),
            .IdlePhysicalBase =
                IdlePhysicalBase + (static_cast<Foundation::UInt64>(index) *
                                    Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeIdleAliasSizeBytes),
            .WorkerPhysicalBase =
                WorkerPhysicalBase + (static_cast<Foundation::UInt64>(index) *
                                      Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeWorkerAliasSizeBytes),
            .Processor = Kernel::Scheduler::CpuId{processor},
            .Reserved = {},
        };
    }
    return plan;
}

[[nodiscard]] TakeoverSnapshot MakeTakeoverSnapshot(const Foundation::UInt8 processorCount) noexcept
{
    TakeoverSnapshot snapshot{};
    snapshot.ProcessorCount = processorCount;
    snapshot.EnabledProcessorMask = ExpectedMask(processorCount);
    snapshot.TerminalMask = static_cast<Foundation::UInt8>(snapshot.EnabledProcessorMask &
                                                           static_cast<Foundation::UInt8>(~Foundation::UInt8{1U}));
    snapshot.State = Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverCoordinatorState::Ready;
    for (Foundation::UInt8 processor{1U}; processor < processorCount; ++processor)
    {
        snapshot.Processors[processor] = TakeoverLifecycle::InitialTranslationsInstalled;
    }
    return snapshot;
}

class RuntimeFixture final
{
public:
    [[nodiscard]] bool Configure(const Foundation::UInt8 processorCount) noexcept
    {
        m_plan = MakePlan(processorCount);
        for (Foundation::Size index{0U}; index < Kernel::Scheduler::MaximumProcessorCount; ++index)
        {
            m_processorCapabilities[index] = PortableCapabilities();
        }
        if (m_notifier.Initialize(m_plan.EnabledProcessorMask) !=
            Platform::Ia64::Memory::Ia64PolledTlbShootdownNotifierInitializationError::None)
        {
            return false;
        }
        m_mailbox = ::new (static_cast<void*>(m_mailboxStorage))
            TlbShootdownMailboxTransport{m_notifier, m_plan.EnabledProcessorMask};
        const TakeoverSnapshot takeover = MakeTakeoverSnapshot(processorCount);
        return m_coordinator.Initialize(takeover, m_plan.EnabledProcessorMask) == CoordinatorInitializationError::None;
    }

    [[nodiscard]] bool Prepare() noexcept
    {
        return m_runtime.Prepare(m_plan, PhysicalAddressBits, VirtualAddressMostSignificantBit, m_coordinator)
            .HasValue();
    }

    [[nodiscard]] Runtime& GetRuntime() noexcept
    {
        return m_runtime;
    }

    [[nodiscard]] Coordinator& GetCoordinator() noexcept
    {
        return m_coordinator;
    }

    [[nodiscard]] LayoutPlan& GetPlan() noexcept
    {
        return m_plan;
    }

    [[nodiscard]] Ia64PolledTlbShootdownNotifier& GetNotifier() noexcept
    {
        return m_notifier;
    }

    /// Recreates the retained notifier in place as a fresh, unready object.
    /// The runtime and mailbox keep their references to this exact storage,
    /// which models a boot-owned notifier that loses its configuration after
    /// the runtime retained it. No shootdown may be in flight.
    void ResetNotifier() noexcept
    {
        m_notifier.~Ia64PolledTlbShootdownNotifier();
        ::new (static_cast<void*>(&m_notifier)) Ia64PolledTlbShootdownNotifier{};
    }

    [[nodiscard]] TlbShootdownMailboxTransport& GetMailbox() noexcept
    {
        return *m_mailbox;
    }

    [[nodiscard]] PalVirtualMemoryCapabilities (
        &GetProcessorCapabilities() noexcept)[Kernel::Scheduler::MaximumProcessorCount]
    {
        return m_processorCapabilities;
    }

private:
    Coordinator m_coordinator;
    Runtime m_runtime;
    Ia64PolledTlbShootdownNotifier m_notifier;
    alignas(TlbShootdownMailboxTransport) Foundation::UInt8 m_mailboxStorage[sizeof(TlbShootdownMailboxTransport)]{};
    TlbShootdownMailboxTransport* m_mailbox{nullptr};
    PalVirtualMemoryCapabilities m_processorCapabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    LayoutPlan m_plan{};
};

[[nodiscard]] constexpr TlbShootdownDrainSummary ValidTlbDrainSummary(const Foundation::UInt8 processor) noexcept
{
    return TlbShootdownDrainSummary{
        .ReceivedCount = 1U,
        .CompletedCount = 1U,
        .RejectedCount = 0U,
        .ReceiveError = TlbShootdownReceiveError::None,
        .FirstRemoteError = TlbShootdownError::None,
        .FirstLocalError = Hal::Memory::LocalTlbInvalidationError::None,
        .FirstAddressSpaceError = AddressSpaceError::None,
        .Processor = Kernel::Scheduler::CpuId{processor},
        .Reserved = {},
    };
}

// These placement-constructed fixtures deliberately have no matching
// destructor. Their retained scheduler contexts and evidence pointers safely
// outlive every detached suspended worker through process termination.
alignas(RuntimeFixture) Foundation::UInt8 g_returnedRuntimeFixtureStorage[sizeof(RuntimeFixture)]{};
alignas(RuntimeFixture) Foundation::UInt8 g_persistentRuntimeFixtureStorage[sizeof(RuntimeFixture)]{};
alignas(RuntimeFixture) Foundation::UInt8 g_timerDeliveryRuntimeFixtureStorage[sizeof(RuntimeFixture)]{};
alignas(RuntimeFixture) Foundation::UInt8 g_timerDeliveryRaceRuntimeFixtureStorage[sizeof(RuntimeFixture)]{};
alignas(RuntimeFixture) Foundation::UInt8 g_timerDeliveryDrainFailureFixtureStorage[sizeof(RuntimeFixture)]{};
alignas(RuntimeFixture) Foundation::UInt8 g_timerDeliveryArmFailureFixtureStorage[sizeof(RuntimeFixture)]{};
alignas(RuntimeFixture) Foundation::UInt8 g_timerDeliveryGenerationFailureFixtureStorage[sizeof(RuntimeFixture)]{};
alignas(RuntimeFixture) Foundation::UInt8 g_timerDeliveryEvidenceFailureFixtureStorage[sizeof(RuntimeFixture)]{};
alignas(RuntimeFixture) Foundation::UInt8 g_timerDeliveryChronologyFailureFixtureStorage[sizeof(RuntimeFixture)]{};
alignas(RuntimeFixture) Foundation::UInt8 g_timerDeliveryRegistrationFailureFixtureStorage[sizeof(RuntimeFixture)]{};
alignas(RuntimeFixture) Foundation::UInt8 g_tlbShootdownEmptyFixtureStorage[sizeof(RuntimeFixture)]{};
alignas(RuntimeFixture) Foundation::UInt8 g_tlbShootdownPartialMaskFixtureStorage[sizeof(RuntimeFixture)]{};
alignas(RuntimeFixture) Foundation::UInt8 g_tlbShootdownNotReadyFixtureStorage[sizeof(RuntimeFixture)]{};
alignas(RuntimeFixture) Foundation::UInt8 g_tlbShootdownInvalidProcessorFixtureStorage[sizeof(RuntimeFixture)]{};
alignas(RuntimeFixture) Foundation::UInt8 g_tlbShootdownPendingSuccessFixtureStorage[sizeof(RuntimeFixture)]{};
alignas(RuntimeFixture) Foundation::UInt8 g_tlbShootdownCoalescedFixtureStorage[sizeof(RuntimeFixture)]{};
alignas(RuntimeFixture) Foundation::UInt8 g_tlbShootdownTerminalFixtureStorage[sizeof(RuntimeFixture)]{};
// The local-timer arm retry scenarios (TestLocalTimerProofBeginFailures);
// static like the others, which keeps them out of main's frame.
alignas(RuntimeFixture) Foundation::UInt8 g_localTimerArmRetryFixtureStorage[4U][sizeof(RuntimeFixture)]{};
alignas(Kernel::Ipc::Notification)
    Foundation::UInt8 g_registryFillNotificationStorage[Asteria::TestSupport::TestObjectRegistry::EntryCapacity]
                                                       [sizeof(Kernel::Ipc::Notification)]{};
RuntimeFixture* const g_returnedRuntimeFixture{::new (static_cast<void*>(g_returnedRuntimeFixtureStorage))
                                                   RuntimeFixture{}};
RuntimeFixture* const g_persistentRuntimeFixture{::new (static_cast<void*>(g_persistentRuntimeFixtureStorage))
                                                     RuntimeFixture{}};
RuntimeFixture* const g_timerDeliveryRuntimeFixture{::new (static_cast<void*>(g_timerDeliveryRuntimeFixtureStorage))
                                                        RuntimeFixture{}};
RuntimeFixture* const g_timerDeliveryRaceRuntimeFixture{
    ::new (static_cast<void*>(g_timerDeliveryRaceRuntimeFixtureStorage)) RuntimeFixture{}};
RuntimeFixture* const g_timerDeliveryDrainFailureFixture{
    ::new (static_cast<void*>(g_timerDeliveryDrainFailureFixtureStorage)) RuntimeFixture{}};
RuntimeFixture* const g_timerDeliveryArmFailureFixture{
    ::new (static_cast<void*>(g_timerDeliveryArmFailureFixtureStorage)) RuntimeFixture{}};
RuntimeFixture* const g_timerDeliveryGenerationFailureFixture{
    ::new (static_cast<void*>(g_timerDeliveryGenerationFailureFixtureStorage)) RuntimeFixture{}};
RuntimeFixture* const g_timerDeliveryEvidenceFailureFixture{
    ::new (static_cast<void*>(g_timerDeliveryEvidenceFailureFixtureStorage)) RuntimeFixture{}};
RuntimeFixture* const g_timerDeliveryChronologyFailureFixture{
    ::new (static_cast<void*>(g_timerDeliveryChronologyFailureFixtureStorage)) RuntimeFixture{}};
RuntimeFixture* const g_timerDeliveryRegistrationFailureFixture{
    ::new (static_cast<void*>(g_timerDeliveryRegistrationFailureFixtureStorage)) RuntimeFixture{}};
RuntimeFixture* const g_tlbShootdownEmptyFixture{::new (static_cast<void*>(g_tlbShootdownEmptyFixtureStorage))
                                                     RuntimeFixture{}};
RuntimeFixture* const g_tlbShootdownPartialMaskFixture{
    ::new (static_cast<void*>(g_tlbShootdownPartialMaskFixtureStorage)) RuntimeFixture{}};
RuntimeFixture* const g_tlbShootdownNotReadyFixture{::new (static_cast<void*>(g_tlbShootdownNotReadyFixtureStorage))
                                                        RuntimeFixture{}};
RuntimeFixture* const g_tlbShootdownInvalidProcessorFixture{
    ::new (static_cast<void*>(g_tlbShootdownInvalidProcessorFixtureStorage)) RuntimeFixture{}};
RuntimeFixture* const g_tlbShootdownPendingSuccessFixture{
    ::new (static_cast<void*>(g_tlbShootdownPendingSuccessFixtureStorage)) RuntimeFixture{}};
RuntimeFixture* const g_localTimerSampleRetryFixture{::new (static_cast<void*>(g_localTimerArmRetryFixtureStorage[0U]))
                                                         RuntimeFixture{}};
RuntimeFixture* const g_localTimerAdapterRetryFixture{::new (static_cast<void*>(g_localTimerArmRetryFixtureStorage[1U]))
                                                          RuntimeFixture{}};
RuntimeFixture* const g_localTimerArmExhaustedFixture{::new (static_cast<void*>(g_localTimerArmRetryFixtureStorage[2U]))
                                                          RuntimeFixture{}};
RuntimeFixture* const g_localTimerLateSampleFixture{::new (static_cast<void*>(g_localTimerArmRetryFixtureStorage[3U]))
                                                        RuntimeFixture{}};
RuntimeFixture* const g_tlbShootdownCoalescedFixture{::new (static_cast<void*>(g_tlbShootdownCoalescedFixtureStorage))
                                                         RuntimeFixture{}};
RuntimeFixture* const g_tlbShootdownTerminalFixture{::new (static_cast<void*>(g_tlbShootdownTerminalFixtureStorage))
                                                        RuntimeFixture{}};
InterruptEvidence g_tlbShootdownTerminalEvidence{};
InterruptEvidence g_tlbShootdownTerminalProcessorTwoEvidence{};
bool g_tlbShootdownTerminalIdleReady{false};

[[nodiscard]] RuntimeEvidence MakeEvidence(const Layout& layout, const EvidencePlacement placement,
                                           const Foundation::UInt64 discriminator) noexcept
{
    const Foundation::UInt64 stackStart = placement == EvidencePlacement::Worker
                                              ? LayoutPlanner::GetWorkerAliasVirtualStart(layout)
                                              : LayoutPlanner::GetIdleMemoryStackVirtualStart(layout);
    const Foundation::UInt64 rseStart = placement == EvidencePlacement::Worker
                                            ? LayoutPlanner::GetWorkerRseBackingStoreVirtualStart(layout)
                                            : LayoutPlanner::GetIdleRseBackingStoreVirtualStart(layout);
    const Foundation::UInt64 evidenceOffset = 0x100U + (discriminator * 16U);
    return RuntimeEvidence{
        .StackPointer = stackStart + evidenceOffset,
        .RseBackingStorePointer = rseStart + evidenceOffset,
        .RseBackingStoreBase = rseStart,
        .RseNatCollection = 0xA500U + discriminator,
        .RseConfiguration = 0U,
        .ProcessorStatus = TranslationStateMask | InstructionCollectionMask,
        .CaptureComplete = 1U,
        .Reserved = {},
    };
}

[[nodiscard]] bool EvidenceEquals(const RuntimeEvidence& first, const RuntimeEvidence& second) noexcept
{
    bool reservedEqual{true};
    for (Foundation::Size index{0U}; index < sizeof(first.Reserved); ++index)
    {
        reservedEqual = reservedEqual && (first.Reserved[index] == second.Reserved[index]);
    }
    return (first.StackPointer == second.StackPointer) &&
           (first.RseBackingStorePointer == second.RseBackingStorePointer) &&
           (first.RseBackingStoreBase == second.RseBackingStoreBase) &&
           (first.RseNatCollection == second.RseNatCollection) && (first.RseConfiguration == second.RseConfiguration) &&
           (first.ProcessorStatus == second.ProcessorStatus) && (first.CaptureComplete == second.CaptureComplete) &&
           reservedEqual;
}

[[nodiscard]] bool PortableRecordEquals(const RuntimePortableRecord& first,
                                        const RuntimePortableRecord& second) noexcept
{
    return first.AddressSpaceIdentity.IsEqualTo(second.AddressSpaceIdentity) &&
           first.ProtectionDomainIdentity.IsEqualTo(second.ProtectionDomainIdentity) &&
           first.ThreadIdentity.IsEqualTo(second.ThreadIdentity) &&
           first.SchedulingContextIdentity.IsEqualTo(second.SchedulingContextIdentity) &&
           (first.InitialRunSequence == second.InitialRunSequence) &&
           (first.FirstResumedRunSequence == second.FirstResumedRunSequence) &&
           (first.InitialBlockToken == second.InitialBlockToken) &&
           (first.ServingBlockToken == second.ServingBlockToken) &&
           (first.Processor.GetValue() == second.Processor.GetValue()) && (first.Phase == second.Phase) &&
           (first.ThreadState == second.ThreadState) &&
           (first.SchedulingContextState == second.SchedulingContextState) &&
           (first.ControllerState == second.ControllerState) && (first.RunLoopState == second.RunLoopState) &&
           (first.DeadlineState == second.DeadlineState) && (first.LocalTimerState == second.LocalTimerState) &&
           (first.TimerQueueScheduledEntries == second.TimerQueueScheduledEntries) &&
           (first.TimerQueuePendingEntries == second.TimerQueuePendingEntries) &&
           (first.TimerQueueClaimedEntries == second.TimerQueueClaimedEntries) &&
           (first.RunLoopBindingCount == second.RunLoopBindingCount);
}

[[nodiscard]] bool PortableFailureIsCanonicalIdle(const RuntimePortableFailure& failure) noexcept
{
    return (failure.RunLoop.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::ControllerRejected) &&
           (failure.RunLoop.ControllerError == Kernel::Scheduler::ProcessorRunError::NoRunnableThread) &&
           (failure.RunLoop.SchedulerError == Kernel::Scheduler::ProcessorSchedulerError::NoRunnableThread) &&
           (failure.RunLoop.DeadlineError == Kernel::Time::ProcessorDeadlineError::None) &&
           (failure.RunLoop.DeadlineReceipt.State == Kernel::Time::ProcessorDeadlineState::Disarmed) &&
           (failure.RunLoop.DeadlineReceipt.SelectedDeadlineTicks == 0U) &&
           (failure.RunLoop.DeadlineReceipt.SelectedSources == 0U) && (failure.RunLoop.ActiveRunRetained == 0U);
}

[[nodiscard]] bool FailureEquals(const RuntimeFailure& first, const RuntimeFailure& second) noexcept
{
    return (first.Generation == second.Generation) && (first.Detail == second.Detail) &&
           (first.Error == second.Error) && (first.Operation == second.Operation) &&
           (first.ObservedState == second.ObservedState) && (first.ReleaseError == second.ReleaseError) &&
           (first.ContextError == second.ContextError) && (first.ProcessorValue == second.ProcessorValue);
}

[[nodiscard]] bool ReleaseAndClaim(RuntimeFixture& fixture) noexcept
{
    const auto released = fixture.GetCoordinator().Release(Kernel::Scheduler::CpuId{1U}, 1U);
    return released.HasValue() &&
           (fixture.GetRuntime().ClaimPhysicalRelease(released.GetValue()) == RuntimeError::None);
}

void ResetSwitchAdapter() noexcept
{
    g_switchCount.store(0U, std::memory_order_release);
    g_workerSwitchEnabled.store(true, std::memory_order_release);
    g_holdWorkerResume.store(false, std::memory_order_release);
    g_workerResumeObserved.store(false, std::memory_order_release);
}

void ResetLocalTimerAdapter() noexcept
{
    g_timerTicks[0U] = 1'000U;
    g_timerTicks[1U] = 1'001U;
    g_timerTicks[2U] = 1'002U;
    g_timerTicks[3U] = 1'003U;
    g_timerTickCount = 4U;
    g_timerTickIndex = 0U;
    g_timerInitializeStatus = 0U;
    g_timerProgramStatus = 0U;
    g_timerProgramStatusCalls = ~Foundation::Size{0U};
    g_timerCancelStatus = 0U;
    g_taskPriorityStatus = 0U;
    g_programmedDeadline = 0U;
    g_programmedVector = 0U;
    g_timerInitializeCalls = 0U;
    g_timerProgramCalls = 0U;
    g_timerCancelCalls = 0U;
    g_taskPriorityCalls = 0U;
}

void SetLocalTimerTicks(const Foundation::UInt64 first, const Foundation::UInt64 second, const Foundation::UInt64 third,
                        const Foundation::UInt64 fourth) noexcept
{
    g_timerTicks[0U] = first;
    g_timerTicks[1U] = second;
    g_timerTicks[2U] = third;
    g_timerTicks[3U] = fourth;
    g_timerTickCount = 4U;
    g_timerTickIndex = 0U;
}

[[nodiscard]] InterruptEvidence MakeInterruptEvidence(const Foundation::UInt8 processor,
                                                      const Foundation::UInt64 programmedDeadline,
                                                      const PollRange range) noexcept
{
    return InterruptEvidence{
        .Sequence = 1U,
        .ProgrammedDeadlineTicks = programmedDeadline,
        .ObservedIntervalTimeCounter = programmedDeadline,
        .InterruptedInstructionPointer = range.Start,
        .InterruptedProcessorStatus = Platform::Ia64::Interrupt::Ia64ExternalInterruptInterruptedProcessorStatusValue,
        .InterruptedIfsValid = 0U,
        .ProcessorIndex = processor,
        .Vector = Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector,
        .Reserved = {},
        .AcknowledgedSequence = 0U,
    };
}

[[nodiscard]] InterruptEvidence MakeTimerDeliveryInterruptEvidence(const Foundation::UInt8 processor,
                                                                   const Foundation::UInt64 programmedDeadline,
                                                                   const PollRange range) noexcept
{
    InterruptEvidence evidence = MakeInterruptEvidence(processor, programmedDeadline, range);
    evidence.Sequence = 2U;
    evidence.AcknowledgedSequence = 1U;
    return evidence;
}

[[nodiscard]] bool AdvanceProcessorToRuntimeEntered(RuntimeFixture& fixture, InterruptEvidence& evidence,
                                                    const Foundation::UInt8 processor) noexcept
{
    const auto released = fixture.GetCoordinator().Release(Kernel::Scheduler::CpuId{processor}, processor);
    if (!released.HasValue() ||
        (fixture.GetRuntime().ClaimPhysicalRelease(released.GetValue()) != RuntimeError::None) ||
        (fixture.GetRuntime().ConfigureLocalTimerInterruptProof(Kernel::Scheduler::CpuId{processor}, processor,
                                                                &evidence) != RuntimeError::None))
    {
        return false;
    }
    const Layout& layout = fixture.GetPlan().Processors[processor - 1U];
    const RuntimeEvidence entry = MakeEvidence(layout, EvidencePlacement::Idle, 1U);
    if (fixture.GetRuntime().EnterTranslatedIdle(Kernel::Scheduler::CpuId{processor}, processor, entry) !=
        RuntimeError::None)
    {
        return false;
    }
    return true;
}

[[nodiscard]] bool ReachRuntimeEntered(RuntimeFixture& fixture, InterruptEvidence& evidence,
                                       const Foundation::UInt8 processor = 1U) noexcept
{
    ResetLocalTimerAdapter();
    return fixture.Configure(static_cast<Foundation::UInt8>(processor + 1U)) && fixture.Prepare() &&
           AdvanceProcessorToRuntimeEntered(fixture, evidence, processor);
}

[[nodiscard]] bool BeginProof(RuntimeFixture& fixture, InterruptEvidence& evidence) noexcept
{
    return ReachRuntimeEntered(fixture, evidence) &&
           (fixture.GetRuntime().BeginLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U) == RuntimeError::None);
}

[[nodiscard]] RuntimeError ConfigurePortableDependencies(RuntimeFixture& fixture) noexcept
{
    const RuntimeError portable = fixture.GetRuntime().ConfigurePortableSchedulerDependencies(
        g_portableRuntimeArena.RuntimeMemory, g_portableRegistry, g_portableResourceIdentity, g_portableResourceDomain,
        g_portableIssuer);
    if (portable != RuntimeError::None)
    {
        return portable;
    }
    return fixture.GetRuntime().ConfigureTlbShootdownDependencies(fixture.GetNotifier(), fixture.GetMailbox(),
                                                                  fixture.GetProcessorCapabilities());
}

[[nodiscard]] bool CompleteProof(RuntimeFixture& fixture, InterruptEvidence& evidence,
                                 const Foundation::UInt8 processor, const PollRange range) noexcept
{
    if (fixture.GetRuntime().BeginLocalTimerInterruptProof(Kernel::Scheduler::CpuId{processor}, processor) !=
        RuntimeError::None)
    {
        return false;
    }
    evidence = MakeInterruptEvidence(processor, g_programmedDeadline, range);
    return fixture.GetRuntime().CompleteLocalTimerInterruptProof(Kernel::Scheduler::CpuId{processor}, processor,
                                                                 PollStatus::Completed, range) == RuntimeError::None;
}

[[nodiscard]] bool PrepareTlbShootdownRuntime(RuntimeFixture& fixture) noexcept
{
    return fixture.Configure(3U) && fixture.Prepare() && (ConfigurePortableDependencies(fixture) == RuntimeError::None);
}

/// Advances one AP through its public path to IdleServing: the local-timer
/// proof, scheduler preparation, first worker dispatch, and one completed
/// service, then the BSP serving observation that permits the next release.
/// The dispatched worker stays suspended in a hosted switch slot, so fixture
/// and evidence must be permanent.
[[nodiscard]] bool AdvanceTlbProcessorToIdle(RuntimeFixture& fixture, InterruptEvidence& evidence,
                                             const Foundation::UInt8 processor, const PollRange range) noexcept
{
    const Kernel::Scheduler::CpuId cpu{processor};
    if (!AdvanceProcessorToRuntimeEntered(fixture, evidence, processor) ||
        !CompleteProof(fixture, evidence, processor, range) ||
        (fixture.GetRuntime().PreparePortableScheduler(cpu, processor) != RuntimeError::None))
    {
        return false;
    }
    ResetSwitchAdapter();
    if (fixture.GetRuntime().DispatchPortableSchedulerFromIdle(cpu, processor) != RuntimeError::None)
    {
        return false;
    }
    const auto request = fixture.GetRuntime().RequestPortableSchedulerService(cpu, processor, 0x7100U + processor);
    return request.HasValue() &&
           (fixture.GetRuntime().ServicePortableSchedulerFromIdle(cpu, processor) == RuntimeError::None) &&
           fixture.GetCoordinator().ObserveServing(cpu, processor).HasValue();
}

[[nodiscard]] bool ReachFirstIdleServing(RuntimeFixture& fixture, InterruptEvidence& evidence, const PollRange range,
                                         const Foundation::UInt64 cookie) noexcept
{
    ResetLocalTimerAdapter();
    if (!fixture.Configure(2U) || !fixture.Prepare() ||
        (ConfigurePortableDependencies(fixture) != RuntimeError::None) ||
        !AdvanceProcessorToRuntimeEntered(fixture, evidence, 1U) || !CompleteProof(fixture, evidence, 1U, range) ||
        (fixture.GetRuntime().PreparePortableScheduler(Kernel::Scheduler::CpuId{1U}, 1U) != RuntimeError::None))
    {
        return false;
    }
    ResetSwitchAdapter();
    if (fixture.GetRuntime().DispatchPortableSchedulerFromIdle(Kernel::Scheduler::CpuId{1U}, 1U) != RuntimeError::None)
    {
        return false;
    }
    const auto request = fixture.GetRuntime().RequestPortableSchedulerService(Kernel::Scheduler::CpuId{1U}, 1U, cookie);
    return request.HasValue() && (request.GetValue().Sequence == 1U) &&
           (fixture.GetRuntime().ServicePortableSchedulerFromIdle(Kernel::Scheduler::CpuId{1U}, 1U) ==
            RuntimeError::None);
}

[[nodiscard]] bool ReachRequestedTimerDelivery(RuntimeFixture& fixture, InterruptEvidence& evidence,
                                               const PollRange range, const Foundation::UInt64 cookie) noexcept
{
    return ReachFirstIdleServing(fixture, evidence, range, cookie) &&
           (Platform::Ia64::Interrupt::AcknowledgeIa64LocalTimerInterruptEvidence(evidence, 1U) ==
            Platform::Ia64::Interrupt::Ia64LocalTimerInterruptAcknowledgementError::None) &&
           fixture.GetRuntime().RequestTimerDelivery(Kernel::Scheduler::CpuId{1U}, 1U).HasValue();
}

void TestTlbShootdownConfiguration(TestContext& runner) noexcept
{
    {
        RuntimeFixture fixture;
        const bool configured = fixture.Configure(3U);
        ASTERIA_EXPECT(runner, "TLB shootdown dependencies cannot precede runtime preparation", configured,
                       fixture.GetRuntime().ConfigureTlbShootdownDependencies(
                           fixture.GetNotifier(), fixture.GetMailbox(), fixture.GetProcessorCapabilities()) ==
                           RuntimeError::RuntimeNotReady);
    }
    {
        RuntimeFixture fixture;
        const bool prepared = fixture.Configure(3U) && fixture.Prepare();
        const RuntimeError first =
            prepared ? fixture.GetRuntime().ConfigureTlbShootdownDependencies(
                           fixture.GetNotifier(), fixture.GetMailbox(), fixture.GetProcessorCapabilities())
                     : RuntimeError::RuntimeNotReady;
        const RuntimeError second =
            prepared ? fixture.GetRuntime().ConfigureTlbShootdownDependencies(
                           fixture.GetNotifier(), fixture.GetMailbox(), fixture.GetProcessorCapabilities())
                     : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "TLB shootdown dependencies configure exactly once", first == RuntimeError::None,
                       second == RuntimeError::TlbShootdownAlreadyConfigured);
    }
    {
        RuntimeFixture fixture;
        const bool released = fixture.Configure(3U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        ASTERIA_EXPECT(runner, "TLB shootdown configuration rejects any released AP slot", released,
                       fixture.GetRuntime().ConfigureTlbShootdownDependencies(
                           fixture.GetNotifier(), fixture.GetMailbox(), fixture.GetProcessorCapabilities()) ==
                           RuntimeError::TlbShootdownDependenciesRejected);
    }
    {
        RuntimeFixture fixture;
        Ia64PolledTlbShootdownNotifier unreadyNotifier;
        TlbShootdownMailboxTransport mailbox{unreadyNotifier, 0x07U};
        const bool prepared = fixture.Configure(3U) && fixture.Prepare();
        ASTERIA_EXPECT(runner, "TLB shootdown configuration rejects an unready notifier", prepared,
                       fixture.GetRuntime().ConfigureTlbShootdownDependencies(unreadyNotifier, mailbox,
                                                                              fixture.GetProcessorCapabilities()) ==
                           RuntimeError::TlbShootdownDependenciesRejected);
    }
    {
        RuntimeFixture fixture;
        Ia64PolledTlbShootdownNotifier wrongNotifier;
        const bool wrongReady = wrongNotifier.Initialize(0x03U) ==
                                Platform::Ia64::Memory::Ia64PolledTlbShootdownNotifierInitializationError::None;
        TlbShootdownMailboxTransport wrongMailbox{wrongNotifier, 0x03U};
        const bool prepared = fixture.Configure(3U) && fixture.Prepare();
        ASTERIA_EXPECT(runner, "TLB shootdown configuration rejects a profiled-mask mismatch", wrongReady, prepared,
                       fixture.GetRuntime().ConfigureTlbShootdownDependencies(wrongNotifier, wrongMailbox,
                                                                              fixture.GetProcessorCapabilities()) ==
                           RuntimeError::TlbShootdownDependenciesRejected);
    }
    {
        RuntimeFixture fixture;
        const bool prepared = fixture.Configure(3U) && fixture.Prepare();
        if (prepared)
        {
            fixture.GetNotifier().Notify(0x02U);
        }
        ASTERIA_EXPECT(runner, "TLB shootdown configuration rejects pending notification state", prepared,
                       fixture.GetRuntime().ConfigureTlbShootdownDependencies(
                           fixture.GetNotifier(), fixture.GetMailbox(), fixture.GetProcessorCapabilities()) ==
                           RuntimeError::TlbShootdownDependenciesRejected);
    }
    {
        RuntimeFixture fixture;
        const bool prepared = fixture.Configure(3U) && fixture.Prepare();
        TlbShootdownMailboxTransport wrongMailbox{fixture.GetNotifier(), 0x03U};
        ASTERIA_EXPECT(runner, "TLB shootdown configuration rejects a mailbox-mask mismatch", prepared,
                       fixture.GetRuntime().ConfigureTlbShootdownDependencies(fixture.GetNotifier(), wrongMailbox,
                                                                              fixture.GetProcessorCapabilities()) ==
                           RuntimeError::TlbShootdownDependenciesRejected);
    }
    {
        RuntimeFixture fixture;
        const bool prepared = fixture.Configure(3U) && fixture.Prepare();
        fixture.GetProcessorCapabilities()[1U].PurgePageSizeMask = 0U;
        const RuntimeError invalid =
            prepared ? fixture.GetRuntime().ConfigureTlbShootdownDependencies(
                           fixture.GetNotifier(), fixture.GetMailbox(), fixture.GetProcessorCapabilities())
                     : RuntimeError::RuntimeNotReady;
        fixture.GetProcessorCapabilities()[1U] = PortableCapabilities();
        const RuntimeError retry =
            prepared ? fixture.GetRuntime().ConfigureTlbShootdownDependencies(
                           fixture.GetNotifier(), fixture.GetMailbox(), fixture.GetProcessorCapabilities())
                     : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "TLB shootdown configuration validates each enabled AP PAL record before publication",
                       invalid == RuntimeError::TlbShootdownDependenciesRejected, retry == RuntimeError::None);
    }
    {
        RuntimeFixture fixture;
        const bool prepared = fixture.Configure(2U) && fixture.Prepare();
        fixture.GetProcessorCapabilities()[0U].PurgePageSizeMask = 0U;
        fixture.GetProcessorCapabilities()[2U].PurgePageSizeMask = 0U;
        fixture.GetProcessorCapabilities()[3U].PurgePageSizeMask = 0U;
        ASTERIA_EXPECT(runner, "TLB shootdown configuration ignores CPU0 and disabled PAL records", prepared,
                       fixture.GetRuntime().ConfigureTlbShootdownDependencies(
                           fixture.GetNotifier(), fixture.GetMailbox(), fixture.GetProcessorCapabilities()) ==
                           RuntimeError::None);
    }
    {
        RuntimeFixture fixture;
        const bool prepared = fixture.Configure(1U) && fixture.Prepare();
        for (PalVirtualMemoryCapabilities& capabilities : fixture.GetProcessorCapabilities())
        {
            capabilities.PurgePageSizeMask = 0U;
        }
        ASTERIA_EXPECT(runner, "SMP1 configures shootdown dependencies without creating an AP drain", prepared,
                       fixture.GetRuntime().ConfigureTlbShootdownDependencies(
                           fixture.GetNotifier(), fixture.GetMailbox(), fixture.GetProcessorCapabilities()) ==
                           RuntimeError::None);
    }
}

void TestTlbShootdownDrainSummaryValidation(TestContext& runner) noexcept
{
    const Kernel::Scheduler::CpuId processor{1U};
    const auto accepted = [](const TlbShootdownDrainSummary& summary) noexcept
    {
        return ValidateTlbShootdownDrainSummary(summary, Kernel::Scheduler::CpuId{1U});
    };
    TlbShootdownDrainSummary summary = ValidTlbDrainSummary(1U);
    ASTERIA_EXPECT(runner, "one exact completed TLB drain summary is accepted", accepted(summary));

    summary = ValidTlbDrainSummary(1U);
    summary.ReceivedCount = 0U;
    summary.CompletedCount = 0U;
    ASTERIA_EXPECT(runner, "an empty TLB drain, the pass of a coalesced leftover notification, is accepted",
                   accepted(summary));
    summary = ValidTlbDrainSummary(1U);
    summary.ReceivedCount = static_cast<Foundation::UInt8>(Kernel::Memory::TlbShootdownMailboxSlotCount + 1U);
    summary.CompletedCount = summary.ReceivedCount;
    ASTERIA_EXPECT(runner, "an over-capacity TLB drain is rejected", !accepted(summary));
    summary = ValidTlbDrainSummary(1U);
    summary.CompletedCount = 0U;
    ASTERIA_EXPECT(runner, "an incomplete TLB drain is rejected", !accepted(summary));
    summary = ValidTlbDrainSummary(1U);
    summary.RejectedCount = 1U;
    ASTERIA_EXPECT(runner, "a remotely rejected TLB drain is rejected", !accepted(summary));
    summary = ValidTlbDrainSummary(1U);
    summary.ReceiveError = TlbShootdownReceiveError::CorruptState;
    ASTERIA_EXPECT(runner, "a receive error is retained and rejected", !accepted(summary));
    summary = ValidTlbDrainSummary(1U);
    summary.FirstRemoteError = TlbShootdownError::LocalInvalidationRejected;
    ASTERIA_EXPECT(runner, "a remote-dispatch error is retained and rejected", !accepted(summary));
    summary = ValidTlbDrainSummary(1U);
    summary.FirstLocalError = Hal::Memory::LocalTlbInvalidationError::InvalidRange;
    ASTERIA_EXPECT(runner, "a local-invalidation error is retained and rejected", !accepted(summary));
    summary = ValidTlbDrainSummary(1U);
    summary.FirstAddressSpaceError = AddressSpaceError::InvalidShootdownToken;
    ASTERIA_EXPECT(runner, "an AddressSpace acknowledgement error is retained and rejected", !accepted(summary));
    summary = ValidTlbDrainSummary(2U);
    ASTERIA_EXPECT(runner, "a drain for another processor is rejected", !accepted(summary));
    summary = ValidTlbDrainSummary(1U);
    summary.Reserved[7U] = 1U;
    ASTERIA_EXPECT(runner, "nonzero TLB drain reserved state is rejected", !accepted(summary));
    ASTERIA_EXPECT(runner, "TLB drain validation is deterministic for the bound processor",
                   ValidateTlbShootdownDrainSummary(summary, processor) == accepted(summary));
}

void TestTlbShootdownIdleEmptyDuringProgressiveAdmission(TestContext& runner) noexcept
{
    const PollRange range{
        .Start = 0x1000U,
        .End = 0x1100U,
    };
    RuntimeFixture& fixture = *g_tlbShootdownEmptyFixture;
    static InterruptEvidence evidence{};
    ResetLocalTimerAdapter();
    const bool idle = PrepareTlbShootdownRuntime(fixture) && AdvanceTlbProcessorToIdle(fixture, evidence, 1U, range);
    const auto root = g_portableRuntimeArena.RuntimeMemory.BorrowReadyRoot();
    const Kernel::Memory::AddressSpaceSnapshot before =
        root.HasValue() ? root.GetValue().AddressSpaceStorage->GetSnapshot() : Kernel::Memory::AddressSpaceSnapshot{};
    g_tlbPurgeCalls = 0U;
    g_tlbSerializationCalls = 0U;
    const RuntimeError first = idle && root.HasValue()
                                   ? fixture.GetRuntime().ServiceTlbShootdownFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
                                   : RuntimeError::RuntimeNotReady;
    const RuntimeError second = first == RuntimeError::None
                                    ? fixture.GetRuntime().ServiceTlbShootdownFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
                                    : RuntimeError::RuntimeNotReady;
    const Kernel::Memory::AddressSpaceSnapshot after =
        root.HasValue() ? root.GetValue().AddressSpaceStorage->GetSnapshot() : Kernel::Memory::AddressSpaceSnapshot{};
    RuntimeProcessorSnapshot snapshot{};
    RuntimeFailure failure{};
    const RuntimeAccessError snapshotAccess =
        fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, snapshot);
    const RuntimeFailurePoll failurePoll = fixture.GetRuntime().PollFirstFailure(failure);
    const auto notifier = fixture.GetNotifier().GetSnapshot();
    ASTERIA_EXPECT(runner, "an early AP accepts repeated Empty polls while later profiled APs remain unadmitted", idle,
                   root.HasValue(), fixture.GetPlan().EnabledProcessorMask == 0x07U,
                   before.State == Kernel::Memory::AddressSpaceState::Ready, before.ResidentProcessorMask == 0x03U,
                   first == RuntimeError::None, second == RuntimeError::None,
                   after.State == Kernel::Memory::AddressSpaceState::Ready, after.ResidentProcessorMask == 0x03U,
                   snapshotAccess == RuntimeAccessError::None, snapshot.State == RuntimeProcessorState::IdleServing,
                   failurePoll == RuntimeFailurePoll::None, notifier.PendingProcessorMask == 0U, g_tlbPurgeCalls == 0U,
                   g_tlbSerializationCalls == 0U);
}

void TestTlbShootdownIdlePollFailures(TestContext& runner) noexcept
{
    const PollRange range{
        .Start = 0x1000U,
        .End = 0x1100U,
    };
    {
        // Every AP controller belongs to the shared root and is never
        // replaced, so the proof that idle polling performs before Poll is
        // exercised on the admitted AP1 controller and an uninitialized one.
        const auto root =
            g_portableRuntimeArena.RuntimeMemory.BorrowReadyApplicationProcessorRoot(Kernel::Scheduler::CpuId{1U});
        Kernel::Memory::ProcessorAddressSpaceController invalidController;
        const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot invalidSnapshot = invalidController.GetSnapshot();
        const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot admittedSnapshot =
            root.HasValue() ? root.GetValue().ControllerStorage->GetSnapshot() : invalidSnapshot;
        Kernel::Memory::ProcessorAddressSpaceControllerSnapshot reservedSnapshot = admittedSnapshot;
        reservedSnapshot.Reserved[0U] = 1U;
        ASTERIA_EXPECT(runner,
                       "idle polling fail-stops before Poll when the processor-local current-root proof is invalid",
                       root.HasValue(),
                       FixedRootControllerMatches(admittedSnapshot, root.GetValue(), Kernel::Scheduler::CpuId{1U}),
                       !FixedRootControllerMatches(admittedSnapshot, root.GetValue(), Kernel::Scheduler::CpuId{2U}),
                       !FixedRootControllerMatches(reservedSnapshot, root.GetValue(), Kernel::Scheduler::CpuId{1U}),
                       invalidSnapshot.State == Kernel::Memory::ProcessorAddressSpaceControllerState::Uninitialized,
                       !FixedRootControllerMatches(invalidSnapshot, root.GetValue(), Kernel::Scheduler::CpuId{1U}));
    }
    {
        RuntimeFixture& fixture = *g_tlbShootdownNotReadyFixture;
        static InterruptEvidence evidence{};
        ResetLocalTimerAdapter();
        const bool idle =
            PrepareTlbShootdownRuntime(fixture) && AdvanceTlbProcessorToIdle(fixture, evidence, 1U, range);
        if (idle)
        {
            fixture.ResetNotifier();
        }
        const RuntimeError result =
            idle ? fixture.GetRuntime().ServiceTlbShootdownFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
                 : RuntimeError::RuntimeNotReady;
        RuntimeFailure first{};
        RuntimeFailure retained{};
        RuntimePortableFailure failure{};
        const RuntimeFailurePoll firstPoll = fixture.GetRuntime().PollFirstFailure(first);
        if (idle)
        {
            static_cast<void>(fixture.GetRuntime().ServiceTlbShootdownFromIdle(Kernel::Scheduler::CpuId{1U}, 1U));
        }
        const RuntimeFailurePoll retainedPoll = fixture.GetRuntime().PollFirstFailure(retained);
        const RuntimeAccessError failureAccess =
            fixture.GetRuntime().GetPortableSchedulerFailure(Kernel::Scheduler::CpuId{1U}, failure);
        ASTERIA_EXPECT(runner, "a NotReady poll is terminal and preserves the first failure exactly", idle,
                       result == RuntimeError::TlbShootdownPollRejected, firstPoll == RuntimeFailurePoll::Available,
                       retainedPoll == RuntimeFailurePoll::Available, FailureEquals(first, retained),
                       first.Operation == RuntimeOperation::ServiceTlbShootdown,
                       failureAccess == RuntimeAccessError::None, failure.TlbShootdownPollAttempted == 1U,
                       failure.TlbShootdownPollResult == Ia64PolledTlbShootdownPollResult::NotReady);
    }
    {
        RuntimeFixture& fixture = *g_tlbShootdownInvalidProcessorFixture;
        static InterruptEvidence evidence{};
        ResetLocalTimerAdapter();
        const bool idle =
            PrepareTlbShootdownRuntime(fixture) && AdvanceTlbProcessorToIdle(fixture, evidence, 1U, range);
        bool notifierReady{false};
        if (idle)
        {
            fixture.ResetNotifier();
            notifierReady = fixture.GetNotifier().Initialize(0x01U) ==
                            Platform::Ia64::Memory::Ia64PolledTlbShootdownNotifierInitializationError::None;
        }
        const RuntimeError result =
            notifierReady ? fixture.GetRuntime().ServiceTlbShootdownFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
                          : RuntimeError::RuntimeNotReady;
        RuntimePortableFailure failure{};
        ASTERIA_EXPECT(runner, "an InvalidProcessor poll is terminal and retained precisely", idle, notifierReady,
                       result == RuntimeError::TlbShootdownPollRejected,
                       fixture.GetRuntime().GetPortableSchedulerFailure(Kernel::Scheduler::CpuId{1U}, failure) ==
                           RuntimeAccessError::None,
                       failure.TlbShootdownPollResult == Ia64PolledTlbShootdownPollResult::InvalidProcessor);
    }
}

[[nodiscard]] bool PrepareTlbShootdownMapping(const MemoryObjectId memoryObject, const FrameId frame,
                                              const Foundation::UInt64 virtualAddress,
                                              const Foundation::UInt64 pageOffset,
                                              PinnedFrameMappingPreparation& preparation) noexcept
{
    return preparation.Prepare(g_portableRegistry, memoryObject, g_tlbShootdownMemoryObject, frame, g_tlbShootdownFrame,
                               PinnedFrameMappingRequest{
                                   .VirtualStart = VirtualAddress{virtualAddress},
                                   .Pages = PageCount{1U},
                                   .MemoryObjectOffsetPages = pageOffset,
                                   .FrameOffsetPages = pageOffset,
                                   .Permissions = PagePermissions{PagePermission::Read}.With(PagePermission::Write),
                                   .Reserved = {},
                               }) == PinnedFrameMappingError::None;
}

void TestTlbShootdownIdlePendingRejectsPartialMasks(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 ShootdownVirtualAddress{0x2000'0000'0500'0000ULL};
    const PollRange range{
        .Start = 0x1000U,
        .End = 0x1100U,
    };
    RuntimeFixture& fixture = *g_tlbShootdownPartialMaskFixture;
    static InterruptEvidence evidence{};
    ResetLocalTimerAdapter();
    const bool idle = PrepareTlbShootdownRuntime(fixture) && AdvanceTlbProcessorToIdle(fixture, evidence, 1U, range);
    const auto root = g_portableRuntimeArena.RuntimeMemory.BorrowReadyRoot();
    Ia64LocalTlbInvalidator originInvalidator;
    const bool originReady =
        originInvalidator.Initialize(PortableCapabilities()) == Ia64LocalTlbInvalidatorInitializationError::None;
    TlbShootdownDispatcher origin{Kernel::Scheduler::CpuId{0U}, originInvalidator};
    PinnedFrameMappingPreparation preparation;
    const MemoryObjectId memoryObject{g_portableRegistry, g_tlbShootdownMemoryObjectId};
    const FrameId frame{g_portableRegistry, g_tlbShootdownFrameObject};
    const bool mappingPrepared =
        idle && root.HasValue() &&
        PrepareTlbShootdownMapping(memoryObject, frame, ShootdownVirtualAddress, 0U, preparation);
    const AddressSpaceId rootIdentity = root.HasValue() ? root.GetValue().AddressSpaceIdentity : AddressSpaceId{};
    const auto begin = mappingPrepared
                           ? root.GetValue().AddressSpaceStorage->BeginPinnedFrameMapping(rootIdentity, preparation)
                           : decltype(root.GetValue().AddressSpaceStorage->BeginPinnedFrameMapping(
                                 rootIdentity, preparation))::Failure({});
    const auto dispatched =
        begin.HasValue() && originReady
            ? origin.Dispatch(*root.GetValue().AddressSpaceStorage, begin.GetValue(), fixture.GetMailbox())
            : decltype(origin.Dispatch(*root.GetValue().AddressSpaceStorage, begin.GetValue(),
                                       fixture.GetMailbox()))::Failure({});
    const Kernel::Memory::AddressSpaceSnapshot partial =
        root.HasValue() ? root.GetValue().AddressSpaceStorage->GetSnapshot() : Kernel::Memory::AddressSpaceSnapshot{};

    g_tlbPurgeCalls = 0U;
    g_tlbSerializationCalls = 0U;
    const RuntimeError result = dispatched.HasValue()
                                    ? fixture.GetRuntime().ServiceTlbShootdownFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
                                    : RuntimeError::RuntimeNotReady;
    const Foundation::Size servicePurgeCalls = g_tlbPurgeCalls;
    const Foundation::Size serviceSerializationCalls = g_tlbSerializationCalls;
    RuntimePortableFailure failure{};
    RuntimeFailure firstFailure{};
    const RuntimeAccessError failureAccess =
        fixture.GetRuntime().GetPortableSchedulerFailure(Kernel::Scheduler::CpuId{1U}, failure);
    const RuntimeFailurePoll failurePoll = fixture.GetRuntime().PollFirstFailure(firstFailure);

    Ia64LocalTlbInvalidator cleanupInvalidator;
    const bool cleanupReady =
        cleanupInvalidator.Initialize(PortableCapabilities()) == Ia64LocalTlbInvalidatorInitializationError::None;
    TlbShootdownDispatcher cleanupDispatcher{Kernel::Scheduler::CpuId{1U}, cleanupInvalidator};
    TlbShootdownDrain cleanupDrain{fixture.GetMailbox(), cleanupDispatcher};
    const TlbShootdownDrainSummary cleanup =
        dispatched.HasValue() && cleanupReady ? cleanupDrain.Drain() : TlbShootdownDrainSummary{};
    const auto completed =
        (cleanup.CompletedCount == 1U)
            ? root.GetValue().AddressSpaceStorage->CompleteMapping(begin.GetValue().GetToken())
            : decltype(root.GetValue().AddressSpaceStorage->CompleteMapping(begin.GetValue().GetToken()))::Failure({});
    const Kernel::Memory::AddressSpaceSnapshot afterCleanup =
        root.HasValue() ? root.GetValue().AddressSpaceStorage->GetSnapshot() : Kernel::Memory::AddressSpaceSnapshot{};

    ASTERIA_EXPECT(
        runner, "Pending service rejects partial resident and target masks before draining", idle, root.HasValue(),
        originReady, mappingPrepared, begin.HasValue(), dispatched.HasValue(),
        dispatched.GetValue().QueuedProcessorMask == 0x02U, dispatched.GetValue().AcknowledgedProcessorMask == 0x01U,
        partial.State == Kernel::Memory::AddressSpaceState::ShootdownPending, partial.ResidentProcessorMask == 0x03U,
        partial.TargetProcessorMask == 0x03U, result == RuntimeError::TlbShootdownFixedRootRejected,
        failureAccess == RuntimeAccessError::None, failure.TlbShootdownPollAttempted == 1U,
        failure.TlbShootdownPollResult == Ia64PolledTlbShootdownPollResult::Pending,
        failure.TlbShootdownAddressSpaceBefore.State == Kernel::Memory::AddressSpaceState::ShootdownPending,
        failure.TlbShootdownAddressSpaceBefore.ResidentProcessorMask == 0x03U,
        failure.TlbShootdownAddressSpaceBefore.TargetProcessorMask == 0x03U,
        failure.TlbShootdownDrain.ReceivedCount == 0U, failurePoll == RuntimeFailurePoll::Available,
        firstFailure.Error == RuntimeError::TlbShootdownFixedRootRejected, servicePurgeCalls == 0U,
        serviceSerializationCalls == 0U);
    ASTERIA_EXPECT(runner, "the rejected partial-mask fixture is drained and returned to Ready before later admission",
                   cleanupReady, cleanup.ReceivedCount == 1U, cleanup.CompletedCount == 1U, cleanup.RejectedCount == 0U,
                   completed.HasValue(), afterCleanup.State == Kernel::Memory::AddressSpaceState::Ready,
                   afterCleanup.ResidentProcessorMask == 0x03U);
}

void TestTlbShootdownIdlePendingSuccess(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 ShootdownVirtualAddress{0x2000'0000'0500'1000ULL};
    const PollRange range{
        .Start = 0x1000U,
        .End = 0x1100U,
    };
    RuntimeFixture& fixture = *g_tlbShootdownPendingSuccessFixture;
    static InterruptEvidence processorOneEvidence{};
    static InterruptEvidence processorTwoEvidence{};
    ResetLocalTimerAdapter();
    const bool runtimeReady = PrepareTlbShootdownRuntime(fixture);
    const bool processorOneIdle = runtimeReady && AdvanceTlbProcessorToIdle(fixture, processorOneEvidence, 1U, range);
    const bool processorTwoIdle =
        processorOneIdle && AdvanceTlbProcessorToIdle(fixture, processorTwoEvidence, 2U, range);
    const bool idle = processorTwoIdle;
    const auto root = g_portableRuntimeArena.RuntimeMemory.BorrowReadyRoot();
    Ia64LocalTlbInvalidator originInvalidator;
    const bool invalidatorReady =
        originInvalidator.Initialize(PortableCapabilities()) == Ia64LocalTlbInvalidatorInitializationError::None;
    TlbShootdownDispatcher origin{Kernel::Scheduler::CpuId{0U}, originInvalidator};
    PinnedFrameMappingPreparation preparation;
    const MemoryObjectId memoryObject{g_portableRegistry, g_tlbShootdownMemoryObjectId};
    const FrameId frame{g_portableRegistry, g_tlbShootdownFrameObject};
    const bool mappingPrepared =
        idle && root.HasValue() &&
        PrepareTlbShootdownMapping(memoryObject, frame, ShootdownVirtualAddress, 0U, preparation);
    const AddressSpaceId rootIdentity = root.HasValue() ? root.GetValue().AddressSpaceIdentity : AddressSpaceId{};
    const auto begin = mappingPrepared
                           ? root.GetValue().AddressSpaceStorage->BeginPinnedFrameMapping(rootIdentity, preparation)
                           : decltype(root.GetValue().AddressSpaceStorage->BeginPinnedFrameMapping(
                                 rootIdentity, preparation))::Failure({});

    g_tlbPurgeCalls = 0U;
    g_tlbSerializationCalls = 0U;
    const auto dispatched =
        begin.HasValue() && invalidatorReady
            ? origin.Dispatch(*root.GetValue().AddressSpaceStorage, begin.GetValue(), fixture.GetMailbox())
            : decltype(origin.Dispatch(*root.GetValue().AddressSpaceStorage, begin.GetValue(),
                                       fixture.GetMailbox()))::Failure({});
    const RuntimeError processorOne =
        dispatched.HasValue() && idle
            ? fixture.GetRuntime().ServiceTlbShootdownFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
            : RuntimeError::RuntimeNotReady;
    const RuntimeError processorTwo =
        processorOne == RuntimeError::None
            ? fixture.GetRuntime().ServiceTlbShootdownFromIdle(Kernel::Scheduler::CpuId{2U}, 2U)
            : RuntimeError::RuntimeNotReady;
    const auto completed =
        (processorTwo == RuntimeError::None)
            ? root.GetValue().AddressSpaceStorage->CompleteMapping(begin.GetValue().GetToken())
            : decltype(root.GetValue().AddressSpaceStorage->CompleteMapping(begin.GetValue().GetToken()))::Failure({});
    RuntimePortableFailure processorOneFailure{};
    RuntimePortableFailure processorTwoFailure{};
    const auto notifier = fixture.GetNotifier().GetSnapshot();
    ASTERIA_EXPECT(runner, "both AP idle paths invalidate locally before the shared-root mapping can complete", idle,
                   invalidatorReady, mappingPrepared, begin.HasValue(), dispatched.HasValue(),
                   dispatched.GetValue().QueuedProcessorMask == 0x06U,
                   dispatched.GetValue().AcknowledgedProcessorMask == 0x01U, processorOne == RuntimeError::None,
                   processorTwo == RuntimeError::None, completed.HasValue(),
                   completed.GetValue().VirtualStart.GetValue() == ShootdownVirtualAddress, g_tlbPurgeCalls == 3U,
                   g_tlbSerializationCalls == 3U, notifier.PendingProcessorMask == 0U);
    ASTERIA_EXPECT(runner, "successful AP drains retain exact Pending, count, and acknowledgement evidence",
                   fixture.GetRuntime().GetPortableSchedulerFailure(Kernel::Scheduler::CpuId{1U},
                                                                    processorOneFailure) == RuntimeAccessError::None,
                   fixture.GetRuntime().GetPortableSchedulerFailure(Kernel::Scheduler::CpuId{2U},
                                                                    processorTwoFailure) == RuntimeAccessError::None,
                   processorOneFailure.TlbShootdownPollResult == Ia64PolledTlbShootdownPollResult::Pending,
                   processorTwoFailure.TlbShootdownPollResult == Ia64PolledTlbShootdownPollResult::Pending,
                   processorOneFailure.TlbShootdownDrain.ReceivedCount == 1U,
                   processorOneFailure.TlbShootdownDrain.CompletedCount == 1U,
                   processorTwoFailure.TlbShootdownDrain.ReceivedCount == 1U,
                   processorTwoFailure.TlbShootdownDrain.CompletedCount == 1U,
                   (processorOneFailure.TlbShootdownAddressSpaceAfter.AcknowledgedProcessorMask & 0x02U) != 0U,
                   (processorTwoFailure.TlbShootdownAddressSpaceAfter.AcknowledgedProcessorMask & 0x04U) != 0U);
}

/// Drains AP `processor`'s mailbox outside its runtime, as the AP's own pass
/// does when a notification arrives between its poll and its drain: the work
/// is handled and acknowledged, and the notification stays latched.
[[nodiscard]] TlbShootdownDrainSummary DrainBehindLatchedNotification(RuntimeFixture& fixture,
                                                                      const Foundation::UInt8 processor) noexcept
{
    Ia64LocalTlbInvalidator invalidator;
    if (invalidator.Initialize(PortableCapabilities()) != Ia64LocalTlbInvalidatorInitializationError::None)
    {
        return TlbShootdownDrainSummary{};
    }
    TlbShootdownDispatcher dispatcher{Kernel::Scheduler::CpuId{processor}, invalidator};
    TlbShootdownDrain drain{fixture.GetMailbox(), dispatcher};
    return drain.Drain();
}

[[nodiscard]] bool DrainedOne(const TlbShootdownDrainSummary& summary) noexcept
{
    return (summary.ReceivedCount == 1U) && (summary.CompletedCount == 1U) && (summary.RejectedCount == 0U);
}

/// Two back-to-back mappings on the shared root, with the interleavings the
/// notifier's contract allows: a notification whose work an earlier pass
/// already drained stays latched (Ia64PolledTlbShootdownNotifier.hpp), so an
/// AP polls Pending while the root is Ready, while its own acknowledgement
/// is already recorded, and while the next shootdown has begun but its work
/// is not yet published. Each of these passes finds no work and must succeed.
void TestTlbShootdownIdleAcceptsCoalescedNotifications(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 FirstVirtualAddress{0x2000'0000'0500'3000ULL};
    constexpr Foundation::UInt64 SecondVirtualAddress{0x2000'0000'0500'4000ULL};
    const PollRange range{
        .Start = 0x1000U,
        .End = 0x1100U,
    };
    RuntimeFixture& fixture = *g_tlbShootdownCoalescedFixture;
    static InterruptEvidence processorOneEvidence{};
    static InterruptEvidence processorTwoEvidence{};
    ResetLocalTimerAdapter();
    const bool idle = PrepareTlbShootdownRuntime(fixture) &&
                      AdvanceTlbProcessorToIdle(fixture, processorOneEvidence, 1U, range) &&
                      AdvanceTlbProcessorToIdle(fixture, processorTwoEvidence, 2U, range);
    const auto root = g_portableRuntimeArena.RuntimeMemory.BorrowReadyRoot();
    Ia64LocalTlbInvalidator originInvalidator;
    const bool originReady =
        originInvalidator.Initialize(PortableCapabilities()) == Ia64LocalTlbInvalidatorInitializationError::None;
    TlbShootdownDispatcher origin{Kernel::Scheduler::CpuId{0U}, originInvalidator};
    const MemoryObjectId memoryObject{g_portableRegistry, g_tlbShootdownMemoryObjectId};
    const FrameId frame{g_portableRegistry, g_tlbShootdownFrameObject};
    PinnedFrameMappingPreparation firstPreparation;
    PinnedFrameMappingPreparation secondPreparation;
    const bool prepared = idle && originReady && root.HasValue() &&
                          PrepareTlbShootdownMapping(memoryObject, frame, FirstVirtualAddress, 0U, firstPreparation) &&
                          PrepareTlbShootdownMapping(memoryObject, frame, SecondVirtualAddress, 1U, secondPreparation);
    ASTERIA_EXPECT(runner, "the coalescing fixture serves AP1 and AP2 and prepares two shared-root mappings", prepared);
    if (!prepared)
    {
        return;
    }
    Kernel::Memory::AddressSpace& space = *root.GetValue().AddressSpaceStorage;
    const AddressSpaceId identity = root.GetValue().AddressSpaceIdentity;
    const auto service = [&fixture](const Foundation::UInt8 processor) noexcept
    {
        return fixture.GetRuntime().ServiceTlbShootdownFromIdle(Kernel::Scheduler::CpuId{processor}, processor);
    };
    const auto record = [&fixture](const Foundation::UInt8 processor) noexcept
    {
        RuntimePortableFailure failure{};
        static_cast<void>(
            fixture.GetRuntime().GetPortableSchedulerFailure(Kernel::Scheduler::CpuId{processor}, failure));
        return failure;
    };
    g_tlbPurgeCalls = 0U;

    // The first mapping: both APs drain its work behind latched notifications
    // and the initiator completes it.
    const auto first = space.BeginPinnedFrameMapping(identity, firstPreparation);
    const bool firstPublished =
        first.HasValue() && origin.Dispatch(space, first.GetValue(), fixture.GetMailbox()).HasValue();
    const TlbShootdownDrainSummary firstOne =
        firstPublished ? DrainBehindLatchedNotification(fixture, 1U) : TlbShootdownDrainSummary{};
    const TlbShootdownDrainSummary firstTwo =
        firstPublished ? DrainBehindLatchedNotification(fixture, 2U) : TlbShootdownDrainSummary{};
    const bool firstCompleted =
        DrainedOne(firstOne) && DrainedOne(firstTwo) && space.CompleteMapping(first.GetValue().GetToken()).HasValue();

    // AP2 polls its leftover notification while the root is Ready.
    const RuntimeError afterCompletion = firstCompleted ? service(2U) : RuntimeError::RuntimeNotReady;
    const RuntimePortableFailure afterCompletionRecord = record(2U);

    // The back-to-back mapping begins; its work is not published yet when
    // AP1 polls its leftover notification.
    const auto second = firstCompleted
                            ? space.BeginPinnedFrameMapping(identity, secondPreparation)
                            : decltype(space.BeginPinnedFrameMapping(identity, secondPreparation))::Failure({});
    const Kernel::Memory::AddressSpaceSnapshot begun = space.GetSnapshot();
    const RuntimeError beforePublication = second.HasValue() ? service(1U) : RuntimeError::RuntimeNotReady;
    const RuntimePortableFailure beforePublicationRecord = record(1U);

    // Its publication: AP1's runtime pass drains it; AP2's work is drained
    // behind its latched notification, which AP2 then polls with its own
    // acknowledgement already recorded.
    const bool secondPublished =
        second.HasValue() && origin.Dispatch(space, second.GetValue(), fixture.GetMailbox()).HasValue();
    const RuntimeError published = secondPublished ? service(1U) : RuntimeError::RuntimeNotReady;
    const RuntimePortableFailure publishedRecord = record(1U);
    const TlbShootdownDrainSummary secondTwo =
        secondPublished ? DrainBehindLatchedNotification(fixture, 2U) : TlbShootdownDrainSummary{};
    const RuntimeError afterAcknowledgement = DrainedOne(secondTwo) ? service(2U) : RuntimeError::RuntimeNotReady;
    const RuntimePortableFailure afterAcknowledgementRecord = record(2U);
    const bool secondCompleted =
        (afterAcknowledgement == RuntimeError::None) && space.CompleteMapping(second.GetValue().GetToken()).HasValue();

    const RuntimeError finalOne = service(1U);
    const RuntimeError finalTwo = service(2U);
    RuntimeFailure failure{};
    const RuntimeFailurePoll failurePoll = fixture.GetRuntime().PollFirstFailure(failure);
    const Kernel::Memory::AddressSpaceSnapshot after = space.GetSnapshot();
    ASTERIA_EXPECT(runner, "a leftover notification is served while the root is Ready", firstCompleted,
                   afterCompletion == RuntimeError::None,
                   afterCompletionRecord.TlbShootdownPollResult == Ia64PolledTlbShootdownPollResult::Pending,
                   afterCompletionRecord.TlbShootdownAddressSpaceBefore.State ==
                       Kernel::Memory::AddressSpaceState::Ready,
                   afterCompletionRecord.TlbShootdownDrain.ReceivedCount == 0U);
    ASTERIA_EXPECT(runner, "a leftover notification is served while the next shootdown awaits publication",
                   second.HasValue(), begun.State == Kernel::Memory::AddressSpaceState::ShootdownPending,
                   (begun.AcknowledgedProcessorMask & 0x02U) == 0U, beforePublication == RuntimeError::None,
                   beforePublicationRecord.TlbShootdownDrain.ReceivedCount == 0U,
                   beforePublicationRecord.TlbShootdownAddressSpaceAfter.ShootdownSequence == begun.ShootdownSequence,
                   (beforePublicationRecord.TlbShootdownAddressSpaceAfter.AcknowledgedProcessorMask & 0x02U) == 0U);
    ASTERIA_EXPECT(
        runner,
        "the published back-to-back shootdown is drained, and a leftover is served after the AP's acknowledgement",
        secondPublished, published == RuntimeError::None, DrainedOne(publishedRecord.TlbShootdownDrain),
        DrainedOne(secondTwo), afterAcknowledgement == RuntimeError::None,
        (afterAcknowledgementRecord.TlbShootdownAddressSpaceBefore.AcknowledgedProcessorMask & 0x04U) != 0U,
        afterAcknowledgementRecord.TlbShootdownDrain.ReceivedCount == 0U, secondCompleted);
    ASTERIA_EXPECT(runner, "both APs keep serving with no pending notification and no failure",
                   finalOne == RuntimeError::None, finalTwo == RuntimeError::None,
                   failurePoll == RuntimeFailurePoll::None,
                   fixture.GetNotifier().GetSnapshot().PendingProcessorMask == 0U,
                   after.State == Kernel::Memory::AddressSpaceState::Ready, g_tlbPurgeCalls == 6U);

    // Common progress can consume the mailbox before the polled notification, and the initiator can
    // then change roots.
    const auto transition = space.ReserveProcessorTransition(Kernel::Scheduler::CpuId{0U});
    if (transition.HasValue())
    {
        fixture.GetNotifier().Notify(0x02U);
    }
    const RuntimeError duringTransition = transition.HasValue() ? service(1U) : RuntimeError::RuntimeNotReady;
    const AddressSpaceError released =
        transition.HasValue() ? space.ReleaseProcessorTransition(transition.GetValue()) : AddressSpaceError::NotReady;
    ASTERIA_EXPECT(runner, "an empty latched notification survives another CPU's root transition",
                   transition.HasValue(), duringTransition == RuntimeError::None, released == AddressSpaceError::None);
}

void PrepareTlbShootdownTerminalFixture(TestContext& runner) noexcept
{
    constexpr PollRange range{
        .Start = 0x1000U,
        .End = 0x1100U,
    };
    ResetLocalTimerAdapter();
    g_tlbShootdownTerminalIdleReady =
        PrepareTlbShootdownRuntime(*g_tlbShootdownTerminalFixture) &&
        AdvanceTlbProcessorToIdle(*g_tlbShootdownTerminalFixture, g_tlbShootdownTerminalEvidence, 1U, range) &&
        AdvanceTlbProcessorToIdle(*g_tlbShootdownTerminalFixture, g_tlbShootdownTerminalProcessorTwoEvidence, 2U,
                                  range);
    ASTERIA_EXPECT(runner, "the terminal TLB drain fixture reaches fixed-root IdleServing before registry saturation",
                   g_tlbShootdownTerminalIdleReady);
}

void TestTlbShootdownIdleDrainFailureIsTerminal(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 ShootdownVirtualAddress{0x2000'0000'0500'2000ULL};
    RuntimeFixture& fixture = *g_tlbShootdownTerminalFixture;
    const bool idle = g_tlbShootdownTerminalIdleReady;
    const auto root = g_portableRuntimeArena.RuntimeMemory.BorrowReadyRoot();
    Ia64LocalTlbInvalidator originInvalidator;
    const bool invalidatorReady =
        originInvalidator.Initialize(PortableCapabilities()) == Ia64LocalTlbInvalidatorInitializationError::None;
    TlbShootdownDispatcher origin{Kernel::Scheduler::CpuId{0U}, originInvalidator};
    PinnedFrameMappingPreparation preparation;
    const MemoryObjectId memoryObject{g_portableRegistry, g_tlbShootdownMemoryObjectId};
    const FrameId frame{g_portableRegistry, g_tlbShootdownFrameObject};
    const bool mappingPrepared =
        root.HasValue() && PrepareTlbShootdownMapping(memoryObject, frame, ShootdownVirtualAddress, 1U, preparation);
    const auto begin = mappingPrepared ? root.GetValue().AddressSpaceStorage->BeginPinnedFrameMapping(
                                             root.GetValue().AddressSpaceIdentity, preparation)
                                       : decltype(root.GetValue().AddressSpaceStorage->BeginPinnedFrameMapping(
                                             root.GetValue().AddressSpaceIdentity, preparation))::Failure({});
    const auto dispatched =
        begin.HasValue() && invalidatorReady
            ? origin.Dispatch(*root.GetValue().AddressSpaceStorage, begin.GetValue(), fixture.GetMailbox())
            : decltype(origin.Dispatch(*root.GetValue().AddressSpaceStorage, begin.GetValue(),
                                       fixture.GetMailbox()))::Failure({});

    // A stray report acknowledges AP1 without its work being handled, so the
    // initiator completes the shootdown while AP1's work item is still
    // Published. AP1's runtime pass then receives work for a settled shootdown; its refused
    // acknowledgement makes the bounded drain reject.
    const bool installed =
        dispatched.HasValue() &&
        (root.GetValue().AddressSpaceStorage->AcknowledgeShootdown(
             begin.GetValue().GetToken(), Kernel::Scheduler::CpuId{1U}) == AddressSpaceError::None) &&
        DrainedOne(DrainBehindLatchedNotification(fixture, 2U)) &&
        root.GetValue().AddressSpaceStorage->CompleteMapping(begin.GetValue().GetToken()).HasValue();
    const RuntimeError result = installed && idle
                                    ? fixture.GetRuntime().ServiceTlbShootdownFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
                                    : RuntimeError::RuntimeNotReady;
    RuntimeFailure first{};
    RuntimeFailure retained{};
    RuntimePortableFailure failure{};
    const RuntimeFailurePoll firstPoll = fixture.GetRuntime().PollFirstFailure(first);
    if (installed)
    {
        static_cast<void>(fixture.GetRuntime().ServiceTlbShootdownFromIdle(Kernel::Scheduler::CpuId{1U}, 1U));
    }
    const RuntimeFailurePoll retainedPoll = fixture.GetRuntime().PollFirstFailure(retained);
    ASTERIA_EXPECT(runner, "a rejected bounded drain fail-stops once and retains its exact first-failure evidence",
                   idle, mappingPrepared, begin.HasValue(), dispatched.HasValue(), installed,
                   result == RuntimeError::TlbShootdownDrainRejected, firstPoll == RuntimeFailurePoll::Available,
                   retainedPoll == RuntimeFailurePoll::Available, FailureEquals(first, retained),
                   first.Operation == RuntimeOperation::ServiceTlbShootdown,
                   fixture.GetRuntime().GetPortableSchedulerFailure(Kernel::Scheduler::CpuId{1U}, failure) ==
                       RuntimeAccessError::None,
                   failure.TlbShootdownPollResult == Ia64PolledTlbShootdownPollResult::Pending,
                   failure.TlbShootdownDrain.ReceivedCount == 1U, failure.TlbShootdownDrain.CompletedCount == 0U,
                   failure.TlbShootdownDrain.RejectedCount == 1U,
                   failure.TlbShootdownDrain.ReceiveError == TlbShootdownReceiveError::None,
                   failure.TlbShootdownDrain.FirstRemoteError == TlbShootdownError::AcknowledgementRejected,
                   failure.TlbShootdownDrain.FirstAddressSpaceError == AddressSpaceError::InvalidShootdownToken,
                   failure.TlbShootdownAddressSpaceBefore.State == Kernel::Memory::AddressSpaceState::Ready);
}

void TestCanonicalTopologies(TestContext& runner) noexcept
{
    for (Foundation::UInt8 processorCount{1U}; processorCount <= Kernel::Scheduler::MaximumProcessorCount;
         ++processorCount)
    {
        RuntimeFixture fixture;
        const bool configured = fixture.Configure(processorCount);
        const auto prepared = fixture.GetRuntime().Prepare(fixture.GetPlan(), PhysicalAddressBits,
                                                           VirtualAddressMostSignificantBit, fixture.GetCoordinator());
        ASTERIA_EXPECT(runner, "SMP1-SMP4 canonical plans prepare successfully", configured, prepared.HasValue());
        if (!configured || !prepared.HasValue())
        {
            continue;
        }

        const auto& summary = prepared.GetValue();
        ASTERIA_EXPECT(runner, "each topology publishes the exact immutable summary",
                       summary.ProcessorCount == processorCount,
                       summary.EnabledProcessorMask == ExpectedMask(processorCount),
                       summary.PreparedContextCount == static_cast<Foundation::UInt8>(processorCount - 1U),
                       summary.CommonPhysicalAddressBits == PhysicalAddressBits,
                       summary.CommonImplementedVirtualAddressMostSignificantBit == VirtualAddressMostSignificantBit,
                       summary.State == RuntimeState::Ready);
        ASTERIA_EXPECT(runner, "every canonical topology reaches Ready",
                       fixture.GetRuntime().GetState() == RuntimeState::Ready);

        Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeLayoutPlan retainedPlan{};
        ASTERIA_EXPECT(runner, "the complete plan is copied into boot-lifetime storage",
                       fixture.GetRuntime().GetPlan(retainedPlan) == RuntimeAccessError::None,
                       retainedPlan.ProcessorCount == processorCount,
                       retainedPlan.LayoutCount == static_cast<Foundation::UInt8>(processorCount - 1U));

        RuntimeFailure failure{};
        ASTERIA_EXPECT(runner, "canonical preparation publishes no failure",
                       fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::None);

        if (processorCount == 1U)
        {
            RuntimeProcessorSnapshot absent{};
            ASTERIA_EXPECT(runner, "SMP1 has canonical zero descriptor and no AP context",
                           summary.WorkerEntryPoint == 0U, summary.WorkerEntryGlobalPointer == 0U,
                           fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, absent) ==
                               RuntimeAccessError::InvalidProcessor);
            continue;
        }

        ASTERIA_EXPECT(runner, "SMP2-SMP4 retain a nonzero raw worker bundle address", summary.WorkerEntryPoint != 0U);
        RuntimePortableFailure unpublishedFailure{};
        ASTERIA_EXPECT(runner, "stable sequence zero returns the value-initialized portable failure",
                       fixture.GetRuntime().GetPortableSchedulerFailure(Kernel::Scheduler::CpuId{1U},
                                                                        unpublishedFailure) == RuntimeAccessError::None,
                       unpublishedFailure.RunLoop.ThreadStorage == nullptr,
                       unpublishedFailure.RunLoop.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::None,
                       unpublishedFailure.Controller.Error == Kernel::Scheduler::ProcessorRunError::None);
        for (Foundation::UInt8 processor{1U}; processor < processorCount; ++processor)
        {
            RuntimeProcessorSnapshot snapshot{};
            const Layout& layout = retainedPlan.Processors[processor - 1U];
            const bool snapshotReady = fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{processor},
                                                                                 snapshot) == RuntimeAccessError::None;
            ASTERIA_EXPECT(runner, "each AP retains its exact CPU, generation, and initial stage", snapshotReady,
                           snapshot.Processor.GetValue() == processor, snapshot.Generation == processor,
                           snapshot.State == RuntimeProcessorState::AwaitingRelease, snapshot.EvidenceMask == 0U);
            ASTERIA_EXPECT(runner, "worker contexts use only the exact Region-1 16-KiB aliases", snapshotReady,
                           snapshot.WorkerContext.State == KernelContextState::Prepared,
                           snapshot.WorkerContext.MemoryStackVirtualStart ==
                               LayoutPlanner::GetWorkerAliasVirtualStart(layout),
                           snapshot.WorkerContext.MemoryStackSizeBytes ==
                               Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeWorkerStackSizeBytes,
                           snapshot.WorkerContext.RseBackingStoreVirtualStart ==
                               LayoutPlanner::GetWorkerRseBackingStoreVirtualStart(layout),
                           snapshot.WorkerContext.RseBackingStoreSizeBytes ==
                               Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeWorkerRseSizeBytes,
                           snapshot.WorkerContext.ArgumentAddress != 0U);
            ASTERIA_EXPECT(runner, "the pivoted current idle context remains fresh until SwitchTo", snapshotReady,
                           snapshot.IdleContext.State == KernelContextState::Fresh);
        }
    }
}

void TestPreparationRejection(TestContext& runner) noexcept
{
    {
        RuntimeFixture fixture;
        const bool configured = fixture.Configure(2U);
        fixture.GetPlan().Reserved[0U] = 1U;
        const auto result = fixture.GetRuntime().Prepare(fixture.GetPlan(), PhysicalAddressBits,
                                                         VirtualAddressMostSignificantBit, fixture.GetCoordinator());
        ASTERIA_EXPECT(runner, "a nonzero plan reserved byte is a terminal preparation failure", configured,
                       !result.HasValue(), result.GetErrorReference().Error == RuntimeError::InvalidLayout,
                       fixture.GetRuntime().GetState() == RuntimeState::Failed);
    }
    {
        RuntimeFixture fixture;
        const bool configured = fixture.Configure(3U);
        fixture.GetPlan().Processors[1U].SlotVirtualBase += 0x1000U;
        const auto result = fixture.GetRuntime().Prepare(fixture.GetPlan(), PhysicalAddressBits,
                                                         VirtualAddressMostSignificantBit, fixture.GetCoordinator());
        ASTERIA_EXPECT(runner, "a mutated slot base is rejected before context preparation", configured,
                       !result.HasValue(), result.GetErrorReference().Error == RuntimeError::InvalidLayout);
    }
    {
        RuntimeFixture fixture;
        const bool configured = fixture.Configure(2U);
        fixture.GetPlan().Processors[0U].WorkerPhysicalBase = fixture.GetPlan().Processors[0U].IdlePhysicalBase;
        const auto result = fixture.GetRuntime().Prepare(fixture.GetPlan(), PhysicalAddressBits,
                                                         VirtualAddressMostSignificantBit, fixture.GetCoordinator());
        ASTERIA_EXPECT(runner, "overlapping physical aliases are rejected", configured, !result.HasValue(),
                       result.GetErrorReference().Error == RuntimeError::InvalidLayout);
    }
    {
        RuntimeFixture fixture;
        const bool configured = fixture.Configure(1U);
        fixture.GetPlan().Processors[0U].Processor = Kernel::Scheduler::CpuId{0U};
        const auto result = fixture.GetRuntime().Prepare(fixture.GetPlan(), PhysicalAddressBits,
                                                         VirtualAddressMostSignificantBit, fixture.GetCoordinator());
        ASTERIA_EXPECT(runner, "SMP1 rejects a noncanonical inactive layout slot", configured, !result.HasValue(),
                       result.GetErrorReference().Error == RuntimeError::InvalidLayout);
    }
    {
        Coordinator uninitializedCoordinator;
        Runtime runtime;
        const LayoutPlan plan = MakePlan(2U);
        const auto result =
            runtime.Prepare(plan, PhysicalAddressBits, VirtualAddressMostSignificantBit, uninitializedCoordinator);
        ASTERIA_EXPECT(runner, "an uninitialized release coordinator is rejected", !result.HasValue(),
                       result.GetErrorReference().Error == RuntimeError::ReleaseCoordinatorRejected);
    }
    {
        RuntimeFixture fixture;
        const bool configured = fixture.Configure(2U);
        const auto released = fixture.GetCoordinator().Release(Kernel::Scheduler::CpuId{1U}, 1U);
        const auto result = fixture.GetRuntime().Prepare(fixture.GetPlan(), PhysicalAddressBits,
                                                         VirtualAddressMostSignificantBit, fixture.GetCoordinator());
        ASTERIA_EXPECT(runner, "preparation rejects a coordinator whose first release began", configured,
                       released.HasValue(), !result.HasValue(),
                       result.GetErrorReference().Error == RuntimeError::ReleaseCoordinatorRejected);
    }
    {
        RuntimeFixture fixture;
        const bool prepared = fixture.Configure(1U) && fixture.Prepare();
        const auto duplicate = fixture.GetRuntime().Prepare(fixture.GetPlan(), PhysicalAddressBits,
                                                            VirtualAddressMostSignificantBit, fixture.GetCoordinator());
        RuntimeFailure failure{};
        ASTERIA_EXPECT(runner, "duplicate Prepare is nonmutating after canonical readiness", prepared,
                       !duplicate.HasValue(), duplicate.GetErrorReference().Error == RuntimeError::AlreadyPrepared,
                       fixture.GetRuntime().GetState() == RuntimeState::Ready,
                       fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::None);
    }
    {
        RuntimeFixture fixture;
        const bool configured = fixture.Configure(2U);
        const auto result =
            fixture.GetRuntime().Prepare(fixture.GetPlan(), PhysicalAddressBits, 49U, fixture.GetCoordinator());
        ASTERIA_EXPECT(runner, "an unsupported common virtual MSB is rejected", configured, !result.HasValue(),
                       result.GetErrorReference().Error == RuntimeError::InvalidAddressProfile);
    }
}

void TestPhysicalTransitions(TestContext& runner) noexcept
{
    {
        RuntimeFixture fixture;
        const bool prepared = fixture.Configure(2U) && fixture.Prepare();
        const bool claimed = prepared && ReleaseAndClaim(fixture);
        RuntimeProcessorSnapshot snapshot{};
        ASTERIA_EXPECT(runner, "an exact BSP token is claimed once on the physical side", claimed,
                       fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, snapshot) ==
                           RuntimeAccessError::None,
                       snapshot.State == RuntimeProcessorState::PhysicalClaimed);
        const Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeRelease duplicateReceipt{
            .Generation = 1U,
            .Processor = Kernel::Scheduler::CpuId{1U},
            .Reserved = {},
        };
        const RuntimeError duplicate =
            claimed ? fixture.GetRuntime().ClaimPhysicalRelease(duplicateReceipt) : RuntimeError::RuntimeNotReady;
        RuntimeFailure failure{};
        ASTERIA_EXPECT(runner, "a physical release claim is single-use",
                       duplicate == RuntimeError::InvalidProcessorState,
                       fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available,
                       failure.ObservedState == RuntimeProcessorState::PhysicalClaimed);
    }
    {
        RuntimeFixture fixture;
        const bool prepared = fixture.Configure(2U) && fixture.Prepare();
        const auto released = fixture.GetCoordinator().Release(Kernel::Scheduler::CpuId{1U}, 1U);
        Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeRelease corrupted =
            released.HasValue() ? released.GetValue()
                                : Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeRelease{};
        corrupted.Reserved[0U] = 1U;
        const RuntimeError error = prepared && released.HasValue()
                                       ? fixture.GetRuntime().ClaimPhysicalRelease(corrupted)
                                       : RuntimeError::RuntimeNotReady;
        RuntimeFailure failure{};
        ASTERIA_EXPECT(runner, "a corrupt release receipt fails before coordinator claim",
                       error == RuntimeError::NonzeroReserved,
                       fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available,
                       failure.Error == RuntimeError::NonzeroReserved);
    }
    {
        RuntimeFixture fixture;
        const bool prepared = fixture.Configure(2U) && fixture.Prepare();
        const auto released = fixture.GetCoordinator().Release(Kernel::Scheduler::CpuId{1U}, 1U);
        Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeRelease wrongGeneration{
            .Generation = 2U,
            .Processor = Kernel::Scheduler::CpuId{1U},
            .Reserved = {},
        };
        const RuntimeError error = prepared && released.HasValue()
                                       ? fixture.GetRuntime().ClaimPhysicalRelease(wrongGeneration)
                                       : RuntimeError::RuntimeNotReady;
        RuntimeFailure failure{};
        ASTERIA_EXPECT(
            runner, "a foreign generation is retained as the first failure", error == RuntimeError::InvalidGeneration,
            fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available, failure.Detail == 2U);
    }
    {
        RuntimeFixture fixture;
        const bool prepared = fixture.Configure(2U) && fixture.Prepare();
        const Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeRelease unreleased{
            .Generation = 1U,
            .Processor = Kernel::Scheduler::CpuId{1U},
            .Reserved = {},
        };
        const RuntimeError error =
            prepared ? fixture.GetRuntime().ClaimPhysicalRelease(unreleased) : RuntimeError::RuntimeNotReady;
        RuntimeFailure failure{};
        ASTERIA_EXPECT(runner, "owner validation cannot manufacture a missing BSP release",
                       error == RuntimeError::ReleaseClaimRejected,
                       fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available,
                       failure.ReleaseError == CoordinatorTransitionError::ProcessorNotReleased);
    }
    {
        RuntimeFixture fixture;
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        constexpr Foundation::UInt64 RootFailureDetail{0x1234U};
        const RuntimeError first = claimed ? fixture.GetRuntime().PublishPhysicalFailure(
                                                 Kernel::Scheduler::CpuId{1U}, 1U,
                                                 RuntimePhysicalFailure::RootAdmissionRejected, RootFailureDetail)
                                           : RuntimeError::RuntimeNotReady;
        RuntimeFailure retained{};
        const RuntimeFailurePoll poll = fixture.GetRuntime().PollFirstFailure(retained);
        const RuntimeError second = fixture.GetRuntime().PublishPhysicalFailure(
            Kernel::Scheduler::CpuId{1U}, 1U, RuntimePhysicalFailure::PivotRejected, 0x5678U);
        RuntimeFailure afterSecond{};
        const RuntimeFailurePoll secondPoll = fixture.GetRuntime().PollFirstFailure(afterSecond);
        ASTERIA_EXPECT(runner, "physical failure publication is first-writer-wins",
                       first == RuntimeError::PhysicalRootAdmissionRejected, poll == RuntimeFailurePoll::Available,
                       retained.Detail == RootFailureDetail,
                       retained.Operation == RuntimeOperation::PublishPhysicalFailure,
                       second == RuntimeError::InvalidProcessorState, secondPoll == RuntimeFailurePoll::Available,
                       FailureEquals(retained, afterSecond));
    }
}

void TestPortableSchedulerConfigurationAndOrder(TestContext& runner) noexcept
{
    {
        Runtime runtime;
        ASTERIA_EXPECT(runner, "portable dependencies cannot precede runtime preparation",
                       runtime.ConfigurePortableSchedulerDependencies(
                           g_portableRuntimeArena.RuntimeMemory, g_portableRegistry, g_portableResourceIdentity,
                           g_portableResourceDomain, g_portableIssuer) == RuntimeError::RuntimeNotReady);
    }
    {
        RuntimeFixture fixture;
        SpinLock foreignLock;
        Asteria::TestSupport::TestObjectRegistry foreignRegistry{foreignLock};
        RootCapabilityIssuer foreignIssuer{foreignRegistry};
        const bool prepared = fixture.Configure(2U) && fixture.Prepare();
        const RuntimeError rejected = prepared
                                          ? fixture.GetRuntime().ConfigurePortableSchedulerDependencies(
                                                g_portableRuntimeArena.RuntimeMemory, foreignRegistry,
                                                g_portableResourceIdentity, g_portableResourceDomain, foreignIssuer)
                                          : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "a registry-incoherent portable dependency graph is rejected",
                       rejected == RuntimeError::PortableSchedulerDependenciesRejected);
    }
    {
        RuntimeFixture fixture;
        const bool prepared = fixture.Configure(2U) && fixture.Prepare();
        const RuntimeError configured =
            prepared ? ConfigurePortableDependencies(fixture) : RuntimeError::RuntimeNotReady;
        const RuntimeError repeated =
            configured == RuntimeError::None ? ConfigurePortableDependencies(fixture) : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "a coherent dependency graph is retained exactly once", configured == RuntimeError::None,
                       repeated == RuntimeError::PortableSchedulerAlreadyConfigured);
    }
    {
        RuntimeFixture fixture;
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        const RuntimeError configured =
            claimed ? ConfigurePortableDependencies(fixture) : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "portable dependencies are immutable once AP release is claimed",
                       configured == RuntimeError::PortableSchedulerDependenciesRejected);
    }
    {
        RuntimeFixture fixture;
        ResetLocalTimerAdapter();
        InterruptEvidence evidence{};
        constexpr PollRange range{.Start = 0x5100U, .End = 0x5200U};
        const bool proofCompleted = fixture.Configure(2U) && fixture.Prepare() &&
                                    AdvanceProcessorToRuntimeEntered(fixture, evidence, 1U) &&
                                    CompleteProof(fixture, evidence, 1U, range);
        const RuntimeError error = proofCompleted
                                       ? fixture.GetRuntime().PreparePortableScheduler(Kernel::Scheduler::CpuId{1U}, 1U)
                                       : RuntimeError::RuntimeNotReady;
        RuntimeFailure failure{};
        ASTERIA_EXPECT(runner, "portable scheduler preparation requires retained dependencies",
                       error == RuntimeError::PortableSchedulerNotConfigured,
                       fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available,
                       failure.Operation == RuntimeOperation::PreparePortableScheduler);
    }
    {
        RuntimeFixture fixture;
        ResetLocalTimerAdapter();
        InterruptEvidence evidence{};
        const bool entered = fixture.Configure(2U) && fixture.Prepare() &&
                             (ConfigurePortableDependencies(fixture) == RuntimeError::None) &&
                             AdvanceProcessorToRuntimeEntered(fixture, evidence, 1U);
        const RuntimeError error = entered
                                       ? fixture.GetRuntime().PreparePortableScheduler(Kernel::Scheduler::CpuId{1U}, 1U)
                                       : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "scheduler construction cannot overtake the local-timer proof",
                       error == RuntimeError::LocalTimerProofNotCompleted);
    }
    {
        RuntimeFixture fixture;
        ResetLocalTimerAdapter();
        InterruptEvidence evidence{};
        constexpr PollRange range{.Start = 0x5300U, .End = 0x5400U};
        const bool proofCompleted = fixture.Configure(2U) && fixture.Prepare() &&
                                    (ConfigurePortableDependencies(fixture) == RuntimeError::None) &&
                                    AdvanceProcessorToRuntimeEntered(fixture, evidence, 1U) &&
                                    CompleteProof(fixture, evidence, 1U, range);
        const RuntimeError error =
            proofCompleted ? fixture.GetRuntime().DispatchPortableSchedulerFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
                           : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "dispatch cannot overtake fixed-storage scheduler preparation",
                       error == RuntimeError::PortableSchedulerPostconditionRejected);
    }
}

void TestPortableSchedulerRoundTripAndIsolation(TestContext& runner) noexcept
{
    {
        RuntimeFixture& returnedFixture = *g_returnedRuntimeFixture;
        static InterruptEvidence returnedEvidence{};
        constexpr PollRange returnedRange{
            .Start = 0x5900U,
            .End = 0x5A00U,
        };
        ResetLocalTimerAdapter();
        const bool returnedProof = returnedFixture.Configure(2U) && returnedFixture.Prepare() &&
                                   (ConfigurePortableDependencies(returnedFixture) == RuntimeError::None) &&
                                   AdvanceProcessorToRuntimeEntered(returnedFixture, returnedEvidence, 1U) &&
                                   CompleteProof(returnedFixture, returnedEvidence, 1U, returnedRange) &&
                                   (returnedFixture.GetRuntime().PreparePortableScheduler(Kernel::Scheduler::CpuId{1U},
                                                                                          1U) == RuntimeError::None);
        ResetSwitchAdapter();
        g_workerSwitchEnabled.store(false, std::memory_order_release);
        const RuntimeError returnedDispatch =
            returnedProof
                ? returnedFixture.GetRuntime().DispatchPortableSchedulerFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
                : RuntimeError::RuntimeNotReady;
        RuntimeProcessorSnapshot returnedSnapshot{};
        ASTERIA_EXPECT(runner, "an immediately returning native switch is rejected without fallback",
                       returnedDispatch == RuntimeError::PortableSchedulerDispatchRejected,
                       g_switchCount.load(std::memory_order_acquire) == 1U,
                       returnedFixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U},
                                                                         returnedSnapshot) == RuntimeAccessError::None,
                       returnedSnapshot.State == RuntimeProcessorState::Failed,
                       returnedSnapshot.PortableScheduler.Phase == RuntimePortablePhase::Failed);
    }

    RuntimeFixture& fixture = *g_persistentRuntimeFixture;
    static InterruptEvidence firstEvidence{};
    static InterruptEvidence secondEvidence{};
    constexpr PollRange firstRange{.Start = 0x6100U, .End = 0x6200U};
    constexpr PollRange secondRange{.Start = 0x6300U, .End = 0x6400U};
    ResetLocalTimerAdapter();
    const bool configured =
        fixture.Configure(3U) && fixture.Prepare() && (ConfigurePortableDependencies(fixture) == RuntimeError::None);
    RuntimeProcessorSnapshot beforeWaiting{};
    const bool beforeWaitingReady =
        configured && (fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, beforeWaiting) ==
                       RuntimeAccessError::None);
    const auto earlyRequest =
        fixture.GetRuntime().RequestPortableSchedulerService(Kernel::Scheduler::CpuId{1U}, 1U, 0x1111U);
    RuntimeProcessorSnapshot afterEarlyRequest{};
    const bool earlyRequestSnapshotReady =
        fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, afterEarlyRequest) ==
        RuntimeAccessError::None;
    ASTERIA_EXPECT(
        runner, "a pre-wait service request is retryable and mutation-free", beforeWaitingReady,
        !earlyRequest.HasValue(), earlyRequest.GetError() == RuntimeError::PortableSchedulerServiceUnavailable,
        earlyRequestSnapshotReady, afterEarlyRequest.Service.RequestSequence == beforeWaiting.Service.RequestSequence,
        afterEarlyRequest.Service.CompletionSequence == beforeWaiting.Service.CompletionSequence,
        afterEarlyRequest.Service.RequestCookie == beforeWaiting.Service.RequestCookie,
        afterEarlyRequest.Service.State == beforeWaiting.Service.State);

    const bool firstProof = configured && AdvanceProcessorToRuntimeEntered(fixture, firstEvidence, 1U) &&
                            CompleteProof(fixture, firstEvidence, 1U, firstRange);
    RuntimeProcessorSnapshot proofSnapshot{};
    const bool proofOrdered = firstProof &&
                              (fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, proofSnapshot) ==
                               RuntimeAccessError::None) &&
                              (proofSnapshot.State == RuntimeProcessorState::RuntimeEntered) &&
                              (proofSnapshot.LocalTimerProof.Phase == RuntimeProofPhase::Completed) &&
                              (proofSnapshot.PortableScheduler.Phase == RuntimePortablePhase::Unconfigured);
    const RuntimeError prepared = proofOrdered
                                      ? fixture.GetRuntime().PreparePortableScheduler(Kernel::Scheduler::CpuId{1U}, 1U)
                                      : RuntimeError::RuntimeNotReady;
    RuntimeProcessorSnapshot preparedSnapshot{};
    const bool schedulerPrepared = prepared == RuntimeError::None &&
                                   (fixture.GetRuntime().GetProcessorSnapshot(
                                        Kernel::Scheduler::CpuId{1U}, preparedSnapshot) == RuntimeAccessError::None) &&
                                   (preparedSnapshot.State == RuntimeProcessorState::RuntimeEntered) &&
                                   (preparedSnapshot.PortableScheduler.Phase == RuntimePortablePhase::Prepared);
    ResetSwitchAdapter();
    const RuntimeError dispatched =
        schedulerPrepared ? fixture.GetRuntime().DispatchPortableSchedulerFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
                          : RuntimeError::RuntimeNotReady;
    const Foundation::Size firstSwitchCount = g_switchCount.load(std::memory_order_acquire);
    RuntimeProcessorSnapshot waitingSnapshot{};
    const bool waitingSnapshotReady = dispatched == RuntimeError::None &&
                                      (fixture.GetRuntime().GetProcessorSnapshot(
                                           Kernel::Scheduler::CpuId{1U}, waitingSnapshot) == RuntimeAccessError::None);
    ASTERIA_EXPECT(runner, "the first worker run blocks and publishes SchedulerWaiting", waitingSnapshotReady,
                   waitingSnapshot.State == RuntimeProcessorState::SchedulerWaiting,
                   waitingSnapshot.PortableScheduler.Phase == RuntimePortablePhase::Waiting,
                   waitingSnapshot.EvidenceMask == 3U, waitingSnapshot.Service.RequestSequence == 0U,
                   waitingSnapshot.Service.CompletionSequence == 0U, waitingSnapshot.Service.DispatchCount == 1U,
                   waitingSnapshot.Service.WakeCount == 0U, waitingSnapshot.Service.ServiceCount == 0U,
                   waitingSnapshot.Service.State == RuntimeServiceState::Idle, firstSwitchCount == 2U);

    const RuntimeError emptyService =
        waitingSnapshotReady ? fixture.GetRuntime().ServicePortableSchedulerFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
                             : RuntimeError::RuntimeNotReady;
    RuntimeProcessorSnapshot afterEmptyService{};
    ASTERIA_EXPECT(runner, "an empty AP idle poll is successful and mutation-free", emptyService == RuntimeError::None,
                   fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, afterEmptyService) ==
                       RuntimeAccessError::None,
                   afterEmptyService.State == waitingSnapshot.State,
                   afterEmptyService.Service.RequestSequence == waitingSnapshot.Service.RequestSequence,
                   afterEmptyService.Service.CompletionSequence == waitingSnapshot.Service.CompletionSequence,
                   afterEmptyService.Service.DispatchCount == waitingSnapshot.Service.DispatchCount,
                   afterEmptyService.Service.WakeCount == waitingSnapshot.Service.WakeCount);

    const auto invalidProcessorRequest =
        fixture.GetRuntime().RequestPortableSchedulerService(Kernel::Scheduler::CpuId{0U}, 1U, 0x2222U);
    const auto invalidGenerationRequest =
        fixture.GetRuntime().RequestPortableSchedulerService(Kernel::Scheduler::CpuId{1U}, 2U, 0x3333U);
    const auto firstRequest =
        fixture.GetRuntime().RequestPortableSchedulerService(Kernel::Scheduler::CpuId{1U}, 1U, 0xA501U);
    const auto duplicateRequest =
        fixture.GetRuntime().RequestPortableSchedulerService(Kernel::Scheduler::CpuId{1U}, 1U, 0xA5FFU);
    RuntimeProcessorSnapshot pendingSnapshot{};
    const bool pendingSnapshotReady = fixture.GetRuntime().GetProcessorSnapshot(
                                          Kernel::Scheduler::CpuId{1U}, pendingSnapshot) == RuntimeAccessError::None;
    ASTERIA_EXPECT(
        runner, "request validation publishes exactly one outstanding AP sequence", !invalidProcessorRequest.HasValue(),
        invalidProcessorRequest.GetError() == RuntimeError::InvalidProcessor, !invalidGenerationRequest.HasValue(),
        invalidGenerationRequest.GetError() == RuntimeError::InvalidGeneration, firstRequest.HasValue(),
        firstRequest.GetValue().Sequence == 1U, firstRequest.GetValue().Cookie == 0xA501U,
        firstRequest.GetValue().Processor.GetValue() == 1U, !duplicateRequest.HasValue(),
        duplicateRequest.GetError() == RuntimeError::PortableSchedulerServiceAlreadyPending, pendingSnapshotReady,
        pendingSnapshot.Service.RequestSequence == 1U, pendingSnapshot.Service.CompletionSequence == 0U,
        pendingSnapshot.Service.RequestCookie == 0xA501U,
        pendingSnapshot.Service.State == RuntimeServiceState::Pending);

    const RuntimeError firstService =
        firstRequest.HasValue()
            ? fixture.GetRuntime().ServicePortableSchedulerFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
            : RuntimeError::RuntimeNotReady;
    RuntimeProcessorSnapshot firstOnline{};
    const bool firstOnlineReady = firstService == RuntimeError::None &&
                                  (fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U},
                                                                             firstOnline) == RuntimeAccessError::None);
    const auto firstServing = fixture.GetCoordinator().ObserveServing(Kernel::Scheduler::CpuId{1U}, 1U);
    RuntimePortableFailure firstFailure{};
    const bool firstFailureReady = fixture.GetRuntime().GetPortableSchedulerFailure(
                                       Kernel::Scheduler::CpuId{1U}, firstFailure) == RuntimeAccessError::None;
    const auto sharedRoot = g_portableRuntimeArena.RuntimeMemory.BorrowReadyRoot();

    ASTERIA_EXPECT(runner, "the retained worker resumes for the first cooperative service", configured, proofOrdered,
                   schedulerPrepared, dispatched == RuntimeError::None, waitingSnapshotReady,
                   firstService == RuntimeError::None, firstServing.HasValue());
    ASTERIA_EXPECT(
        runner, "first service publishes immutable blocked-worker and serving evidence", firstOnlineReady,
        firstOnline.State == RuntimeProcessorState::IdleServing, firstOnline.EvidenceMask == 15U,
        EvidenceEquals(firstOnline.RuntimeEntryEvidence,
                       MakeEvidence(fixture.GetPlan().Processors[0U], EvidencePlacement::Idle, 1U)),
        firstOnline.FirstWorkerEntryEvidence.StackPointer != 0U,
        firstOnline.ResumedWorkerEntryEvidence.StackPointer != 0U, firstOnline.ServingIdleEvidence.StackPointer != 0U,
        firstOnline.PortableScheduler.Phase == RuntimePortablePhase::Online,
        firstOnline.PortableScheduler.ThreadState == Kernel::Scheduler::ThreadState::Blocked,
        firstOnline.PortableScheduler.InitialRunSequence == 1U,
        firstOnline.PortableScheduler.FirstResumedRunSequence == 2U,
        firstOnline.PortableScheduler.InitialBlockToken != 0U, firstOnline.PortableScheduler.ServingBlockToken != 0U,
        firstOnline.PortableScheduler.InitialBlockToken != firstOnline.PortableScheduler.ServingBlockToken,
        firstOnline.PortableScheduler.SchedulingContextState == Kernel::Scheduler::SchedulingContextState::Bound,
        firstOnline.PortableScheduler.ControllerState == Kernel::Scheduler::ProcessorRunState::Idle,
        firstOnline.PortableScheduler.RunLoopState == Kernel::Scheduler::ProcessorKernelRunLoopState::Idle,
        firstOnline.PortableScheduler.DeadlineState == Kernel::Time::ProcessorDeadlineState::Disarmed,
        firstOnline.PortableScheduler.LocalTimerState == Platform::Ia64::Time::Ia64LocalTimerState::Ready,
        firstOnline.PortableScheduler.TimerQueueScheduledEntries == 0U,
        firstOnline.PortableScheduler.TimerQueuePendingEntries == 0U,
        firstOnline.PortableScheduler.TimerQueueClaimedEntries == 0U,
        firstOnline.PortableScheduler.RunLoopBindingCount == 1U);
    ASTERIA_EXPECT(runner, "first-online service counters include initial and resumed dispatches", firstOnlineReady,
                   firstOnline.Service.RequestSequence == 1U, firstOnline.Service.CompletionSequence == 1U,
                   firstOnline.Service.RequestCookie == 0xA501U, firstOnline.Service.CompletionCookie == 0xA501U,
                   firstOnline.Service.CompletionEvidenceValue == firstOnline.ResumedWorkerEntryEvidence.StackPointer,
                   firstOnline.Service.CompletionEvidenceValue != 0U, firstOnline.Service.CompletionRunSequence == 2U,
                   firstOnline.Service.DispatchCount == 2U, firstOnline.Service.WakeCount == 1U,
                   firstOnline.Service.ServiceCount == 1U, firstOnline.Service.State == RuntimeServiceState::Idle,
                   firstOnline.Service.Result == RuntimeServiceResult::Completed);
    ASTERIA_EXPECT(
        runner, "coordinator observation matches the runtime's four serving values", firstServing.HasValue(),
        firstServing.GetValue().Evidence.RuntimeEntryValue == firstOnline.RuntimeEntryEvidence.StackPointer,
        firstServing.GetValue().Evidence.FirstWorkerEntryValue == firstOnline.FirstWorkerEntryEvidence.StackPointer,
        firstServing.GetValue().Evidence.ResumedWorkerEntryValue == firstOnline.ResumedWorkerEntryEvidence.StackPointer,
        firstServing.GetValue().Evidence.ServingIdleValue == firstOnline.ServingIdleEvidence.StackPointer);
    ASTERIA_EXPECT(runner, "the idle return is exactly the canonical no-runnable result", firstFailureReady,
                   firstFailure.RunLoop.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::ControllerRejected,
                   firstFailure.RunLoop.ControllerError == Kernel::Scheduler::ProcessorRunError::NoRunnableThread,
                   firstFailure.RunLoop.SchedulerError == Kernel::Scheduler::ProcessorSchedulerError::NoRunnableThread,
                   firstFailure.RunLoop.DeadlineError == Kernel::Time::ProcessorDeadlineError::None,
                   firstFailure.RunLoop.DeadlineReceipt.State == Kernel::Time::ProcessorDeadlineState::Disarmed,
                   firstFailure.RunLoop.DeadlineReceipt.SelectedDeadlineTicks == 0U,
                   firstFailure.RunLoop.DeadlineReceipt.SelectedSources == 0U,
                   firstFailure.RunLoop.ActiveRunRetained == 0U);
    ASTERIA_EXPECT(
        runner, "the AP scheduler serves on the exact admitted shared root", sharedRoot.HasValue(), firstOnlineReady,
        firstOnline.PortableScheduler.AddressSpaceIdentity.IsEqualTo(sharedRoot.GetValue().AddressSpaceIdentity),
        firstOnline.PortableScheduler.Processor.GetValue() == 1U);

    const auto secondRequest =
        fixture.GetRuntime().RequestPortableSchedulerService(Kernel::Scheduler::CpuId{1U}, 1U, 0xA502U);
    const RuntimeError secondService =
        secondRequest.HasValue()
            ? fixture.GetRuntime().ServicePortableSchedulerFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
            : RuntimeError::RuntimeNotReady;
    RuntimeProcessorSnapshot afterSecondService{};
    const bool secondServiceReady = secondService == RuntimeError::None &&
                                    (fixture.GetRuntime().GetProcessorSnapshot(
                                         Kernel::Scheduler::CpuId{1U}, afterSecondService) == RuntimeAccessError::None);
    const RuntimeProcessorSnapshot immutableFirstOnline = afterSecondService;
    ASTERIA_EXPECT(
        runner, "a second request resumes the same worker and preserves first-online evidence",
        secondRequest.HasValue(), secondRequest.GetValue().Sequence == 2U, secondServiceReady,
        afterSecondService.State == RuntimeProcessorState::IdleServing,
        afterSecondService.Service.RequestSequence == 2U, afterSecondService.Service.CompletionSequence == 2U,
        afterSecondService.Service.RequestCookie == 0xA502U, afterSecondService.Service.CompletionCookie == 0xA502U,
        afterSecondService.Service.CompletionRunSequence == 3U, afterSecondService.Service.DispatchCount == 3U,
        afterSecondService.Service.WakeCount == 2U, afterSecondService.Service.ServiceCount == 2U,
        afterSecondService.Service.State == RuntimeServiceState::Idle,
        PortableRecordEquals(afterSecondService.PortableScheduler, firstOnline.PortableScheduler),
        EvidenceEquals(afterSecondService.ResumedWorkerEntryEvidence, firstOnline.ResumedWorkerEntryEvidence),
        EvidenceEquals(afterSecondService.ServingIdleEvidence, firstOnline.ServingIdleEvidence));

    ResetLocalTimerAdapter();
    const bool secondProof = secondServiceReady && AdvanceProcessorToRuntimeEntered(fixture, secondEvidence, 2U) &&
                             CompleteProof(fixture, secondEvidence, 2U, secondRange);
    const RuntimeError secondPrepared =
        secondProof ? fixture.GetRuntime().PreparePortableScheduler(Kernel::Scheduler::CpuId{2U}, 2U)
                    : RuntimeError::RuntimeNotReady;
    const RuntimeError secondDispatched =
        secondPrepared == RuntimeError::None
            ? fixture.GetRuntime().DispatchPortableSchedulerFromIdle(Kernel::Scheduler::CpuId{2U}, 2U)
            : RuntimeError::RuntimeNotReady;
    const auto peerRequest =
        secondDispatched == RuntimeError::None
            ? fixture.GetRuntime().RequestPortableSchedulerService(Kernel::Scheduler::CpuId{2U}, 2U, 0xB501U)
            : Foundation::Result<Platform::Ia64::Scheduler::Ia64ApplicationProcessorPortableSchedulerServiceRequest,
                                 RuntimeError>::Failure(RuntimeError::RuntimeNotReady);
    const RuntimeError peerService =
        peerRequest.HasValue() ? fixture.GetRuntime().ServicePortableSchedulerFromIdle(Kernel::Scheduler::CpuId{2U}, 2U)
                               : RuntimeError::RuntimeNotReady;
    const auto peerServing =
        peerService == RuntimeError::None
            ? fixture.GetCoordinator().ObserveServing(Kernel::Scheduler::CpuId{2U}, 2U)
            : Foundation::Result<Platform::Ia64::Interrupt::Ia64ApplicationProcessorRuntimeServingObservation,
                                 CoordinatorTransitionError>::Failure(CoordinatorTransitionError::ServingPending);
    RuntimeProcessorSnapshot firstAfterPeer{};
    RuntimeProcessorSnapshot peerSnapshot{};
    const bool isolationSnapshots =
        fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, firstAfterPeer) ==
            RuntimeAccessError::None &&
        fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{2U}, peerSnapshot) ==
            RuntimeAccessError::None;
    ASTERIA_EXPECT(runner, "independent AP mailboxes start at sequence one and preserve peer state",
                   secondDispatched == RuntimeError::None, peerRequest.HasValue(),
                   peerRequest.GetValue().Sequence == 1U, peerService == RuntimeError::None, peerServing.HasValue(),
                   isolationSnapshots, peerSnapshot.State == RuntimeProcessorState::IdleServing,
                   peerSnapshot.Service.RequestSequence == 1U, peerSnapshot.Service.CompletionSequence == 1U,
                   peerSnapshot.Service.CompletionRunSequence == 2U, peerSnapshot.Service.DispatchCount == 2U,
                   peerSnapshot.Service.WakeCount == 1U, peerSnapshot.Service.ServiceCount == 1U,
                   firstAfterPeer.State == immutableFirstOnline.State,
                   firstAfterPeer.EvidenceMask == immutableFirstOnline.EvidenceMask,
                   PortableRecordEquals(firstAfterPeer.PortableScheduler, immutableFirstOnline.PortableScheduler),
                   firstAfterPeer.Service.RequestSequence == immutableFirstOnline.Service.RequestSequence,
                   firstAfterPeer.Service.CompletionSequence == immutableFirstOnline.Service.CompletionSequence,
                   firstAfterPeer.Service.CompletionCookie == immutableFirstOnline.Service.CompletionCookie);

    constexpr Foundation::Size concurrentPollLimit{1'000'000U};
    std::atomic<bool> stopService{false};
    std::atomic<bool> stopObservation{false};
    std::atomic<Foundation::UInt64> serviceError{static_cast<Foundation::UInt64>(RuntimeError::None)};
    std::atomic<Foundation::Size> idlePollCount{0U};
    std::atomic<Foundation::Size> failureSnapshotCount{0U};
    std::atomic<bool> failureSnapshotsValid{true};
    std::thread serviceThread{
        [&fixture, &stopService, &serviceError, &idlePollCount]() noexcept
        {
            while (!stopService.load(std::memory_order_acquire))
            {
                const RuntimeError error =
                    fixture.GetRuntime().ServicePortableSchedulerFromIdle(Kernel::Scheduler::CpuId{1U}, 1U);
                if (error != RuntimeError::None)
                {
                    serviceError.store(static_cast<Foundation::UInt64>(error), std::memory_order_release);
                    return;
                }
                idlePollCount.fetch_add(1U, std::memory_order_release);
                std::this_thread::yield();
            }
        }};
    std::thread observationThread{
        [&fixture, &stopObservation, &failureSnapshotCount, &failureSnapshotsValid]() noexcept
        {
            while (!stopObservation.load(std::memory_order_acquire))
            {
                RuntimePortableFailure failure{};
                const RuntimeAccessError access =
                    fixture.GetRuntime().GetPortableSchedulerFailure(Kernel::Scheduler::CpuId{1U}, failure);
                if (access == RuntimeAccessError::None)
                {
                    if (!PortableFailureIsCanonicalIdle(failure))
                    {
                        failureSnapshotsValid.store(false, std::memory_order_release);
                        return;
                    }
                    failureSnapshotCount.fetch_add(1U, std::memory_order_release);
                }
                else if (access != RuntimeAccessError::PublicationPending)
                {
                    failureSnapshotsValid.store(false, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
            }
        }};

    bool serviceThreadStarted{false};
    for (Foundation::Size attempt{0U}; attempt < concurrentPollLimit; ++attempt)
    {
        if ((idlePollCount.load(std::memory_order_acquire) != 0U) &&
            (failureSnapshotCount.load(std::memory_order_acquire) != 0U))
        {
            serviceThreadStarted = true;
            break;
        }
        std::this_thread::yield();
    }

    const auto concurrentRequest =
        fixture.GetRuntime().RequestPortableSchedulerService(Kernel::Scheduler::CpuId{1U}, 1U, 0xA503U);
    RuntimeProcessorSnapshot concurrentCompletion{};
    bool concurrentCompleted{false};
    for (Foundation::Size attempt{0U}; attempt < concurrentPollLimit; ++attempt)
    {
        if ((fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, concurrentCompletion) ==
             RuntimeAccessError::None) &&
            (concurrentCompletion.Service.CompletionSequence == 3U) &&
            (concurrentCompletion.Service.State == RuntimeServiceState::Idle))
        {
            concurrentCompleted = true;
            break;
        }
        std::this_thread::yield();
    }

    g_workerResumeObserved.store(false, std::memory_order_release);
    g_holdWorkerResume.store(true, std::memory_order_release);
    const auto heldRequest =
        fixture.GetRuntime().RequestPortableSchedulerService(Kernel::Scheduler::CpuId{1U}, 1U, 0xA504U);
    bool resumeHeld{false};
    for (Foundation::Size attempt{0U}; attempt < concurrentPollLimit; ++attempt)
    {
        if (g_workerResumeObserved.load(std::memory_order_acquire))
        {
            resumeHeld = true;
            break;
        }
        std::this_thread::yield();
    }

    RuntimeProcessorSnapshot executingSnapshot{};
    const bool executingObserved =
        resumeHeld &&
        (fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, executingSnapshot) ==
         RuntimeAccessError::None) &&
        (executingSnapshot.Service.State == RuntimeServiceState::Executing) &&
        (executingSnapshot.Service.RequestSequence == 4U) && (executingSnapshot.Service.CompletionSequence == 3U);
    const auto executingDuplicate =
        fixture.GetRuntime().RequestPortableSchedulerService(Kernel::Scheduler::CpuId{1U}, 1U, 0xA5FFU);
    g_holdWorkerResume.store(false, std::memory_order_release);

    RuntimeProcessorSnapshot heldCompletion{};
    bool heldCompleted{false};
    for (Foundation::Size attempt{0U}; attempt < concurrentPollLimit; ++attempt)
    {
        if ((fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, heldCompletion) ==
             RuntimeAccessError::None) &&
            (heldCompletion.Service.CompletionSequence == 4U) &&
            (heldCompletion.Service.State == RuntimeServiceState::Idle))
        {
            heldCompleted = true;
            break;
        }
        std::this_thread::yield();
    }

    stopObservation.store(true, std::memory_order_release);
    stopService.store(true, std::memory_order_release);
    observationThread.join();
    serviceThread.join();
    ASTERIA_EXPECT(
        runner,
        "concurrent AP polling preserves published requests, rejects an executing duplicate, and exposes only coherent diagnostics",
        serviceThreadStarted, concurrentRequest.HasValue(), concurrentRequest.GetValue().Sequence == 3U,
        concurrentCompleted, concurrentCompletion.Service.CompletionCookie == 0xA503U, heldRequest.HasValue(),
        heldRequest.GetValue().Sequence == 4U, executingObserved, !executingDuplicate.HasValue(),
        executingDuplicate.GetError() == RuntimeError::PortableSchedulerServiceAlreadyPending, heldCompleted,
        heldCompletion.Service.CompletionCookie == 0xA504U, heldCompletion.Service.CompletionRunSequence == 5U,
        heldCompletion.Service.DispatchCount == 5U, heldCompletion.Service.WakeCount == 4U,
        heldCompletion.Service.ServiceCount == 4U,
        serviceError.load(std::memory_order_acquire) == static_cast<Foundation::UInt64>(RuntimeError::None),
        failureSnapshotsValid.load(std::memory_order_acquire),
        failureSnapshotCount.load(std::memory_order_acquire) != 0U);

    // A mailbox cannot complete 2^64 services, so the request sequence
    // admission boundary is exercised on the runtime's allocation rule.
    Foundation::UInt64 lastAccepted{0U};
    const bool acceptsLast = TryAdvancePortableSchedulerServiceSequence(MaximumUInt64 - 3U, lastAccepted);
    Foundation::UInt64 exhausted{0xA5A5U};
    const bool acceptsExhausted = TryAdvancePortableSchedulerServiceSequence(MaximumUInt64 - 2U, exhausted);
    Foundation::UInt64 first{0U};
    const bool acceptsFirst = TryAdvancePortableSchedulerServiceSequence(0U, first);
    ASTERIA_EXPECT(
        runner,
        "the final admissible sequence is maximum minus two and the next request is rejected without publication",
        acceptsFirst, first == 1U, acceptsLast, lastAccepted == MaximumUInt64 - 2U, !acceptsExhausted,
        exhausted == 0xA5A5U);
}

void TestTimerDeliveryTransaction(TestContext& runner) noexcept
{
    {
        // The completion window between the Completed/Delivered stores and
        // the completion sequence exists only while an AP publishes, so the
        // snapshot's coherence rule is exercised directly on its words.
        constexpr Foundation::UInt64 executing{static_cast<Foundation::UInt64>(RuntimeTimerDeliveryState::Executing)};
        constexpr Foundation::UInt64 completed{static_cast<Foundation::UInt64>(RuntimeTimerDeliveryState::Completed)};
        constexpr Foundation::UInt64 unknownState{static_cast<Foundation::UInt64>(RuntimeTimerDeliveryState::Failed) +
                                                  1U};
        constexpr Foundation::UInt64 noResult{static_cast<Foundation::UInt64>(RuntimeTimerDeliveryResult::None)};
        constexpr Foundation::UInt64 delivered{static_cast<Foundation::UInt64>(RuntimeTimerDeliveryResult::Delivered)};
        RuntimeFixture fixture;
        RuntimeProcessorSnapshot snapshot{};
        const bool prepared = fixture.Configure(2U) && fixture.Prepare();
        const RuntimeAccessError stable =
            prepared ? fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, snapshot)
                     : RuntimeAccessError::NotReady;
        ASTERIA_EXPECT(runner, "timer completion publication never exposes Completed without its immutable record",
                       !TimerDeliveryPublicationIsCoherent(0U, completed, 0U, delivered),
                       !TimerDeliveryPublicationIsCoherent(0U, completed, 1U, delivered),
                       !TimerDeliveryPublicationIsCoherent(1U, executing, 1U, delivered),
                       !TimerDeliveryPublicationIsCoherent(0U, unknownState, 0U, noResult),
                       !TimerDeliveryPublicationIsCoherent(2U, completed, 1U, delivered),
                       TimerDeliveryPublicationIsCoherent(0U, executing, 1U, noResult),
                       TimerDeliveryPublicationIsCoherent(1U, completed, 1U, delivered), prepared,
                       stable == RuntimeAccessError::None, snapshot.TimerDelivery.CompletionSequence == 0U,
                       snapshot.TimerDelivery.State == RuntimeTimerDeliveryState::Ready,
                       snapshot.TimerDelivery.Result == RuntimeTimerDeliveryResult::None);
    }

    RuntimeFixture& fixture = *g_timerDeliveryRuntimeFixture;
    static InterruptEvidence evidence{};
    constexpr PollRange proofRange{.Start = 0x7100U, .End = 0x7200U};
    constexpr PollRange deliveryRange{
        .Start = 0x7300U,
        .End = 0x7400U,
    };
    const bool online = ReachFirstIdleServing(fixture, evidence, proofRange, 0xC501U);
    RuntimeProcessorSnapshot beforeRequest{};
    const bool beforeReady = online && (fixture.GetRuntime().GetProcessorSnapshot(
                                            Kernel::Scheduler::CpuId{1U}, beforeRequest) == RuntimeAccessError::None);
    const auto invalidProcessor = fixture.GetRuntime().RequestTimerDelivery(Kernel::Scheduler::CpuId{0U}, 1U);
    const auto invalidGeneration = fixture.GetRuntime().RequestTimerDelivery(Kernel::Scheduler::CpuId{1U}, 2U);
    const auto unacknowledged = fixture.GetRuntime().RequestTimerDelivery(Kernel::Scheduler::CpuId{1U}, 1U);
    RuntimeProcessorSnapshot afterRejected{};
    const bool rejectedStable = fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U},
                                                                          afterRejected) == RuntimeAccessError::None;
    const auto acknowledged =
        online ? Platform::Ia64::Interrupt::AcknowledgeIa64LocalTimerInterruptEvidence(evidence, 1U)
               : Platform::Ia64::Interrupt::Ia64LocalTimerInterruptAcknowledgementError::InvalidSequence;
    const auto requested =
        acknowledged == Platform::Ia64::Interrupt::Ia64LocalTimerInterruptAcknowledgementError::None
            ? fixture.GetRuntime().RequestTimerDelivery(Kernel::Scheduler::CpuId{1U}, 1U)
            : Foundation::Result<Platform::Ia64::Scheduler::Ia64ApplicationProcessorTimerDeliveryRequest,
                                 RuntimeError>::Failure(RuntimeError::RuntimeNotReady);
    const auto duplicate = fixture.GetRuntime().RequestTimerDelivery(Kernel::Scheduler::CpuId{1U}, 1U);
    const auto conflictingService =
        fixture.GetRuntime().RequestPortableSchedulerService(Kernel::Scheduler::CpuId{1U}, 1U, 0xC5FFU);
    const RuntimeError idleService =
        fixture.GetRuntime().ServicePortableSchedulerFromIdle(Kernel::Scheduler::CpuId{1U}, 1U);
    RuntimeProcessorSnapshot pendingSnapshot{};
    const bool pendingReady = fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U},
                                                                        pendingSnapshot) == RuntimeAccessError::None;
    ASTERIA_EXPECT(
        runner, "the one-shot gate validates ack1 and excludes duplicate or scheduler publication", beforeReady,
        beforeRequest.TimerDelivery.State == RuntimeTimerDeliveryState::Ready, !invalidProcessor.HasValue(),
        invalidProcessor.GetError() == RuntimeError::InvalidProcessor, !invalidGeneration.HasValue(),
        invalidGeneration.GetError() == RuntimeError::InvalidGeneration, !unacknowledged.HasValue(),
        unacknowledged.GetError() == RuntimeError::TimerDeliveryEvidenceRejected, rejectedStable,
        afterRejected.TimerDelivery.RequestSequence == 0U, afterRejected.TimerDelivery.CompletionSequence == 0U,
        afterRejected.TimerDelivery.State == RuntimeTimerDeliveryState::Ready, requested.HasValue(),
        requested.GetValue().Sequence == 1U, requested.GetValue().Processor.GetValue() == 1U, !duplicate.HasValue(),
        duplicate.GetError() == RuntimeError::TimerDeliveryAlreadyRequested, !conflictingService.HasValue(),
        conflictingService.GetError() == RuntimeError::TimerDeliveryGateConflict, idleService == RuntimeError::None,
        pendingReady, pendingSnapshot.TimerDelivery.RequestSequence == 1U,
        pendingSnapshot.TimerDelivery.CompletionSequence == 0U,
        pendingSnapshot.TimerDelivery.State == RuntimeTimerDeliveryState::Pending,
        pendingSnapshot.Service.RequestSequence == 1U, pendingSnapshot.Service.CompletionSequence == 1U);

    SetLocalTimerTicks(10'000U, 10'001U, 10'002U, 10'003U);
    const RuntimeError begun = requested.HasValue()
                                   ? fixture.GetRuntime().BeginTimerDeliveryFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
                                   : RuntimeError::RuntimeNotReady;
    const Foundation::UInt64 deliveryDeadline = g_programmedDeadline;
    evidence = MakeTimerDeliveryInterruptEvidence(1U, deliveryDeadline, deliveryRange);
    SetLocalTimerTicks(deliveryDeadline, deliveryDeadline + 1U, deliveryDeadline + 2U, deliveryDeadline + 3U);
    const RuntimeError completed = begun == RuntimeError::None
                                       ? fixture.GetRuntime().CompleteTimerDeliveryFromIdle(
                                             Kernel::Scheduler::CpuId{1U}, 1U, PollStatus::Completed, deliveryRange)
                                       : RuntimeError::RuntimeNotReady;
    RuntimeProcessorSnapshot completedSnapshot{};
    const bool completionReady = completed == RuntimeError::None &&
                                 (fixture.GetRuntime().GetProcessorSnapshot(
                                      Kernel::Scheduler::CpuId{1U}, completedSnapshot) == RuntimeAccessError::None);
    const auto& record = completedSnapshot.TimerDeliveryRecord;
    ASTERIA_EXPECT(
        runner, "timer completion preserves exact registration, arm, queue, and hardware-deadline evidence",
        completionReady, completedSnapshot.TimerDelivery.RequestSequence == 1U,
        completedSnapshot.TimerDelivery.CompletionSequence == 1U,
        completedSnapshot.TimerDelivery.State == RuntimeTimerDeliveryState::Completed,
        completedSnapshot.TimerDelivery.Result == RuntimeTimerDeliveryResult::Delivered,
        record.Result == RuntimeTimerDeliveryResult::Delivered, record.Processor.GetValue() == 1U,
        record.EvidenceAddress == AddressOf(&evidence), record.TimerStorageAddress != 0U,
        record.NotificationStorageAddress != 0U, record.TimerStorageAddress != record.NotificationStorageAddress,
        record.Registration.Registration.IsValid(), record.Registration.Registration.GetSequence() == 1U,
        record.Registration.TimerIdentity.IsValid(), record.Registration.NotificationIdentity.IsValid(),
        record.Arm.Timer.Token.IsValid(), record.Arm.Timer.Token.GetSequence() == 1U,
        record.Arm.Timer.DeadlineTicks == deliveryDeadline, record.Arm.NextDeadlineTicks == deliveryDeadline,
        record.ArmedQueue.NextDeadlineTicks == deliveryDeadline, record.ArmedQueue.NextClaimSequence == 1U,
        record.ArmedQueue.ScheduledEntries == 1U, record.ArmedQueue.PendingEntries == 0U,
        record.ArmedQueue.ClaimedEntries == 0U, record.ArmedDeadline.SelectedDeadlineTicks == deliveryDeadline,
        record.ArmedDeadline.ObservedTicks >= record.Arm.Timer.ObservedTicks,
        record.ArmedDeadline.State == Kernel::Time::ProcessorDeadlineState::Armed,
        record.ArmedDeadline.SelectedSources ==
            static_cast<Foundation::UInt8>(Kernel::Time::ProcessorDeadlineSource::TimerQueue),
        record.ArmedDeadline.Reprogrammed == 1U, record.ArmedLocalTimer.Armed == 1U);
    ASTERIA_EXPECT(runner, "sequence two remains outstanding and contains exact E8 poll-leaf evidence", completionReady,
                   record.InterruptEvidence.Sequence == 2U, record.InterruptEvidence.AcknowledgedSequence == 1U,
                   record.InterruptEvidence.ProgrammedDeadlineTicks == deliveryDeadline,
                   record.InterruptEvidence.ObservedIntervalTimeCounter >= deliveryDeadline,
                   record.InterruptEvidence.ProcessorIndex == 1U,
                   record.InterruptEvidence.Vector == Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector,
                   record.InterruptEvidence.InterruptedIfsValid == 0U,
                   record.PollCodeRange.Start == deliveryRange.Start, record.PollCodeRange.End == deliveryRange.End,
                   record.PollStatus == PollStatus::Completed, record.PollDeadlineTicks > deliveryDeadline,
                   evidence.Sequence == 2U, evidence.AcknowledgedSequence == 1U);
    ASTERIA_EXPECT(
        runner, "the retained drain delivers once and commits the exact notification transaction", completionReady,
        record.Drain.DeliveredCount == 1U, record.Drain.Stop == Kernel::Time::DeferredTimerDrainStop::Empty,
        record.Drain.FirstClaimSequence == 1U, record.Drain.LastClaimSequence == 1U,
        record.Delivery.TimerIdentity.IsEqualTo(record.Registration.TimerIdentity),
        record.Delivery.NotificationIdentity.IsEqualTo(record.Registration.NotificationIdentity),
        record.Delivery.Expiration.Token.IsEqualTo(record.Arm.Timer.Token),
        record.Delivery.Expiration.ObservedTicks >= record.InterruptEvidence.ObservedIntervalTimeCounter,
        record.Delivery.NotificationPublished == 1U, record.Delivery.QueueCommitted == 1U,
        record.Delivery.NotificationState.State == Kernel::Ipc::NotificationState::Ready,
        record.Delivery.NotificationState.Pending == 1U, record.NotificationClaim.IsValid(),
        record.NotificationClaim.GetSequence() == 1U,
        record.ClaimedNotification.State == Kernel::Ipc::NotificationState::Claimed,
        record.ClaimedNotification.Pending == 0U,
        record.FinalNotification.State == Kernel::Ipc::NotificationState::Ready, record.FinalNotification.Pending == 0U,
        record.FinalTimer.State == Kernel::Time::TimerState::Disarmed, record.FinalWorker.RegisteredBindings == 1U,
        record.FinalWorker.QueueState.NextDeadlineTicks == 0U, record.FinalWorker.QueueState.NextClaimSequence == 2U,
        record.FinalWorker.QueueState.ScheduledEntries == 0U, record.FinalWorker.QueueState.PendingEntries == 0U,
        record.FinalWorker.QueueState.ClaimedEntries == 0U);
    ASTERIA_EXPECT(
        runner, "deadline reconciliation leaves the scheduler worker blocked and all service counters unchanged",
        completionReady, record.FinalDeadline.State == Kernel::Time::ProcessorDeadlineState::Disarmed,
        record.FinalDeadline.SelectedDeadlineTicks == 0U, record.FinalDeadline.SelectedSources == 0U,
        record.FinalDeadline.Reprogrammed == 1U,
        record.FinalDeadline.ObservedTicks >= record.Delivery.Expiration.ObservedTicks,
        record.FinalLocalTimer.State == Platform::Ia64::Time::Ia64LocalTimerState::Ready,
        record.FinalLocalTimer.Armed == 0U, record.FinalLocalTimer.ProgrammedDeadlineTicks == 0U,
        record.FinalController.State == Kernel::Scheduler::ProcessorRunState::Idle,
        record.FinalController.Scheduler.State == Kernel::Scheduler::ProcessorSchedulerState::Idle,
        record.FinalController.Scheduler.Queue.NextSequence == 3U,
        record.FinalController.Scheduler.Queue.ReadyEntries == 0U,
        record.FinalController.Scheduler.Queue.ClaimedEntries == 0U,
        record.FinalController.Scheduler.Queue.ReservedEntries == 0U,
        !record.FinalController.Scheduler.ActiveThread.IsValid(),
        !record.FinalController.Scheduler.ActiveContext.IsValid(),
        record.FinalController.TimerQueue.NextClaimSequence == 2U,
        record.FinalController.Deadline.State == Kernel::Time::ProcessorDeadlineState::Disarmed,
        record.FinalRunLoop.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Idle,
        record.FinalThread.State == Kernel::Scheduler::ThreadState::Blocked, record.FinalThread.RunSequence == 2U,
        record.FinalSchedulingContext.State == Kernel::Scheduler::SchedulingContextState::Bound,
        record.FinalSchedulingContext.BaseThread.IsEqualTo(completedSnapshot.PortableScheduler.ThreadIdentity),
        record.FinalSchedulingContext.CurrentThread.IsEqualTo(completedSnapshot.PortableScheduler.ThreadIdentity),
        !record.FinalSchedulingContext.RunningProcessor.IsValid(), record.FinalSchedulingContext.RunStartTicks == 0U,
        record.FinalSchedulingContext.RemainingBudgetTicks != 0U,
        record.FinalSchedulingContext.NextReplenishmentTicks == record.FinalSchedulingContext.AbsoluteDeadlineTicks,
        record.FinalSchedulingContext.DonationDepth == 0U, record.FinalSchedulingContext.IsBudgetExhausted == 0U,
        record.FinalService.RequestSequence == 1U, record.FinalService.CompletionSequence == 1U,
        record.FinalService.DispatchCount == 2U, record.FinalService.WakeCount == 1U,
        record.FinalService.ServiceCount == 1U);

    const auto secondTimerRequest = fixture.GetRuntime().RequestTimerDelivery(Kernel::Scheduler::CpuId{1U}, 1U);
    const auto preAcknowledgementService =
        fixture.GetRuntime().RequestPortableSchedulerService(Kernel::Scheduler::CpuId{1U}, 1U, 0xC5FEU);
    const auto acknowledgedCompletion =
        Platform::Ia64::Interrupt::AcknowledgeIa64LocalTimerInterruptEvidence(evidence, 2U);
    const auto laterService =
        fixture.GetRuntime().RequestPortableSchedulerService(Kernel::Scheduler::CpuId{1U}, 1U, 0xC502U);
    const RuntimeError laterServiceResult =
        laterService.HasValue()
            ? fixture.GetRuntime().ServicePortableSchedulerFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
            : RuntimeError::RuntimeNotReady;
    RuntimeProcessorSnapshot afterLaterService{};
    ASTERIA_EXPECT(
        runner, "post-timer service remains gated until sequence two is stably acknowledged",
        !secondTimerRequest.HasValue(), secondTimerRequest.GetError() == RuntimeError::TimerDeliveryAlreadyRequested,
        !preAcknowledgementService.HasValue(),
        preAcknowledgementService.GetError() == RuntimeError::TimerDeliveryGateConflict,
        acknowledgedCompletion == Platform::Ia64::Interrupt::Ia64LocalTimerInterruptAcknowledgementError::None,
        laterService.HasValue(), laterService.GetValue().Sequence == 2U, laterServiceResult == RuntimeError::None,
        fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, afterLaterService) ==
            RuntimeAccessError::None,
        afterLaterService.Service.CompletionSequence == 2U, afterLaterService.TimerDelivery.CompletionSequence == 1U,
        afterLaterService.TimerDeliveryRecord.FinalService.CompletionSequence == 1U, evidence.Sequence == 2U,
        evidence.AcknowledgedSequence == 2U);

    // The BSP publishes a service against the stable 2/2 mailbox, then E8
    // publishes sequence three before the AP consumes the request.
    const auto gatedService =
        fixture.GetRuntime().RequestPortableSchedulerService(Kernel::Scheduler::CpuId{1U}, 1U, 0xC503U);
    evidence.Sequence = 3U;
    const RuntimeError gatedServiceResult =
        gatedService.HasValue()
            ? fixture.GetRuntime().ServicePortableSchedulerFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
            : RuntimeError::RuntimeNotReady;
    RuntimeProcessorSnapshot afterGatedService{};
    const RuntimeAccessError gatedSnapshotAccess =
        fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, afterGatedService);
    RuntimeFailure gatedFailure{};
    ASTERIA_EXPECT(
        runner, "AP service consumption revalidates acknowledged sequence two before waking the scheduler",
        gatedService.HasValue(), gatedService.GetValue().Sequence == 3U,
        gatedServiceResult == RuntimeError::TimerDeliveryGateConflict, gatedSnapshotAccess == RuntimeAccessError::None,
        afterGatedService.State == RuntimeProcessorState::Failed, afterGatedService.Service.RequestSequence == 3U,
        afterGatedService.Service.CompletionSequence == 2U, afterGatedService.Service.DispatchCount == 3U,
        afterGatedService.Service.WakeCount == 2U, afterGatedService.Service.ServiceCount == 2U,
        afterGatedService.TimerDelivery.State == RuntimeTimerDeliveryState::Completed,
        fixture.GetRuntime().PollFirstFailure(gatedFailure) == RuntimeFailurePoll::Available,
        gatedFailure.Error == RuntimeError::TimerDeliveryGateConflict,
        gatedFailure.Operation == RuntimeOperation::ServicePortableScheduler);
}

void TestTimerDeliveryFailuresAndGateRace(TestContext& runner) noexcept
{
    {
        constexpr Foundation::UInt64 leadTicks{400'000'000U};
        constexpr Foundation::UInt64 pollGraceTicks{4'000'000'000U};
        constexpr Foundation::UInt64 boundaryTicks{MaximumUInt64 - leadTicks - pollGraceTicks};
        Foundation::UInt64 acceptedDeadline{0U};
        Foundation::UInt64 acceptedPollDeadline{0U};
        Foundation::UInt64 rejectedDeadline{0U};
        Foundation::UInt64 rejectedPollDeadline{0U};
        const bool accepted =
            TryComputeTimerDeliveryDeadlines(boundaryTicks - 1U, acceptedDeadline, acceptedPollDeadline);
        const bool rejected = TryComputeTimerDeliveryDeadlines(boundaryTicks, rejectedDeadline, rejectedPollDeadline);
        ASTERIA_EXPECT(runner, "the exact UInt64-maximum poll deadline is rejected before arithmetic", accepted,
                       acceptedDeadline == boundaryTicks - 1U + leadTicks, acceptedPollDeadline == MaximumUInt64 - 1U,
                       !rejected, rejectedDeadline == 0U, rejectedPollDeadline == 0U);
    }

    {
        RuntimeFixture& fixture = *g_timerDeliveryRaceRuntimeFixture;
        static InterruptEvidence evidence{};
        constexpr PollRange range{.Start = 0x7500U, .End = 0x7600U};
        const bool ready = ReachFirstIdleServing(fixture, evidence, range, 0xD501U) &&
                           (Platform::Ia64::Interrupt::AcknowledgeIa64LocalTimerInterruptEvidence(evidence, 1U) ==
                            Platform::Ia64::Interrupt::Ia64LocalTimerInterruptAcknowledgementError::None);
        std::atomic<Foundation::Size> arrived{0U};
        std::atomic<bool> start{false};
        std::atomic<bool> timerSucceeded{false};
        std::atomic<bool> serviceSucceeded{false};
        std::atomic<Foundation::UInt64> timerError{static_cast<Foundation::UInt64>(RuntimeError::RuntimeNotReady)};
        std::atomic<Foundation::UInt64> serviceError{static_cast<Foundation::UInt64>(RuntimeError::RuntimeNotReady)};
        std::thread timerPublisher{
            [&]() noexcept
            {
                arrived.fetch_add(1U, std::memory_order_acq_rel);
                while (!start.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                const auto request = fixture.GetRuntime().RequestTimerDelivery(Kernel::Scheduler::CpuId{1U}, 1U);
                timerSucceeded.store(request.HasValue(), std::memory_order_release);
                timerError.store(static_cast<Foundation::UInt64>(request.GetError()), std::memory_order_release);
            }};
        std::thread servicePublisher{
            [&]() noexcept
            {
                arrived.fetch_add(1U, std::memory_order_acq_rel);
                while (!start.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                const auto request =
                    fixture.GetRuntime().RequestPortableSchedulerService(Kernel::Scheduler::CpuId{1U}, 1U, 0xD502U);
                serviceSucceeded.store(request.HasValue(), std::memory_order_release);
                serviceError.store(static_cast<Foundation::UInt64>(request.GetError()), std::memory_order_release);
            }};
        while (arrived.load(std::memory_order_acquire) != 2U)
        {
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);
        timerPublisher.join();
        servicePublisher.join();
        RuntimeProcessorSnapshot snapshot{};
        const bool snapshotReady = fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, snapshot) ==
                                   RuntimeAccessError::None;
        const bool timerWon = timerSucceeded.load(std::memory_order_acquire);
        const bool serviceWon = serviceSucceeded.load(std::memory_order_acquire);
        const RuntimeError timerFailure = static_cast<RuntimeError>(timerError.load(std::memory_order_acquire));
        const RuntimeError serviceFailure = static_cast<RuntimeError>(serviceError.load(std::memory_order_acquire));
        ASTERIA_EXPECT(runner, "concurrent level-triggered publishers admit exactly one service or timer gate", ready,
                       snapshotReady, timerWon != serviceWon,
                       timerWon ? ((serviceFailure == RuntimeError::TimerDeliveryGateConflict) &&
                                   (snapshot.TimerDelivery.State == RuntimeTimerDeliveryState::Pending) &&
                                   (snapshot.Service.RequestSequence == 1U))
                                : (((timerFailure == RuntimeError::TimerDeliveryGateConflict) ||
                                    (timerFailure == RuntimeError::TimerDeliveryUnavailable)) &&
                                   (snapshot.TimerDelivery.State == RuntimeTimerDeliveryState::Ready) &&
                                   (snapshot.Service.RequestSequence == 2U) &&
                                   (snapshot.Service.State == RuntimeServiceState::Pending)));
    }

    {
        RuntimeFixture& fixture = *g_timerDeliveryArmFailureFixture;
        static InterruptEvidence evidence{};
        constexpr PollRange range{.Start = 0x7700U, .End = 0x7800U};
        const bool requested = ReachRequestedTimerDelivery(fixture, evidence, range, 0xE501U);
        SetLocalTimerTicks(60'000U, 60'001U, 60'002U, 60'003U);
        g_timerProgramStatus = 1U;
        const Foundation::Size programCallsBefore = g_timerProgramCalls;
        const Foundation::Size cancelCallsBefore = g_timerCancelCalls;
        const RuntimeError error =
            requested ? fixture.GetRuntime().BeginTimerDeliveryFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
                      : RuntimeError::RuntimeNotReady;
        RuntimeFailure failure{};
        RuntimePortableFailure portableFailure{};
        const RuntimeAccessError portableFailureAccess =
            fixture.GetRuntime().GetPortableSchedulerFailure(Kernel::Scheduler::CpuId{1U}, portableFailure);
        const auto& settlement = portableFailure.TimerDeliverySettlement;
        ASTERIA_EXPECT(runner,
                       "a controller-program failure retains its cause while settling "
                       "the armed queue and attempting a local mask",
                       requested, error == RuntimeError::TimerDeliveryDeadlineProgramRejected,
                       fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available,
                       failure.Error == RuntimeError::TimerDeliveryDeadlineProgramRejected,
                       failure.Operation == RuntimeOperation::BeginTimerDelivery,
                       portableFailureAccess == RuntimeAccessError::None,
                       portableFailure.Controller.Error == Kernel::Scheduler::ProcessorRunError::DeadlineRejected,
                       portableFailure.Controller.Deadline.HardwareError == Hal::Time::LocalTimerError::HardwareFailure,
                       settlement.QueueCancelAttempted == 1U,
                       settlement.QueueCancelError == Kernel::Time::TimerQueueError::None,
                       settlement.ControllerRefreshAttempted == 1U,
                       settlement.ControllerRefreshError == Kernel::Scheduler::ProcessorRunError::Faulted,
                       settlement.LocalTimerCancelAttempted == 1U,
                       settlement.LocalTimerCancelError == Hal::Time::LocalTimerError::HardwareFailure,
                       settlement.QueueEmptyProven == 1U, settlement.TimerDisarmedProven == 1U,
                       settlement.ControllerDisarmedProven == 0U, settlement.LocalTimerDisarmedProven == 0U,
                       settlement.FinalQueue.NextDeadlineTicks == 0U, settlement.FinalQueue.ScheduledEntries == 0U,
                       settlement.FinalTimer.State == Kernel::Time::TimerState::Disarmed,
                       settlement.FinalLocalTimer.State == Platform::Ia64::Time::Ia64LocalTimerState::Faulted,
                       g_timerProgramCalls == programCallsBefore + 1U, g_timerCancelCalls > cancelCallsBefore);
    }

    {
        RuntimeFixture& fixture = *g_timerDeliveryGenerationFailureFixture;
        static InterruptEvidence evidence{};
        constexpr PollRange range{.Start = 0x8B00U, .End = 0x8C00U};
        const bool requested = ReachRequestedTimerDelivery(fixture, evidence, range, 0xE506U);
        SetLocalTimerTicks(65'000U, 65'001U, 65'002U, 65'003U);
        const RuntimeError begun =
            requested ? fixture.GetRuntime().BeginTimerDeliveryFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
                      : RuntimeError::RuntimeNotReady;
        const Foundation::Size cancelCallsBefore = g_timerCancelCalls;
        const RuntimeError error = begun == RuntimeError::None
                                       ? fixture.GetRuntime().CompleteTimerDeliveryFromIdle(
                                             Kernel::Scheduler::CpuId{1U}, 2U, PollStatus::TimedOut, range)
                                       : RuntimeError::RuntimeNotReady;
        RuntimeFailure failure{};
        RuntimePortableFailure portableFailure{};
        RuntimeProcessorSnapshot snapshot{};
        const RuntimeAccessError portableFailureAccess =
            fixture.GetRuntime().GetPortableSchedulerFailure(Kernel::Scheduler::CpuId{1U}, portableFailure);
        const RuntimeAccessError snapshotAccess =
            fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, snapshot);
        const auto& settlement = portableFailure.TimerDeliverySettlement;
        ASTERIA_EXPECT(runner,
                       "wrong-generation completion settles the retained arm before "
                       "publishing the original failure",
                       requested, begun == RuntimeError::None, error == RuntimeError::InvalidGeneration,
                       fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available,
                       failure.Error == RuntimeError::InvalidGeneration,
                       failure.Operation == RuntimeOperation::CompleteTimerDelivery, failure.Generation == 1U,
                       failure.Detail == 2U, portableFailureAccess == RuntimeAccessError::None,
                       snapshotAccess == RuntimeAccessError::None, snapshot.State == RuntimeProcessorState::Failed,
                       snapshot.TimerDelivery.State == RuntimeTimerDeliveryState::Failed,
                       settlement.QueueCancelAttempted == 1U,
                       settlement.QueueCancelError == Kernel::Time::TimerQueueError::None,
                       settlement.ControllerRefreshAttempted == 1U,
                       settlement.ControllerRefreshError == Kernel::Scheduler::ProcessorRunError::None,
                       settlement.LocalTimerCancelAttempted == 0U, settlement.QueueEmptyProven == 1U,
                       settlement.TimerDisarmedProven == 1U, settlement.ControllerDisarmedProven == 1U,
                       settlement.LocalTimerDisarmedProven == 1U, g_timerCancelCalls == cancelCallsBefore + 1U);
    }

    {
        RuntimeFixture& fixture = *g_timerDeliveryEvidenceFailureFixture;
        static InterruptEvidence evidence{};
        constexpr PollRange proofRange{
            .Start = 0x7900U,
            .End = 0x7A00U,
        };
        constexpr PollRange deliveryRange{
            .Start = 0x7B00U,
            .End = 0x7C00U,
        };
        const bool requested = ReachRequestedTimerDelivery(fixture, evidence, proofRange, 0xE502U);
        SetLocalTimerTicks(20'000U, 20'001U, 20'002U, 20'003U);
        const RuntimeError begun =
            requested ? fixture.GetRuntime().BeginTimerDeliveryFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
                      : RuntimeError::RuntimeNotReady;
        evidence = MakeTimerDeliveryInterruptEvidence(1U, g_programmedDeadline, deliveryRange);
        evidence.AcknowledgedSequence = 0U;
        const RuntimeError error = begun == RuntimeError::None
                                       ? fixture.GetRuntime().CompleteTimerDeliveryFromIdle(
                                             Kernel::Scheduler::CpuId{1U}, 1U, PollStatus::Completed, deliveryRange)
                                       : RuntimeError::RuntimeNotReady;
        RuntimeFailure failure{};
        ASTERIA_EXPECT(runner,
                       "an E8 sequence-two acknowledgement mismatch cancels the armed queue deadline and fails closed",
                       requested, begun == RuntimeError::None, error == RuntimeError::TimerDeliveryEvidenceRejected,
                       fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available,
                       failure.Error == RuntimeError::TimerDeliveryEvidenceRejected,
                       failure.Operation == RuntimeOperation::CompleteTimerDelivery, g_timerCancelCalls != 0U);
    }

    {
        RuntimeFixture& fixture = *g_timerDeliveryDrainFailureFixture;
        static InterruptEvidence evidence{};
        constexpr PollRange proofRange{
            .Start = 0x7D00U,
            .End = 0x7E00U,
        };
        constexpr PollRange deliveryRange{
            .Start = 0x7F00U,
            .End = 0x8000U,
        };
        const bool requested = ReachRequestedTimerDelivery(fixture, evidence, proofRange, 0xE503U);
        SetLocalTimerTicks(30'000U, 30'001U, 30'002U, 30'003U);
        const RuntimeError begun =
            requested ? fixture.GetRuntime().BeginTimerDeliveryFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
                      : RuntimeError::RuntimeNotReady;
        const Foundation::UInt64 deadline = g_programmedDeadline;
        evidence = MakeTimerDeliveryInterruptEvidence(1U, deadline, deliveryRange);
        SetLocalTimerTicks(30'003U, 30'003U, 30'003U, 30'003U);
        const Foundation::Size cancelCallsBefore = g_timerCancelCalls;
        const RuntimeError error = begun == RuntimeError::None
                                       ? fixture.GetRuntime().CompleteTimerDeliveryFromIdle(
                                             Kernel::Scheduler::CpuId{1U}, 1U, PollStatus::Completed, deliveryRange)
                                       : RuntimeError::RuntimeNotReady;
        RuntimeFailure failure{};
        RuntimePortableFailure portableFailure{};
        const RuntimeAccessError portableFailureAccess =
            fixture.GetRuntime().GetPortableSchedulerFailure(Kernel::Scheduler::CpuId{1U}, portableFailure);
        const auto& settlement = portableFailure.TimerDeliverySettlement;
        ASTERIA_EXPECT(
            runner,
            "a contradictory NotDue drain preserves its error and proves queue/controller/local-timer settlement",
            requested, begun == RuntimeError::None, deadline > 30'003U,
            error == RuntimeError::TimerDeliveryDrainRejected,
            fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available,
            failure.Error == RuntimeError::TimerDeliveryDrainRejected,
            portableFailureAccess == RuntimeAccessError::None, settlement.QueueCancelAttempted == 1U,
            settlement.QueueCancelError == Kernel::Time::TimerQueueError::None,
            settlement.ControllerRefreshAttempted == 1U,
            settlement.ControllerRefreshError == Kernel::Scheduler::ProcessorRunError::None,
            settlement.LocalTimerCancelAttempted == 0U,
            settlement.LocalTimerCancelError == Hal::Time::LocalTimerError::None, settlement.QueueEmptyProven == 1U,
            settlement.TimerDisarmedProven == 1U, settlement.ControllerDisarmedProven == 1U,
            settlement.LocalTimerDisarmedProven == 1U, settlement.FinalQueue.NextDeadlineTicks == 0U,
            settlement.FinalQueue.ScheduledEntries == 0U, settlement.FinalQueue.PendingEntries == 0U,
            settlement.FinalQueue.ClaimedEntries == 0U,
            settlement.FinalTimer.State == Kernel::Time::TimerState::Disarmed,
            settlement.ControllerRefresh.State == Kernel::Time::ProcessorDeadlineState::Disarmed,
            settlement.FinalLocalTimer.State == Platform::Ia64::Time::Ia64LocalTimerState::Ready,
            settlement.FinalLocalTimer.Armed == 0U, g_timerCancelCalls == cancelCallsBefore + 1U);
    }

    {
        // Only the runtime holds its timer-delivery Notification, so the
        // consumption rule is exercised on an equivalent notification whose
        // single publication another owner has already claimed.
        Kernel::Ipc::Notification competing;
        const bool competingReady =
            (competing.Initialize(ResourceDomainId{0xE504U}) == Kernel::Ipc::NotificationError::None) &&
            (competing.Signal() == Kernel::Ipc::NotificationError::None);
        const auto activeClaim = competing.Claim();
        RuntimeTimerDeliveryRecord competingRecord{};
        Kernel::Ipc::NotificationFailure competingFailure{};
        const RuntimeError claimRejected =
            ConsumeTimerDeliveryNotification(competing, competingRecord, competingFailure);

        Kernel::Ipc::Notification exact;
        const bool exactReady = (exact.Initialize(ResourceDomainId{0xE505U}) == Kernel::Ipc::NotificationError::None) &&
                                (exact.Signal() == Kernel::Ipc::NotificationError::None);
        RuntimeTimerDeliveryRecord exactRecord{};
        Kernel::Ipc::NotificationFailure exactFailure{};
        const RuntimeError consumed = ConsumeTimerDeliveryNotification(exact, exactRecord, exactFailure);
        ASTERIA_EXPECT(runner, "an already-active Notification claim is attributed after retained timer delivery",
                       competingReady, activeClaim.HasValue(),
                       claimRejected == RuntimeError::TimerDeliveryNotificationClaimRejected,
                       competingFailure.Error == Kernel::Ipc::NotificationError::ClaimActive,
                       !competingRecord.NotificationClaim.IsValid(), exactReady, consumed == RuntimeError::None,
                       exactFailure.Error == Kernel::Ipc::NotificationError::None,
                       exactRecord.NotificationClaim.GetSequence() == 1U,
                       exactRecord.FinalNotification.State == Kernel::Ipc::NotificationState::Ready,
                       exactRecord.FinalNotification.Pending == 0U);
    }

    {
        RuntimeFixture& fixture = *g_timerDeliveryChronologyFailureFixture;
        static InterruptEvidence evidence{};
        constexpr PollRange proofRange{
            .Start = 0x8700U,
            .End = 0x8800U,
        };
        constexpr PollRange deliveryRange{
            .Start = 0x8900U,
            .End = 0x8A00U,
        };
        const bool requested = ReachRequestedTimerDelivery(fixture, evidence, proofRange, 0xE505U);
        SetLocalTimerTicks(50'000U, 50'001U, 50'002U, 50'003U);
        const RuntimeError begun =
            requested ? fixture.GetRuntime().BeginTimerDeliveryFromIdle(Kernel::Scheduler::CpuId{1U}, 1U)
                      : RuntimeError::RuntimeNotReady;
        const Foundation::UInt64 deadline = g_programmedDeadline;
        evidence = MakeTimerDeliveryInterruptEvidence(1U, deadline, deliveryRange);
        evidence.ObservedIntervalTimeCounter = deadline + 100U;
        SetLocalTimerTicks(deadline, deadline, deadline, deadline);
        const RuntimeError error = begun == RuntimeError::None
                                       ? fixture.GetRuntime().CompleteTimerDeliveryFromIdle(
                                             Kernel::Scheduler::CpuId{1U}, 1U, PollStatus::Completed, deliveryRange)
                                       : RuntimeError::RuntimeNotReady;
        RuntimeFailure failure{};
        ASTERIA_EXPECT(runner, "a delivered Timer whose clock sample precedes E8 evidence fails chronology validation",
                       requested, begun == RuntimeError::None, error == RuntimeError::TimerDeliveryDrainRejected,
                       fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available,
                       failure.Error == RuntimeError::TimerDeliveryDrainRejected,
                       failure.Operation == RuntimeOperation::CompleteTimerDelivery);
    }
}

void TestTimerDeliveryCapacityAndRegistrationFailure(TestContext& runner) noexcept
{
    RuntimeFixture& registrationFixture = *g_timerDeliveryRegistrationFailureFixture;
    static InterruptEvidence registrationEvidence{};
    constexpr PollRange proofRange{.Start = 0x8500U, .End = 0x8600U};
    ResetLocalTimerAdapter();
    const bool configured = registrationFixture.Configure(2U) && registrationFixture.Prepare() &&
                            (ConfigurePortableDependencies(registrationFixture) == RuntimeError::None);

    Foundation::Size filled{0U};
    const Foundation::Size targetActive = Asteria::TestSupport::TestObjectRegistry::EntryCapacity - 3U;
    while (configured && (g_portableRegistry.GetActiveCount() < targetActive) &&
           (filled < Asteria::TestSupport::TestObjectRegistry::EntryCapacity))
    {
        auto* const notification =
            ::new (static_cast<void*>(g_registryFillNotificationStorage[filled])) Kernel::Ipc::Notification{};
        if ((notification->Initialize(g_portableResourceDomain.GetIdentifier()) !=
             Kernel::Ipc::NotificationError::None) ||
            !g_portableIssuer.CreateObject(*notification).HasValue())
        {
            break;
        }
        ++filled;
    }
    const bool capacityPrepared = g_portableRegistry.GetActiveCount() == targetActive;
    RuntimeFixture capacityFixture;
    const bool capacityRuntimeReady = capacityPrepared && capacityFixture.Configure(2U) && capacityFixture.Prepare();
    const RuntimeError capacityError =
        capacityRuntimeReady ? ConfigurePortableDependencies(capacityFixture) : RuntimeError::RuntimeNotReady;
    ASTERIA_EXPECT(runner, "dependency configuration preflights five boot-lifetime registry slots per AP", configured,
                   capacityPrepared, capacityRuntimeReady,
                   capacityError == RuntimeError::PortableSchedulerObjectCapacityRejected);

    const bool proofCompleted = configured &&
                                AdvanceProcessorToRuntimeEntered(registrationFixture, registrationEvidence, 1U) &&
                                CompleteProof(registrationFixture, registrationEvidence, 1U, proofRange);
    const RuntimeError registrationError =
        proofCompleted ? registrationFixture.GetRuntime().PreparePortableScheduler(Kernel::Scheduler::CpuId{1U}, 1U)
                       : RuntimeError::RuntimeNotReady;
    RuntimePortableFailure failure{};
    const RuntimeAccessError failureAccess =
        registrationFixture.GetRuntime().GetPortableSchedulerFailure(Kernel::Scheduler::CpuId{1U}, failure);
    ASTERIA_EXPECT(runner, "a post-preflight registry consumer is retained as an exact Timer registration failure",
                   proofCompleted, registrationError == RuntimeError::PortableSchedulerTimerRegistrationRejected,
                   g_portableRegistry.GetActiveCount() == Asteria::TestSupport::TestObjectRegistry::EntryCapacity,
                   failureAccess == RuntimeAccessError::None,
                   failure.ObjectRegistry == Kernel::Object::ObjectRegistryError::RegistryExhausted);
}

void TestEvidenceAndStageRejection(TestContext& runner) noexcept
{
    {
        RuntimeFixture fixture;
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        const Layout& layout = fixture.GetPlan().Processors[0U];
        RuntimeEvidence invalid = MakeEvidence(layout, EvidencePlacement::Idle, 0U);
        invalid.StackPointer = LayoutPlanner::GetFirstGuardVirtualStart(layout);
        const RuntimeError error =
            claimed ? fixture.GetRuntime().EnterTranslatedIdle(Kernel::Scheduler::CpuId{1U}, 1U, invalid)
                    : RuntimeError::RuntimeNotReady;
        RuntimeFailure failure{};
        ASTERIA_EXPECT(runner, "an idle SP in the guard is rejected and retained",
                       error == RuntimeError::InvalidRegisterEvidence,
                       fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available,
                       failure.Operation == RuntimeOperation::EnterTranslatedIdle);
    }
    {
        RuntimeFixture fixture;
        const bool prepared = fixture.Configure(2U) && fixture.Prepare();
        const RuntimeEvidence evidence = MakeEvidence(fixture.GetPlan().Processors[0U], EvidencePlacement::Idle, 0U);
        const RuntimeError error =
            prepared ? fixture.GetRuntime().EnterTranslatedIdle(Kernel::Scheduler::CpuId{1U}, 1U, evidence)
                     : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "translated entry before physical claim is rejected",
                       error == RuntimeError::InvalidProcessorState);
    }
    {
        RuntimeFixture fixture;
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        const Layout& layout = fixture.GetPlan().Processors[0U];
        const RuntimeEvidence entry = MakeEvidence(layout, EvidencePlacement::Idle, 0U);
        RuntimeEvidence invalidWorker = MakeEvidence(layout, EvidencePlacement::Worker, 0U);
        invalidWorker.RseConfiguration = 1U;
        const RuntimeError entered =
            claimed ? fixture.GetRuntime().EnterTranslatedIdle(Kernel::Scheduler::CpuId{1U}, 1U, entry)
                    : RuntimeError::RuntimeNotReady;
        const RuntimeError completed =
            entered == RuntimeError::None
                ? fixture.GetRuntime().PublishWorkerEntry(Kernel::Scheduler::CpuId{1U}, 1U, invalidWorker)
                : RuntimeError::RuntimeNotReady;
        RuntimeFailure failure{};
        ASTERIA_EXPECT(runner, "invalid worker RSC is rejected before worker publication",
                       completed == RuntimeError::InvalidRegisterEvidence,
                       fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available,
                       failure.Error == RuntimeError::InvalidRegisterEvidence,
                       failure.Operation == RuntimeOperation::PublishWorkerEntry);
    }
    {
        RuntimeFixture fixture;
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        const Layout& layout = fixture.GetPlan().Processors[0U];
        RuntimeEvidence invalid = MakeEvidence(layout, EvidencePlacement::Idle, 0U);
        invalid.Reserved[6U] = 1U;
        const RuntimeError error =
            claimed ? fixture.GetRuntime().EnterTranslatedIdle(Kernel::Scheduler::CpuId{1U}, 1U, invalid)
                    : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "a nonzero capture reserved byte is rejected",
                       error == RuntimeError::InvalidRegisterEvidence);
    }
    {
        RuntimeFixture fixture;
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        RuntimeEvidence invalid = MakeEvidence(fixture.GetPlan().Processors[0U], EvidencePlacement::Idle, 0U);
        invalid.ProcessorStatus |= Foundation::UInt64{1U} << 14U;
        const RuntimeError error =
            claimed ? fixture.GetRuntime().EnterTranslatedIdle(Kernel::Scheduler::CpuId{1U}, 1U, invalid)
                    : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "translated evidence with PSR.i set is rejected",
                       error == RuntimeError::InvalidRegisterEvidence);
    }
    {
        RuntimeFixture fixture;
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        RuntimeEvidence invalid = MakeEvidence(fixture.GetPlan().Processors[0U], EvidencePlacement::Idle, 0U);
        invalid.ProcessorStatus &= ~InstructionCollectionMask;
        const RuntimeError error =
            claimed ? fixture.GetRuntime().EnterTranslatedIdle(Kernel::Scheduler::CpuId{1U}, 1U, invalid)
                    : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "translated evidence with PSR.ic clear is rejected",
                       error == RuntimeError::InvalidRegisterEvidence);
    }
    {
        RuntimeFixture fixture;
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        RuntimeEvidence invalid = MakeEvidence(fixture.GetPlan().Processors[0U], EvidencePlacement::Idle, 0U);
        ++invalid.StackPointer;
        const RuntimeError error =
            claimed ? fixture.GetRuntime().EnterTranslatedIdle(Kernel::Scheduler::CpuId{1U}, 1U, invalid)
                    : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "a one-byte stack-pointer misalignment is rejected",
                       error == RuntimeError::InvalidRegisterEvidence);
    }
    {
        RuntimeFixture fixture;
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        RuntimeEvidence invalid = MakeEvidence(fixture.GetPlan().Processors[0U], EvidencePlacement::Idle, 0U);
        ++invalid.RseBackingStorePointer;
        const RuntimeError error =
            claimed ? fixture.GetRuntime().EnterTranslatedIdle(Kernel::Scheduler::CpuId{1U}, 1U, invalid)
                    : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "a one-byte ar.bsp misalignment is rejected",
                       error == RuntimeError::InvalidRegisterEvidence);
    }
    {
        RuntimeFixture fixture;
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        RuntimeEvidence invalid = MakeEvidence(fixture.GetPlan().Processors[0U], EvidencePlacement::Idle, 0U);
        ++invalid.RseBackingStoreBase;
        const RuntimeError error =
            claimed ? fixture.GetRuntime().EnterTranslatedIdle(Kernel::Scheduler::CpuId{1U}, 1U, invalid)
                    : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "a one-byte ar.bspstore misalignment is rejected",
                       error == RuntimeError::InvalidRegisterEvidence);
    }
}

void TestLocalTimerProofConfiguration(TestContext& runner) noexcept
{
    {
        RuntimeFixture fixture;
        InterruptEvidence evidence{};
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        RuntimeProcessorSnapshot before{};
        const bool initial = claimed && (fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U},
                                                                                   before) == RuntimeAccessError::None);
        const RuntimeError configured =
            claimed
                ? fixture.GetRuntime().ConfigureLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U, &evidence)
                : RuntimeError::RuntimeNotReady;
        RuntimeProcessorSnapshot after{};
        ASTERIA_EXPECT(
            runner, "timer proof configuration is separately published at PhysicalClaimed", initial,
            before.LocalTimerProof.Phase == RuntimeProofPhase::Unconfigured, configured == RuntimeError::None,
            fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, after) == RuntimeAccessError::None,
            after.State == RuntimeProcessorState::PhysicalClaimed,
            after.LocalTimerProof.Phase == RuntimeProofPhase::Configured, after.EvidenceMask == 0U);
        const RuntimeError duplicate =
            fixture.GetRuntime().ConfigureLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U, &evidence);
        RuntimeFailure failure{};
        ASTERIA_EXPECT(runner, "timer proof configuration is single-use and owns its failure",
                       duplicate == RuntimeError::InvalidLocalTimerProofState,
                       fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available,
                       failure.Operation == RuntimeOperation::ConfigureLocalTimerInterruptProof);
    }
    {
        RuntimeFixture fixture;
        InterruptEvidence evidence{};
        evidence.Reserved[5U] = 1U;
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        ASTERIA_EXPECT(
            runner, "configuration rejects any initially nonzero mailbox field", claimed,
            fixture.GetRuntime().ConfigureLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U, &evidence) ==
                RuntimeError::InvalidLocalTimerProofEvidence);
    }
    for (Foundation::UInt8 mutation{0U}; mutation < 2U; ++mutation)
    {
        RuntimeFixture fixture;
        InterruptEvidence evidence{};
        if (mutation == 0U)
        {
            evidence.Sequence = 1U;
        }
        else
        {
            evidence.AcknowledgedSequence = 1U;
        }
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        ASTERIA_EXPECT(
            runner, "configuration rejects nonzero sequence or acknowledgement state", claimed,
            fixture.GetRuntime().ConfigureLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U, &evidence) ==
                RuntimeError::InvalidLocalTimerProofEvidence);
    }
    {
        RuntimeFixture fixture;
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        ASTERIA_EXPECT(runner, "configuration rejects a null mailbox", claimed,
                       fixture.GetRuntime().ConfigureLocalTimerInterruptProof(
                           Kernel::Scheduler::CpuId{1U}, 1U, nullptr) == RuntimeError::InvalidLocalTimerProofEvidence);
    }
    {
        RuntimeFixture fixture;
        alignas(8U) Foundation::UInt8 storage[sizeof(InterruptEvidence) + 1U]{};
        const Foundation::UIntPointer misalignedAddress = reinterpret_cast<Foundation::UIntPointer>(storage) + 1U;
        const auto* const misaligned = reinterpret_cast<const InterruptEvidence*>(misalignedAddress);
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        ASTERIA_EXPECT(
            runner, "configuration rejects a misaligned mailbox without reading it", claimed,
            fixture.GetRuntime().ConfigureLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U, misaligned) ==
                RuntimeError::InvalidLocalTimerProofEvidence);
    }
    {
        RuntimeFixture fixture;
        constexpr Foundation::UInt64 invalidAddress =
            Platform::Ia64::Interrupt::Ia64ExternalInterruptRegionZeroLimit - 8U;
        const auto* const outsideRegion =
            reinterpret_cast<const InterruptEvidence*>(static_cast<Foundation::UIntPointer>(invalidAddress));
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        ASTERIA_EXPECT(
            runner, "configuration rejects a mailbox whose complete extent leaves Region 0", claimed,
            fixture.GetRuntime().ConfigureLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U, outsideRegion) ==
                RuntimeError::InvalidLocalTimerProofEvidence);
    }
}

void TestLocalTimerPhysicalFailureClassification(TestContext& runner) noexcept
{
    constexpr RuntimePhysicalFailure failures[]{
        RuntimePhysicalFailure::LocalTimerProofContextPublicationRejected,
        RuntimePhysicalFailure::LocalTimerProofContextAccessRejected,
    };
    constexpr RuntimeError errors[]{
        RuntimeError::LocalTimerProofContextPublicationRejected,
        RuntimeError::LocalTimerProofContextAccessRejected,
    };
    for (Foundation::Size index{0U}; index < 2U; ++index)
    {
        RuntimeFixture fixture;
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        const RuntimeError error =
            claimed ? fixture.GetRuntime().PublishPhysicalFailure(Kernel::Scheduler::CpuId{1U}, 1U, failures[index], 3U)
                    : RuntimeError::RuntimeNotReady;
        RuntimeFailure retained{};
        ASTERIA_EXPECT(
            runner, "pre-configuration publication and access failures remain distinct", error == errors[index],
            fixture.GetRuntime().PollFirstFailure(retained) == RuntimeFailurePoll::Available, retained.Detail == 3U);
    }
}

void TestLocalTimerProofBeginFailures(TestContext& runner) noexcept
{
    {
        RuntimeFixture fixture;
        InterruptEvidence evidence{};
        const bool ready = ReachRuntimeEntered(fixture, evidence);
        g_timerInitializeStatus = 1U;
        const RuntimeError error =
            ready ? fixture.GetRuntime().BeginLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U)
                  : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "masked E8 initialization failure stops before TPR or programming",
                       error == RuntimeError::LocalTimerInitializationRejected, g_timerInitializeCalls == 1U,
                       g_taskPriorityCalls == 0U);
    }
    {
        RuntimeFixture fixture;
        InterruptEvidence evidence{};
        const bool ready = ReachRuntimeEntered(fixture, evidence);
        g_taskPriorityStatus = 7U;
        const RuntimeError error =
            ready ? fixture.GetRuntime().BeginLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U)
                  : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "exact cr.tpr readback failure is terminal before ITM programming",
                       error == RuntimeError::LocalTimerTaskPriorityRejected, g_timerInitializeCalls == 1U,
                       g_taskPriorityCalls == 1U, g_timerProgramCalls == 0U);
    }
    {
        RuntimeFixture fixture;
        InterruptEvidence evidence{};
        const bool ready = ReachRuntimeEntered(fixture, evidence);
        g_timerTicks[0U] = ~Foundation::UInt64{0U};
        const RuntimeError error =
            ready ? fixture.GetRuntime().BeginLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U)
                  : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "an all-ones same-AP ITC sample is rejected",
                       error == RuntimeError::LocalTimerCounterUnavailable);
    }
    {
        RuntimeFixture fixture;
        InterruptEvidence evidence{};
        const bool ready = ReachRuntimeEntered(fixture, evidence);
        g_timerTicks[0U] = ~Foundation::UInt64{0U} - 9'999'999U;
        const RuntimeError error =
            ready ? fixture.GetRuntime().BeginLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U)
                  : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "the programmed and poll deadline additions cannot wrap",
                       error == RuntimeError::LocalTimerDeadlineOverflow);
    }
    {
        // The AP lost its 10'000'000-tick lead between its ITC sample and
        // programming cr.itm: it samples again and doubles the lead.
        RuntimeFixture& fixture = *g_localTimerSampleRetryFixture;
        static InterruptEvidence evidence{};
        const bool ready = ReachRuntimeEntered(fixture, evidence);
        SetLocalTimerTicks(1'000U, 10'001'000U, 10'002'000U, 10'002'001U);
        const RuntimeError error =
            ready ? fixture.GetRuntime().BeginLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U)
                  : RuntimeError::RuntimeNotReady;
        RuntimeProcessorSnapshot armed{};
        const bool readArmed =
            fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, armed) == RuntimeAccessError::None;
        const Foundation::Size programCalls{g_timerProgramCalls};
        const Foundation::UInt64 deadline{g_programmedDeadline};
        constexpr PollRange range{.Start = 0x2000U, .End = 0x2100U};
        evidence = MakeInterruptEvidence(1U, deadline, range);
        const RuntimeError completed = (error == RuntimeError::None)
                                           ? fixture.GetRuntime().CompleteLocalTimerInterruptProof(
                                                 Kernel::Scheduler::CpuId{1U}, 1U, PollStatus::Completed, range)
                                           : error;
        RuntimeProcessorSnapshot proven{};
        const bool readProven =
            fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, proven) == RuntimeAccessError::None;
        ASTERIA_EXPECT(runner,
                       "a deadline crossed before timer programming is retried with a fresh sample and twice the lead",
                       error == RuntimeError::None, programCalls == 1U, deadline == 30'002'000U, readArmed,
                       armed.LocalTimerProof.Phase == RuntimeProofPhase::Armed, completed == RuntimeError::None,
                       readProven, proven.LocalTimerProof.ProgrammedDeadlineTicks == 30'002'000U,
                       proven.LocalTimerProof.PollDeadlineTicks == 30'002'000U + 4'000'000'000U);
    }
    {
        // The native adapter found the deadline crossed after writing
        // cr.itm and restored the masked, disarmed timer.
        RuntimeFixture& fixture = *g_localTimerAdapterRetryFixture;
        static InterruptEvidence evidence{};
        const bool ready = ReachRuntimeEntered(fixture, evidence);
        g_timerProgramStatus = 2U;
        g_timerProgramStatusCalls = 1U;
        const RuntimeError error =
            ready ? fixture.GetRuntime().BeginLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U)
                  : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "a deadline the adapter finds crossed after programming is retried",
                       error == RuntimeError::None, g_timerProgramCalls == 2U,
                       g_programmedDeadline == 1'002U + 20'000'000U, g_timerCancelCalls == 0U);
    }
    {
        // An ITC far from both zero and all-ones, as after a long uptime.
        RuntimeFixture& fixture = *g_localTimerLateSampleFixture;
        static InterruptEvidence evidence{};
        const bool ready = ReachRuntimeEntered(fixture, evidence);
        constexpr Foundation::UInt64 late{1'000'000'000'000U};
        SetLocalTimerTicks(late, late + 1U, late + 2U, late + 3U);
        const RuntimeError error =
            ready ? fixture.GetRuntime().BeginLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U)
                  : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "a late ITC sample arms the proof one lead later", error == RuntimeError::None,
                       g_timerProgramCalls == 1U, g_programmedDeadline == late + 10'000'000U);
    }
    {
        RuntimeFixture& fixture = *g_localTimerArmExhaustedFixture;
        static InterruptEvidence evidence{};
        const bool ready = ReachRuntimeEntered(fixture, evidence);
        g_timerProgramStatus = 2U;
        const RuntimeError error =
            ready ? fixture.GetRuntime().BeginLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U)
                  : RuntimeError::RuntimeNotReady;
        RuntimeFailure failure{};
        const bool failed = fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available;
        ASTERIA_EXPECT(runner,
                       "a timer whose every arm finds its deadline crossed fails the proof after eight doubling leads",
                       error == RuntimeError::LocalTimerProgramRejected, g_timerProgramCalls == 8U,
                       g_programmedDeadline == 1'003U + (10'000'000U << 7U), g_timerCancelCalls == 0U, failed,
                       failure.Error == RuntimeError::LocalTimerProgramRejected,
                       failure.Detail == static_cast<Foundation::UInt64>(Hal::Time::LocalTimerError::DeadlineReached));
    }
    {
        RuntimeFixture fixture;
        InterruptEvidence evidence{};
        const bool ready = ReachRuntimeEntered(fixture, evidence);
        g_timerProgramStatus = 1U;
        const RuntimeError error =
            ready ? fixture.GetRuntime().BeginLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U)
                  : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "timer hardware programming failure retains the first failure after cleanup",
                       error == RuntimeError::LocalTimerProgramRejected, g_timerProgramCalls == 1U,
                       g_timerCancelCalls == 1U);
    }
}

void TestLocalTimerProofCompletion(TestContext& runner) noexcept
{
    constexpr PollRange range{.Start = 0x2000U, .End = 0x2100U};
    constexpr PollStatus failureStatuses[]{
        PollStatus::TimedOut,
        PollStatus::InvalidEvidence,
        PollStatus::IncompatibleProcessorState,
        PollStatus::NativeUnavailable,
    };
    {
        RuntimeFixture fixture;
        InterruptEvidence evidence{};
        const bool armed = BeginProof(fixture, evidence);
        const RuntimeError error = armed ? fixture.GetRuntime().CompleteLocalTimerInterruptProof(
                                               Kernel::Scheduler::CpuId{1U}, 2U, PollStatus::TimedOut, range)
                                         : RuntimeError::RuntimeNotReady;
        RuntimeFailure failure{};
        RuntimeProcessorSnapshot snapshot{};
        ASTERIA_EXPECT(runner, "wrong-generation completion cancels once before publishing failure",
                       error == RuntimeError::InvalidGeneration, g_timerCancelCalls == 1U,
                       fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available,
                       failure.Error == RuntimeError::InvalidGeneration,
                       failure.Operation == RuntimeOperation::CompleteLocalTimerInterruptProof,
                       failure.Generation == 1U, failure.ProcessorValue == 1U,
                       failure.ObservedState == RuntimeProcessorState::RuntimeEntered,
                       failure.ReleaseError == CoordinatorTransitionError::InvalidGeneration, failure.Detail == 2U,
                       fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, snapshot) ==
                           RuntimeAccessError::None,
                       snapshot.State == RuntimeProcessorState::Failed,
                       snapshot.LocalTimerProof.Phase == RuntimeProofPhase::Failed);
    }
    {
        RuntimeFixture fixture;
        InterruptEvidence evidence{};
        const bool armed = BeginProof(fixture, evidence);
        g_timerCancelStatus = 1U;
        const RuntimeError error = armed ? fixture.GetRuntime().CompleteLocalTimerInterruptProof(
                                               Kernel::Scheduler::CpuId{1U}, 2U, PollStatus::TimedOut, range)
                                         : RuntimeError::RuntimeNotReady;
        RuntimeFailure failure{};
        RuntimeProcessorSnapshot snapshot{};
        ASTERIA_EXPECT(runner, "cancel failure owns wrong-generation completion exactly once",
                       error == RuntimeError::LocalTimerCancelRejected, g_timerCancelCalls == 1U,
                       fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available,
                       failure.Error == RuntimeError::LocalTimerCancelRejected,
                       failure.Operation == RuntimeOperation::CompleteLocalTimerInterruptProof,
                       failure.Generation == 1U,
                       failure.Detail == static_cast<Foundation::UInt64>(Hal::Time::LocalTimerError::HardwareFailure),
                       failure.ProcessorValue == 1U, failure.ObservedState == RuntimeProcessorState::RuntimeEntered,
                       failure.ReleaseError == CoordinatorTransitionError::None,
                       fixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, snapshot) ==
                           RuntimeAccessError::None,
                       snapshot.State == RuntimeProcessorState::Failed,
                       snapshot.LocalTimerProof.Phase == RuntimeProofPhase::Failed);
    }
    for (const PollStatus status : failureStatuses)
    {
        RuntimeFixture fixture;
        InterruptEvidence evidence{};
        const bool armed = BeginProof(fixture, evidence);
        const RuntimeError error =
            armed
                ? fixture.GetRuntime().CompleteLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U, status, range)
                : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "every noncompleted poll status cancels and fails terminally",
                       error == RuntimeError::LocalTimerPollRejected, g_timerCancelCalls == 1U);
    }

    for (Foundation::UInt8 mutation{0U}; mutation < 11U; ++mutation)
    {
        RuntimeFixture fixture;
        InterruptEvidence evidence{};
        const bool armed = BeginProof(fixture, evidence);
        evidence = MakeInterruptEvidence(1U, g_programmedDeadline, range);
        PollRange completionRange{range};
        ValidationError expected{ValidationError::Incomplete};
        switch (mutation)
        {
        case 0U:
            evidence.Sequence = 2U;
            expected = ValidationError::Incomplete;
            break;
        case 1U:
            ++evidence.ProgrammedDeadlineTicks;
            expected = ValidationError::DeadlineMismatch;
            break;
        case 2U:
            --evidence.ObservedIntervalTimeCounter;
            expected = ValidationError::CounterBeforeDeadline;
            break;
        case 3U:
            evidence.InterruptedInstructionPointer = range.End;
            expected = ValidationError::InstructionPointerOutsidePollingRange;
            break;
        case 4U:
            evidence.InterruptedProcessorStatus = 0U;
            expected = ValidationError::InvalidInterruptedProcessorStatus;
            break;
        case 5U:
            evidence.InterruptedIfsValid = 1U;
            expected = ValidationError::IfsValid;
            break;
        case 6U:
            evidence.ProcessorIndex = 2U;
            expected = ValidationError::ProcessorMismatch;
            break;
        case 7U:
            evidence.Vector = 0xE9U;
            expected = ValidationError::VectorMismatch;
            break;
        case 8U:
            evidence.Reserved[0U] = 1U;
            expected = ValidationError::ReservedNotZero;
            break;
        case 9U:
            evidence.AcknowledgedSequence = 1U;
            expected = ValidationError::Incomplete;
            break;
        default:
            completionRange.End = completionRange.Start;
            expected = ValidationError::InvalidPollingRange;
            break;
        }
        const RuntimeError error = armed ? fixture.GetRuntime().CompleteLocalTimerInterruptProof(
                                               Kernel::Scheduler::CpuId{1U}, 1U, PollStatus::Completed, completionRange)
                                         : RuntimeError::RuntimeNotReady;
        RuntimeFailure failure{};
        ASTERIA_EXPECT(runner, "each external timer evidence field is validated after acquire",
                       error == RuntimeError::LocalTimerEvidenceRejected, g_timerCancelCalls == 1U,
                       fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available,
                       failure.Detail == static_cast<Foundation::UInt64>(expected));
    }

    {
        RuntimeFixture fixture;
        InterruptEvidence evidence{};
        const bool armed = BeginProof(fixture, evidence);
        evidence = MakeInterruptEvidence(1U, g_programmedDeadline, range);
        g_timerCancelStatus = 1U;
        const RuntimeError error = armed ? fixture.GetRuntime().CompleteLocalTimerInterruptProof(
                                               Kernel::Scheduler::CpuId{1U}, 1U, PollStatus::Completed, range)
                                         : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "a native timer cancellation failure owns the terminal result",
                       error == RuntimeError::LocalTimerCancelRejected, g_timerCancelCalls == 1U);
    }
    {
        RuntimeFixture fixture;
        InterruptEvidence evidence{};
        const bool armed = BeginProof(fixture, evidence);
        evidence = MakeInterruptEvidence(1U, g_programmedDeadline, range);
        const RuntimeError completed = armed ? fixture.GetRuntime().CompleteLocalTimerInterruptProof(
                                                   Kernel::Scheduler::CpuId{1U}, 1U, PollStatus::Completed, range)
                                             : RuntimeError::RuntimeNotReady;
        const RuntimeError reentry = fixture.GetRuntime().CompleteLocalTimerInterruptProof(
            Kernel::Scheduler::CpuId{1U}, 1U, PollStatus::Completed, range);
        RuntimeFailure failure{};
        ASTERIA_EXPECT(runner, "completion succeeds once and reentry owns the first failure",
                       completed == RuntimeError::None, reentry == RuntimeError::InvalidLocalTimerProofState,
                       fixture.GetRuntime().PollFirstFailure(failure) == RuntimeFailurePoll::Available,
                       failure.Operation == RuntimeOperation::CompleteLocalTimerInterruptProof);
    }
}

void TestLocalTimerProofLifecycleAndIsolation(TestContext& runner) noexcept
{
    {
        RuntimeFixture fixture;
        InterruptEvidence evidence{};
        const bool claimed = fixture.Configure(2U) && fixture.Prepare() && ReleaseAndClaim(fixture);
        const RuntimeError configured =
            claimed
                ? fixture.GetRuntime().ConfigureLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U, &evidence)
                : RuntimeError::RuntimeNotReady;
        ASTERIA_EXPECT(runner, "timer proof cannot begin before RuntimeEntered", configured == RuntimeError::None,
                       fixture.GetRuntime().BeginLocalTimerInterruptProof(Kernel::Scheduler::CpuId{1U}, 1U) ==
                           RuntimeError::InvalidProcessorState);
    }
    {
        RuntimeFixture fixture;
        InterruptEvidence evidence{};
        const bool runtimeEntered = ReachRuntimeEntered(fixture, evidence);
        const auto request = fixture.GetRuntime().RequestPortableSchedulerService(Kernel::Scheduler::CpuId{1U}, 1U, 7U);
        ASTERIA_EXPECT(runner, "BSP service requests remain unavailable before scheduler waiting", runtimeEntered,
                       !request.HasValue(), request.GetError() == RuntimeError::PortableSchedulerServiceUnavailable);
    }
}

void PublishInvalidProcessorFailure(Runtime* const runtime) noexcept
{
    const RuntimeEvidence evidence{};
    static_cast<void>(runtime->EnterTranslatedIdle(Kernel::Scheduler::CpuId{0U}, 1U, evidence));
}

void PublishOutOfRangeProcessorFailure(Runtime* const runtime) noexcept
{
    const RuntimeEvidence evidence{};
    static_cast<void>(runtime->EnterTranslatedIdle(Kernel::Scheduler::CpuId{4U}, 4U, evidence));
}

void TestConcurrentFirstFailure(TestContext& runner)
{
    RuntimeFixture fixture;
    const bool prepared = fixture.Configure(4U) && fixture.Prepare();
    std::thread first{PublishInvalidProcessorFailure, &fixture.GetRuntime()};
    std::thread second{PublishOutOfRangeProcessorFailure, &fixture.GetRuntime()};
    first.join();
    second.join();

    RuntimeFailure retained{};
    const RuntimeFailurePoll poll = fixture.GetRuntime().PollFirstFailure(retained);
    PublishInvalidProcessorFailure(&fixture.GetRuntime());
    RuntimeFailure afterThird{};
    const RuntimeFailurePoll afterPoll = fixture.GetRuntime().PollFirstFailure(afterThird);
    ASTERIA_EXPECT(runner, "concurrent reporters retain exactly one immutable first failure", prepared,
                   poll == RuntimeFailurePoll::Available, retained.Error == RuntimeError::InvalidProcessor,
                   (retained.ProcessorValue == 0U) || (retained.ProcessorValue == 4U),
                   afterPoll == RuntimeFailurePoll::Available, FailureEquals(retained, afterThird));

    RuntimeFixture terminalFixture;
    const bool claimed = terminalFixture.Configure(2U) && terminalFixture.Prepare() && ReleaseAndClaim(terminalFixture);
    RuntimeEvidence invalidEvidence{};
    if (claimed)
    {
        invalidEvidence = MakeEvidence(terminalFixture.GetPlan().Processors[0U], EvidencePlacement::Idle, 0U);
        invalidEvidence.CaptureComplete = 0U;
    }
    std::atomic<bool> beginFailure{false};
    std::thread failingProcessor{[&terminalFixture, &invalidEvidence, &beginFailure]() noexcept
                                 {
                                     while (!beginFailure.load(std::memory_order_acquire))
                                     {
                                         std::this_thread::yield();
                                     }
                                     static_cast<void>(terminalFixture.GetRuntime().EnterTranslatedIdle(
                                         Kernel::Scheduler::CpuId{1U}, 1U, invalidEvidence));
                                 }};
    beginFailure.store(true, std::memory_order_release);
    RuntimeFailure terminalFailure{};
    RuntimeFailurePoll terminalPoll{RuntimeFailurePoll::None};
    for (Foundation::Size attempt{0U}; attempt < 1'000'000U; ++attempt)
    {
        terminalPoll = terminalFixture.GetRuntime().PollFirstFailure(terminalFailure);
        if (terminalPoll == RuntimeFailurePoll::Available)
        {
            break;
        }
        std::this_thread::yield();
    }
    RuntimeProcessorSnapshot terminalSnapshot{};
    const RuntimeAccessError terminalAccess =
        terminalFixture.GetRuntime().GetProcessorSnapshot(Kernel::Scheduler::CpuId{1U}, terminalSnapshot);
    failingProcessor.join();
    ASTERIA_EXPECT(runner, "first-failure availability acquire-publishes the processor terminal state", claimed,
                   terminalPoll == RuntimeFailurePoll::Available,
                   terminalFailure.Error == RuntimeError::InvalidRegisterEvidence, terminalFailure.ProcessorValue == 1U,
                   terminalAccess == RuntimeAccessError::None, terminalSnapshot.State == RuntimeProcessorState::Failed,
                   terminalSnapshot.Service.State == RuntimeServiceState::Failed);
}
} // Namespace

} // Namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64ApplicationProcessorKernelRuntimeTests"};
    const bool progressivePortableEnvironment = Asteria::Test::PreparePortableEnvironment();
    ASTERIA_EXPECT(runner, "the hosted shared-root environment admits BSP and AP1 first",
                   progressivePortableEnvironment);
    if (progressivePortableEnvironment)
    {
        Asteria::Test::TestTlbShootdownIdleEmptyDuringProgressiveAdmission(runner);
        Asteria::Test::TestTlbShootdownIdlePendingRejectsPartialMasks(runner);
    }
    const bool portableEnvironment =
        progressivePortableEnvironment && Asteria::Test::CompletePortableEnvironmentAdmission();
    ASTERIA_EXPECT(runner, "the hosted shared-root environment subsequently admits AP2", portableEnvironment);
    Asteria::Test::TestCanonicalTopologies(runner);
    Asteria::Test::TestPreparationRejection(runner);
    Asteria::Test::TestPhysicalTransitions(runner);
    Asteria::Test::TestTlbShootdownConfiguration(runner);
    Asteria::Test::TestTlbShootdownDrainSummaryValidation(runner);
    if (portableEnvironment)
    {
        Asteria::Test::TestPortableSchedulerConfigurationAndOrder(runner);
        Asteria::Test::TestPortableSchedulerRoundTripAndIsolation(runner);
        Asteria::Test::TestTimerDeliveryTransaction(runner);
        Asteria::Test::TestTimerDeliveryFailuresAndGateRace(runner);
        Asteria::Test::TestTlbShootdownIdlePollFailures(runner);
        Asteria::Test::TestTlbShootdownIdlePendingSuccess(runner);
        Asteria::Test::TestTlbShootdownIdleAcceptsCoalescedNotifications(runner);
        Asteria::Test::PrepareTlbShootdownTerminalFixture(runner);
    }
    Asteria::Test::TestEvidenceAndStageRejection(runner);
    Asteria::Test::TestLocalTimerProofConfiguration(runner);
    Asteria::Test::TestLocalTimerPhysicalFailureClassification(runner);
    Asteria::Test::TestLocalTimerProofBeginFailures(runner);
    Asteria::Test::TestLocalTimerProofCompletion(runner);
    Asteria::Test::TestLocalTimerProofLifecycleAndIsolation(runner);
    Asteria::Test::TestConcurrentFirstFailure(runner);
    if (portableEnvironment)
    {
        Asteria::Test::TestTimerDeliveryCapacityAndRegistrationFailure(runner);
        Asteria::Test::TestTlbShootdownIdleDrainFailureIsTerminal(runner);
    }
    return runner.Finish();
}
