// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/System/Ia64UserThreadTable.hpp"

#include "Kernel/Scheduler/ThreadPool.hpp"
#include "Kernel/System/SystemCallDispatcher.hpp"
#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"
#include "Platform/Ia64/Diagnostics/Ia64EvidenceRecord.hpp"
#include "Platform/Ia64/Ia64UserVirtualLayout.hpp"

// Target images define placement new once (Kernel/Boot/KernelEntry.cpp).
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

extern "C" void AsteriaIa64InitializeUserThreadEntryDescriptor(Asteria::Foundation::UInt64* descriptor) noexcept;
extern "C" [[noreturn]] void AsteriaIa64UserThreadEntry(void* argument) noexcept;

namespace Asteria::Platform::Ia64::System
{
using Foundation::Size;
using Foundation::UInt64;
using Kernel::Scheduler::UserThreadBackendError;

namespace
{
[[nodiscard]] UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
}

// Regions 1-7 hold user addresses; region 0 is the kernel's identity region.
[[nodiscard]] constexpr bool IsUserRegion(const UInt64 address) noexcept
{
    return (address >> 61U) != 0U;
}

constexpr UInt64 EntryBundleAlignment{16U};
constexpr Size DescriptorBytes{16U};
} // Namespace

// One created-thread slot per thread-pool slot, as on AMD64.
static_assert(Ia64UserThreadTable::Slots == Kernel::Scheduler::ThreadPool::Capacity);

Ia64UserThreadTable::Ia64UserThreadTable(const Ia64UserThreadTableCollaborators& collaborators) noexcept
    : Ia64UserThreadTable{collaborators, Foundation::MakeIndexSequence<Slots>{}}
{
}

bool Ia64UserThreadTable::Configure(const Ia64UserDomainRecord& domain) noexcept
{
    if (m_configured || (domain.Protection == nullptr) || (domain.UserMemory == nullptr) ||
        (domain.Supervisor == nullptr) || (m_collaborators.RunLoop == nullptr) ||
        (m_collaborators.RuntimeMemory == nullptr) || (m_collaborators.Clock == nullptr) ||
        (m_collaborators.Scratch == nullptr) || (m_collaborators.Preemption == nullptr))
    {
        return false;
    }
    m_domains[0U] = domain;
    for (Size index{0U}; index < Slots; ++index)
    {
        Slot& slot = m_slots[index];
        slot.Owner = this;
        slot.Index = index;
        slot.Bridge = Ia64SystemCallBridgeContext{
            .RunLoop = m_collaborators.RunLoop,
            .UserMemory = domain.UserMemory,
            .Scratch = m_collaborators.Scratch,
            .WaitContinuation = nullptr,
            .WaitSetDispatcher = nullptr,
            .Thread = &slot,
            .PagerResolution = domain.PagerResolution,
            .ObjectManagement = domain.ObjectManagement,
        };
        const Interrupt::IIa64UserFaultHandler* const handler = &slot;
        if ((m_collaborators.RuntimeMemory->AdmitSystemCallEntryContext(m_collaborators.Processor, EntrySlot(slot),
                                                                        AddressOf(&slot.Bridge)) !=
             Memory::Ia64BspRuntimeSystemCallPublicationError::None) ||
            (m_collaborators.RuntimeMemory->AdmitUserFaultEntryContext(m_collaborators.Processor, EntrySlot(slot),
                                                                       AddressOf(handler)) !=
             Memory::Ia64RuntimeInterruptVectorTablePublicationError::None))
        {
            return false;
        }
    }
#if defined(ASTERIA_TARGET_IA64)
    // Closed KernelElf profile: native IA-64 progress uses a direct service
    // call, as shootdown progress does, rather than an extra function descriptor.
    Platform::Synchronization::BindThreadRequests(this, nullptr);
#endif
    m_configured = true;
    return true;
}

Kernel::Memory::FaultDelivery
Ia64ChannelSupervisor::DeliverFault(Kernel::Scheduler::Thread& thread, Scheduler::Ia64KernelContext& context,
                                    const Kernel::Memory::PagerFaultSource& source) noexcept
{
    auto& channel = m_channel->GetChannel();
    const auto prepared = channel.PrepareFault(source);
    if (prepared != Kernel::Memory::PagerChannelError::None)
    {
        return prepared == Kernel::Memory::PagerChannelError::Busy ? Kernel::Memory::FaultDelivery::Busy
                                                                   : Kernel::Memory::FaultDelivery::Failed;
    }
    return m_loop->FinishFaultAndDispatch(thread, context, channel, m_receipt, m_failure) &&
                   m_receipt.ThreadStorage == &thread &&
                   m_receipt.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Active
               ? Kernel::Memory::FaultDelivery::Resolved
               : Kernel::Memory::FaultDelivery::Failed;
}

bool Ia64UserThreadTable::ConfigureCreatedDomains(IIa64FaultSupervisor& supervisor,
                                                  Kernel::System::IObjectManagement& objects) noexcept
{
    if (!m_configured || (m_createdSupervisor != nullptr))
    {
        return false;
    }
    m_createdSupervisor = &supervisor;
    m_createdObjects = &objects;
    return true;
}

