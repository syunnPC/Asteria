// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/System/Amd64PagerRuntime.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"

#include "Platform/Amd64/Interrupt/Amd64ApplicationProcessorStartup.hpp"
#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Amd64/Diagnostics/Amd64Stop.hpp"
#include "Platform/Amd64/Diagnostics/Amd64StopStage.hpp"

namespace Asteria::Platform::Amd64::System
{
using Foundation::Size;
using Foundation::UInt64;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;

namespace
{
// The boot runtime's charge owner, which also owns the client's objects.
constexpr Kernel::Object::ResourceDomainId PagerCharge{1U};
// A resolution that leaves remote invalidation pending waits this long for
// the application processors' service loops to drain it.

[[noreturn]] void FailLaunch() noexcept
{
    Diagnostics::StopAmd64(Kernel::Diagnostics::StopCode::BootStage,
                           static_cast<UInt64>(Diagnostics::Amd64BootStage::PagerStack));
}
} // Namespace

Amd64PagerRuntime::Amd64PagerRuntime(Kernel::Object::ObjectRegistry& registry,
                                     Kernel::Object::RevocationRegistry& revocations, Time::Amd64ApicTimer& timer,
                                     Kernel::Scheduler::ProcessorRunController& controller,
                                     Scheduler::Amd64ProcessorRunLoop& loop,
                                     Kernel::Memory::TlbShootdownDispatcher& shootdown,
                                     Hal::Memory::ITlbShootdownTransport& transport,
                                     const Kernel::Object::CapabilityValidationKey& pagerKey,
                                     const Kernel::Object::CapabilityValidationKey& tokenKey) noexcept
    : m_registry{registry}, m_revocations{revocations}, m_timer{timer}, m_controller{controller}, m_loop{loop},
      m_issuer{registry}, m_space{registry, revocations, timer, pagerKey.Word0, pagerKey.Word1},
      m_tokens{registry, revocations, timer, tokenKey.Word0, tokenKey.Word1}, m_endpoint{m_space, PagerCharge},
      m_waitSet{registry}, m_wait{registry, controller.GetWakeFacade()}, m_waitDispatcher{registry, m_wait},
      m_channel{registry, m_issuer, m_endpoint, controller.GetWakeFacade(), shootdown, transport},
      m_scheduling{registry}
{
}

void Amd64PagerRuntime::Wait() noexcept
{
    Platform::Synchronization::ProcessorWaitProgress();
}

bool Amd64PagerRuntime::Initialize(const Amd64PagerRuntimeInput& input) noexcept
{
    if (m_initialized || (input.Resources == nullptr) || (input.UserMemory == nullptr) || (input.Tables == nullptr) ||
        (input.EntryContext == nullptr) || (input.LazyPages == 0U) || ((input.LazyVirtualStart & 0xFFFU) != 0U) ||
        ((input.UserStackTop & 0xFFFU) != 0U) || (input.UserStackTop < sizeof(m_startBlock)))
    {
        return false;
    }
    if ((input.TicksPerMicrosecond != 0U) && !m_waitDispatcher.ConfigureTimeouts(m_timer, input.TicksPerMicrosecond))
    {
        return false;
    }
    m_userMemory = input.UserMemory;
    m_objectManagement = input.ObjectManagement;
    m_tables = input.Tables;
    m_entryContext = input.EntryContext;
    m_entry = input.Entry;
    m_userStackTop = input.UserStackTop;
    if (!CreateObjects() || !CreateLazyRegion(input) || !MintStartBlock(input) || !PrepareThread(input))
    {
        return false;
    }
    m_initialized = true;
    return true;
}

bool Amd64PagerRuntime::CreateObjects() noexcept
{
    if ((m_readiness.Initialize(PagerCharge) != Kernel::Ipc::NotificationError::None) ||
        (m_waitSet.Initialize(PagerCharge) != Kernel::Ipc::WaitSetError::None))
    {
        return false;
    }
    const auto endpoint = m_issuer.CreateObject(m_endpoint);
    const auto readiness = m_issuer.CreateObject(m_readiness);
    const auto waitSet = m_issuer.CreateObject(m_waitSet);
    const auto root = m_revocations.CreateRoot();
    const auto tokenRoot = m_revocations.CreateRoot();
    if (!endpoint.HasValue() || !readiness.HasValue() || !waitSet.HasValue() || !root.HasValue() ||
        !tokenRoot.HasValue())
    {
        return false;
    }
    m_root = root.GetValue();
    const Kernel::Ipc::NotificationId readinessId{m_registry, readiness.GetValue()};
    if (!m_waitSet.RegisterNotification(readinessId, m_readiness, Abi::PagerReadinessTag).HasValue() ||
        (m_channel.Configure(Kernel::Memory::PagerChannelConfiguration{
             .TokenSpace = &m_tokens,
             .Readiness = &m_readiness,
             .ShootdownProgress = this,
             .ChargeOwner = PagerCharge,
             .Sender = PagerCharge,
             .RevocationDomain = tokenRoot.GetValue(),
         }) != Kernel::Memory::PagerChannelError::None))
    {
        return false;
    }
    return MintInto(Abi::PagerBootstrapEndpointWord, endpoint.GetValue(),
                    CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Read)) &&
           MintInto(Abi::PagerBootstrapWaitSetWord, waitSet.GetValue(),
                    CapabilityRights{CapabilityRight::Wait}.With(CapabilityRight::Manage));
}

