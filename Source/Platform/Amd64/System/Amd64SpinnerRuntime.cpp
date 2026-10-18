// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/System/Amd64SpinnerRuntime.hpp"

#include "Platform/Amd64/Diagnostics/Amd64EvidenceRecord.hpp"
#include "Platform/Amd64/System/Amd64UserScheduling.hpp"
#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Amd64/Diagnostics/Amd64Stop.hpp"
#include "Platform/Amd64/Diagnostics/Amd64StopStage.hpp"

namespace Asteria::Platform::Amd64::System
{
using Foundation::UInt64;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;

namespace
{
// The boot runtime's charge owner, which also owns the client's objects.
constexpr Kernel::Object::ResourceDomainId SpinnerCharge{1U};

[[noreturn]] void FailLaunch() noexcept
{
    Diagnostics::StopAmd64(Kernel::Diagnostics::StopCode::BootStage,
                           static_cast<UInt64>(Diagnostics::Amd64BootStage::SpinnerStack));
}

[[nodiscard]] Diagnostics::Amd64UserPreemptionEvidence& Evidence() noexcept
{
    return Diagnostics::GetAmd64EvidenceRecord().UserPreemption;
}
} // Namespace

Amd64SpinnerRuntime::Amd64SpinnerRuntime(Kernel::Object::ObjectRegistry& registry,
                                         Kernel::Object::RevocationRegistry& revocations, Time::Amd64ApicTimer& timer,
                                         Kernel::Scheduler::ProcessorRunController& controller,
                                         Scheduler::Amd64ProcessorRunLoop& loop,
                                         const Kernel::Object::CapabilityValidationKey& key) noexcept
    : m_registry{registry}, m_revocations{revocations}, m_timer{timer}, m_loop{loop}, m_issuer{registry},
      m_space{registry, revocations, timer, key.Word0, key.Word1}, m_waitSet{registry},
      m_wait{registry, controller.GetWakeFacade()}, m_waitDispatcher{registry, m_wait}, m_scheduling{registry}
{
}

bool Amd64SpinnerRuntime::Initialize(const Amd64SpinnerRuntimeInput& input) noexcept
{
    if (m_initialized || (input.Resources == nullptr) || (input.UserMemory == nullptr) || (input.Tables == nullptr) ||
        (input.EntryContext == nullptr) || ((input.UserStackTop & 0xFFFU) != 0U))
    {
        return false;
    }
    m_userMemory = input.UserMemory;
    m_tables = input.Tables;
    m_entryContext = input.EntryContext;
    m_entry = input.Entry;
    m_userStackTop = input.UserStackTop;
    m_spaceId = input.Space;
    if (!CreateObjects(input) || !PrepareThread(input))
    {
        return false;
    }
    m_initialized = true;
    return true;
}

bool Amd64SpinnerRuntime::CreateObjects(const Amd64SpinnerRuntimeInput& input) noexcept
{
    if (m_waitSet.Initialize(SpinnerCharge) != Kernel::Ipc::WaitSetError::None)
    {
        return false;
    }
    const auto waitSet = m_issuer.CreateObject(m_waitSet);
    const auto root = m_revocations.CreateRoot();
    if (!waitSet.HasValue() || !root.HasValue())
    {
        return false;
    }
    constexpr Kernel::Object::MonotonicTime Forever{~UInt64{0U}};
    const auto diagnostic = m_issuer.Mint(m_space, input.Diagnostic,
                                          CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Write),
                                          root.GetValue(), Forever, 0U);
    const auto wait = m_issuer.Mint(m_space, waitSet.GetValue(), CapabilityRights{CapabilityRight::Wait},
                                    root.GetValue(), Forever, 0U);
    if (!diagnostic.HasValue() || !wait.HasValue())
    {
        return false;
    }
    m_diagnostic = diagnostic.GetValue();
    m_waitSetCapability = wait.GetValue();
    return true;
}