bool Ia64UserThreadTable::RecordDomain(const Kernel::Protection::CreatedDomain& domain) noexcept
{
    if ((m_createdSupervisor == nullptr) || (domain.Protection == nullptr) || (domain.AddressSpace == nullptr) ||
        (domain.SupervisionTag == 0U))
    {
        return false;
    }
    for (Size created{0U}; created < Ia64CreatedDomainRecords; ++created)
    {
        Ia64UserDomainRecord& record = m_domains[1U + created];
        if (record.Protection != nullptr)
        {
            continue;
        }
        if (!m_accessors[created].Bind(*domain.AddressSpace, domain.AddressSpaceIdentity))
        {
            return false;
        }
        // The selected channel supplies supervision; FaultResolve independently selects the issuer
        // from the token.
        if (domain.Channel != nullptr)
        {
            m_channelSupervisors[created].Bind(*m_collaborators.RunLoop, *domain.Channel, *m_createdSupervisor);
        }
        record = Ia64UserDomainRecord{
            .Protection = domain.Protection,
            .UserMemory = &m_accessors[created],
            .ObjectManagement = m_createdObjects,
            .PagerResolution = nullptr,
            .Supervisor = domain.Channel != nullptr ? &m_channelSupervisors[created] : m_createdSupervisor,
            .SupervisionTag = domain.SupervisionTag,
        };
        ++m_domainsRecorded;
        return true;
    }
    return false;
}

bool Ia64UserThreadTable::RetireDomain(const Kernel::Protection::ProtectionDomain& domain) noexcept
{
    for (Size created{0U}; created < Ia64CreatedDomainRecords; ++created)
    {
        Ia64UserDomainRecord& record = m_domains[1U + created];
        if (record.Protection != &domain)
        {
            continue;
        }
        // A slot stays Bound from its start until the reaper frees it, a
        // killed or exited thread's included.
        for (const Slot& slot : m_slots)
        {
            if ((slot.State != SlotState::Free) && (slot.Domain == &record))
            {
                return false;
            }
        }
        if (!m_accessors[created].Reset())
        {
            return false;
        }
        record = Ia64UserDomainRecord{};
        ++m_domainsRetired;
        return true;
    }
    return false;
}

const Ia64UserDomainRecord* Ia64UserThreadTable::FindCallerDomain() const noexcept
{
    const auto active = m_collaborators.RunLoop->GetActiveProtectionDomainBinding();
    return active.HasValue() ? FindDomain(*active.GetValue().Storage) : nullptr;
}

const Ia64UserDomainRecord*
Ia64UserThreadTable::FindDomain(const Kernel::Protection::ProtectionDomain& domain) const noexcept
{
    for (const Ia64UserDomainRecord& record : m_domains)
    {
        if ((record.Protection != nullptr) && (record.Protection == &domain))
        {
            return &record;
        }
    }
    return nullptr;
}

UInt64 Ia64UserThreadTable::ComputeNativeSlotBytes() noexcept
{
    return sizeof(Slot) + Ia64SystemCallMemoryStackSizeBytes + Ia64SystemCallRseBackingStoreSizeBytes +
           sizeof(Ia64SystemCallEntryFrame) + sizeof(Interrupt::Ia64PageFaultFrame);
}

UInt64 Ia64UserThreadTable::GetNativeSlotBytes() const noexcept
{
    return ComputeNativeSlotBytes();
}

Kernel::Scheduler::UserThreadSchedule Ia64UserThreadTable::GetSchedule() const noexcept
{
    return Kernel::Scheduler::UserThreadSchedule{
        .Configuration = MakeIa64UserSchedulingConfiguration(
            static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << m_collaborators.Processor.GetValue())),
        .Now = Kernel::Object::MonotonicTime{m_collaborators.Clock->GetTicks()},
    };
}

UserThreadBackendError Ia64UserThreadTable::Reserve(Kernel::Scheduler::Thread& thread,
                                                    const Kernel::Scheduler::ThreadId threadIdentity,
                                                    Kernel::Scheduler::SchedulingContext& context,
                                                    const Kernel::Scheduler::SchedulingContextId contextIdentity,
                                                    const Kernel::Scheduler::UserThreadStart& start) noexcept
{
    if (!m_configured)
    {
        return UserThreadBackendError::Exhausted;
    }
    // The caller's record reads the entry descriptor (its root is the
    // active one); the thread runs in the start's domain.
    const Ia64UserDomainRecord* const caller = FindCallerDomain();
    const Ia64UserDomainRecord* const domain = (start.Domain != nullptr) ? FindDomain(*start.Domain) : caller;
    if ((caller == nullptr) || (domain == nullptr))
    {
        return UserThreadBackendError::Rejected;
    }
    for (Slot& slot : m_slots)
    {
        if (!slot.Request.Reserve())
        {
            continue;
        }
        if (!PrepareLaunch(slot, *caller, start))
        {
            Clear(slot);
            return UserThreadBackendError::Rejected;
        }
        // Only the bridge's address was admitted, and a Free slot's thread
        // has left the kernel for good, so its contents may change.
        slot.Domain = domain;
        slot.Bridge.UserMemory = &slot.Access;
        slot.Bridge.PagerResolution = domain->PagerResolution;
        slot.Bridge.ObjectManagement = domain->ObjectManagement;
        slot.Home = Kernel::Scheduler::CpuId{0U};
        slot.Thread = &thread;
        slot.ThreadIdentity = threadIdentity;
        slot.ContextIdentity = contextIdentity;
        slot.Scheduling = &context;
        const auto prepared = Prepare(slot, context);
        if (prepared != UserThreadBackendError::None)
        {
            return prepared;
        }
        slot.State = SlotState::Pending;
        slot.Request.PublishReserved(thread, slot.Home);
        return UserThreadBackendError::None;
    }
    return UserThreadBackendError::Exhausted;
}

