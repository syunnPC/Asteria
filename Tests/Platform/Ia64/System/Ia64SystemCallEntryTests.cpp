// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Hal/Memory/IAddressSpacePageTable.hpp"
#include "Hal/Time/ILocalTimer.hpp"
#include "Kernel/Ipc/Notification.hpp"
#include "Kernel/Ipc/WaitSet.hpp"
#include "Kernel/Ipc/WaitSetThreadDispatcher.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/DirectUserMemoryAccess.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Kernel/Memory/ProcessorAddressSpaceController.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Scheduler/ProcessorRunController.hpp"
#include "Kernel/System/SystemCallDispatcher.hpp"
#include "Kernel/System/WaitSetSystemCallDispatcher.hpp"
#include "Kernel/Time/TimerQueue.hpp"
#include "Platform/Ia64/System/Ia64SystemCallEntry.hpp"
#include "Platform/Ia64/System/Ia64SystemCallWaitContinuation.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Ia64KernelSwitchHook = void (*)(void*) noexcept;

Ia64KernelSwitchHook g_ia64KernelSwitchHook{nullptr};
void* g_ia64KernelSwitchHookArgument{nullptr};
} // Namespace Asteria::Test

extern "C" void
AsteriaIa64SwitchKernelContext(Asteria::Platform::Ia64::Scheduler::Ia64KernelRegisterState* current,
                               const Asteria::Platform::Ia64::Scheduler::Ia64KernelRegisterState* next) noexcept
{
    static_cast<void>(current);
    static_cast<void>(next);
    const Asteria::Test::Ia64KernelSwitchHook hook = Asteria::Test::g_ia64KernelSwitchHook;
    void* const argument = Asteria::Test::g_ia64KernelSwitchHookArgument;
    Asteria::Test::g_ia64KernelSwitchHook = nullptr;
    Asteria::Test::g_ia64KernelSwitchHookArgument = nullptr;
    if (hook != nullptr)
    {
        hook(argument);
    }
}