bool Amd64SpinnerRuntime::PrepareThread(const Amd64SpinnerRuntimeInput& input) noexcept
{
    if (m_protection.Initialize(m_registry, input.ResourceIdentity, *input.Resources, input.Space, m_space).Error !=
        Kernel::Protection::ProtectionDomainError::None)
    {
        return false;
    }
    m_protection.SetDiagnosticComponentKind(Abi::WatchdogSpinnerComponent);
    const auto protection = m_issuer.CreateObject(m_protection);
    if (!protection.HasValue() ||
        m_protection.BindIdentity(m_registry, {m_registry, protection.GetValue()}).Error !=
            Kernel::Protection::ProtectionDomainError::None ||
        (m_thread.Initialize(SpinnerCharge) != Kernel::Scheduler::ThreadError::None))
    {
        return false;
    }
    const auto thread = m_issuer.CreateObject(m_thread);
    if (!thread.HasValue())
    {
        return false;
    }
    m_threadId = {m_registry, thread.GetValue()};
    if (m_scheduling.Initialize(MakeAmd64UserSchedulingConfiguration(), SpinnerCharge,
                                Kernel::Object::MonotonicTime{m_timer.GetTicks()}) !=
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

bool Amd64SpinnerRuntime::Admit(Kernel::Scheduler::ProcessorKernelRunLoopFailure& failure) noexcept
{
    return m_initialized && (m_thread.MakeReady() == Kernel::Scheduler::ThreadError::None) &&
           (LocalLoop().Bind(m_thread, m_context, m_protection) ==
            Kernel::Scheduler::ProcessorKernelRunLoopError::None) &&
           LocalLoop().EnqueueBoundThread(m_schedulingId, m_scheduling, m_threadId, m_thread, failure);
}

bool Amd64SpinnerRuntime::IsRunning() const noexcept
{
    const auto snapshot = LocalLoop().GetSnapshot();
    return m_initialized && (snapshot.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Active) &&
           (snapshot.ActiveThread == &m_thread);
}

[[noreturn]] void Amd64SpinnerRuntime::Launch(void* const argument) noexcept
{
    auto& runtime = *static_cast<Amd64SpinnerRuntime*>(argument);
    auto& frame = runtime.m_launchFrame;
    frame.Rip = runtime.m_entry;
    frame.Rsp = runtime.m_userStackTop;
    frame.Rdi = runtime.m_diagnostic.GetSelector();
    frame.Rsi = runtime.m_diagnostic.GetAuthenticator();
    frame.Rdx = runtime.m_waitSetCapability.GetSelector();
    frame.Rcx = runtime.m_waitSetCapability.GetAuthenticator();
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

bool Amd64SpinnerRuntime::DispatchSystemCall(const Kernel::System::SystemCallRequest& request,
                                             Kernel::System::SystemCallResponse& response,
                                             Kernel::System::SystemCallScratch& scratch) noexcept
{
    if (request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::WaitSetWait))
    {
        return DispatchWait(request, response);
    }
    if (!Kernel::System::SystemCallIngress::Dispatch(LocalLoop(), *m_userMemory, scratch, request, response,
                                                     m_ingressFailure))
    {
        return false;
    }
    if ((request.Number == static_cast<UInt64>(Kernel::System::SystemCallNumber::DebugWrite)) &&
        (response.Error == Kernel::System::SystemCallError::None))
    {
        ++Evidence().SpinnerLines;
        Diagnostics::PublishAmd64Evidence(Evidence().Published);
    }
    return true;
}

bool Amd64SpinnerRuntime::DispatchWait(const Kernel::System::SystemCallRequest& request,
                                       Kernel::System::SystemCallResponse& response) noexcept
{
    if (!Kernel::System::WaitSetSystemCallIngress::Dispatch(LocalLoop(), m_waitDispatcher, request, m_result,
                                                            m_waitFailure))
    {
        return false;
    }
    if (m_result.Disposition == Kernel::System::WaitSetSystemCallDisposition::SuspendPrepared)
    {
        // Nothing is registered with the spinner's WaitSet and nothing polls
        // its wait: the switch below does not return.
        Evidence().SpinnerWaitSuspended = 1U;
        Diagnostics::PublishAmd64Evidence(Evidence().Published);
        if (!LocalLoop().FinishWaitAndDispatch(m_thread, m_context, m_wait, m_waitReceipt, m_waitSwitchFailure) ||
            (m_waitReceipt.ThreadStorage != &m_thread) ||
            (m_waitReceipt.State != Kernel::Scheduler::ProcessorKernelRunLoopState::Active) ||
            !Kernel::System::WaitSetSystemCallIngress::ConsumeCompletion(LocalLoop(), m_waitDispatcher, m_result,
                                                                         m_waitFailure))
        {
            return false;
        }
    }
    response = m_result.Response;
    return true;
}

Kernel::Memory::FaultDelivery Amd64SpinnerRuntime::DeliverFault(IAmd64FaultSupervisor& supervisor,
                                                                const Kernel::Memory::FaultRecord& fault) noexcept
{
    return m_initialized ? supervisor.DeliverFault(m_thread, m_context,
                                                   {m_threadId, m_spaceId, m_schedulingId, fault, 0U,
                                                    m_protection.GetIdentity().GetValue()},
                                                   m_faultState)
                         : Kernel::Memory::FaultDelivery::Failed;
}

bool Amd64SpinnerRuntime::Preempt() noexcept
{
    return LocalLoop().FinishAndDispatch(m_thread, m_context, Kernel::Scheduler::ThreadRunDisposition::Ready, 0U,
                                         m_preemptReceipt, m_preemptFailure) &&
           (m_preemptReceipt.ThreadStorage == &m_thread) &&
           (m_preemptReceipt.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Active);
}
} // Namespace Asteria::Platform::Amd64::System