UserThreadBackendError Ia64UserThreadTable::Start(Kernel::Scheduler::Thread& thread,
                                                  const Kernel::Scheduler::ThreadId threadIdentity,
                                                  Kernel::Scheduler::SchedulingContext& context,
                                                  const Kernel::Scheduler::SchedulingContextId contextIdentity,
                                                  const Kernel::Scheduler::UserThreadStart&) noexcept
{
    for (Slot& slot : m_slots)
    {
        if (!slot.Request.Matches(thread))
        {
            continue;
        }
        const auto current = Platform::Synchronization::CurrentProcessor();
        // After publication the home can run, reap and reuse
        // the native slot before this poster returns. Retain the destination.
        const auto home = slot.Home;
        const bool remote = current.IsValid() && current.GetValue() != home.GetValue();
        if (remote)
        {
            slot.Request.Publish(thread, home, true);
            Platform::Synchronization::KickProcessor(home);
            return UserThreadBackendError::None;
        }
        const auto admitted = Admit(slot, thread, threadIdentity, context, contextIdentity);
        if (admitted == UserThreadBackendError::None)
        {
            slot.Request.Publish(thread, home, false);
            ++m_started;
        }
        return admitted;
    }
    return UserThreadBackendError::Faulted;
}

// The entry names a function descriptor {ip, gp} in the caller's memory; the launch runs the bundle
// at ip with gp, the page holding ip as its code range, and the 32 KiB below the stack top as its
// RSE backing store and memory stack.
bool Ia64UserThreadTable::PrepareLaunch(Slot& slot, const Ia64UserDomainRecord& caller,
                                        const Kernel::Scheduler::UserThreadStart& start) noexcept
{
    const UInt64 top = start.UserStackTop;
    if (((start.Entry % sizeof(UInt64)) != 0U) || ((top % Ia64UserPageSizeBytes) != 0U) ||
        (top < (UserRseBytes + UserStackBytes)) || !IsUserRegion(top - UserRseBytes - UserStackBytes))
    {
        return false;
    }
    const auto range = Kernel::Memory::UserMemoryRange::Create(start.Entry, DescriptorBytes);
    UInt64 descriptor[2U]{};
    if (!range.HasValue() ||
        (caller.UserMemory->CopyFromUser(
             range.GetValue(), Kernel::Memory::WritableKernelBuffer{
                                   .Data = reinterpret_cast<Foundation::UInt8*>(descriptor),
                                   .SizeBytes = DescriptorBytes}) != Kernel::Memory::UserMemoryAccessError::None))
    {
        return false;
    }
    const UInt64 ip = descriptor[0U];
    if (((ip % EntryBundleAlignment) != 0U) || !IsUserRegion(ip))
    {
        return false;
    }
    slot.LaunchRequest = Scheduler::Ia64UserModeLaunchRequest{
        .Code = {.VirtualStart = ip - (ip % Ia64UserPageSizeBytes), .SizeBytes = Ia64UserPageSizeBytes},
        .MemoryStack = {.VirtualStart = top - UserStackBytes, .SizeBytes = UserStackBytes},
        .RseBackingStore = {.VirtualStart = top - UserStackBytes - UserRseBytes, .SizeBytes = UserRseBytes},
        .EntryBundleAddress = ip,
        .InitialGlobalPointer = descriptor[1U],
        .InitialArguments = {.Register4 = start.Argument, .Register5 = 0U, .Register6 = 0U, .Register7 = 0U},
        .Requirements =
            Scheduler::Ia64UserModeLaunchRequirements{
                .CurrentProcessorStatusMask = Scheduler::Ia64UserModeCurrentProcessorStatusMask,
                .CurrentProcessorStatusValue = Scheduler::Ia64UserModeCurrentProcessorStatusValue,
                .UserProcessorStatus = Scheduler::Ia64UserModePreemptibleProcessorStatus,
                .ImplementedVirtualAddressMostSignificantBit =
                    m_collaborators.ImplementedVirtualAddressMostSignificantBit,
                .CurrentPrivilegeLevel = 0U,
                .CurrentVirtualMachineMode = 0U,
                .CurrentInstructionSet = 0U,
                .CurrentRegisterBank = 1U,
                .Reserved = {},
            },
        .SupervisorContinuation = &slot.Supervisor,
        .Reserved = 0U,
    };
    return true;
}