bool Amd64PagerRuntime::CreateLazyRegion(const Amd64PagerRuntimeInput& input) noexcept
{
    const Kernel::Memory::PagePermissions permissions =
        Kernel::Memory::PagePermissions{Kernel::Memory::PagePermission::Read}
            .With(Kernel::Memory::PagePermission::Write)
            .With(Kernel::Memory::PagePermission::User);
    if ((m_lazyFrame.Initialize(PagerCharge, {Kernel::Memory::PhysicalAddress{input.LazyPhysicalStart},
                                              Kernel::Memory::PageCount{input.LazyPages},
                                              Kernel::Memory::MemoryCachePolicy::WriteBack,
                                              Kernel::Memory::FrameKind::Ram,
                                              {}}) != Kernel::Memory::FrameError::None) ||
        (m_lazyMemory
             .Initialize(m_registry, input.ResourceIdentity, *input.Resources,
                         {Kernel::Memory::PageCount{input.LazyPages},
                          Kernel::Memory::MemoryCachePolicy::WriteBack,
                          permissions,
                          Kernel::Memory::MemoryCommitment::Pinned,
                          {}})
             .Error != Kernel::Memory::MemoryObjectError::None))
    {
        return false;
    }
    const auto frame = m_issuer.CreateObject(m_lazyFrame);
    const auto memory = m_issuer.CreateObject(m_lazyMemory);
    const CapabilityRights map{CapabilityRight::Map};
    return frame.HasValue() && memory.HasValue() &&
           MintInto(Abi::PagerBootstrapLazyMemoryWord, memory.GetValue(), map)
           // The lazy fault installs a writable mapping.
           && MintInto(Abi::PagerBootstrapLazyFrameWord, frame.GetValue(), map.With(CapabilityRight::Write));
}