namespace Asteria::Test
{
namespace
{
using Kernel::System::SystemCallError;
using Kernel::System::SystemCallNumber;
using Platform::Ia64::System::ActivateIa64SystemCallEntryFrame;
using Platform::Ia64::System::AdvanceIa64SystemCallResumeTarget;
using Platform::Ia64::System::DeactivateIa64SystemCallEntryFrame;
using Platform::Ia64::System::Ia64SystemCallBankedRegisterSaveSizeBytes;
using Platform::Ia64::System::Ia64SystemCallBridgeStatus;
using Platform::Ia64::System::Ia64SystemCallBridgeContext;
using Platform::Ia64::System::Ia64SystemCallBridgeContextSizeBytes;
using Platform::Ia64::System::Ia64SystemCallBreakImmediate;
using Platform::Ia64::System::Ia64SystemCallBundleSizeBytes;
using Platform::Ia64::System::Ia64SystemCallContextValidationError;
using Platform::Ia64::System::Ia64SystemCallCurrentProcessorStatusValue;
using Platform::Ia64::System::Ia64SystemCallEntryContext;
using Platform::Ia64::System::Ia64SystemCallEntryContextSizeBytes;
using Platform::Ia64::System::Ia64SystemCallEntryFrame;
using Platform::Ia64::System::Ia64SystemCallEntryFrameState;
using Platform::Ia64::System::Ia64SystemCallFirstClobberedFloatingRegister;
using Platform::Ia64::System::Ia64SystemCallFirstNonKernelRegionAddress;
using Platform::Ia64::System::Ia64SystemCallFirstRestoreCarrierRegister;
using Platform::Ia64::System::Ia64SystemCallFloatingPointDisableMask;
using Platform::Ia64::System::Ia64SystemCallFrameConfigurationError;
using Platform::Ia64::System::Ia64SystemCallFrameMarkerMask;
using Platform::Ia64::System::Ia64SystemCallFrameTransitionError;
using Platform::Ia64::System::Ia64SystemCallIfsValidMask;
using Platform::Ia64::System::Ia64SystemCallInstructionSlotMask;
using Platform::Ia64::System::Ia64SystemCallInstructionSlotShift;
using Platform::Ia64::System::Ia64SystemCallInterruptedProcessorStatusValue;
using Platform::Ia64::System::Ia64SystemCallInterruptedStateValidationError;
using Platform::Ia64::System::Ia64SystemCallKernelRseConfiguration;
using Platform::Ia64::System::Ia64SystemCallLastClobberedFloatingRegister;
using Platform::Ia64::System::Ia64SystemCallLastRestoreCarrierRegister;
using Platform::Ia64::System::Ia64SystemCallMemoryStackSizeBytes;
using Platform::Ia64::System::Ia64SystemCallRefillContextPublicationOffsetBytes;
using Platform::Ia64::System::Ia64SystemCallRefillContextSizeBytes;
using Platform::Ia64::System::Ia64SystemCallResumeAdvanceError;
using Platform::Ia64::System::Ia64SystemCallResumeTarget;
using Platform::Ia64::System::Ia64SystemCallRseBackingStoreSizeBytes;
using Platform::Ia64::System::Ia64SystemCallRseTransitionSizeBytes;
using Platform::Ia64::System::Ia64SystemCallStoragePageSizeBytes;
using Platform::Ia64::System::Ia64SystemCallUserRseConfiguration;
using Platform::Ia64::System::IsIa64SystemCallEntryContextAddressValid;
using Platform::Ia64::System::IsIa64SystemCallBridgeContextAddressValid;
using Platform::Ia64::System::IsIa64SystemCallRefillContextAddressValid;
using Platform::Ia64::System::IsIa64SystemCallWaitPairValid;
using Platform::Ia64::System::DispatchIa64SystemCallBridge;
using Platform::Ia64::System::ValidateIa64SystemCallFrameConfiguration;
using Platform::Ia64::System::ValidateIa64SystemCallInterruptedState;
using Platform::Ia64::System::ValidateIa64SystemCallTrustedContext;

static_assert(sizeof(Ia64SystemCallEntryContext) == 64U);
static_assert(alignof(Ia64SystemCallEntryContext) == 16U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryContext, FrameAddress) == 0U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryContext, MemoryStackStart) == 8U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryContext, MemoryStackEnd) == 16U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryContext, RseBackingStoreStart) == 24U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryContext, RseBackingStoreEnd) == 32U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryContext, BridgeContextAddress) == 40U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryContext, ProcessorIndex) == 48U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryContext, Reserved) == 56U);
static_assert(sizeof(Ia64SystemCallEntryFrame) == 416U);
static_assert(sizeof(Ia64SystemCallBridgeContext) == 64U);
static_assert(alignof(Ia64SystemCallBridgeContext) == 8U);
static_assert(__builtin_offsetof(Ia64SystemCallBridgeContext, RunLoop) == 0U);
static_assert(__builtin_offsetof(Ia64SystemCallBridgeContext, UserMemory) == 8U);
static_assert(__builtin_offsetof(Ia64SystemCallBridgeContext, Scratch) == 16U);
static_assert(__builtin_offsetof(Ia64SystemCallBridgeContext, WaitContinuation) == 24U);
static_assert(__builtin_offsetof(Ia64SystemCallBridgeContext, WaitSetDispatcher) == 32U);
static_assert(alignof(Ia64SystemCallEntryFrame) == 16U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, UserRseBackingStoreStorePointer) == 80U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, KernelRseLoadConfiguration) == 88U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, UserRseNatCollection) == 96U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, Registers) == 320U);
static_assert(__builtin_offsetof(Ia64SystemCallEntryFrame, Reserved) == 408U);
static_assert(Ia64SystemCallBreakImmediate == 0x10'0000U);
static_assert(Ia64SystemCallFrameMarkerMask == 0x0000'003F'FFFF'FFFFULL);
static_assert(Ia64SystemCallRefillContextSizeBytes == 48U);
static_assert(Ia64SystemCallRefillContextPublicationOffsetBytes == 16U);
static_assert(Ia64SystemCallEntryContextSizeBytes == 64U);
static_assert(Ia64SystemCallBridgeContextSizeBytes == 64U);
static_assert(Ia64SystemCallMemoryStackSizeBytes == 16U * 1024U);
static_assert(Ia64SystemCallRseTransitionSizeBytes == 16U * 1024U);
static_assert(Ia64SystemCallRseBackingStoreSizeBytes == 32U * 1024U);
static_assert(Ia64SystemCallBankedRegisterSaveSizeBytes == 144U);
static_assert(Ia64SystemCallStoragePageSizeBytes == 4096U);
static_assert(Ia64SystemCallFloatingPointDisableMask == 0xC'0000U);
static_assert(Ia64SystemCallFirstClobberedFloatingRegister == 6U);
static_assert(Ia64SystemCallLastClobberedFloatingRegister == 15U);
static_assert(Ia64SystemCallFirstRestoreCarrierRegister >= Ia64SystemCallFirstClobberedFloatingRegister);
static_assert(Ia64SystemCallLastRestoreCarrierRegister <= Ia64SystemCallLastClobberedFloatingRegister);
static_assert(IsIa64SystemCallWaitPairValid(nullptr, nullptr));
static_assert(Ia64SystemCallKernelRseConfiguration == 0x3U);
static_assert(Ia64SystemCallUserRseConfiguration == 0xCU);
static_assert(ValidateIa64SystemCallFrameConfiguration(Ia64SystemCallIfsValidMask | 6U) ==
              Ia64SystemCallFrameConfigurationError::None);
static_assert(ValidateIa64SystemCallFrameConfiguration(Ia64SystemCallIfsValidMask | 7U | (1U << 7U)) ==
              Ia64SystemCallFrameConfigurationError::InvalidLocalSize);
static_assert(IsIa64SystemCallRefillContextAddressValid(Ia64SystemCallFirstNonKernelRegionAddress -
                                                        Ia64SystemCallRefillContextSizeBytes));
static_assert(!IsIa64SystemCallRefillContextAddressValid(Ia64SystemCallFirstNonKernelRegionAddress -
                                                         Ia64SystemCallRefillContextSizeBytes + 16U));
static_assert(IsIa64SystemCallEntryContextAddressValid(Ia64SystemCallFirstNonKernelRegionAddress -
                                                       Ia64SystemCallEntryContextSizeBytes));
static_assert(!IsIa64SystemCallEntryContextAddressValid(Ia64SystemCallFirstNonKernelRegionAddress -
                                                        Ia64SystemCallEntryContextSizeBytes + 16U));
static_assert(static_cast<Foundation::UInt64>(SystemCallError::None) == 0U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::UnknownSystemCall) == 1U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::InvalidArguments) == 2U);
static_assert(static_cast<Foundation::UInt64>(SystemCallError::InvalidHandle) == 4U);
static_assert(static_cast<Foundation::UInt64>(SystemCallNumber::CapabilityQuery) == 2U);

class ManualLocalTimer final : public Hal::Time::ILocalTimer
{
public:
    [[nodiscard]] Foundation::UInt64 GetTicks() const noexcept override
    {
        return 10U;
    }

    [[nodiscard]] Hal::Time::LocalTimerError ProgramDeadline(const Foundation::UInt64 deadlineTicks) noexcept override
    {
        static_cast<void>(deadlineTicks);
        return Hal::Time::LocalTimerError::None;
    }

    [[nodiscard]] Hal::Time::LocalTimerError CancelDeadline() noexcept override
    {
        return Hal::Time::LocalTimerError::None;
    }

    [[nodiscard]] Hal::Time::LocalTimerError
    AcknowledgeExternallyMaskedDeadline(const Foundation::UInt64 deadlineTicks) noexcept override
    {
        static_cast<void>(deadlineTicks);
        return Hal::Time::LocalTimerError::InvalidDeadline;
    }
};

class BridgeRunLoopFixture final
{
public:
    BridgeRunLoopFixture() noexcept
        : m_registry{m_registryLock}, m_timerQueue{m_registry, m_timer},
          m_controller{m_registry, m_timerQueue, m_timer, Kernel::Scheduler::CpuId{0U}},
          m_runLoop{m_controller, m_addressSpaces, m_idleContext}
    {
    }

    [[nodiscard]] Platform::Ia64::Scheduler::Ia64ProcessorRunLoop& GetRunLoop() noexcept
    {
        return m_runLoop;
    }

    [[nodiscard]] Kernel::Memory::DirectUserMemoryAccess& GetUserMemory() noexcept
    {
        return m_userMemory;
    }

    [[nodiscard]] Kernel::System::SystemCallScratch& GetScratch() noexcept
    {
        return m_scratch;
    }

    [[nodiscard]] Ia64SystemCallBridgeContext GetBridgeContext() noexcept
    {
        return Ia64SystemCallBridgeContext{
            .RunLoop = &m_runLoop,
            .UserMemory = &m_userMemory,
            .Scratch = &m_scratch,
            .WaitContinuation = nullptr,
            .WaitSetDispatcher = nullptr,
            .Thread = nullptr,
            .PagerResolution = nullptr,
            .ObjectManagement = nullptr,
        };
    }

private:
    Platform::Synchronization::SpinLock m_registryLock;
    Asteria::TestSupport::TestObjectRegistry m_registry;
    ManualLocalTimer m_timer;
    Kernel::Time::TimerQueue m_timerQueue;
    Kernel::Scheduler::ProcessorRunController m_controller;
    Kernel::Memory::ProcessorAddressSpaceController m_addressSpaces;
    Platform::Ia64::Scheduler::Ia64KernelContext m_idleContext;
    Platform::Ia64::Scheduler::Ia64ProcessorRunLoop m_runLoop;
    Kernel::Memory::DirectUserMemoryAccess m_userMemory;
    Kernel::System::SystemCallScratch m_scratch;
};

[[nodiscard]] constexpr Ia64SystemCallEntryContext MakeValidContext() noexcept
{
    return Ia64SystemCallEntryContext{
        .FrameAddress = 0x0010'0000U,
        .MemoryStackStart = 0x0020'0000U,
        .MemoryStackEnd = 0x0020'0000U + Ia64SystemCallMemoryStackSizeBytes,
        .RseBackingStoreStart = 0x0030'0000U,
        .RseBackingStoreEnd = 0x0030'0000U + Ia64SystemCallRseBackingStoreSizeBytes,
        .BridgeContextAddress = 0x0040'0000U,
        .ProcessorIndex = 0U,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr Ia64SystemCallEntryFrame MakeValidFrame(const Foundation::UInt64 instructionSlot = 0U) noexcept
{
    Ia64SystemCallEntryFrame frame{};
    frame.State = Ia64SystemCallEntryFrameState::Active;
    frame.CurrentProcessorStatus = Ia64SystemCallCurrentProcessorStatusValue;
    frame.InterruptedInstructionPointer = 0x0000'0000'0040'0000U;
    frame.InterruptedProcessorStatus =
        Ia64SystemCallInterruptedProcessorStatusValue | (instructionSlot << Ia64SystemCallInstructionSlotShift);
    frame.BreakImmediate = Ia64SystemCallBreakImmediate;
    frame.HardwareInterruptionFunctionState = 0U;
    frame.CapturedUserInterruptionFunctionState = Ia64SystemCallIfsValidMask | 6U;
    frame.UserRseConfiguration = Ia64SystemCallUserRseConfiguration;
    frame.Registers.R15 = static_cast<Foundation::UInt64>(SystemCallNumber::CapabilityQuery);
    frame.Registers.R32 = 0x11U;
    frame.Registers.R33 = 0x22U;
    frame.Registers.R34 = 0U;
    frame.Registers.R35 = 0U;
    frame.Registers.R36 = 0U;
    frame.Registers.R37 = 0U;
    frame.Registers.InputNatBits = 0U;
    frame.Registers.R8 = 0xA8U;
    frame.Registers.R9 = 0xA9U;
    frame.Registers.R10 = 0xAAU;
    return frame;
}

class WaitEntryAddressSpacePageTable final : public Hal::Memory::IAddressSpacePageTable
{
public:
    [[nodiscard]] bool IsReady() const noexcept override
    {
        return true;
    }

    [[nodiscard]] Hal::Memory::AddressSpacePageTableOwnershipError
    Claim(const Kernel::Memory::AddressSpace& owner, const Kernel::Memory::InitialMappingPlan&) noexcept override
    {
        if (m_owner != nullptr)
        {
            return Hal::Memory::AddressSpacePageTableOwnershipError::AlreadyOwned;
        }
        m_owner = &owner;
        return Hal::Memory::AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] Hal::Memory::AddressSpacePageTableOwnershipError
    Release(const Kernel::Memory::AddressSpace& owner) noexcept override
    {
        if (m_owner != &owner)
        {
            return Hal::Memory::AddressSpacePageTableOwnershipError::InvalidOwner;
        }
        m_owner = nullptr;
        return Hal::Memory::AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] Hal::Memory::AddressSpacePageTableActivationError
    Activate(const Kernel::Memory::AddressSpace& owner, const Kernel::Scheduler::CpuId processor) noexcept override
    {
        return ((m_owner == &owner) && processor.IsValid())
                   ? Hal::Memory::AddressSpacePageTableActivationError::None
                   : Hal::Memory::AddressSpacePageTableActivationError::InvalidOwner;
    }

    [[nodiscard]] Hal::Memory::AddressSpacePageTableError
    Unmap(const Kernel::Memory::AddressSpaceMappingRecord&,
          Hal::Memory::AddressSpacePageTableMapResult&) noexcept override
    {
        return Hal::Memory::AddressSpacePageTableError::UnsupportedConfiguration;
    }

    [[nodiscard]] Hal::Memory::AddressSpacePageTableError
    Map(const Kernel::Memory::AddressSpaceMappingRecord&,
        Hal::Memory::AddressSpacePageTableMapResult&) noexcept override
    {
        return Hal::Memory::AddressSpacePageTableError::InternalFailure;
    }

private:
    const Kernel::Memory::AddressSpace* m_owner{nullptr};
};

[[nodiscard]] constexpr Kernel::Memory::MappingPermissions WaitEntryReadWrite() noexcept
{
    return Kernel::Memory::MappingPermissions{Kernel::Memory::MappingPermission::Read}.With(
        Kernel::Memory::MappingPermission::Write);
}

[[nodiscard]] constexpr Kernel::Memory::MappingPermissions WaitEntryReadExecute() noexcept
{
    return Kernel::Memory::MappingPermissions{Kernel::Memory::MappingPermission::Read}.With(
        Kernel::Memory::MappingPermission::Execute);
}

[[nodiscard]] constexpr Kernel::Memory::InitialMapping
MakeWaitEntryMapping(const Foundation::UInt64 virtualStart, const Foundation::UInt64 physicalStart,
                     const Foundation::UInt64 pages, const Kernel::Memory::MappingPermissions permissions,
                     const Kernel::Memory::InitialMappingOwner owner) noexcept
{
    return Kernel::Memory::InitialMapping{
        .VirtualStart = Kernel::Memory::VirtualAddress{virtualStart},
        .PhysicalStart = Kernel::Memory::PhysicalAddress{physicalStart},
        .Pages = Kernel::Memory::PageCount{pages},
        .Permissions = permissions,
        .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
        .Owner = owner,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool BuildWaitEntrySeedPlan(Kernel::Memory::InitialMappingPlan& plan) noexcept
{
    constexpr Foundation::UInt64 base{0xFFFF'8000'0000'0000ULL};
    return (plan.AddMapping(MakeWaitEntryMapping(base + 0x1000U, 0x0010'0000U, 2U, WaitEntryReadExecute(),
                                                 Kernel::Memory::InitialMappingOwner::NucleusCode)) ==
            Kernel::Memory::InitialMappingPlanError::None) &&
           (plan.AddMapping(MakeWaitEntryMapping(base + 0x4000U, 0x0010'4000U, 4U, WaitEntryReadWrite(),
                                                 Kernel::Memory::InitialMappingOwner::NucleusStack)) ==
            Kernel::Memory::InitialMappingPlanError::None) &&
           (plan.AddMapping(MakeWaitEntryMapping(base + 0x8000U, 0x0010'8000U, 16U, WaitEntryReadWrite(),
                                                 Kernel::Memory::InitialMappingOwner::PageTable)) ==
            Kernel::Memory::InitialMappingPlanError::None) &&
           (plan.Seal() == Kernel::Memory::InitialMappingPlanError::None);
}

[[nodiscard]] constexpr Kernel::Resource::ResourceDomainLimits WaitEntryResourceLimits() noexcept
{
    return Kernel::Resource::ResourceDomainLimits{
        .CommittedMemoryPages = 4096U,
        .ResidentMemoryPages = 4096U,
        .KernelMetadataBytes = 64U * 1024U,
        .CapabilityHandles = 4096U,
        .IpcQueueBytes = 64U * 1024U,
        .Threads = 256U,
        .ChildDomains = 16U,
    };
}

[[nodiscard]] constexpr Kernel::Scheduler::SchedulingContextConfiguration WaitEntrySchedulingConfiguration() noexcept
{
    return Kernel::Scheduler::SchedulingContextConfiguration{
        .BudgetTicks = 20U,
        .PeriodTicks = 100U,
        .RelativeDeadlineTicks = 80U,
        .Weight = 10U,
        .Priority = 8U,
        .Criticality = Kernel::Scheduler::SchedulingCriticality::Normal,
        .CpuAffinityMask = 0x01U,
        .Class = Kernel::Scheduler::SchedulingClass::DeadlineSporadic,
        .Reserved = 0U,
    };
}

alignas(4096U) Foundation::UInt8 g_waitEntryNativeMemoryStack[16U * 1024U]{};
alignas(4096U) Foundation::UInt8 g_waitEntryNativeRseBackingStore[16U * 1024U]{};
alignas(4096U) Foundation::UInt8 g_waitEntrySupervisorMemoryStack[16U * 1024U]{};
alignas(4096U) Foundation::UInt8 g_waitEntrySupervisorRseBackingStore[16U * 1024U]{};
alignas(4096U) Foundation::UInt8 g_waitEntrySystemCallMemoryStack[Ia64SystemCallMemoryStackSizeBytes]{};
alignas(4096U) Foundation::UInt8 g_waitEntrySystemCallRseBackingStore[Ia64SystemCallRseBackingStoreSizeBytes]{};

class ResumableWaitEntryFixture final
{
public:
    static constexpr Foundation::UInt64 WaitTag{0xA57E'0000'0000'0042ULL};

    ResumableWaitEntryFixture() noexcept
        : m_registry{m_registryLock}, m_revocations{m_registryLock}, m_issuer{m_registry},
          m_capabilities{m_registry, m_revocations, m_timer, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL},
          m_timerQueue{m_registry, m_timer},
          m_controller{m_registry, m_timerQueue, m_timer, Kernel::Scheduler::CpuId{0U}},
          m_resourceDomain{m_resourceLedger}, m_runLoop{m_controller, m_addressSpaces, m_idleContext},
          m_waitSet{m_registry}, m_schedulingContext{m_registry},
          m_threadDispatcher{m_registry, m_controller.GetWakeFacade()}, m_waitDispatcher{m_registry, m_threadDispatcher}
    {
    }

    ResumableWaitEntryFixture(const ResumableWaitEntryFixture&) = delete;
    ResumableWaitEntryFixture& operator=(const ResumableWaitEntryFixture&) = delete;
    ResumableWaitEntryFixture(ResumableWaitEntryFixture&&) = delete;
    ResumableWaitEntryFixture& operator=(ResumableWaitEntryFixture&&) = delete;

    [[nodiscard]] bool Initialize() noexcept
    {
        constexpr Kernel::Object::ResourceDomainId resource{19U};
        const bool resourceInitialized = m_resourceDomain.InitializeRoot(resource, WaitEntryResourceLimits()).Error ==
                                         Kernel::Resource::ResourceDomainError::None;
        const auto resourceObject =
            resourceInitialized
                ? m_issuer.CreateObject(m_resourceDomain)
                : Foundation::Result<Kernel::Object::ObjectId, Kernel::Object::ObjectRegistryError>::Failure(
                      Kernel::Object::ObjectRegistryError::InvalidObject);
        if (!resourceObject.HasValue())
        {
            return false;
        }
        const Kernel::Resource::ResourceDomainObjectId resourceIdentity{m_registry, resourceObject.GetValue()};

        if (!BuildWaitEntrySeedPlan(m_seedPlan) ||
            (m_addressSpace.Initialize(resource, m_pageTable, m_seedPlan) != Kernel::Memory::AddressSpaceError::None))
        {
            return false;
        }
        const auto addressObject = m_issuer.CreateObject(m_addressSpace);
        if (!addressObject.HasValue())
        {
            return false;
        }
        const Kernel::Memory::AddressSpaceId addressIdentity{m_registry, addressObject.GetValue()};
        if (m_addressSpace.BindIdentity(m_registry, addressIdentity).Error != Kernel::Memory::AddressSpaceError::None)
        {
            return false;
        }

        if ((m_protectionDomain
                 .Initialize(m_registry, resourceIdentity, m_resourceDomain, addressIdentity, m_capabilities)
                 .Error != Kernel::Protection::ProtectionDomainError::None) ||
            !m_capabilities.IsReady())
        {
            return false;
        }
        const auto protectionObject = m_issuer.CreateObject(m_protectionDomain);
        if (!protectionObject.HasValue())
        {
            return false;
        }
        const Kernel::Protection::ProtectionDomainId protectionIdentity{m_registry, protectionObject.GetValue()};
        if ((m_protectionDomain.BindIdentity(m_registry, protectionIdentity).Error !=
             Kernel::Protection::ProtectionDomainError::None) ||
            !m_addressSpaces.Initialize(Kernel::Scheduler::CpuId{0U}, m_addressSpace).HasValue())
        {
            return false;
        }

        if ((m_event.Initialize(resource) != Kernel::Ipc::NotificationError::None) ||
            (m_waitSet.Initialize(resource) != Kernel::Ipc::WaitSetError::None) ||
            (m_thread.Initialize(resource) != Kernel::Scheduler::ThreadError::None) ||
            (m_schedulingContext.Initialize(WaitEntrySchedulingConfiguration(), resource,
                                            Kernel::Object::MonotonicTime{0U}) !=
             Kernel::Scheduler::SchedulingContextError::None))
        {
            return false;
        }

        const auto eventObject = m_issuer.CreateObject(m_event);
        const auto waitSetObject = m_issuer.CreateObject(m_waitSet);
        const auto threadObject = m_issuer.CreateObject(m_thread);
        const auto schedulingObject = m_issuer.CreateObject(m_schedulingContext);
        if (!eventObject.HasValue() || !waitSetObject.HasValue() || !threadObject.HasValue() ||
            !schedulingObject.HasValue())
        {
            return false;
        }
        m_threadIdentity = Kernel::Scheduler::ThreadId{m_registry, threadObject.GetValue()};
        m_schedulingContextIdentity = Kernel::Scheduler::SchedulingContextId{m_registry, schedulingObject.GetValue()};
        const Kernel::Ipc::NotificationId eventIdentity{m_registry, eventObject.GetValue()};
        const auto registered = m_waitSet.RegisterNotification(eventIdentity, m_event, WaitTag);
        if (!registered.HasValue())
        {
            return false;
        }
        m_registrationSequence = registered.GetValue().Registration.GetSequence();

        const auto revocationDomain = m_revocations.CreateRoot();
        if (!revocationDomain.HasValue())
        {
            return false;
        }
        const auto waitHandle =
            m_issuer.Mint(m_capabilities, waitSetObject.GetValue(),
                          Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Wait},
                          revocationDomain.GetValue(), Kernel::Object::MonotonicTime::Infinite(), 0xA57EU);
        if (!waitHandle.HasValue())
        {
            return false;
        }
        m_waitHandle = waitHandle.GetValue();

        const Platform::Ia64::Scheduler::WritableIa64KernelStorageView nativeMemoryStack{
            .Data = g_waitEntryNativeMemoryStack,
            .SizeBytes = sizeof(g_waitEntryNativeMemoryStack),
            .VirtualStart = AddressOf(g_waitEntryNativeMemoryStack),
        };
        const Platform::Ia64::Scheduler::WritableIa64KernelStorageView nativeRseBackingStore{
            .Data = g_waitEntryNativeRseBackingStore,
            .SizeBytes = sizeof(g_waitEntryNativeRseBackingStore),
            .VirtualStart = AddressOf(g_waitEntryNativeRseBackingStore),
        };
        if (m_threadContext.Prepare(53U, nativeMemoryStack, nativeRseBackingStore, NativeThreadEntry, this) !=
            Platform::Ia64::Scheduler::Ia64KernelContextError::None)
        {
            return false;
        }

        if ((m_schedulingContext.Bind(m_threadIdentity) != Kernel::Scheduler::SchedulingContextError::None) ||
            (m_thread.MakeReady() != Kernel::Scheduler::ThreadError::None) ||
            (m_runLoop.Bind(m_thread, m_threadContext, m_protectionDomain) !=
             Kernel::Scheduler::ProcessorKernelRunLoopError::None) ||
            !m_controller.Enqueue(m_schedulingContextIdentity, m_schedulingContext, m_threadIdentity, m_thread)
                 .HasValue())
        {
            return false;
        }

        m_supervisorContinuation = {};
        m_supervisorContinuation.MemoryStack = {
            .VirtualStart = AddressOf(g_waitEntrySupervisorMemoryStack),
            .SizeBytes = sizeof(g_waitEntrySupervisorMemoryStack),
        };
        m_supervisorContinuation.RseBackingStore = {
            .VirtualStart = AddressOf(g_waitEntrySupervisorRseBackingStore),
            .SizeBytes = sizeof(g_waitEntrySupervisorRseBackingStore),
        };
        m_supervisorContinuation.Registers.InstructionPointer = 0x1000U;
        m_supervisorContinuation.Registers.StackPointer =
            AddressOf(g_waitEntrySupervisorMemoryStack) + sizeof(g_waitEntrySupervisorMemoryStack) - 32U;
        m_supervisorContinuation.Registers.RseBackingStorePointer = AddressOf(g_waitEntrySupervisorRseBackingStore);
        m_supervisorContinuation.State = Platform::Ia64::Scheduler::Ia64UserModeSupervisorContinuationState::Retained;

        m_frame = {};
        if (m_waitContinuation.Initialize(Platform::Ia64::System::Ia64SystemCallWaitContinuationConfiguration{
                .EntryFrame = &m_frame,
                .SupervisorContinuation = &m_supervisorContinuation,
                .SystemCallMemoryStack =
                    {
                        .VirtualStart = AddressOf(g_waitEntrySystemCallMemoryStack),
                        .SizeBytes = sizeof(g_waitEntrySystemCallMemoryStack),
                    },
                .SystemCallRseBackingStore =
                    {
                        .VirtualStart = AddressOf(g_waitEntrySystemCallRseBackingStore),
                        .SizeBytes = sizeof(g_waitEntrySystemCallRseBackingStore),
                    },
                .Processor = Kernel::Scheduler::CpuId{0U},
            }) != Platform::Ia64::System::Ia64SystemCallWaitContinuationError::None)
        {
            return false;
        }
        m_frame = MakeValidFrame();
        return true;
    }

    void SetWaitRequest(const bool validHandle) noexcept
    {
        m_frame.Registers.R15 = static_cast<Foundation::UInt64>(SystemCallNumber::WaitSetWait);
        m_frame.Registers.R32 = validHandle ? m_waitHandle.GetSelector() : 0U;
        m_frame.Registers.R33 = validHandle ? m_waitHandle.GetAuthenticator() : 0U;
        m_frame.Registers.R34 = 0U;
        m_frame.Registers.R35 = 0U;
        m_frame.Registers.R36 = 0U;
        m_frame.Registers.R37 = 0U;
    }

    void SetCapabilityQueryRequest() noexcept
    {
        m_frame.Registers.R15 = static_cast<Foundation::UInt64>(SystemCallNumber::CapabilityQuery);
        m_frame.Registers.R32 = 0U;
        m_frame.Registers.R33 = 0U;
        m_frame.Registers.R34 = 0U;
        m_frame.Registers.R35 = 0U;
        m_frame.Registers.R36 = 0U;
        m_frame.Registers.R37 = 0U;
    }

    [[nodiscard]] Kernel::Ipc::NotificationError Signal() noexcept
    {
        return m_event.Signal();
    }

    void Dispatch(const bool configuredWait) noexcept
    {
        Dispatch(configuredWait, configuredWait);
    }

    void Dispatch(const bool useWaitContinuation, const bool useWaitSetDispatcher) noexcept
    {
        m_useWaitContinuation = useWaitContinuation;
        m_useWaitSetDispatcher = useWaitSetDispatcher;
        m_bridgeCalled = false;
        g_ia64KernelSwitchHook = RunBridge;
        g_ia64KernelSwitchHookArgument = this;
        Kernel::Scheduler::ProcessorKernelRunLoopReceipt receipt{};
        Kernel::Scheduler::ProcessorKernelRunLoopFailure failure{};
        static_cast<void>(m_runLoop.DispatchFromIdle(receipt, failure));
        g_ia64KernelSwitchHook = nullptr;
        g_ia64KernelSwitchHookArgument = nullptr;
    }

    [[nodiscard]] Foundation::UInt64 InvokeRawBridge(const bool useWaitContinuation,
                                                     const bool useWaitSetDispatcher) noexcept
    {
        Ia64SystemCallBridgeContext bridgeContext{
            .RunLoop = &m_runLoop,
            .UserMemory = &m_userMemory,
            .Scratch = &m_scratch,
            .WaitContinuation = useWaitContinuation ? &m_waitContinuation : nullptr,
            .WaitSetDispatcher = useWaitSetDispatcher ? &m_waitDispatcher : nullptr,
            .Thread = nullptr,
            .PagerResolution = nullptr,
            .ObjectManagement = nullptr,
        };
        return AsteriaIa64SystemCallBridge(&m_frame, &bridgeContext);
    }

    [[nodiscard]] constexpr bool BridgeWasCalled() const noexcept
    {
        return m_bridgeCalled;
    }

    [[nodiscard]] constexpr Ia64SystemCallBridgeStatus GetStatus() const noexcept
    {
        return m_status;
    }

    [[nodiscard]] constexpr const Ia64SystemCallEntryFrame& GetFrame() const noexcept
    {
        return m_frame;
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetRegistrationSequence() const noexcept
    {
        return m_registrationSequence;
    }

    [[nodiscard]] Platform::Ia64::System::Ia64SystemCallWaitContinuationSnapshot
    GetContinuationSnapshot() const noexcept
    {
        return m_waitContinuation.GetSnapshot();
    }

    [[nodiscard]] Kernel::Ipc::WaitSetThreadDispatcherSnapshot GetDispatcherSnapshot() const noexcept
    {
        return m_threadDispatcher.GetSnapshot();
    }

private:
    [[nodiscard]] static Foundation::UInt64 AddressOf(const void* const value) noexcept
    {
        return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
    }

    static void NativeThreadEntry(void*) noexcept
    {
    }

    static void RunBridge(void* const argument) noexcept
    {
        if (argument != nullptr)
        {
            static_cast<ResumableWaitEntryFixture*>(argument)->RunBridge();
        }
    }

    void RunBridge() noexcept
    {
        m_bridgeCalled = true;
        m_status = DispatchIa64SystemCallBridge(m_frame, m_runLoop, m_userMemory, m_scratch,
                                                m_useWaitContinuation ? &m_waitContinuation : nullptr,
                                                m_useWaitSetDispatcher ? &m_waitDispatcher : nullptr);
    }

    Platform::Synchronization::SpinLock m_registryLock;
    ManualLocalTimer m_timer;
    Asteria::TestSupport::TestObjectRegistry m_registry;
    Kernel::Object::RevocationRegistry m_revocations;
    Kernel::Object::RootCapabilityIssuer m_issuer;
    Kernel::Object::CapabilitySpace m_capabilities;
    Kernel::Time::TimerQueue m_timerQueue;
    Kernel::Scheduler::ProcessorRunController m_controller;
    Kernel::Resource::ResourceDomainLedger m_resourceLedger;
    Kernel::Resource::ResourceDomain m_resourceDomain;
    WaitEntryAddressSpacePageTable m_pageTable;
    Kernel::Memory::InitialMappingPlan m_seedPlan;
    Kernel::Memory::AddressSpace m_addressSpace;
    Kernel::Protection::ProtectionDomain m_protectionDomain;
    Kernel::Memory::ProcessorAddressSpaceController m_addressSpaces;
    Platform::Ia64::Scheduler::Ia64KernelContext m_idleContext;
    Platform::Ia64::Scheduler::Ia64KernelContext m_threadContext;
    Platform::Ia64::Scheduler::Ia64ProcessorRunLoop m_runLoop;
    Kernel::Ipc::Notification m_event;
    Kernel::Ipc::WaitSet m_waitSet;
    Kernel::Scheduler::Thread m_thread;
    Kernel::Scheduler::SchedulingContext m_schedulingContext;
    Kernel::Ipc::WaitSetThreadDispatcher m_threadDispatcher;
    Kernel::System::WaitSetSystemCallDispatcher m_waitDispatcher;
    Platform::Ia64::System::Ia64SystemCallWaitContinuation m_waitContinuation;
    Platform::Ia64::Scheduler::Ia64UserModeSupervisorContinuation m_supervisorContinuation{};
    Kernel::Memory::DirectUserMemoryAccess m_userMemory;
    Kernel::System::SystemCallScratch m_scratch;
    Ia64SystemCallEntryFrame m_frame{};
    Kernel::Scheduler::ThreadId m_threadIdentity{};
    Kernel::Scheduler::SchedulingContextId m_schedulingContextIdentity{};
    Kernel::Object::CapabilityHandle m_waitHandle{};
    Foundation::UInt64 m_registrationSequence{0U};
    Ia64SystemCallBridgeStatus m_status{Ia64SystemCallBridgeStatus::InvalidFrameState};
    bool m_useWaitContinuation{false};
    bool m_useWaitSetDispatcher{false};
    bool m_bridgeCalled{false};
};

void TestContextValidation(TestContext& runner) noexcept
{
    const Foundation::UInt64 lastRefillContextAddress{Ia64SystemCallFirstNonKernelRegionAddress -
                                                      Ia64SystemCallRefillContextSizeBytes};
    ASTERIA_EXPECT(runner, "last full refill-context extent in region zero is valid",
                   IsIa64SystemCallRefillContextAddressValid(lastRefillContextAddress));
    ASTERIA_EXPECT(runner, "refill context crossing the region-zero boundary is rejected",
                   !IsIa64SystemCallRefillContextAddressValid(lastRefillContextAddress + 16U));
    ASTERIA_EXPECT(runner, "misaligned refill-context root is rejected before publication load",
                   !IsIa64SystemCallRefillContextAddressValid(lastRefillContextAddress - 8U));

    const Foundation::UInt64 lastEntryContextAddress{Ia64SystemCallFirstNonKernelRegionAddress -
                                                     Ia64SystemCallEntryContextSizeBytes};
    ASTERIA_EXPECT(runner, "last full syscall-context extent in region zero is valid",
                   IsIa64SystemCallEntryContextAddressValid(lastEntryContextAddress));
    ASTERIA_EXPECT(runner, "syscall context crossing the region-zero boundary is rejected",
                   !IsIa64SystemCallEntryContextAddressValid(lastEntryContextAddress + 16U));
    ASTERIA_EXPECT(runner, "null syscall-context address is rejected before field loads",
                   !IsIa64SystemCallEntryContextAddressValid(0U));

    const Foundation::UInt64 lastBridgeContextAddress{Ia64SystemCallFirstNonKernelRegionAddress -
                                                      Ia64SystemCallBridgeContextSizeBytes};
    ASTERIA_EXPECT(runner, "last full bridge-context extent in region zero is valid",
                   IsIa64SystemCallBridgeContextAddressValid(lastBridgeContextAddress));
    ASTERIA_EXPECT(runner, "bridge context crossing the region-zero boundary is rejected",
                   !IsIa64SystemCallBridgeContextAddressValid(lastBridgeContextAddress + 8U));
    ASTERIA_EXPECT(runner, "misaligned bridge context is rejected before member loads",
                   !IsIa64SystemCallBridgeContextAddressValid(lastBridgeContextAddress - 4U));

    const Ia64SystemCallEntryContext valid{MakeValidContext()};
    ASTERIA_EXPECT(runner, "exact page-aligned context with a trusted bridge context is valid",
                   ValidateIa64SystemCallTrustedContext(valid) == Ia64SystemCallContextValidationError::None);

    Ia64SystemCallEntryContext candidate{valid};
    candidate.Reserved = 1U;
    ASTERIA_EXPECT(runner, "nonzero context reserved word is rejected",
                   ValidateIa64SystemCallTrustedContext(candidate) ==
                       Ia64SystemCallContextValidationError::InvalidReserved);
    candidate = valid;
    candidate.ProcessorIndex = 4U;
    ASTERIA_EXPECT(runner, "processor index outside the four-processor profile is rejected",
                   ValidateIa64SystemCallTrustedContext(candidate) ==
                       Ia64SystemCallContextValidationError::InvalidProcessorIndex);

    candidate = valid;
    candidate.FrameAddress += 8U;
    ASTERIA_EXPECT(runner, "entry frame requires natural 16-byte alignment",
                   ValidateIa64SystemCallTrustedContext(candidate) ==
                       Ia64SystemCallContextValidationError::InvalidFrame);
    candidate = valid;
    candidate.FrameAddress = 0x1FFF'FFFF'FFFF'FF00ULL;
    ASTERIA_EXPECT(runner, "entry frame cannot cross out of kernel virtual region zero",
                   ValidateIa64SystemCallTrustedContext(candidate) ==
                       Ia64SystemCallContextValidationError::InvalidFrame);

    candidate = valid;
    candidate.MemoryStackStart += 16U;
    candidate.MemoryStackEnd += 16U;
    ASTERIA_EXPECT(runner, "16-byte but not page-aligned memory stack is rejected",
                   ValidateIa64SystemCallTrustedContext(candidate) ==
                       Ia64SystemCallContextValidationError::InvalidMemoryStack);
    candidate = valid;
    candidate.MemoryStackEnd += Ia64SystemCallStoragePageSizeBytes;
    ASTERIA_EXPECT(runner, "oversized memory stack is rejected instead of silently widened",
                   ValidateIa64SystemCallTrustedContext(candidate) ==
                       Ia64SystemCallContextValidationError::InvalidMemoryStack);
    candidate = valid;
    candidate.MemoryStackEnd = candidate.MemoryStackStart;
    ASTERIA_EXPECT(runner, "empty memory stack is rejected",
                   ValidateIa64SystemCallTrustedContext(candidate) ==
                       Ia64SystemCallContextValidationError::InvalidMemoryStack);

    candidate = valid;
    candidate.RseBackingStoreStart += 16U;
    candidate.RseBackingStoreEnd += 16U;
    ASTERIA_EXPECT(runner, "16-byte but not page-aligned RSE range is rejected",
                   ValidateIa64SystemCallTrustedContext(candidate) ==
                       Ia64SystemCallContextValidationError::InvalidRseBackingStore);
    candidate = valid;
    candidate.RseBackingStoreEnd += Ia64SystemCallStoragePageSizeBytes;
    ASTERIA_EXPECT(runner, "oversized RSE range is rejected instead of silently widened",
                   ValidateIa64SystemCallTrustedContext(candidate) ==
                       Ia64SystemCallContextValidationError::InvalidRseBackingStore);
    candidate = valid;
    candidate.RseBackingStoreEnd -= Ia64SystemCallRseTransitionSizeBytes;
    ASTERIA_EXPECT(runner, "transition-only RSE storage leaves no P64 backing store and is rejected",
                   ValidateIa64SystemCallTrustedContext(candidate) ==
                       Ia64SystemCallContextValidationError::InvalidRseBackingStore);
    candidate = valid;
    candidate.RseBackingStoreStart = candidate.MemoryStackStart;
    candidate.RseBackingStoreEnd = candidate.RseBackingStoreStart + Ia64SystemCallRseBackingStoreSizeBytes;
    ASTERIA_EXPECT(runner, "memory stack and RSE backing store must be dedicated and disjoint",
                   ValidateIa64SystemCallTrustedContext(candidate) ==
                       Ia64SystemCallContextValidationError::StorageRangesOverlap);

    candidate = valid;
    candidate.FrameAddress = candidate.MemoryStackStart;
    ASTERIA_EXPECT(runner, "entry frame cannot alias the dedicated memory stack",
                   ValidateIa64SystemCallTrustedContext(candidate) ==
                       Ia64SystemCallContextValidationError::FrameAliasesStorage);
    candidate = valid;
    candidate.FrameAddress = candidate.RseBackingStoreEnd - 16U;
    ASTERIA_EXPECT(runner, "entry frame cannot partially alias the RSE backing store",
                   ValidateIa64SystemCallTrustedContext(candidate) ==
                       Ia64SystemCallContextValidationError::FrameAliasesStorage);

    candidate = valid;
    candidate.BridgeContextAddress = 0U;
    ASTERIA_EXPECT(runner, "missing trusted bridge context is rejected",
                   ValidateIa64SystemCallTrustedContext(candidate) ==
                       Ia64SystemCallContextValidationError::InvalidBridgeContextAddress);
    candidate = valid;
    candidate.BridgeContextAddress = 0x0040'0004U;
    ASTERIA_EXPECT(runner, "trusted bridge-context address must be eight-byte aligned",
                   ValidateIa64SystemCallTrustedContext(candidate) ==
                       Ia64SystemCallContextValidationError::InvalidBridgeContextAddress);
    candidate = valid;
    candidate.BridgeContextAddress = Ia64SystemCallFirstNonKernelRegionAddress;
    ASTERIA_EXPECT(runner, "complete trusted bridge context must remain in kernel region zero",
                   ValidateIa64SystemCallTrustedContext(candidate) ==
                       Ia64SystemCallContextValidationError::InvalidBridgeContextAddress);
}

void TestFrameConfiguration(TestContext& runner) noexcept
{
    const Foundation::UInt64 validIfs{Ia64SystemCallIfsValidMask | 6U};
    ASTERIA_EXPECT(runner, "direct cover captures a valid reserved-zero six-output IFS",
                   ValidateIa64SystemCallFrameConfiguration(validIfs) == Ia64SystemCallFrameConfigurationError::None);
    const Foundation::UInt64 capturedLocalFrame{Ia64SystemCallIfsValidMask | 7U | (1U << 7U)};
    ASTERIA_EXPECT(runner, "direct cover exposes SOL one for fixed-window rejection",
                   ValidateIa64SystemCallFrameConfiguration(capturedLocalFrame) ==
                       Ia64SystemCallFrameConfigurationError::InvalidLocalSize);
    ASTERIA_EXPECT(runner, "six-output nonrotating CFM is valid",
                   ValidateIa64SystemCallFrameConfiguration(validIfs) == Ia64SystemCallFrameConfigurationError::None);
    ASTERIA_EXPECT(runner, "covered CFM requires the IFS valid bit",
                   ValidateIa64SystemCallFrameConfiguration(validIfs & ~Ia64SystemCallIfsValidMask) ==
                       Ia64SystemCallFrameConfigurationError::InvalidValidBit);
    ASTERIA_EXPECT(runner, "covered IFS reserved bits are rejected",
                   ValidateIa64SystemCallFrameConfiguration(validIfs | (Foundation::UInt64{1U} << 38U)) ==
                       Ia64SystemCallFrameConfigurationError::ReservedBitsSet);
    ASTERIA_EXPECT(runner, "CFM cannot name more than 96 stacked registers",
                   ValidateIa64SystemCallFrameConfiguration(Ia64SystemCallIfsValidMask | 97U) ==
                       Ia64SystemCallFrameConfigurationError::InvalidFrameSize);
    ASTERIA_EXPECT(runner, "any CFM local region is outside the fixed SOL-zero syscall ABI",
                   ValidateIa64SystemCallFrameConfiguration(Ia64SystemCallIfsValidMask | 6U | (7U << 7U)) ==
                       Ia64SystemCallFrameConfigurationError::InvalidLocalSize);
    ASTERIA_EXPECT(runner, "SOL one is rejected even when six output registers remain",
                   ValidateIa64SystemCallFrameConfiguration(Ia64SystemCallIfsValidMask | 7U | (1U << 7U)) ==
                       Ia64SystemCallFrameConfigurationError::InvalidLocalSize);
    ASTERIA_EXPECT(runner, "rotating CFM is outside the fixed syscall ABI",
                   ValidateIa64SystemCallFrameConfiguration(Ia64SystemCallIfsValidMask | 6U | (1U << 14U)) ==
                       Ia64SystemCallFrameConfigurationError::RotatingFrame);
    ASTERIA_EXPECT(runner, "fewer than six output registers cannot carry the syscall request",
                   ValidateIa64SystemCallFrameConfiguration(Ia64SystemCallIfsValidMask | 5U) ==
                       Ia64SystemCallFrameConfigurationError::InsufficientOutputRegisters);
}

void TestInterruptedStateValidation(TestContext& runner) noexcept
{
    const Ia64SystemCallEntryFrame valid{MakeValidFrame()};
    ASTERIA_EXPECT(runner, "translated collecting PL3 native bank-one syscall state is valid",
                   ValidateIa64SystemCallInterruptedState(valid) ==
                       Ia64SystemCallInterruptedStateValidationError::None);

    Ia64SystemCallEntryFrame candidate{valid};
    candidate.CurrentProcessorStatus |= 0x4000U;
    ASTERIA_EXPECT(runner, "current interruption delivery state is checked",
                   ValidateIa64SystemCallInterruptedState(candidate) ==
                       Ia64SystemCallInterruptedStateValidationError::IncompatibleCurrentState);
    candidate = valid;
    candidate.CurrentProcessorStatus |= Ia64SystemCallFloatingPointDisableMask;
    ASTERIA_EXPECT(runner, "architectural entry must expose both floating halves as enabled",
                   ValidateIa64SystemCallInterruptedState(candidate) ==
                       Ia64SystemCallInterruptedStateValidationError::IncompatibleCurrentState);
    candidate = valid;
    candidate.InterruptedProcessorStatus |= Ia64SystemCallFloatingPointDisableMask;
    ASTERIA_EXPECT(runner, "disabled user floating halves remain valid interrupted state",
                   ValidateIa64SystemCallInterruptedState(candidate) ==
                       Ia64SystemCallInterruptedStateValidationError::None);
    candidate = valid;
    candidate.InterruptedProcessorStatus &= ~(Foundation::UInt64{1U} << 44U);
    ASTERIA_EXPECT(runner, "interrupted bank zero is rejected",
                   ValidateIa64SystemCallInterruptedState(candidate) ==
                       Ia64SystemCallInterruptedStateValidationError::IncompatibleInterruptedState);
    candidate = valid;
    candidate.BreakImmediate = Ia64SystemCallBreakImmediate - 1U;
    ASTERIA_EXPECT(runner, "only the native syscall break immediate is accepted",
                   ValidateIa64SystemCallInterruptedState(candidate) ==
                       Ia64SystemCallInterruptedStateValidationError::WrongBreakImmediate);
    candidate = valid;
    candidate.HardwareInterruptionFunctionState = Ia64SystemCallIfsValidMask;
    ASTERIA_EXPECT(runner, "break entry requires hardware cr.ifs.v clear",
                   ValidateIa64SystemCallInterruptedState(candidate) ==
                       Ia64SystemCallInterruptedStateValidationError::HardwareIfsIsValid);
    candidate = valid;
    candidate.UserRseConfiguration = 0U;
    ASTERIA_EXPECT(runner, "interrupted PL3 enforced-lazy RSE configuration is required",
                   ValidateIa64SystemCallInterruptedState(candidate) ==
                       Ia64SystemCallInterruptedStateValidationError::InvalidRseConfiguration);
    candidate = valid;
    candidate.CapturedUserInterruptionFunctionState = Ia64SystemCallIfsValidMask | 5U;
    ASTERIA_EXPECT(runner, "invalid captured CFM is rejected by interrupted-state validation",
                   ValidateIa64SystemCallInterruptedState(candidate) ==
                       Ia64SystemCallInterruptedStateValidationError::InvalidFrameConfiguration);
    candidate = valid;
    candidate.CapturedUserInterruptionFunctionState = Ia64SystemCallIfsValidMask | 7U | (1U << 7U);
    ASTERIA_EXPECT(runner, "interrupted-state validation rejects a shifted SOL-one output window",
                   ValidateIa64SystemCallInterruptedState(candidate) ==
                       Ia64SystemCallInterruptedStateValidationError::InvalidFrameConfiguration);
}

void TestResumeAdvance(TestContext& runner) noexcept
{
    for (Foundation::UInt64 slot{0U}; slot < 3U; ++slot)
    {
        Ia64SystemCallResumeTarget target{
            .InstructionPointer = 0xDEADU,
            .ProcessorStatus = 0xBEEFU,
        };
        const Foundation::UInt64 status =
            Ia64SystemCallInterruptedProcessorStatusValue | (slot << Ia64SystemCallInstructionSlotShift);
        const Ia64SystemCallResumeAdvanceError error = AdvanceIa64SystemCallResumeTarget(0x4000U, status, target);
        const Foundation::UInt64 expectedSlot{(slot == 2U) ? 0U : slot + 1U};
        const Foundation::UInt64 expectedPointer{(slot == 2U) ? 0x4000U + Ia64SystemCallBundleSizeBytes : 0x4000U};
        ASTERIA_EXPECT(runner, "each legal RI advances to the architecturally next instruction",
                       error == Ia64SystemCallResumeAdvanceError::None, target.InstructionPointer == expectedPointer,
                       (target.ProcessorStatus & Ia64SystemCallInstructionSlotMask) ==
                           (expectedSlot << Ia64SystemCallInstructionSlotShift));
    }

    Ia64SystemCallResumeTarget target{
        .InstructionPointer = 0xDEADU,
        .ProcessorStatus = 0xBEEFU,
    };
    ASTERIA_EXPECT(runner, "unaligned cr.iip is rejected without changing the destination",
                   AdvanceIa64SystemCallResumeTarget(0x4008U, Ia64SystemCallInterruptedProcessorStatusValue, target) ==
                       Ia64SystemCallResumeAdvanceError::InvalidBundleAddress,
                   target.InstructionPointer == 0xDEADU);
    ASTERIA_EXPECT(runner, "reserved RI value three is rejected without changing the destination",
                   AdvanceIa64SystemCallResumeTarget(0x4000U,
                                                     Ia64SystemCallInterruptedProcessorStatusValue |
                                                         (Foundation::UInt64{3U} << Ia64SystemCallInstructionSlotShift),
                                                     target) ==
                       Ia64SystemCallResumeAdvanceError::InvalidInstructionSlot,
                   target.InstructionPointer == 0xDEADU);
    ASTERIA_EXPECT(runner, "slot-two bundle advance rejects address wrap atomically",
                   AdvanceIa64SystemCallResumeTarget(0xFFFF'FFFF'FFFF'FFF0ULL,
                                                     Ia64SystemCallInterruptedProcessorStatusValue |
                                                         (Foundation::UInt64{2U} << Ia64SystemCallInstructionSlotShift),
                                                     target) == Ia64SystemCallResumeAdvanceError::AddressOverflow,
                   target.InstructionPointer == 0xDEADU);
}

[[nodiscard]] bool FormatsUnknownBreak(const Foundation::UInt64 immediate, const Foundation::UInt64 instructionPointer,
                                       const char* const expected) noexcept
{
    Platform::Ia64::System::Ia64UnknownBreakDiagnostic line{};
    for (char& character : line)
    {
        character = '#';
    }
    Platform::Ia64::System::FormatIa64UnknownBreakDiagnostic(immediate, instructionPointer, line);
    for (Foundation::Size index{0U}; index < sizeof(line); ++index)
    {
        if (line[index] != expected[index])
        {
            return false;
        }
    }
    return true;
}

void TestUnknownBreakDiagnostic(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "an unknown break names its immediate and bundle in one FAIL line with a terminator",
                   FormatsUnknownBreak(0U, 0x2000'0000'0400'0040ULL,
                                       "ASTERIA:IA64-USER-BREAK:FAIL:IMMEDIATE=0x0000000000000000"
                                       ":IIP=0x2000000004000040\r\n"),
                   FormatsUnknownBreak(0x1F'FFFFU, 0xFEDC'BA98'7654'3210ULL,
                                       "ASTERIA:IA64-USER-BREAK:FAIL:IMMEDIATE=0x00000000001fffff"
                                       ":IIP=0xfedcba9876543210\r\n"));
}

void TestFrameTransitions(TestContext& runner) noexcept
{
    Ia64SystemCallEntryFrame frame{};
    ASTERIA_EXPECT(runner, "inactive frame activates exactly once",
                   ActivateIa64SystemCallEntryFrame(frame) == Ia64SystemCallFrameTransitionError::None,
                   frame.State == Ia64SystemCallEntryFrameState::Active);
    ASTERIA_EXPECT(runner, "active frame rejects re-entry without changing state",
                   ActivateIa64SystemCallEntryFrame(frame) == Ia64SystemCallFrameTransitionError::AlreadyActive,
                   frame.State == Ia64SystemCallEntryFrameState::Active);
    ASTERIA_EXPECT(runner, "active frame returns to inactive after the modeled restore",
                   DeactivateIa64SystemCallEntryFrame(frame) == Ia64SystemCallFrameTransitionError::None,
                   frame.State == Ia64SystemCallEntryFrameState::Inactive);
    ASTERIA_EXPECT(runner, "inactive frame cannot be completed twice",
                   DeactivateIa64SystemCallEntryFrame(frame) == Ia64SystemCallFrameTransitionError::NotActive);
    frame.State = static_cast<Ia64SystemCallEntryFrameState>(9U);
    ASTERIA_EXPECT(runner, "malformed frame state is never normalized silently",
                   ActivateIa64SystemCallEntryFrame(frame) == Ia64SystemCallFrameTransitionError::InvalidState,
                   DeactivateIa64SystemCallEntryFrame(frame) == Ia64SystemCallFrameTransitionError::InvalidState);
}

void TestBridgeDispatch(TestContext& runner) noexcept
{
    BridgeRunLoopFixture fixture{};
    Ia64SystemCallEntryFrame frame{MakeValidFrame(0U)};
    const Foundation::UInt64 ingressInstructionPointer{frame.InterruptedInstructionPointer};
    const Foundation::UInt64 ingressProcessorStatus{frame.InterruptedProcessorStatus};
    const Foundation::UInt64 ingressR8{frame.Registers.R8};
    const Foundation::UInt64 ingressR9{frame.Registers.R9};
    const Foundation::UInt64 ingressR10{frame.Registers.R10};
    ASTERIA_EXPECT(runner, "portable ingress rejection leaves response and resume state uncommitted",
                   DispatchIa64SystemCallBridge(frame, fixture.GetRunLoop(), fixture.GetUserMemory(),
                                                fixture.GetScratch()) == Ia64SystemCallBridgeStatus::IngressRejected,
                   frame.InterruptedInstructionPointer == ingressInstructionPointer,
                   frame.InterruptedProcessorStatus == ingressProcessorStatus, frame.Registers.R8 == ingressR8,
                   frame.Registers.R9 == ingressR9, frame.Registers.R10 == ingressR10,
                   frame.State == Ia64SystemCallEntryFrameState::Active);

    for (Foundation::UInt64 bit{0U}; bit < 7U; ++bit)
    {
        frame = MakeValidFrame();
        frame.Registers.InputNatBits = Foundation::UInt64{1U} << bit;
        ASTERIA_EXPECT(runner, "each known input NaT maps to canonical InvalidArguments zeros",
                       DispatchIa64SystemCallBridge(frame, fixture.GetRunLoop(), fixture.GetUserMemory(),
                                                    fixture.GetScratch()) == Ia64SystemCallBridgeStatus::None,
                       frame.Registers.R8 == static_cast<Foundation::UInt64>(SystemCallError::InvalidArguments),
                       frame.Registers.R9 == 0U, frame.Registers.R10 == 0U);
    }

    frame = MakeValidFrame();
    frame.Registers.InputNatBits = 0x80U;
    const Foundation::UInt64 originalInstructionPointer{frame.InterruptedInstructionPointer};
    const Foundation::UInt64 originalR8{frame.Registers.R8};
    ASTERIA_EXPECT(runner, "malformed NaT metadata is fatal and leaves the active record uncommitted",
                   DispatchIa64SystemCallBridge(frame, fixture.GetRunLoop(), fixture.GetUserMemory(),
                                                fixture.GetScratch()) == Ia64SystemCallBridgeStatus::InvalidNatMetadata,
                   frame.InterruptedInstructionPointer == originalInstructionPointer, frame.Registers.R8 == originalR8,
                   frame.State == Ia64SystemCallEntryFrameState::Active);

    frame = MakeValidFrame();
    frame.State = Ia64SystemCallEntryFrameState::Inactive;
    ASTERIA_EXPECT(runner, "bridge refuses a frame that native entry did not activate",
                   DispatchIa64SystemCallBridge(frame, fixture.GetRunLoop(), fixture.GetUserMemory(),
                                                fixture.GetScratch()) == Ia64SystemCallBridgeStatus::InvalidFrameState);
    frame = MakeValidFrame();
    frame.Reserved = 1U;
    ASTERIA_EXPECT(runner, "bridge treats nonzero frame reserved state as fatal",
                   DispatchIa64SystemCallBridge(frame, fixture.GetRunLoop(), fixture.GetUserMemory(),
                                                fixture.GetScratch()) == Ia64SystemCallBridgeStatus::InvalidFrameState);
    frame = MakeValidFrame();
    frame.BreakImmediate = 0U;
    ASTERIA_EXPECT(
        runner, "bridge fail-stops incompatible interrupted state",
        DispatchIa64SystemCallBridge(frame, fixture.GetRunLoop(), fixture.GetUserMemory(), fixture.GetScratch()) ==
            Ia64SystemCallBridgeStatus::InvalidInterruptedState);
    frame = MakeValidFrame();
    frame.CapturedUserInterruptionFunctionState = Ia64SystemCallIfsValidMask | 7U | (1U << 7U);
    const Foundation::UInt64 localFrameInstructionPointer{frame.InterruptedInstructionPointer};
    const Foundation::UInt64 localFrameProcessorStatus{frame.InterruptedProcessorStatus};
    const Foundation::UInt64 localFrameR8{frame.Registers.R8};
    const Foundation::UInt64 localFrameR9{frame.Registers.R9};
    const Foundation::UInt64 localFrameR10{frame.Registers.R10};
    ASTERIA_EXPECT(
        runner, "SOL-one bridge rejection leaves the active record uncommitted",
        DispatchIa64SystemCallBridge(frame, fixture.GetRunLoop(), fixture.GetUserMemory(), fixture.GetScratch()) ==
            Ia64SystemCallBridgeStatus::InvalidInterruptedState,
        frame.InterruptedInstructionPointer == localFrameInstructionPointer,
        frame.InterruptedProcessorStatus == localFrameProcessorStatus, frame.Registers.R8 == localFrameR8,
        frame.Registers.R9 == localFrameR9, frame.Registers.R10 == localFrameR10,
        frame.State == Ia64SystemCallEntryFrameState::Active);
    frame = MakeValidFrame();
    frame.InterruptedInstructionPointer += 8U;
    ASTERIA_EXPECT(
        runner, "bridge treats malformed resume coordinates as fatal",
        DispatchIa64SystemCallBridge(frame, fixture.GetRunLoop(), fixture.GetUserMemory(), fixture.GetScratch()) ==
            Ia64SystemCallBridgeStatus::InvalidResumeTarget);

    frame = MakeValidFrame();
    const Foundation::UInt64 hostedInstructionPointer{frame.InterruptedInstructionPointer};
    const Foundation::UInt64 hostedProcessorStatus{frame.InterruptedProcessorStatus};
    Ia64SystemCallBridgeContext bridgeContext{fixture.GetBridgeContext()};
    const Foundation::UInt64 hostedStatus{AsteriaIa64SystemCallBridge(&frame, &bridgeContext)};
    ASTERIA_EXPECT(runner, "two-argument hosted bridge reports unavailable without mutation",
                   hostedStatus ==
                       static_cast<Foundation::UInt64>(Ia64SystemCallBridgeStatus::NativeTransportUnavailable),
                   frame.Registers.R8 == 0xA8U, frame.InterruptedInstructionPointer == hostedInstructionPointer,
                   frame.InterruptedProcessorStatus == hostedProcessorStatus);

    frame = MakeValidFrame();
    const Ia64SystemCallEntryFrame originalFrame{frame};
    ASTERIA_EXPECT(runner, "null bridge context is rejected before mutating the active frame",
                   AsteriaIa64SystemCallBridge(&frame, nullptr) ==
                       static_cast<Foundation::UInt64>(Ia64SystemCallBridgeStatus::InvalidBridgeContext),
                   __builtin_memcmp(&frame, &originalFrame, sizeof(frame)) == 0);
    auto* const malformedContext =
        reinterpret_cast<Ia64SystemCallBridgeContext*>(static_cast<Foundation::UIntPointer>(1U));
    ASTERIA_EXPECT(runner, "malformed bridge geometry is rejected without dereferencing it",
                   AsteriaIa64SystemCallBridge(&frame, malformedContext) ==
                       static_cast<Foundation::UInt64>(Ia64SystemCallBridgeStatus::InvalidBridgeContext),
                   __builtin_memcmp(&frame, &originalFrame, sizeof(frame)) == 0);
    bridgeContext.UserMemory = nullptr;
    ASTERIA_EXPECT(runner, "null bridge collaborator is rejected before frame publication",
                   AsteriaIa64SystemCallBridge(&frame, &bridgeContext) ==
                       static_cast<Foundation::UInt64>(Ia64SystemCallBridgeStatus::InvalidBridgeContext),
                   __builtin_memcmp(&frame, &originalFrame, sizeof(frame)) == 0);
}

void TestConfiguredWaitImmediateCompletion(TestContext& runner) noexcept
{
    ResumableWaitEntryFixture fixture;
    const bool initialized = fixture.Initialize();
    if (initialized)
    {
        fixture.SetWaitRequest(true);
    }
    const Kernel::Ipc::NotificationError signal =
        initialized ? fixture.Signal() : Kernel::Ipc::NotificationError::NotInitialized;
    if (initialized)
    {
        fixture.Dispatch(true);
    }
    const auto continuation = fixture.GetContinuationSnapshot();
    const auto dispatcher = fixture.GetDispatcherSnapshot();
    const Ia64SystemCallEntryFrame& frame = fixture.GetFrame();

    ASTERIA_EXPECT(runner, "configured syscall 8 returns an immediate exact WaitSet completion", initialized,
                   signal == Kernel::Ipc::NotificationError::None, fixture.BridgeWasCalled(),
                   fixture.GetStatus() == Ia64SystemCallBridgeStatus::None,
                   frame.Registers.R8 == static_cast<Foundation::UInt64>(SystemCallError::None),
                   frame.Registers.R9 == ResumableWaitEntryFixture::WaitTag,
                   frame.Registers.R10 == fixture.GetRegistrationSequence());
    ASTERIA_EXPECT(runner, "immediate completion cancels the uncommitted continuation reservation",
                   continuation.State == Platform::Ia64::System::Ia64SystemCallWaitContinuationState::Idle,
                   continuation.Epoch == 0U, continuation.PendingEpoch == 0U, continuation.EntryFrameRetained == 0U,
                   dispatcher.State == Kernel::Ipc::WaitSetThreadDispatcherState::Idle);
}

void TestConfiguredWaitPrecommitCancellation(TestContext& runner) noexcept
{
    ResumableWaitEntryFixture fixture;
    const bool initialized = fixture.Initialize();
    if (initialized)
    {
        fixture.SetWaitRequest(false);
        fixture.Dispatch(true);
    }
    const auto continuation = fixture.GetContinuationSnapshot();
    const auto dispatcher = fixture.GetDispatcherSnapshot();
    const Ia64SystemCallEntryFrame& frame = fixture.GetFrame();

    ASTERIA_EXPECT(runner, "capability rejection completes normally and cancels the reserved slot", initialized,
                   fixture.BridgeWasCalled(), fixture.GetStatus() == Ia64SystemCallBridgeStatus::None,
                   frame.Registers.R8 == static_cast<Foundation::UInt64>(SystemCallError::InvalidHandle),
                   frame.Registers.R9 == 0U, frame.Registers.R10 == 0U,
                   continuation.State == Platform::Ia64::System::Ia64SystemCallWaitContinuationState::Idle,
                   continuation.Epoch == 0U, continuation.PendingEpoch == 0U,
                   dispatcher.State == Kernel::Ipc::WaitSetThreadDispatcherState::Idle);
}

void TestConfiguredWaitSuspensionFailure(TestContext& runner) noexcept
{
    ResumableWaitEntryFixture fixture;
    const bool initialized = fixture.Initialize();
    Foundation::UInt64 originalInstructionPointer{0U};
    Foundation::UInt64 originalR8{0U};
    if (initialized)
    {
        fixture.SetWaitRequest(true);
        originalInstructionPointer = fixture.GetFrame().InterruptedInstructionPointer;
        originalR8 = fixture.GetFrame().Registers.R8;
        fixture.Dispatch(true);
    }
    const auto continuation = fixture.GetContinuationSnapshot();
    const auto dispatcher = fixture.GetDispatcherSnapshot();
    const Ia64SystemCallEntryFrame& frame = fixture.GetFrame();

    ASTERIA_EXPECT(runner, "a host-immediate raw return is fatal after exact wait preparation", initialized,
                   fixture.BridgeWasCalled(),
                   fixture.GetStatus() == Ia64SystemCallBridgeStatus::WaitContinuationRejected,
                   frame.InterruptedInstructionPointer == originalInstructionPointer, frame.Registers.R8 == originalR8);
    ASTERIA_EXPECT(runner, "post-prepare contradiction retains portable wait state and clears native action records",
                   continuation.State == Platform::Ia64::System::Ia64SystemCallWaitContinuationState::Faulted,
                   continuation.TerminalError ==
                       Platform::Ia64::System::Ia64SystemCallWaitContinuationError::NativeTransitionUnavailable,
                   continuation.Epoch == 1U, continuation.EntryFrameRetained == 0U,
                   dispatcher.State == Kernel::Ipc::WaitSetThreadDispatcherState::Prepared);
}

void TestRawBridgeWaitPairValidation(TestContext& runner) noexcept
{
    ResumableWaitEntryFixture fixture;
    const bool initialized = fixture.Initialize();
    const Ia64SystemCallEntryFrame originalFrame{fixture.GetFrame()};
    const Foundation::UInt64 configuredStatus = initialized ? fixture.InvokeRawBridge(true, true) : 0U;
    const Foundation::UInt64 continuationOnlyStatus = initialized ? fixture.InvokeRawBridge(true, false) : 0U;
    const Foundation::UInt64 dispatcherOnlyStatus = initialized ? fixture.InvokeRawBridge(false, true) : 0U;

    ASTERIA_EXPECT(
        runner, "raw bridge accepts a complete wait pair and rejects both asymmetric pairs atomically", initialized,
        configuredStatus == static_cast<Foundation::UInt64>(Ia64SystemCallBridgeStatus::NativeTransportUnavailable),
        continuationOnlyStatus == static_cast<Foundation::UInt64>(Ia64SystemCallBridgeStatus::InvalidBridgeContext),
        dispatcherOnlyStatus == static_cast<Foundation::UInt64>(Ia64SystemCallBridgeStatus::InvalidBridgeContext),
        __builtin_memcmp(&fixture.GetFrame(), &originalFrame, sizeof(originalFrame)) == 0);
}

void TestDirectBridgeWaitPairMismatch(TestContext& runner, const bool useWaitContinuation,
                                      const bool useWaitSetDispatcher, const char* const description) noexcept
{
    ResumableWaitEntryFixture fixture;
    const bool initialized = fixture.Initialize();
    if (initialized)
    {
        fixture.SetCapabilityQueryRequest();
    }
    const Ia64SystemCallEntryFrame originalFrame{fixture.GetFrame()};
    if (initialized)
    {
        fixture.Dispatch(useWaitContinuation, useWaitSetDispatcher);
    }
    const auto continuation = fixture.GetContinuationSnapshot();
    const auto dispatcher = fixture.GetDispatcherSnapshot();

    ASTERIA_EXPECT(runner, description, initialized, fixture.BridgeWasCalled(),
                   fixture.GetStatus() == Ia64SystemCallBridgeStatus::InvalidBridgeContext,
                   __builtin_memcmp(&fixture.GetFrame(), &originalFrame, sizeof(originalFrame)) == 0,
                   continuation.State == Platform::Ia64::System::Ia64SystemCallWaitContinuationState::Idle,
                   continuation.Epoch == 0U, dispatcher.State == Kernel::Ipc::WaitSetThreadDispatcherState::Idle);
}

void TestWaitNullCompatibility(TestContext& runner) noexcept
{
    ResumableWaitEntryFixture fixture;
    const bool initialized = fixture.Initialize();
    if (initialized)
    {
        fixture.SetWaitRequest(true);
        fixture.Dispatch(false);
    }
    const auto continuation = fixture.GetContinuationSnapshot();
    const auto dispatcher = fixture.GetDispatcherSnapshot();
    const Ia64SystemCallEntryFrame& frame = fixture.GetFrame();

    ASTERIA_EXPECT(runner, "null wait collaborators preserve syscall 8 completed-only compatibility", initialized,
                   fixture.BridgeWasCalled(), fixture.GetStatus() == Ia64SystemCallBridgeStatus::None,
                   frame.Registers.R8 == static_cast<Foundation::UInt64>(SystemCallError::InvalidConfiguration),
                   frame.Registers.R9 == 0U, frame.Registers.R10 == 0U,
                   continuation.State == Platform::Ia64::System::Ia64SystemCallWaitContinuationState::Idle,
                   dispatcher.State == Kernel::Ipc::WaitSetThreadDispatcherState::Idle);
}

void TestConfiguredWaitPreservesLegacyDispatch(TestContext& runner) noexcept
{
    ResumableWaitEntryFixture fixture;
    const bool initialized = fixture.Initialize();
    if (initialized)
    {
        fixture.SetCapabilityQueryRequest();
        fixture.Dispatch(true);
    }
    const auto continuation = fixture.GetContinuationSnapshot();
    const auto dispatcher = fixture.GetDispatcherSnapshot();
    const Ia64SystemCallEntryFrame& frame = fixture.GetFrame();

    ASTERIA_EXPECT(runner, "configured wait collaborators do not intercept syscall numbers 1 through 7", initialized,
                   fixture.BridgeWasCalled(), fixture.GetStatus() == Ia64SystemCallBridgeStatus::None,
                   frame.Registers.R8 == static_cast<Foundation::UInt64>(SystemCallError::InvalidHandle),
                   continuation.State == Platform::Ia64::System::Ia64SystemCallWaitContinuationState::Idle,
                   continuation.Epoch == 0U, dispatcher.State == Kernel::Ipc::WaitSetThreadDispatcherState::Idle);
}

void TestNullWaitPairPreservesLegacyDispatch(TestContext& runner) noexcept
{
    ResumableWaitEntryFixture fixture;
    const bool initialized = fixture.Initialize();
    if (initialized)
    {
        fixture.SetCapabilityQueryRequest();
        fixture.Dispatch(false);
    }
    const auto continuation = fixture.GetContinuationSnapshot();
    const auto dispatcher = fixture.GetDispatcherSnapshot();
    const Ia64SystemCallEntryFrame& frame = fixture.GetFrame();

    ASTERIA_EXPECT(runner, "null wait pair keeps syscall numbers 1 through 7 on ordinary ingress", initialized,
                   fixture.BridgeWasCalled(), fixture.GetStatus() == Ia64SystemCallBridgeStatus::None,
                   frame.Registers.R8 == static_cast<Foundation::UInt64>(SystemCallError::InvalidHandle),
                   frame.Registers.R9 == 0U, frame.Registers.R10 == 0U,
                   continuation.State == Platform::Ia64::System::Ia64SystemCallWaitContinuationState::Idle,
                   continuation.Epoch == 0U, dispatcher.State == Kernel::Ipc::WaitSetThreadDispatcherState::Idle);
}

// A thread hook that records its calls.
class RecordingSystemCallThread final : public Platform::Ia64::System::IIa64SystemCallThread
{
public:
    [[nodiscard]] bool DispatchWait(const Kernel::System::SystemCallRequest&,
                                    Kernel::System::SystemCallResponse& response) noexcept override
    {
        response = Kernel::System::SystemCallResponse{.Error = SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        return true;
    }

    [[nodiscard]] bool DispatchExit(const Kernel::System::SystemCallRequest& request,
                                    Kernel::System::SystemCallResponse& response) noexcept override
    {
        ++Exits;
        ExitCode = request.Arguments[0U];
        response =
            Kernel::System::SystemCallResponse{.Error = SystemCallError::UnknownSystemCall, .Value0 = 0U, .Value1 = 0U};
        return ExitAccepted;
    }

    [[nodiscard]] bool PrepareManagement() noexcept override
    {
        ++Preparations;
        return PreparationAccepted;
    }

    Foundation::Size Exits{0U};
    Foundation::UInt64 ExitCode{0U};
    Foundation::Size Preparations{0U};
    bool ExitAccepted{true};
    bool PreparationAccepted{true};
};

// The bridge hands ThreadExit to the thread instead of the ingress, and runs
// the thread's management preparation (the reap) before every management
// Call and no other; a refused hook fails the bridge without a response.
void TestThreadHooks(TestContext& runner) noexcept
{
    BridgeRunLoopFixture fixture{};
    RecordingSystemCallThread thread;
    const auto dispatch = [&](Ia64SystemCallEntryFrame& frame) noexcept
    {
        return DispatchIa64SystemCallBridge(frame, fixture.GetRunLoop(), fixture.GetUserMemory(), fixture.GetScratch(),
                                            nullptr, nullptr, &thread, nullptr, nullptr);
    };
    Ia64SystemCallEntryFrame exit{MakeValidFrame()};
    exit.Registers.R15 = static_cast<Foundation::UInt64>(SystemCallNumber::ThreadExit);
    exit.Registers.R32 = 0x5EU;
    exit.Registers.R33 = 0U;
    const Ia64SystemCallBridgeStatus exited = dispatch(exit);
    Ia64SystemCallEntryFrame refusedExit{exit};
    refusedExit.Registers.R8 = 0xA8U;
    thread.ExitAccepted = false;
    const Ia64SystemCallBridgeStatus exitRefused = dispatch(refusedExit);

    Ia64SystemCallEntryFrame destroy{MakeValidFrame()};
    destroy.Registers.R15 = static_cast<Foundation::UInt64>(SystemCallNumber::ObjectDestroy);
    const Ia64SystemCallBridgeStatus prepared = dispatch(destroy);
    const Foundation::Size preparationsAfterDestroy = thread.Preparations;
    Ia64SystemCallEntryFrame query{MakeValidFrame()};
    static_cast<void>(dispatch(query));
    const Foundation::Size preparationsAfterQuery = thread.Preparations;
    thread.PreparationAccepted = false;
    Ia64SystemCallEntryFrame refusedDestroy{MakeValidFrame()};
    refusedDestroy.Registers.R15 = static_cast<Foundation::UInt64>(SystemCallNumber::ObjectCreate);
    const Ia64SystemCallBridgeStatus preparationRefused = dispatch(refusedDestroy);
    ASTERIA_EXPECT(runner, "ThreadExit goes to the thread hook and management calls reap first",
                   exited == Ia64SystemCallBridgeStatus::None, thread.Exits == 2U, thread.ExitCode == 0x5EU,
                   exit.Registers.R8 == static_cast<Foundation::UInt64>(SystemCallError::UnknownSystemCall),
                   exitRefused == Ia64SystemCallBridgeStatus::ThreadExitRejected, refusedExit.Registers.R8 == 0xA8U,
                   // The fixture's run loop has no active thread, so the ingress rejects
                   // the destroy after the preparation ran.
                   prepared == Ia64SystemCallBridgeStatus::IngressRejected, preparationsAfterDestroy == 1U,
                   preparationsAfterQuery == 1U,
                   preparationRefused == Ia64SystemCallBridgeStatus::ManagementPreparationRejected,
                   thread.Preparations == 2U, refusedDestroy.Registers.R8 == 0xA8U);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64SystemCallEntryTests"};
    Asteria::Test::TestContextValidation(runner);
    Asteria::Test::TestFrameConfiguration(runner);
    Asteria::Test::TestInterruptedStateValidation(runner);
    Asteria::Test::TestResumeAdvance(runner);
    Asteria::Test::TestUnknownBreakDiagnostic(runner);
    Asteria::Test::TestFrameTransitions(runner);
    Asteria::Test::TestBridgeDispatch(runner);
    Asteria::Test::TestConfiguredWaitImmediateCompletion(runner);
    Asteria::Test::TestConfiguredWaitPrecommitCancellation(runner);
    Asteria::Test::TestConfiguredWaitSuspensionFailure(runner);
    Asteria::Test::TestRawBridgeWaitPairValidation(runner);
    Asteria::Test::TestDirectBridgeWaitPairMismatch(
        runner, true, false, "direct bridge rejects a continuation without a WaitSet dispatcher atomically");
    Asteria::Test::TestDirectBridgeWaitPairMismatch(
        runner, false, true, "direct bridge rejects a WaitSet dispatcher without a continuation atomically");
    Asteria::Test::TestWaitNullCompatibility(runner);
    Asteria::Test::TestConfiguredWaitPreservesLegacyDispatch(runner);
    Asteria::Test::TestNullWaitPairPreservesLegacyDispatch(runner);
    Asteria::Test::TestThreadHooks(runner);
    return runner.Finish();
}