UserThreadBackendError Ia64UserThreadTable::Prepare(Slot& slot, Kernel::Scheduler::SchedulingContext& context) noexcept
{
    const auto binding = slot.Domain->Protection->GetBinding();
    if (!binding.HasValue() || !slot.Access.Bind(*binding.GetValue().AddressSpaceStorage,
                                                 binding.GetValue().AddressSpace, slot.Domain->SupervisionTag == 0U))
    {
        Clear(slot);
        return UserThreadBackendError::Rejected;
    }
    slot.Home = Kernel::Scheduler::CpuId{0U};
    if (context.GetConfiguration().CpuAffinityMask != (Foundation::ProcessorMask{1U} << slot.Home.GetValue()))
    {
        Clear(slot);
        return UserThreadBackendError::Rejected;
    }

    const auto storage = m_collaborators.RuntimeMemory->GetEntryStorage(EntrySlot(slot));
    if (!storage.HasValue())
    {
        Clear(slot);
        return UserThreadBackendError::Faulted;
    }
    const Memory::Ia64BspRuntimeUserEntryStorageView& view = storage.GetValue();
    const Scheduler::WritableIa64KernelStorageView stack{
        .Data = view.MemoryStack, .SizeBytes = view.MemoryStackSizeBytes, .VirtualStart = view.MemoryStackStart};
    const Scheduler::WritableIa64KernelStorageView rse{.Data = view.RseBackingStore,
                                                       .SizeBytes = view.RseBackingStoreSizeBytes,
                                                       .VirtualStart = view.RseBackingStoreStart};
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    AsteriaIa64InitializeUserThreadEntryDescriptor(slot.EntryDescriptor);
    const auto entry = reinterpret_cast<Scheduler::Ia64KernelContextEntry>(
        static_cast<Foundation::UIntPointer>(AddressOf(slot.EntryDescriptor)));
#else
    const Scheduler::Ia64KernelContextEntry entry{&AsteriaIa64UserThreadEntry};
#endif
    // Each start prepares a fresh context on the slot's entry storage and
    // clears f32-f127 so the previous thread's state cannot reach the next.
    slot.HighFloating = Scheduler::Ia64HighFloatingArea{};
    slot.Context = ::new (static_cast<void*>(slot.ContextStorage)) Scheduler::Ia64KernelContext{};
    if (!slot.Context->AttachHighFloatingArea(slot.HighFloating) ||
        (slot.Context->Prepare(m_collaborators.ImplementedVirtualAddressMostSignificantBit, stack, rse, entry, &slot) !=
         Scheduler::Ia64KernelContextError::None))
    {
        Clear(slot);
        return UserThreadBackendError::Faulted;
    }
    slot.Supervisor = Scheduler::Ia64UserModeSupervisorContinuation{
        .Registers = {},
        .MemoryStack = {.VirtualStart = view.MemoryStackStart, .SizeBytes = view.MemoryStackSizeBytes},
        .RseBackingStore = {.VirtualStart = view.RseBackingStoreStart, .SizeBytes = view.RseBackingStoreSizeBytes},
        .State = Scheduler::Ia64UserModeSupervisorContinuationState::Fresh,
        .Owner = Scheduler::Ia64UserModeSupervisorContinuationOwner::None,
    };
    if (!Scheduler::Ia64UserModeLaunch::Validate(slot.LaunchRequest).HasValue())
    {
        Clear(slot);
        return UserThreadBackendError::Rejected;
    }
    if (!m_collaborators.RunLoop->ReserveBinding(slot.BindingReservation))
    {
        Clear(slot);
        return UserThreadBackendError::Exhausted;
    }
    return UserThreadBackendError::None;
}

UserThreadBackendError Ia64UserThreadTable::Admit(Slot& slot, Kernel::Scheduler::Thread& thread,
                                                  const Kernel::Scheduler::ThreadId threadIdentity,
                                                  Kernel::Scheduler::SchedulingContext& context,
                                                  const Kernel::Scheduler::SchedulingContextId contextIdentity) noexcept
{
    // Start from the home's clock, not the poster's.
    if (context.SetInitialTime(Kernel::Object::MonotonicTime{m_collaborators.Clock->GetTicks()}) !=
            Kernel::Scheduler::SchedulingContextError::None ||
        thread.MakeReady() != Kernel::Scheduler::ThreadError::None)
    {
        Clear(slot);
        return UserThreadBackendError::Faulted;
    }
    Scheduler::Ia64ProcessorRunLoop& loop = *m_collaborators.RunLoop;
    const Kernel::Scheduler::ProcessorKernelRunLoopError bound =
        loop.Bind(thread, *slot.Context, *slot.Domain->Protection, &slot.BindingReservation);
    if (bound != Kernel::Scheduler::ProcessorKernelRunLoopError::None)
    {
        Clear(slot);
        return (bound == Kernel::Scheduler::ProcessorKernelRunLoopError::BindingCapacityExceeded)
                   ? UserThreadBackendError::Exhausted
                   : UserThreadBackendError::Faulted;
    }
    auto& observed = Diagnostics::GetIa64EvidenceRecord().UserProcessors.Threads[static_cast<Size>(&slot - m_slots)];
    observed = {threadIdentity.GetValue(),
                slot.Domain->Protection->GetIdentity().GetValue(),
                0U,
                slot.Domain->SupervisionTag,
                0U,
                0U,
                0U,
                0U};
    // The home made the thread Ready. A binding never enqueued may be undone
    // while the creator runs.
    if (!loop.EnqueueBoundThread(contextIdentity, context, threadIdentity, thread, slot.Failure))
    {
        if (loop.Unbind(thread) != Kernel::Scheduler::ProcessorKernelRunLoopError::None)
        {
            return UserThreadBackendError::Faulted;
        }
        Clear(slot);
        return UserThreadBackendError::Exhausted;
    }
    slot.Thread = &thread;
    slot.ThreadIdentity = threadIdentity;
    slot.ContextIdentity = contextIdentity;
    slot.Deferrals = 0U;
    slot.State = SlotState::Bound;
    return UserThreadBackendError::None;
}

void Ia64UserThreadTable::Clear(Slot& slot) noexcept
{
    slot.Owner->m_collaborators.RunLoop->CancelBinding(slot.BindingReservation);
    static_cast<void>(slot.Access.Reset());
    slot.Home = Kernel::Scheduler::CpuId{0U};
    if (slot.Context != nullptr)
    {
        slot.Context->~Ia64KernelContext();
    }
    slot.Context = nullptr;
    slot.Domain = nullptr;
    slot.Thread = nullptr;
    slot.ThreadIdentity = {};
    slot.ContextIdentity = {};
    slot.Deferrals = 0U;
    slot.LaunchRequest = {};
    slot.Supervisor = {};
    slot.State = SlotState::Free;
    slot.Scheduling = nullptr;
    slot.Request.Release();
}