bool Amd64PagerRuntime::MintStartBlock(const Amd64PagerRuntimeInput& input) noexcept
{
    m_startBlock[Abi::PagerBootstrapModuleBaseWord] = input.ModuleBytes == 0U ? 0U : Abi::Amd64PagerModuleBase;
    m_startBlock[Abi::PagerBootstrapModuleBytesWord] = input.ModuleBytes;

    if (input.InventoryPhysicalStart != 0U && input.InventoryPages != 0U)
    {
        if (m_inventoryFrame.InitializeReadOnly(PagerCharge,
                                                {Kernel::Memory::PhysicalAddress{input.InventoryPhysicalStart},
                                                 Kernel::Memory::PageCount{input.InventoryPages},
                                                 Kernel::Memory::MemoryCachePolicy::WriteBack,
                                                 Kernel::Memory::FrameKind::Ram,
                                                 {}}) != Kernel::Memory::FrameError::None)
        {
            return false;
        }
        const auto inventory = m_issuer.CreateObject(m_inventoryFrame);
        if (!inventory.HasValue() || !MintInto(Abi::PagerBootstrapInventoryFrameWord, inventory.GetValue(),
                                               CapabilityRights{CapabilityRight::Map}.With(CapabilityRight::Read)))
        {
            return false;
        }
        m_startBlock[Abi::PagerBootstrapInventoryPagesWord] = input.InventoryPages;
    }
    m_startBlock[Abi::PagerBootstrapLazyStartWord] = input.LazyVirtualStart;
    m_startBlock[Abi::PagerBootstrapLazyPagesWord] = input.LazyPages;
    bool device{false};
    for (const Amd64PagerDevice& entry : input.Devices)
    {
        device = device || (entry.Function.IsValid() && entry.Region.IsValid());
    }
    device = device && input.Management.IsValid();
    const UInt64 features =
        (input.Management.IsValid() ? input.ManagementFeatures : 0U) | (device ? Abi::PagerManagementDevices : 0U);
    const bool mapping = (features & Abi::PagerManagementMapping) != 0U;
    m_startBlock[Abi::PagerBootstrapI8042DeclaredWord] = input.I8042Declared;
    m_startBlock[Abi::PagerBootstrapClockTicksPerMicrosecondWord] = input.TicksPerMicrosecond;
    m_startBlock[Abi::PagerBootstrapWatchdogSecondsWord] = input.WatchdogSeconds;
    m_startBlock[Abi::PagerBootstrapSessionModeWord] = input.SessionMode;
    m_startBlock[Abi::PagerBootstrapLoginEnabledWord] = input.LoginEnabled;
    m_startBlock[Abi::PagerBootstrapManagementFeaturesWord] = features;
    m_startBlock[Abi::PagerBootstrapWindowStartWord] = mapping ? input.MappingWindowStart : 0U;
    m_startBlock[Abi::PagerBootstrapWindowPagesWord] = mapping ? input.MappingWindowPages : 0U;
    return MintInto(Abi::PagerBootstrapDiagnosticWord, input.Diagnostic,
                    CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Write)) &&
           MintInto(Abi::PagerBootstrapClientSpaceWord, input.ClientSpace.GetObjectId(),
                    CapabilityRights{CapabilityRight::Map}) &&
           (!input.Management.IsValid() || MintInto(Abi::PagerBootstrapManagementWord, input.Management,
                                                    CapabilityRights{CapabilityRight::Manage}
                                                        .With(CapabilityRight::Inspect)
                                                        .With(CapabilityRight::Code))) &&
           (!mapping || MintInto(Abi::PagerBootstrapOwnSpaceWord, input.PagerSpace.GetObjectId(),
                                 CapabilityRights{CapabilityRight::Map})) &&
           (!input.Management.IsValid() || MintDevice(input)) &&
           (!input.Management.IsValid() || !input.Scanout.Region.IsValid() || MintScanout(input));
}

// The System Display Broker's scanout: the root service hands a copy to the broker and keeps its
// own.
bool Amd64PagerRuntime::MintScanout(const Amd64PagerRuntimeInput& input) noexcept
{
    m_startBlock[Abi::PagerBootstrapScanoutModeWord] = input.Scanout.Mode[0U];
    m_startBlock[Abi::PagerBootstrapScanoutModeWord + 1U] = input.Scanout.Mode[1U];
    return MintInto(Abi::PagerBootstrapScanoutRegionWord, input.Scanout.Region,
                    CapabilityRights{CapabilityRight::Inspect}
                        .With(CapabilityRight::Duplicate)
                        .With(CapabilityRight::Transfer)
                        .With(CapabilityRight::Map)
                        .With(CapabilityRight::Read)
                        .With(CapabilityRight::Write));
}

// The root service, as Device Manager, gives a driver copies of both and keeps its own for the next
// instance.
bool Amd64PagerRuntime::MintDevice(const Amd64PagerRuntimeInput& input) noexcept
{
    const CapabilityRights handOn =
        CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Duplicate).With(CapabilityRight::Transfer);
    for (Size index{0U}; index < Abi::PagerBootstrapDevices; ++index)
    {
        const Amd64PagerDevice& entry = input.Devices[index];
        if ((entry.Function.IsValid() && entry.Region.IsValid()) &&
            (!MintInto(Abi::PagerBootstrapDeviceFunctionWord(index), entry.Function,
                       handOn.With(CapabilityRight::Invoke).With(CapabilityRight::Manage)) ||
             !MintInto(Abi::PagerBootstrapDeviceRegionWord(index), entry.Region,
                       handOn.With(CapabilityRight::Map).With(CapabilityRight::Read).With(CapabilityRight::Write))))
        {
            return false;
        }
    }
    for (Size index{0U}; index < 4U; ++index)
    {
        if (input.I8042[index].IsValid() &&
            !MintInto(Abi::PagerBootstrapI8042DataWord + index * 2U, input.I8042[index],
                      (index < 2U) ? handOn.With(CapabilityRight::Read).With(CapabilityRight::Write)
                                   : handOn.With(CapabilityRight::Invoke)))
        {
            return false;
        }
    }
    return true;
}

