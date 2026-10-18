// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Scheduler/Amd64NativePreemptionProbe.hpp"

#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Platform/Amd64/Diagnostics/Amd64EvidenceRecord.hpp"
#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Amd64/Diagnostics/Amd64Stop.hpp"
#include "Platform/Amd64/Diagnostics/Amd64StopStage.hpp"

#if defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Platform::Amd64::Scheduler
{
namespace
{
using Foundation::UInt64;
using Foundation::UInt8;
using Foundation::Size;
using Kernel::Scheduler::CpuId;
extern "C" void AsteriaAmd64PreemptibleLoop(volatile UInt64*, UInt64*) noexcept;
extern "C" const UInt8 AsteriaAmd64PreemptibleLoopStart[];
extern "C" const UInt8 AsteriaAmd64PreemptibleLoopEnd[];
extern "C" [[noreturn]] void AsteriaAmd64PreemptionCoordinatorEntry(void*) noexcept;
extern "C" [[noreturn]] void AsteriaAmd64RestoreUserFrame(const Interrupt::Amd64TrapFrame*) noexcept;

// The probe's scheduling window, in TSC ticks: the loop runs for its budget,
// and the coordinator waits for the replenishment at the end of the period.
constexpr UInt64 BudgetTicks{250'000'000U};
constexpr UInt64 PeriodTicks{500'000'000U};
// Dispatches of the probe's worker, from idle (Run) or as the coordinator's
// readmission (Coordinate), that a rollback may end before the probe gives up.
constexpr Size DispatchAttempts{8U};
// Polls of the TSC while waiting for a replenishment boundary: a bound only
// for a counter that stops, far above the polls any wait needs.
constexpr Size WaitPolls{20'000'000U};

[[nodiscard]] UInt64 Address(const void* pointer) noexcept
{
    return reinterpret_cast<Foundation::UIntPointer>(pointer);
}
[[nodiscard]] bool Disjoint(const UInt64 first, const UInt64 firstSize, const UInt64 second,
                            const UInt64 secondSize) noexcept
{
    return first <= second ? firstSize <= second - first : secondSize <= first - second;
}
[[nodiscard]] bool DisjointStacks(const WritableAmd64KernelStackView& first,
                                  const WritableAmd64KernelStackView& second) noexcept
{
    return Disjoint(first.VirtualStart, first.SizeBytes, second.VirtualStart, second.SizeBytes) &&
           Disjoint(Address(first.Data), first.SizeBytes, Address(second.Data), second.SizeBytes);
}
/// Waits with interrupts disabled until the TSC reaches `ticks`; false only if
/// it never does within WaitPolls polls.
[[nodiscard]] bool WaitUntil(const Time::Amd64ApicTimer& timer, const UInt64 ticks) noexcept
{
    for (Size poll{0U}; poll < WaitPolls; ++poll)
    {
        if (timer.GetTicks() >= ticks)
        {
            return true;
        }
#if defined(ASTERIA_TARGET_AMD64)
        __asm__ __volatile__("pause" : : : "memory");
#endif
    }
    return false;
}
[[noreturn]] void Fail(const Diagnostics::Amd64BootStage stage) noexcept
{
    Diagnostics::StopAmd64(Kernel::Diagnostics::StopCode::BootStage, static_cast<UInt64>(stage));
}

class Probe final
{
public:
    explicit Probe(const Amd64NativePreemptionInput& input) noexcept
        : m_input{input}, m_issuer{*input.Registry}, m_scheduling{*input.Registry}
    {
    }
    [[nodiscard, gnu::noinline]] bool Prepare() noexcept;
    [[noreturn]] void Run() noexcept;
    [[nodiscard]] bool Capture(Interrupt::Amd64TrapFrame& frame) noexcept;
    [[noreturn]] void Coordinate() noexcept;

private:
    [[nodiscard, gnu::noinline]] bool Admit() noexcept;
    [[nodiscard, gnu::noinline]] bool Arm() noexcept;
    [[nodiscard, gnu::noinline]] bool StopRun() noexcept;
    [[nodiscard, gnu::noinline]] bool Readmit() noexcept;
    [[nodiscard]] bool IsRolledBack() const noexcept;
    void RecordRegisters() noexcept;
    [[noreturn]] static void Worker(void*) noexcept;
    [[noreturn]] static void FreshIdle(void*) noexcept;

    Amd64NativePreemptionInput m_input;
    Kernel::Object::RootCapabilityIssuer m_issuer;
    Kernel::Protection::ProtectionDomain m_protection;
    Kernel::Scheduler::Thread m_thread;
    Kernel::Scheduler::SchedulingContext m_scheduling;
    Kernel::Scheduler::ThreadId m_threadId{};
    Kernel::Scheduler::SchedulingContextId m_schedulingId{};
    Amd64KernelContext m_worker;
    Amd64KernelContext m_coordinatorValidation;
    Kernel::Scheduler::ProcessorRunExternalStopObservation m_observation{};
    Kernel::Scheduler::ProcessorKernelRunLoopReceipt m_receipt{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure m_failure{};
    Interrupt::Amd64TrapFrame m_frame{};
    UInt64 m_after[Diagnostics::Amd64EvidencePreemptionRegisters]{};
    alignas(8U) volatile UInt64 m_resumed{0U};
    Synchronization::AtomicUInt64 m_state; // 0 fresh, 1 armed, 2 captured, 3 resumed, 4 exited.
};

alignas(Probe) UInt8 g_storage[sizeof(Probe)]{};
Probe* g_probe{nullptr};

bool Probe::Prepare() noexcept
{
    if (!DisjointStacks(m_input.WorkerStack, m_input.CoordinatorStack) ||
        !DisjointStacks(m_input.WorkerStack, m_input.FreshIdleStack) ||
        !DisjointStacks(m_input.CoordinatorStack, m_input.FreshIdleStack))
    {
        return false;
    }
    auto& registry = *m_input.Registry;
    auto& resources = *m_input.Resources;
    if (m_protection
            .InitializeKernelOnly(registry, m_input.ResourceIdentity, resources,
                                  m_input.SharedSpace->GetSnapshot().Identity)
            .Error != Kernel::Protection::ProtectionDomainError::None)
    {
        return false;
    }
    const auto protection = m_issuer.CreateObject(m_protection);
    if (!protection.HasValue() ||
        m_protection.BindIdentity(registry, {registry, protection.GetValue()}).Error !=
            Kernel::Protection::ProtectionDomainError::None ||
        m_thread.Initialize(resources.GetIdentifier()) != Kernel::Scheduler::ThreadError::None)
    {
        return false;
    }
    const auto thread = m_issuer.CreateObject(m_thread);
    if (!thread.HasValue())
    {
        return false;
    }
    m_threadId = {registry, thread.GetValue()};
    const Kernel::Scheduler::SchedulingContextConfiguration configuration{
        BudgetTicks, PeriodTicks,
        PeriodTicks, 1U,
        0xFFU,       Kernel::Scheduler::SchedulingCriticality::Emergency,
        1U,          Kernel::Scheduler::SchedulingClass::EmergencyRecovery,
        0U};
    if (m_scheduling.Initialize(configuration, resources.GetIdentifier(),
                                Kernel::Object::MonotonicTime{m_input.Timer->GetTicks()}) !=
        Kernel::Scheduler::SchedulingContextError::None)
    {
        return false;
    }
    const auto scheduling = m_issuer.CreateObject(m_scheduling);
    if (!scheduling.HasValue())
    {
        return false;
    }
    m_schedulingId = {registry, scheduling.GetValue()};
    return m_scheduling.Bind(m_threadId) == Kernel::Scheduler::SchedulingContextError::None &&
           m_worker.Prepare(m_input.WorkerStack, Worker, this) == Amd64KernelContextError::None &&
           m_coordinatorValidation.Prepare(m_input.CoordinatorStack, AsteriaAmd64PreemptionCoordinatorEntry, this) ==
               Amd64KernelContextError::None &&
           m_input.Idle->ConfigureExternalFrameIdleContinuation(m_input.FreshIdleStack, FreshIdle, this) ==
               Amd64KernelContextError::None;
}

bool Probe::Admit() noexcept
{
    return m_thread.MakeReady() == Kernel::Scheduler::ThreadError::None &&
           m_input.Loop->Bind(m_thread, m_worker, m_protection) ==
               Kernel::Scheduler::ProcessorKernelRunLoopError::None &&
           m_input.Loop->EnqueueBoundThread(m_schedulingId, m_scheduling, m_threadId, m_thread, m_failure);
}

// A dispatch returns here only if it did not switch to the worker. When the
// processor's clock passes the admitted stop between the admission sample and
// the timer program or the final entry check (the vCPU was not scheduled for
// longer than the admitted run, which host load decides), the kernel rolls the
// admission back as Ready, and a later dispatch finds the budget spent until
// the next replenishment. Both leave everything as it was before the dispatch,
// so the probe waits for the budget and dispatches again: its verdict does not
// depend on how fast the host runs the vCPU.
[[noreturn]] void Probe::Run() noexcept
{
    if (!Prepare() || !Admit())
    {
        Fail(Diagnostics::Amd64BootStage::NativePreemptionPrepareAdmission);
    }
    for (Size attempt{0U}; attempt < DispatchAttempts; ++attempt)
    {
        static_cast<void>(Amd64DispatchFromIdle(*m_input.Loop, m_receipt, m_failure));
        if (!IsRolledBack() || !WaitUntil(*m_input.Timer, m_scheduling.GetSnapshot().NextReplenishmentTicks))
        {
            break;
        }
    }
    Fail(Diagnostics::Amd64BootStage::NativePreemptionDispatch);
}

bool Probe::IsRolledBack() const noexcept
{
    return (m_input.Loop->GetSnapshot().State == Kernel::Scheduler::ProcessorKernelRunLoopState::Idle) &&
           (m_input.Controller->GetSnapshot().State == Kernel::Scheduler::ProcessorRunState::Idle) &&
           (m_thread.GetSnapshot().State == Kernel::Scheduler::ThreadState::Ready);
}

bool Probe::Arm() noexcept
{
    const auto active = m_input.Loop->GetActiveExecutionBinding();
    const auto controller = m_input.Controller->GetSnapshot();
    if (!active.HasValue() || active.GetValue().ThreadStorage != &m_thread ||
        active.GetValue().SchedulingContextStorage != &m_scheduling ||
        controller.State != Kernel::Scheduler::ProcessorRunState::Active ||
        controller.Deadline.SelectedSources != static_cast<UInt8>(Kernel::Time::ProcessorDeadlineSource::RunStop) ||
        controller.Deadline.SelectedDeadlineTicks != m_input.Timer->GetDeadline())
    {
        return false;
    }
    // The deadline may already have passed (the vCPU was not scheduled): its
    // interrupt then stays pending until the loop enables interrupts and
    // preempts the loop's first instruction, which is still a preemption.
    m_observation = {m_threadId,
                     m_schedulingId,
                     active.GetValue().RunSequence,
                     m_input.Timer->GetDeadline(),
                     0U,
                     1U,
                     CpuId{0U},
                     Kernel::Scheduler::ProcessorRunExternalStopHardwareState::Unverified,
                     {}};
    m_state.StoreRelease(1U);
    return true;
}

[[noreturn]] void Probe::Worker(void* const argument) noexcept
{
    auto& probe = *static_cast<Probe*>(argument);
    if (!probe.Arm())
    {
        Fail(Diagnostics::Amd64BootStage::NativePreemptionArm);
    }
    AsteriaAmd64PreemptibleLoop(&probe.m_resumed, probe.m_after);
    if (probe.m_state.LoadAcquire() != 3U)
    {
        Fail(Diagnostics::Amd64BootStage::NativePreemptionResumeState);
    }
    probe.RecordRegisters();
    probe.m_state.StoreRelease(4U);
    static_cast<void>(probe.m_input.Loop->FinishAndDispatch(probe.m_thread, probe.m_worker,
                                                            Kernel::Scheduler::ThreadRunDisposition::Exited, 0U,
                                                            probe.m_receipt, probe.m_failure));
    Fail(Diagnostics::Amd64BootStage::NativePreemptionWorkerExit);
}

// The frame must be the worker's, interrupted inside the loop by the masked
// and acknowledged timer at its programmed deadline: only such a frame may be
// restored later, and the scheduler is told so (HardwareState).
bool Probe::Capture(Interrupt::Amd64TrapFrame& frame) noexcept
{
    if (m_state.LoadAcquire() != 1U)
    {
        return false;
    }
    if (frame.Vector != Time::ApicTimerVector || frame.Error != 0U || frame.Cs != Interrupt::KernelCodeSelector ||
        frame.Ss != Interrupt::KernelDataSelector || (frame.Rflags & 0x200U) == 0U ||
        frame.Rip < Address(AsteriaAmd64PreemptibleLoopStart) || frame.Rip >= Address(AsteriaAmd64PreemptibleLoopEnd) ||
        frame.Rsp < m_input.WorkerStack.VirtualStart + 4096U ||
        frame.Rsp >= m_input.WorkerStack.VirtualStart + m_input.WorkerStack.SizeBytes ||
        m_input.Timer->GetDeadline() != m_observation.ProgrammedDeadlineTicks ||
        m_input.Timer->Read(Time::ApicLvtTimerRegister) != Time::ApicTimerMaskedLvt ||
        m_input.Timer->Read(Time::ApicInitialCountRegister) != 0U ||
        (m_input.Timer->Read(Time::ApicTimerInServiceRegister) & Time::ApicTimerInServiceBit) != 0U)
    {
        Fail(Diagnostics::Amd64BootStage::NativePreemptionCaptureState);
    }
    m_frame = frame;
    m_observation.ObservedTicks = m_input.Timer->GetTicks();
    m_observation.HardwareState =
        Kernel::Scheduler::ProcessorRunExternalStopHardwareState::TimerMaskedReadBackAndInterruptAcknowledged;
    if (m_observation.ObservedTicks < m_observation.ProgrammedDeadlineTicks)
    {
        Fail(Diagnostics::Amd64BootStage::NativePreemptionCaptureDeadline);
    }
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().NativePreemption;
    evidence.Frame = frame;
    evidence.ProgrammedDeadlineTicks = m_observation.ProgrammedDeadlineTicks;
    evidence.ObservedTicks = m_observation.ObservedTicks;
    evidence.StoppedRunSequence = m_observation.RunSequence;
    ++evidence.CaptureCount;
    m_state.StoreRelease(2U);
    // IRET leaves the hardware interrupt and its worker-stack frame before
    // any registry/scheduler operation. The SysV landing has no return edge.
    frame.Rip = reinterpret_cast<Foundation::UIntPointer>(&AsteriaAmd64PreemptionCoordinatorEntry);
    frame.Rsp = m_input.CoordinatorStack.VirtualStart + m_input.CoordinatorStack.SizeBytes - 8U;
    frame.Rdi = Address(this);
    frame.Rbp = 0U;
    frame.Rflags = 2U;
    return true;
}

bool Probe::StopRun() noexcept
{
    const auto stopped = m_input.Loop->CompleteExternallyStoppedRunAsReady(m_thread, m_worker, m_observation);
    if (!stopped.HasValue() || m_thread.GetSnapshot().State != Kernel::Scheduler::ThreadState::Ready ||
        m_input.Loop->GetSnapshot().State != Kernel::Scheduler::ProcessorKernelRunLoopState::Idle)
    {
        return false;
    }
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().NativePreemption;
    evidence.RemainingBudgetTicks = m_scheduling.GetSnapshot().RemainingBudgetTicks;
    evidence.ReplenishmentTicks = m_scheduling.GetSnapshot().NextReplenishmentTicks;
    return true;
}

// Resumes exactly the captured run (the retained external frame of the run
// that was stopped) at the programmed admission deadline; the token, the
// resume generation and the admission run are recorded.
bool Probe::Readmit() noexcept
{
    if (!m_input.Loop->DispatchRetainedExternalFrameFromCoordinator(m_thread, m_worker, m_receipt, m_failure) ||
        m_receipt.ResumeKind != Kernel::Scheduler::ProcessorKernelRunLoopResumeKind::ExternalFrame ||
        m_receipt.StoppedRunSequence != m_observation.RunSequence ||
        m_receipt.AdmissionDeadlineTicks != m_input.Timer->GetDeadline() ||
        !m_input.Loop->RefreshExternalFrameIdleContinuation(m_receipt, m_failure) ||
        !m_input.Loop->CommitExternalFrameResume(m_thread, m_worker, m_receipt, m_failure) ||
        m_input.Loop->GetSnapshot().ExternalFrameRetained != 0U)
    {
        return false;
    }
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().NativePreemption;
    evidence.ExternalFrameToken = m_receipt.ExternalFrameToken;
    evidence.ExternalFrameResumeGeneration = m_receipt.ExternalFrameResumeGeneration;
    evidence.AdmissionRunSequence = m_receipt.AdmissionRunSequence;
    evidence.IdleContinuationGeneration = m_input.Idle->GetExternalFrameIdleContinuationGeneration();
    return true;
}

// A readmission stalled past its admitted stop before the timer program or
// the final entry check is rolled back like a dispatch from idle: the frame
// stays retained and the run Ready, so the coordinator waits for the budget
// and readmits again (as a later run).
[[noreturn]] void Probe::Coordinate() noexcept
{
    if (m_state.LoadAcquire() != 2U || !StopRun())
    {
        Fail(Diagnostics::Amd64BootStage::NativePreemptionStopRun);
    }
    for (Size attempt{0U}; attempt < DispatchAttempts; ++attempt)
    {
        const UInt64 replenishment = m_scheduling.GetSnapshot().NextReplenishmentTicks;
        if (replenishment == 0U || !WaitUntil(*m_input.Timer, replenishment))
        {
            break;
        }
        if (Readmit())
        {
            ++Diagnostics::GetAmd64EvidenceRecord().NativePreemption.ResumeCount;
            m_state.StoreRelease(3U);
            Synchronization::AtomicUInt64Reference{m_resumed}.StoreRelease(1U);
            AsteriaAmd64RestoreUserFrame(&m_frame);
        }
        if (!IsRolledBack())
        {
            break;
        }
    }
    Fail(Diagnostics::Amd64BootStage::NativePreemptionReadmission);
}

// The registers the loop stored after it resumed (rax, rbx, rcx, rdx, rbp,
// r8-r15, rdi, rsi, rsp), with the addresses it received and the loop and
// stack the capture accepted; the host compares them with the captured frame
// and the loop's sentinels.
void Probe::RecordRegisters() noexcept
{
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().NativePreemption;
    for (Size index{0U}; index < Diagnostics::Amd64EvidencePreemptionRegisters; ++index)
    {
        evidence.Registers[index] = m_after[index];
    }
    evidence.ResumeFlagAddress = reinterpret_cast<Foundation::UIntPointer>(&m_resumed);
    evidence.RegisterBufferAddress = Address(m_after);
    evidence.LoopStart = Address(AsteriaAmd64PreemptibleLoopStart);
    evidence.LoopEnd = Address(AsteriaAmd64PreemptibleLoopEnd);
    evidence.WorkerStackStart = m_input.WorkerStack.VirtualStart;
    evidence.WorkerStackEnd = m_input.WorkerStack.VirtualStart + m_input.WorkerStack.SizeBytes;
}

[[noreturn]] void Probe::FreshIdle(void* const argument) noexcept
{
    auto& probe = *static_cast<Probe*>(argument);
    if (probe.m_state.LoadAcquire() != 4U ||
        probe.m_input.Loop->FinishSelectedFreshIdleLanding(probe.m_receipt, probe.m_failure) ||
        probe.m_failure.Error != Kernel::Scheduler::ProcessorKernelRunLoopError::ControllerRejected ||
        probe.m_failure.ControllerError != Kernel::Scheduler::ProcessorRunError::NoRunnableThread ||
        probe.m_thread.GetSnapshot().State != Kernel::Scheduler::ThreadState::Exited ||
        probe.m_input.Loop->Unbind(probe.m_thread) != Kernel::Scheduler::ProcessorKernelRunLoopError::None)
    {
        Fail(Diagnostics::Amd64BootStage::NativePreemptionIdleLanding);
    }
    auto& evidence = Diagnostics::GetAmd64EvidenceRecord().NativePreemption;
    ++evidence.ExitCount;
    Diagnostics::PublishAmd64Evidence(evidence.Published);
    Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:AMD64-NATIVE-PREEMPTION\r\n");
    probe.m_input.Complete(probe.m_input.Context);
    Fail(Diagnostics::Amd64BootStage::NativePreemptionCompletionReturn);
}

extern "C" [[noreturn]] void AsteriaAmd64PreemptionCoordinatorEntry(void* const argument) noexcept
{
    if (argument != g_probe || g_probe == nullptr)
    {
        Fail(Diagnostics::Amd64BootStage::NativePreemptionCoordinatorOwner);
    }
    g_probe->Coordinate();
}
} // namespace

bool CaptureAmd64NativePreemption(Interrupt::Amd64TrapFrame& frame) noexcept
{
    return g_probe != nullptr && g_probe->Capture(frame);
}

[[noreturn]] void RunAmd64NativePreemption(const Amd64NativePreemptionInput& input) noexcept
{
    if (g_probe != nullptr || input.Registry == nullptr || input.Resources == nullptr || input.SharedSpace == nullptr ||
        input.Timer == nullptr || input.Controller == nullptr || input.Loop == nullptr || input.Idle == nullptr ||
        input.Complete == nullptr)
    {
        Fail(Diagnostics::Amd64BootStage::NativePreemptionInput);
    }
    g_probe = new (g_storage) Probe{input};
    g_probe->Run();
}
} // namespace Asteria::Platform::Amd64::Scheduler