bool Ia64UserThreadTable::IsReleased(const Kernel::Scheduler::Thread& thread) const noexcept
{
    // A quiescing pool slot's storage hosts no new thread before the pool
    // frees it, so a thread no slot holds was reaped (or never started).
    for (const Slot& slot : m_slots)
    {
        if (slot.Request.Matches(thread))
        {
            return false;
        }
    }
    return true;
}

Kernel::Scheduler::UserThreadTerminateError Ia64UserThreadTable::Terminate(Kernel::Scheduler::Thread& thread,
                                                                           const UInt64 exitCode) noexcept
{
    using Kernel::Scheduler::UserThreadTerminateError;
    const auto current = Platform::Synchronization::CurrentProcessor();
    for (Slot& slot : m_slots)
    {
        if (!slot.Request.Matches(thread))
        {
            continue;
        }
        Kernel::Scheduler::CpuId home{};
        if (!slot.Request.PostTerminate(thread, exitCode, &home))
        {
            return UserThreadTerminateError::Pending;
        }
        if (current.IsValid() && current.GetValue() != home.GetValue())
        {
            Platform::Synchronization::KickProcessor(home);
            return UserThreadTerminateError::Pending;
        }
        if (slot.Request.Take(home, nullptr) != Kernel::Scheduler::ThreadRequestKind::Terminate)
        {
            return UserThreadTerminateError::Pending;
        }
        const auto answer = TerminateLocal(thread, exitCode);
        slot.Request.Complete();
        return answer;
    }
    return thread.GetSnapshot().State == Kernel::Scheduler::ThreadState::Exited ? UserThreadTerminateError::None
                                                                                : UserThreadTerminateError::Faulted;
}

Kernel::Scheduler::UserThreadTerminateError Ia64UserThreadTable::TerminateLocal(Kernel::Scheduler::Thread& thread,
                                                                                const UInt64 exitCode) noexcept
{
    using Kernel::Scheduler::UserThreadTerminateError;
    if (thread.GetSnapshot().State == Kernel::Scheduler::ThreadState::Exited)
    {
        return UserThreadTerminateError::None;
    }
    for (Slot& slot : m_slots)
    {
        if ((slot.State != SlotState::Bound) || (slot.Thread != &thread))
        {
            continue;
        }
        switch (thread.GetSnapshot().State)
        {
        case Kernel::Scheduler::ThreadState::Ready:
            break;
        case Kernel::Scheduler::ThreadState::Running:
            return UserThreadTerminateError::Running;
        case Kernel::Scheduler::ThreadState::Blocked:
            return AbortWait(slot.Index, exitCode);
        default:
            return UserThreadTerminateError::Faulted;
        }
        Kernel::Scheduler::ReadyQueueState removed{};
        if (!m_collaborators.RunLoop->RemoveBoundThread(thread, removed, slot.Failure))
        {
            // Retained or selected by the run loop: not removable now.
            return ((slot.Failure.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::ExternalFrameRetained) ||
                    (slot.Failure.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::WrongActiveThread))
                       ? UserThreadTerminateError::Blocked
                       : UserThreadTerminateError::Faulted;
        }
        if (thread.Terminate(exitCode) != Kernel::Scheduler::ThreadError::None)
        {
            return UserThreadTerminateError::Faulted;
        }
        ++m_kills;
        return UserThreadTerminateError::None;
    }
    return UserThreadTerminateError::Faulted;
}

// A wait-blocked thread is woken with its wait aborted; it ends with
// exitCode as it resumes (DispatchWait). A thread blocked on a fault is left
// to its supervisor, and queue pressure to a later kill.
Kernel::Scheduler::UserThreadTerminateError Ia64UserThreadTable::AbortWait(const Size index,
                                                                           const UInt64 exitCode) noexcept
{
    using Kernel::Scheduler::UserThreadTerminateError;
    if (GetWaitState(index) != Kernel::Ipc::WaitSetThreadDispatcherState::Blocked)
    {
        return UserThreadTerminateError::Blocked;
    }
    m_slots[index].AbortExitCode = exitCode;
    const auto aborted = m_waits[index].Abort();
    if (!aborted.HasValue())
    {
        return (GetWaitState(index) == Kernel::Ipc::WaitSetThreadDispatcherState::Faulted)
                   ? UserThreadTerminateError::Faulted
                   : UserThreadTerminateError::Blocked;
    }
    if (!aborted.GetValue().Aborted)
    {
        return UserThreadTerminateError::Blocked;
    }
    ++m_waitAborts;
    ++m_kills;
    return UserThreadTerminateError::None;
}

void Ia64UserThreadTable::ServiceRequests(const bool schedulingPoint) noexcept
{
    if (schedulingPoint)
    {
        ServiceOwnedRequests<true>();
    }
    else
    {
        ServiceOwnedRequests<false>();
    }
}

void Ia64UserThreadTable::ServiceWaitingRequests() noexcept
{
    ServiceOwnedRequests<false>();
}