bool Amd64PagerRuntime::MintInto(const Size word, const Kernel::Object::ObjectId object,
                                 const CapabilityRights rights) noexcept
{
    const auto handle = m_issuer.Mint(m_space, object, rights, m_root, Kernel::Object::MonotonicTime{~UInt64{0U}}, 0U);
    if (!handle.HasValue())
    {
        return false;
    }
    m_startBlock[word] = handle.GetValue().GetSelector();
    m_startBlock[word + 1U] = handle.GetValue().GetAuthenticator();
    return true;
}

bool Amd64PagerRuntime::PrepareThread(const Amd64PagerRuntimeInput& input) noexcept
{
    if (m_protection.Initialize(m_registry, input.ResourceIdentity, *input.Resources, input.PagerSpace, m_space)
            .Error != Kernel::Protection::ProtectionDomainError::None)
    {
        return false;
    }
    m_protection.SetDiagnosticComponentKind(Abi::WatchdogRootComponent);
    const auto protection = m_issuer.CreateObject(m_protection);
    if (!protection.HasValue() ||
        m_protection.BindIdentity(m_registry, {m_registry, protection.GetValue()}).Error !=
            Kernel::Protection::ProtectionDomainError::None ||
        (m_thread.Initialize(PagerCharge) != Kernel::Scheduler::ThreadError::None))
    {
        return false;
    }
    const auto thread = m_issuer.CreateObject(m_thread);
    if (!thread.HasValue())
    {
        return false;
    }
    m_threadId = {m_registry, thread.GetValue()};
    const UInt64 now = m_timer.GetTicks();
    const Kernel::Scheduler::SchedulingContextConfiguration configuration = MakeAmd64UserSchedulingConfiguration();
    if (m_scheduling.Initialize(configuration, PagerCharge, Kernel::Object::MonotonicTime{now}) !=
        Kernel::Scheduler::SchedulingContextError::None)
    {
        return false;
    }
    const auto scheduling = m_issuer.CreateObject(m_scheduling);
    if (!scheduling.HasValue())
    {
        return false;
    }
    m_schedulingId = {m_registry, scheduling.GetValue()};
    m_kernelStackTop = input.KernelStack.VirtualStart + input.KernelStack.SizeBytes;
    // The thread's x87 and SSE state is saved and restored with its context.
    m_context.AttachExtendedState(m_extendedState);
    return (m_scheduling.Bind(m_threadId) == Kernel::Scheduler::SchedulingContextError::None) &&
           (m_context.Prepare(input.KernelStack, Launch, this) == Scheduler::Amd64KernelContextError::None);
}

bool Amd64PagerRuntime::Admit(Kernel::Scheduler::ProcessorKernelRunLoopFailure& failure) noexcept
{
    return m_initialized && (m_thread.MakeReady() == Kernel::Scheduler::ThreadError::None) &&
           (LocalLoop().Bind(m_thread, m_context, m_protection) ==
            Kernel::Scheduler::ProcessorKernelRunLoopError::None) &&
           LocalLoop().EnqueueBoundThread(m_schedulingId, m_scheduling, m_threadId, m_thread, failure);
}

bool Amd64PagerRuntime::IsRunning() const noexcept
{
    const auto snapshot = LocalLoop().GetSnapshot();
    return (snapshot.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Active) &&
           (snapshot.ActiveThread == &m_thread);
}

[[noreturn]] void Amd64PagerRuntime::Launch(void* const argument) noexcept
{
    // The run loop activated the pager's root before entering this context,
    // so the start block is written through the pager's own stack mapping.
    auto& runtime = *static_cast<Amd64PagerRuntime*>(argument);
    const UInt64 block = runtime.m_userStackTop - sizeof(runtime.m_startBlock);
    auto* const words = reinterpret_cast<UInt64*>(static_cast<Foundation::UIntPointer>(block));
    for (Size index{0U}; index < Abi::PagerBootstrapWords; ++index)
    {
        words[index] = runtime.m_startBlock[index];
    }
    auto& frame = runtime.m_launchFrame;
    frame.Rip = runtime.m_entry;
    frame.Rsp = block;
    frame.Rdi = block;
    frame.Cs = Interrupt::UserCodeSelector;
    frame.Ss = Interrupt::UserDataSelector;
    frame.Rflags = Interrupt::UserReturnRequiredFlags;
    if (!Interrupt::Amd64UserEntry::SelectKernelStack(*runtime.m_tables, *runtime.m_entryContext,
                                                      runtime.m_kernelStackTop))
    {
        FailLaunch();
    }
    Interrupt::Amd64UserEntry::Enter(frame);
}

bool Amd64PagerRuntime::DispatchSystemCall(const Kernel::System::SystemCallRequest& request,
                                           Kernel::System::SystemCallResponse& response,
                                           Kernel::System::SystemCallScratch& scratch) noexcept
{
    if (request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::WaitSetWait))
    {
        return DispatchWait(request, response);
    }
    if (!Kernel::System::SystemCallIngress::Dispatch(LocalLoop(), *m_userMemory, scratch, request, response,
                                                     m_ingressFailure, &m_channel, m_objectManagement))
    {
        return false;
    }
    if ((request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::FaultResolve)) &&
        (response.Error != Kernel::System::SystemCallError::None))
    {
        ReportRejectedResolution();
    }
    return true;
}

void Amd64PagerRuntime::ReportRejectedResolution() const noexcept
{
    const auto& failure = m_channel.GetLastResolutionFailure();
    const UInt64 detail = (static_cast<UInt64>(failure.Error) << 40U) |
                          (static_cast<UInt64>(failure.Preparation) << 32U) |
                          (static_cast<UInt64>(failure.AddressSpace.Error) << 24U) |
                          (static_cast<UInt64>(failure.AddressSpace.PageTableError) << 16U) |
                          (static_cast<UInt64>(failure.AddressSpace.PinError) << 8U) | static_cast<UInt64>(failure.Pin);
    char digits[19U]{'0', 'x'};
    constexpr char Alphabet[]{"0123456789ABCDEF"};
    for (Size index{0U}; index < 16U; ++index)
    {
        digits[index + 2U] = Alphabet[(detail >> ((15U - index) * 4U)) & 0xFU];
    }
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-PAGER:RESOLVE-REJECTED:");
    Platform::Firmware::WriteDiagnosticAscii(digits);
    Platform::Firmware::WriteDiagnosticAscii("\r\n");
}

bool Amd64PagerRuntime::DispatchWait(const Kernel::System::SystemCallRequest& request,
                                     Kernel::System::SystemCallResponse& response) noexcept
{
    if (!Kernel::System::WaitSetSystemCallIngress::Dispatch(LocalLoop(), m_waitDispatcher, request, m_result,
                                                            m_waitFailure))
    {
        return RejectWait(1U);
    }
    if (m_result.Disposition == Kernel::System::WaitSetSystemCallDisposition::SuspendPrepared)
    {
        // The pager runs again only once ProgressFromIdle woke it.
        if (!LocalLoop().FinishWaitAndDispatch(m_thread, m_context, m_wait, m_waitReceipt, m_waitSwitchFailure))
        {
            return RejectWait(2U);
        }
        if ((m_waitReceipt.ThreadStorage != &m_thread) ||
            (m_waitReceipt.State != Kernel::Scheduler::ProcessorKernelRunLoopState::Active))
        {
            return RejectWait(3U);
        }
        if (!Kernel::System::WaitSetSystemCallIngress::ConsumeCompletion(LocalLoop(), m_waitDispatcher, m_result,
                                                                         m_waitFailure))
        {
            return RejectWait(4U);
        }
    }
    response = m_result.Response;
    return true;
}

bool Amd64PagerRuntime::RejectWait(const Foundation::UInt8 stage) const noexcept
{
    const UInt64 detail = (static_cast<UInt64>(stage) << 40U) | (static_cast<UInt64>(m_waitFailure.Error) << 32U) |
                          (static_cast<UInt64>(m_waitFailure.WaitSet.Error) << 24U) |
                          (static_cast<UInt64>(m_waitFailure.WaitSet.Dispatcher.Error) << 16U) |
                          (static_cast<UInt64>(m_waitSwitchFailure.Error) << 8U) |
                          static_cast<UInt64>(m_wait.GetSnapshot().State);
    char digits[19U]{'0', 'x'};
    constexpr char Alphabet[]{"0123456789ABCDEF"};
    for (Size index{0U}; index < 16U; ++index)
    {
        digits[index + 2U] = Alphabet[(detail >> ((15U - index) * 4U)) & 0xFU];
    }
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:AMD64-PAGER:WAIT-REJECTED:");
    Platform::Firmware::WriteDiagnosticAscii(digits);
    Platform::Firmware::WriteDiagnosticAscii("\r\n");
    return false;
}