template <bool SchedulingPoint> void Ia64UserThreadTable::ServiceOwnedRequests() noexcept
{
    auto current = Platform::Synchronization::CurrentProcessor();
    if (!current.IsValid())
    {
        current = Kernel::Scheduler::CpuId{0U};
    }
    // Thread homes are CPU 0; AP waits must not read the BSP run loop.
    if (current.GetValue() != m_collaborators.Processor.GetValue())
    {
        return;
    }
    const auto active = m_collaborators.RunLoop->GetSnapshot();
    // External-frame admission/return owns a retained context until it lands.
    // Service at the next complete scheduling point, not inside that transition.
    if (active.State != Kernel::Scheduler::ProcessorKernelRunLoopState::Idle &&
        active.State != Kernel::Scheduler::ProcessorKernelRunLoopState::Active)
    {
        return;
    }
    const auto* protectedThread = SchedulingPoint ? nullptr : active.ActiveThread;
    for (Slot& slot : m_slots)
    {
        const auto request = slot.Request.Take(current, protectedThread);
        if (request == Kernel::Scheduler::ThreadRequestKind::Bind)
        {
            if (Admit(slot, *slot.Thread, slot.ThreadIdentity, *slot.Scheduling, slot.ContextIdentity) !=
                UserThreadBackendError::None)
            {
                Platform::Synchronization::StopProcessorWait(Kernel::Diagnostics::StopCode::InvariantViolated, 3U);
            }
            ++m_started;
            slot.Request.Complete();
        }
        else if (request == Kernel::Scheduler::ThreadRequestKind::Terminate)
        {
            const UInt64 exitCode = slot.Request.ExitCode();
            const bool running = slot.Thread->GetSnapshot().State == Kernel::Scheduler::ThreadState::Running;
            if (running)
            {
                // Waits retain the running continuation. Keep their static call closure free of
                // every context-switch path.
                if constexpr (!SchedulingPoint)
                {
                    slot.Request.Defer(request);
                    continue;
                }
                else
                {
                    if (active.ActiveThread != slot.Thread)
                    {
                        slot.Request.Defer(request);
                        continue;
                    }
                    slot.Request.Complete();
                    ++m_kills;
                    Platform::Synchronization::LeaveThreadRequestService();
                    static_cast<void>(End(slot, exitCode));
                    Platform::Synchronization::StopProcessorWait(Kernel::Diagnostics::StopCode::InvariantViolated, 3U);
                }
            }
            const auto answer = TerminateLocal(*slot.Thread, exitCode);
            slot.Request.Complete();
            // Fault-blocked requests are withdrawn: only FaultResolve can end them.
            if (answer == Kernel::Scheduler::UserThreadTerminateError::Faulted)
            {
                Platform::Synchronization::StopProcessorWait(Kernel::Diagnostics::StopCode::InvariantViolated, 3U);
            }
        }
    }
}

bool Ia64UserThreadTable::Reap() noexcept
{
    for (Slot& slot : m_slots)
    {
        if ((slot.State != SlotState::Bound) ||
            (slot.Thread->GetSnapshot().State != Kernel::Scheduler::ThreadState::Exited))
        {
            continue;
        }
        if (!slot.Request.Retire(*slot.Thread))
        {
            continue;
        }
        // An exit in a system call leaves the slot's system-call frame
        // Active, a termination in a fault its fault frame Retained; a
        // thread killed after its wait completed never consumed it.
        if ((m_collaborators.RunLoop->Unbind(*slot.Thread) != Kernel::Scheduler::ProcessorKernelRunLoopError::None) ||
            !m_collaborators.RuntimeMemory->ResetEntryFrames(EntrySlot(slot)) ||
            !m_waits[slot.Index].Reset().HasValue())
        {
            return false;
        }
        Clear(slot);
        ++m_reaped;
    }
    return true;
}

bool Ia64UserThreadTable::Preempt(const Size index) noexcept
{
    Platform::Synchronization::ServiceThreadRequests(true);
    if ((index >= Slots) || (m_slots[index].State != SlotState::Bound))
    {
        return false;
    }
    Slot& slot = m_slots[index];
    return m_collaborators.RunLoop->FinishAndDispatch(*slot.Thread, *slot.Context,
                                                      Kernel::Scheduler::ThreadRunDisposition::Ready, 0U, slot.Receipt,
                                                      slot.Failure) &&
           (slot.Receipt.ThreadStorage == slot.Thread) &&
           (slot.Receipt.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Active);
}

bool Ia64UserThreadTable::SelectEntryContexts(const Slot& slot) noexcept
{
    return m_collaborators.RuntimeMemory->SelectEntryContexts(m_collaborators.Processor, EntrySlot(slot));
}

bool Ia64UserThreadTable::Exit(Slot& slot, const Kernel::System::SystemCallRequest& request,
                               Kernel::System::SystemCallResponse& response) noexcept
{
    UInt64 exitCode{0U};
    if (!Kernel::System::SystemCallDispatcher::DecodeThreadExit(request, exitCode))
    {
        response = Kernel::System::SystemCallResponse{
            .Error = Kernel::System::SystemCallError::InvalidArguments, .Value0 = 0U, .Value1 = 0U};
        return true;
    }
    ++m_exits;
    return End(slot, exitCode);
}

bool Ia64UserThreadTable::End(Slot& slot, const UInt64 exitCode) noexcept
{
    // A created domain's supervisor learns of the exit; the pager's own threads it polls for.
    if ((slot.Domain->SupervisionTag != 0U) && (slot.Domain->Supervisor != nullptr))
    {
        ++m_domainExits;
        slot.Domain->Supervisor->OnExit();
    }
    // An exited run never resumes; the reaper frees the slot later.
    static_cast<void>(m_collaborators.RunLoop->FinishAndDispatch(*slot.Thread, *slot.Context,
                                                                 Kernel::Scheduler::ThreadRunDisposition::Exited,
                                                                 exitCode, slot.Receipt, slot.Failure));
    return false;
}