Kernel::Memory::FaultDelivery Amd64PagerRuntime::DeliverFault(Kernel::Scheduler::Thread& thread,
                                                              Scheduler::Amd64KernelContext& context,
                                                              const Kernel::Memory::PagerFaultSource& source,
                                                              Scheduler::Amd64ProcessorFaultState& state) noexcept
{
    if (!m_initialized)
    {
        return Kernel::Memory::FaultDelivery::Failed;
    }
    const Kernel::Memory::PagerChannelError prepared = m_channel.PrepareFault(source);
    if (prepared != Kernel::Memory::PagerChannelError::None)
    {
        return (prepared == Kernel::Memory::PagerChannelError::Busy) ? Kernel::Memory::FaultDelivery::Busy
                                                                     : Kernel::Memory::FaultDelivery::Failed;
    }
    return (LocalLoop().FinishFaultAndDispatch(thread, context, m_channel, state.Receipt, state.Failure) &&
            (state.Receipt.ThreadStorage == &thread) &&
            (state.Receipt.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Active))
               ? Kernel::Memory::FaultDelivery::Resolved
               : Kernel::Memory::FaultDelivery::Failed;
}

void Amd64PagerRuntime::OnExit() noexcept
{
    // A signal is coalesced with any pending one; Signal fails only before
    // Initialize, when no thread can exist.
    static_cast<void>(m_readiness.Signal());
}

bool Amd64PagerRuntime::Progress() noexcept
{
    // Foreign faults may retry the CAS channel, while only the pager's
    // CPU-0 scheduling point services its own wait continuation.
    const auto processor = Platform::Synchronization::CurrentProcessor();
    return processor.IsValid() && processor.GetValue() != 0U ? RetryDelivery()
                                                             : ProgressFromIdle() != Amd64PagerIdleProgress::Failed;
}

bool Amd64PagerRuntime::RetryDelivery() noexcept
{
    const auto channel = m_channel.GetSnapshot();
    if ((channel.State == Kernel::Memory::PagerChannelState::FaultPending) &&
        (channel.Ingress.State != Kernel::Memory::PageFaultIngressState::Delivered))
    {
        m_channel.Retry();
    }
    return m_channel.GetSnapshot().State != Kernel::Memory::PagerChannelState::Faulted;
}

bool Amd64PagerRuntime::Preempt() noexcept
{
    return LocalLoop().FinishAndDispatch(m_thread, m_context, Kernel::Scheduler::ThreadRunDisposition::Ready, 0U,
                                         m_preemptReceipt, m_preemptFailure) &&
           (m_preemptReceipt.ThreadStorage == &m_thread) &&
           (m_preemptReceipt.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Active);
}

Amd64PagerIdleProgress Amd64PagerRuntime::ProgressFromIdle() noexcept
{
    if (!m_initialized || !RetryDelivery())
    {
        return Amd64PagerIdleProgress::Failed;
    }
    const auto state = m_wait.GetSnapshot().State;
    if ((state != Kernel::Ipc::WaitSetThreadDispatcherState::Blocked) &&
        (state != Kernel::Ipc::WaitSetThreadDispatcherState::QueuePressure))
    {
        return Amd64PagerIdleProgress::Waiting;
    }
    const auto polled = m_wait.Poll();
    if (!polled.HasValue())
    {
        return Amd64PagerIdleProgress::Failed;
    }
    return (polled.GetValue().State == Kernel::Ipc::WaitSetThreadDispatcherState::Completed)
               ? Amd64PagerIdleProgress::Woken
               : ExpireWait();
}

// Nothing ready: a passed deadline wakes the pager timed out; queue pressure
// leaves the wait for the next scheduling point.
Amd64PagerIdleProgress Amd64PagerRuntime::ExpireWait() noexcept
{
    const auto expired = m_wait.Expire(m_timer.GetTicks());
    if (!expired.HasValue())
    {
        return (m_wait.GetSnapshot().State == Kernel::Ipc::WaitSetThreadDispatcherState::Faulted)
                   ? Amd64PagerIdleProgress::Failed
                   : Amd64PagerIdleProgress::Waiting;
    }
    if (expired.GetValue().State != Kernel::Ipc::WaitSetThreadDispatcherState::Completed)
    {
        return Amd64PagerIdleProgress::Waiting;
    }
    ++m_waitExpiries;
    return Amd64PagerIdleProgress::Woken;
}
} // Namespace Asteria::Platform::Amd64::System