// As the pager's wait (Ia64PagerRuntime::DispatchWait): a wait that cannot complete at once blocks
// the thread's run, and the thread continues here once ProgressWaits woke it and the run loop
// admitted it again; another thread selected its own entry contexts meanwhile.
bool Ia64UserThreadTable::DispatchWait(Slot& slot, const Kernel::System::SystemCallRequest& request,
                                       Kernel::System::SystemCallResponse& response) noexcept
{
    const Size index = slot.Index;
    Kernel::System::WaitSetSystemCallDispatchResult& result = m_waitResults[index];
    Kernel::System::WaitSetSystemCallIngressFailure& failure = m_waitFailures[index];
    Scheduler::Ia64ProcessorRunLoop& loop = *m_collaborators.RunLoop;
    if (!Kernel::System::WaitSetSystemCallIngress::Dispatch(loop, m_waitCalls[index], request, result, failure))
    {
        return false;
    }
    if (result.Disposition == Kernel::System::WaitSetSystemCallDisposition::SuspendPrepared)
    {
        ++m_waitsBlocked;
        if (!loop.FinishWaitAndDispatch(*slot.Thread, *slot.Context, m_waits[index], slot.Receipt, slot.Failure) ||
            (slot.Receipt.ThreadStorage != slot.Thread) ||
            (slot.Receipt.State != Kernel::Scheduler::ProcessorKernelRunLoopState::Active) ||
            !SelectEntryContexts(slot) ||
            !Kernel::System::WaitSetSystemCallIngress::ConsumeCompletion(loop, m_waitCalls[index], result, failure))
        {
            return false;
        }
        // Killed while it waited: it never returns to user mode.
        if (result.Wait.Aborted)
        {
            ++m_abortedExits;
            return End(slot, slot.AbortExitCode);
        }
    }
    response = result.Response;
    return true;
}

bool Ia64UserThreadTable::ConfigureWaits(const UInt64 ticksPerMicrosecond) noexcept
{
    if (m_waitsConfigured || (ticksPerMicrosecond == 0U) || (m_collaborators.WaitClock == nullptr))
    {
        return false;
    }
    for (Kernel::System::WaitSetSystemCallDispatcher& wait : m_waitCalls)
    {
        if (!wait.ConfigureTimeouts(*m_collaborators.WaitClock, ticksPerMicrosecond))
        {
            return false;
        }
    }
    m_waitsConfigured = true;
    return true;
}

Ia64ThreadWaitProgress Ia64UserThreadTable::ProgressWaits() noexcept
{
    Platform::Synchronization::ServiceThreadRequests(true);
    Ia64ThreadWaitProgress progress{Ia64ThreadWaitProgress::Waiting};
    for (Size index{0U}; index < Slots; ++index)
    {
        const Ia64ThreadWaitProgress slot = ProgressWait(index);
        if (slot == Ia64ThreadWaitProgress::Failed)
        {
            return slot;
        }
        progress = (slot == Ia64ThreadWaitProgress::Woken) ? slot : progress;
    }
    return progress;
}

Kernel::Ipc::WaitSetThreadDispatcherState Ia64UserThreadTable::GetWaitState(const Size index) const noexcept
{
    return m_waits[index].GetSnapshot().State;
}

// Readiness first, so a signalled member wins over a passed deadline; queue
// pressure leaves a wait for the next scheduling point.
Ia64ThreadWaitProgress Ia64UserThreadTable::ProgressWait(const Size index) noexcept
{
    const Kernel::Ipc::WaitSetThreadDispatcherState state = GetWaitState(index);
    if (state == Kernel::Ipc::WaitSetThreadDispatcherState::Faulted)
    {
        return Ia64ThreadWaitProgress::Failed;
    }
    if ((m_slots[index].State != SlotState::Bound) ||
        ((state != Kernel::Ipc::WaitSetThreadDispatcherState::Blocked) &&
         (state != Kernel::Ipc::WaitSetThreadDispatcherState::QueuePressure)))
    {
        return Ia64ThreadWaitProgress::Waiting;
    }
    const auto polled = m_waits[index].Poll();
    if (!polled.HasValue())
    {
        return Ia64ThreadWaitProgress::Failed;
    }
    return (polled.GetValue().State == Kernel::Ipc::WaitSetThreadDispatcherState::Completed)
               ? Ia64ThreadWaitProgress::Woken
               : ExpireWait(index);
}

Ia64ThreadWaitProgress Ia64UserThreadTable::ExpireWait(const Size index) noexcept
{
    if (m_collaborators.WaitClock == nullptr)
    {
        return Ia64ThreadWaitProgress::Waiting;
    }
    const auto expired = m_waits[index].Expire(m_collaborators.WaitClock->GetTicks());
    if (!expired.HasValue())
    {
        return (GetWaitState(index) == Kernel::Ipc::WaitSetThreadDispatcherState::Faulted)
                   ? Ia64ThreadWaitProgress::Failed
                   : Ia64ThreadWaitProgress::Waiting;
    }
    if (expired.GetValue().State != Kernel::Ipc::WaitSetThreadDispatcherState::Completed)
    {
        return Ia64ThreadWaitProgress::Waiting;
    }
    ++m_waitExpiries;
    return Ia64ThreadWaitProgress::Woken;
}

UInt64 Ia64UserThreadTable::GetWaitDeadline() const noexcept
{
    UInt64 earliest{0U};
    for (Size index{0U}; index < Slots; ++index)
    {
        if ((m_slots[index].State != SlotState::Bound) ||
            (GetWaitState(index) != Kernel::Ipc::WaitSetThreadDispatcherState::Blocked))
        {
            continue;
        }
        const UInt64 deadline = m_waits[index].GetDeadline();
        if ((deadline != 0U) && ((earliest == 0U) || (deadline < earliest)))
        {
            earliest = deadline;
        }
    }
    return earliest;
}

bool Ia64UserThreadTable::BlockOnFault(Slot& slot, const Kernel::Memory::FaultRecord& fault) noexcept
{
    if (slot.Domain->Supervisor == nullptr)
    {
        return false;
    }
    IIa64FaultSupervisor& supervisor = *slot.Domain->Supervisor;
    ++m_faults;
    m_exceptions += (fault.Cause == Kernel::Memory::FaultCause::Exception) ? 1U : 0U;
    if (slot.Domain->SupervisionTag != 0U)
    {
        ++m_taggedFaults;
        m_lastTag = slot.Domain->SupervisionTag;
    }
    const Kernel::Memory::PagerFaultSource source{
        .Thread = slot.ThreadIdentity,
        .AddressSpace = slot.Domain->Protection->GetSnapshot().AddressSpace,
        .SchedulingContext = slot.ContextIdentity,
        .Fault = fault,
        // Zero for the pager's domain, a boot domain.
        .SupervisionTag = slot.Domain->SupervisionTag,
        .ProtectionDomainIdentity = slot.Domain->Protection->GetIdentity().GetValue(),
    };
    switch (Kernel::Memory::DecideFaultOutcome(supervisor.DeliverFault(*slot.Thread, *slot.Context, source),
                                               slot.Deferrals))
    {
    case Kernel::Memory::FaultOutcome::Resume:
        return SelectEntryContexts(slot);
    case Kernel::Memory::FaultOutcome::Yield:
        // The supervisor serves the fault it holds once it runs; this thread
        // yields and faults again when it next runs, however long that takes.
        ++m_deferrals;
        m_longestDeferral = (slot.Deferrals > m_longestDeferral) ? slot.Deferrals : m_longestDeferral;
        return supervisor.Progress() && Preempt(slot.Index) && SelectEntryContexts(slot);
    case Kernel::Memory::FaultOutcome::Stop:
        break;
    }
    return false;
}

void Ia64UserThreadTable::Launch(Slot& slot) noexcept
{
    Kernel::Diagnostics::RecordUserProcessor(Diagnostics::GetIa64EvidenceRecord().UserProcessors, 0U,
                                             static_cast<Size>(&slot - m_slots), slot.Domain->SupervisionTag == 0U,
                                             false);
    if (SelectEntryContexts(slot))
    {
        static_cast<void>(Scheduler::Ia64UserModeLaunch::Launch(slot.LaunchRequest));
    }
}

void Ia64UserThreadTable::LaunchSlot(void* const argument) noexcept
{
    Slot& slot = *static_cast<Slot*>(argument);
    slot.Owner->Launch(slot);
}

Ia64UserThreadTableSnapshot Ia64UserThreadTable::GetSnapshot() const noexcept
{
    Size free{0U};
    for (const Slot& slot : m_slots)
    {
        free += (m_configured && (slot.State == SlotState::Free)) ? 1U : 0U;
    }
    return Ia64UserThreadTableSnapshot{
        .FreeSlots = free,
        .Started = m_started,
        .Exits = m_exits,
        .Reaped = m_reaped,
        .Faults = m_faults,
        .Deferrals = m_deferrals,
        .LongestDeferral = m_longestDeferral,
        .Kills = m_kills,
        .Exceptions = m_exceptions,
        .DomainsRecorded = m_domainsRecorded,
        .DomainsRetired = m_domainsRetired,
        .DomainExits = m_domainExits,
        .TaggedFaults = m_taggedFaults,
        .LastTag = m_lastTag,
        .WaitsBlocked = m_waitsBlocked,
        .WaitExpiries = m_waitExpiries,
        .WaitAborts = m_waitAborts,
        .AbortedExits = m_abortedExits,
    };
}

bool Ia64UserThreadTable::Slot::DispatchWait(const Kernel::System::SystemCallRequest& request,
                                             Kernel::System::SystemCallResponse& response) noexcept
{
    return Owner->DispatchWait(*this, request, response);
}

bool Ia64UserThreadTable::Slot::DispatchExit(const Kernel::System::SystemCallRequest& request,
                                             Kernel::System::SystemCallResponse& response) noexcept
{
    return Owner->Exit(*this, request, response);
}

bool Ia64UserThreadTable::Slot::PrepareManagement() noexcept
{
    return Owner->Reap();
}

Interrupt::Ia64UserFaultDisposition
Ia64UserThreadTable::Slot::HandleUserFault(Interrupt::Ia64PageFaultFrame&,
                                           const Kernel::Memory::FaultRecord& fault) noexcept
{
    return Owner->BlockOnFault(*this, fault) ? Interrupt::Ia64UserFaultDisposition::Resume
                                             : Interrupt::Ia64UserFaultDisposition::FailStop;
}

Interrupt::Ia64UserFaultDisposition
Ia64UserThreadTable::Slot::HandleUserInterrupt(Interrupt::Ia64PageFaultFrame&) noexcept
{
    return Owner->m_collaborators.Preemption->PreemptThread(Index) ? Interrupt::Ia64UserFaultDisposition::Resume
                                                                   : Interrupt::Ia64UserFaultDisposition::FailStop;
}
} // Namespace Asteria::Platform::Ia64::System

extern "C" [[noreturn]] ASTERIA_NOINLINE_NOIPA void AsteriaIa64UserThreadEntry(void* const argument) noexcept
{
    if (argument != nullptr)
    {
        Asteria::Platform::Ia64::System::Ia64UserThreadTable::LaunchSlot(argument);
    }
    Asteria::Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-THREAD:LAUNCH:FAIL\r\n");
    Asteria::Platform::Firmware::HaltAfterFirmwareExit();
}
