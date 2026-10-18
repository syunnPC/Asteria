// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/System/Ia64SystemCallEntry.hpp"

#include "Kernel/System/SystemCallIngress.hpp"
#include "Kernel/System/WaitSetSystemCallIngress.hpp"
#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"
#include "Platform/Ia64/Diagnostics/Ia64Stop.hpp"
#include "Platform/Ia64/System/Ia64SystemCallWaitContinuation.hpp"

extern template bool
Asteria::Kernel::System::SystemCallIngress::Dispatch<Asteria::Platform::Ia64::Scheduler::Ia64KernelContext>(
    Asteria::Platform::Ia64::Scheduler::Ia64ProcessorRunLoop&, Asteria::Kernel::Memory::IUserMemoryAccess&,
    Asteria::Kernel::System::SystemCallScratch&, const Asteria::Kernel::System::SystemCallRequest&,
    Asteria::Kernel::System::SystemCallResponse&, Asteria::Kernel::System::SystemCallIngressFailure&,
    Asteria::Kernel::Memory::IPagerResolution*, Asteria::Kernel::System::IObjectManagement*) noexcept;

// The target entry points are in Ia64SystemCallEntry.S.
#if !defined(__ia64__)
extern "C" [[noreturn]] void AsteriaIa64SystemCallEntry() noexcept
{
    __builtin_trap();
}
#endif

namespace Asteria::Platform::Ia64::System
{
namespace
{
[[nodiscard]] constexpr Ia64SystemCallWaitCorrelation
MakeWaitCorrelation(const Kernel::Scheduler::ProcessorActiveExecutionBinding& active) noexcept
{
    return Ia64SystemCallWaitCorrelation{
        .Thread = active.ThreadIdentity,
        .SchedulingContext = active.SchedulingContextIdentity,
        .RunSequence = active.RunSequence,
        .Processor = active.Processor,
    };
}

[[nodiscard]] constexpr bool PreparedWaitMatchesCorrelation(const Kernel::Ipc::WaitSetThreadDispatcherReceipt& prepared,
                                                            const Ia64SystemCallWaitCorrelation& correlation) noexcept
{
    return (prepared.State == Kernel::Ipc::WaitSetThreadDispatcherState::Prepared) &&
           prepared.Thread.IsEqualTo(correlation.Thread) &&
           prepared.SchedulingContext.IsEqualTo(correlation.SchedulingContext) &&
           (prepared.RunSequence == correlation.RunSequence);
}

ASTERIA_NOINLINE_NOIPA [[nodiscard]]
Ia64SystemCallBridgeStatus ReserveWaitEntry(Ia64SystemCallEntryFrame& frame, Scheduler::Ia64ProcessorRunLoop& runLoop,
                                            Ia64SystemCallWaitContinuation& waitContinuation,
                                            Ia64SystemCallWaitCorrelation& correlation) noexcept
{
    const auto active = runLoop.GetActiveExecutionBinding();
    if (!active.HasValue())
    {
        return Ia64SystemCallBridgeStatus::IngressRejected;
    }

    const Ia64SystemCallWaitCorrelation candidate{MakeWaitCorrelation(active.GetValue())};
    const Ia64SystemCallWaitReservationResult reserved = waitContinuation.ReserveEntry(frame, candidate);
    if (!reserved.HasValue())
    {
        return Ia64SystemCallBridgeStatus::WaitContinuationRejected;
    }
    correlation = candidate;
    return Ia64SystemCallBridgeStatus::None;
}

ASTERIA_NOINLINE_NOIPA [[nodiscard]]
Ia64SystemCallBridgeStatus DispatchWaitIngress(Scheduler::Ia64ProcessorRunLoop& runLoop,
                                               Kernel::System::WaitSetSystemCallDispatcher& waitSetDispatcher,
                                               const Kernel::System::SystemCallRequest& request,
                                               Kernel::System::WaitSetSystemCallDispatchResult& result) noexcept
{
    Kernel::System::WaitSetSystemCallIngressFailure failure{};
    if (!Kernel::System::WaitSetSystemCallIngress::Dispatch(runLoop, waitSetDispatcher, request, result, failure))
    {
        return Ia64SystemCallBridgeStatus::IngressRejected;
    }
    return Ia64SystemCallBridgeStatus::None;
}

ASTERIA_NOINLINE_NOIPA [[nodiscard]]
Ia64SystemCallBridgeStatus CommitAndSuspendWait(Ia64SystemCallEntryFrame& frame,
                                                Ia64SystemCallWaitContinuation& waitContinuation,
                                                const Ia64SystemCallWaitCorrelation& correlation,
                                                Kernel::System::SystemCallResponse& response) noexcept
{
    const Ia64SystemCallWaitCommitResult committed = waitContinuation.CommitPrepared(frame, correlation);
    if (!committed.HasValue())
    {
        return Ia64SystemCallBridgeStatus::WaitContinuationRejected;
    }

    const Ia64SystemCallWaitResponseResult resumed = waitContinuation.SuspendToSupervisor(frame);
    if (!resumed.HasValue())
    {
        return Ia64SystemCallBridgeStatus::WaitContinuationRejected;
    }
    response = resumed.GetValue();
    return Ia64SystemCallBridgeStatus::None;
}

ASTERIA_NOINLINE_NOIPA [[nodiscard]]
Ia64SystemCallBridgeStatus DispatchResumableWait(Ia64SystemCallEntryFrame& frame,
                                                 Scheduler::Ia64ProcessorRunLoop& runLoop,
                                                 Ia64SystemCallWaitContinuation& waitContinuation,
                                                 Kernel::System::WaitSetSystemCallDispatcher& waitSetDispatcher,
                                                 const Kernel::System::SystemCallRequest& request,
                                                 Kernel::System::SystemCallResponse& response) noexcept
{
    Ia64SystemCallWaitCorrelation correlation{};
    const Ia64SystemCallBridgeStatus reservationStatus =
        ReserveWaitEntry(frame, runLoop, waitContinuation, correlation);
    if (reservationStatus != Ia64SystemCallBridgeStatus::None)
    {
        return reservationStatus;
    }

    Kernel::System::WaitSetSystemCallDispatchResult result{};
    const Ia64SystemCallBridgeStatus ingressStatus = DispatchWaitIngress(runLoop, waitSetDispatcher, request, result);
    if (ingressStatus != Ia64SystemCallBridgeStatus::None)
    {
        const Ia64SystemCallWaitContinuationError cancelled = waitContinuation.CancelReservedEntry(frame, correlation);
        return cancelled == Ia64SystemCallWaitContinuationError::None
                   ? ingressStatus
                   : Ia64SystemCallBridgeStatus::WaitContinuationRejected;
    }

    if (result.Disposition == Kernel::System::WaitSetSystemCallDisposition::Completed)
    {
        const Ia64SystemCallWaitContinuationError cancelled = waitContinuation.CancelReservedEntry(frame, correlation);
        if (cancelled != Ia64SystemCallWaitContinuationError::None)
        {
            return Ia64SystemCallBridgeStatus::WaitContinuationRejected;
        }
        response = result.Response;
        return Ia64SystemCallBridgeStatus::None;
    }
    if ((result.Disposition != Kernel::System::WaitSetSystemCallDisposition::SuspendPrepared) ||
        !PreparedWaitMatchesCorrelation(result.Wait, correlation))
    {
        return Ia64SystemCallBridgeStatus::WaitContinuationRejected;
    }
    return CommitAndSuspendWait(frame, waitContinuation, correlation, response);
}
} // namespace

Ia64SystemCallBridgeStatus DispatchIa64SystemCallBridge(Ia64SystemCallEntryFrame& frame,
                                                        Scheduler::Ia64ProcessorRunLoop& runLoop,
                                                        Kernel::Memory::IUserMemoryAccess& userMemory,
                                                        Kernel::System::SystemCallScratch& scratch) noexcept
{
    return DispatchIa64SystemCallBridge(frame, runLoop, userMemory, scratch, nullptr, nullptr);
}

Ia64SystemCallBridgeStatus DispatchIa64SystemCallBridge(Ia64SystemCallEntryFrame& frame,
                                                        Scheduler::Ia64ProcessorRunLoop& runLoop,
                                                        Kernel::Memory::IUserMemoryAccess& userMemory,
                                                        Kernel::System::SystemCallScratch& scratch,
                                                        Ia64SystemCallWaitContinuation* const waitContinuation) noexcept
{
    return DispatchIa64SystemCallBridge(frame, runLoop, userMemory, scratch, waitContinuation, nullptr);
}

Ia64SystemCallBridgeStatus
DispatchIa64SystemCallBridge(Ia64SystemCallEntryFrame& frame, Scheduler::Ia64ProcessorRunLoop& runLoop,
                             Kernel::Memory::IUserMemoryAccess& userMemory, Kernel::System::SystemCallScratch& scratch,
                             Ia64SystemCallWaitContinuation* const waitContinuation,
                             Kernel::System::WaitSetSystemCallDispatcher* const waitSetDispatcher) noexcept
{
    return DispatchIa64SystemCallBridge(frame, runLoop, userMemory, scratch, waitContinuation, waitSetDispatcher,
                                        nullptr, nullptr);
}

Ia64SystemCallBridgeStatus
DispatchIa64SystemCallBridge(Ia64SystemCallEntryFrame& frame, Scheduler::Ia64ProcessorRunLoop& runLoop,
                             Kernel::Memory::IUserMemoryAccess& userMemory, Kernel::System::SystemCallScratch& scratch,
                             Ia64SystemCallWaitContinuation* const waitContinuation,
                             Kernel::System::WaitSetSystemCallDispatcher* const waitSetDispatcher,
                             IIa64SystemCallThread* const thread,
                             Kernel::Memory::IPagerResolution* const pagerResolution,
                             Kernel::System::IObjectManagement* const objectManagement) noexcept
{
    if (!IsIa64SystemCallWaitPairValid(waitContinuation, waitSetDispatcher) ||
        ((thread != nullptr) && (waitContinuation != nullptr)))
    {
        return Ia64SystemCallBridgeStatus::InvalidBridgeContext;
    }
    if (frame.State != Ia64SystemCallEntryFrameState::Active)
    {
        return Ia64SystemCallBridgeStatus::InvalidFrameState;
    }
    if (frame.Reserved != 0U)
    {
        return Ia64SystemCallBridgeStatus::InvalidFrameState;
    }
    if (ValidateIa64SystemCallInterruptedState(frame) != Ia64SystemCallInterruptedStateValidationError::None)
    {
        return Ia64SystemCallBridgeStatus::InvalidInterruptedState;
    }

    Ia64SystemCallResumeTarget resumeTarget{};
    if (AdvanceIa64SystemCallResumeTarget(frame.InterruptedInstructionPointer, frame.InterruptedProcessorStatus,
                                          resumeTarget) != Ia64SystemCallResumeAdvanceError::None)
    {
        return Ia64SystemCallBridgeStatus::InvalidResumeTarget;
    }

    Kernel::System::SystemCallRequest request{};
    const Ia64SystemCallRegisterDecodeError decodeError =
        Ia64SystemCallRegisterCodec::DecodeRequest(frame.Registers, request);
    Kernel::System::SystemCallResponse response{};
    if (decodeError == Ia64SystemCallRegisterDecodeError::InvalidNatMetadata)
    {
        return Ia64SystemCallBridgeStatus::InvalidNatMetadata;
    }
    if (decodeError == Ia64SystemCallRegisterDecodeError::InputContainsNat)
    {
        response = Kernel::System::SystemCallResponse{
            .Error = Kernel::System::SystemCallError::InvalidArguments,
            .Value0 = 0U,
            .Value1 = 0U,
        };
    }
    else
    {
        const bool isWait = Kernel::System::WaitSetSystemCallDispatcher::IsWaitSetSystemCall(request.Number);
        const bool isResumableWait = (waitContinuation != nullptr) && isWait;
        const bool isExit =
            request.Number == static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::ThreadExit);
        const bool isManagement =
            (request.Number == static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::ObjectCreate)) ||
            (request.Number == static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::ObjectDestroy)) ||
            (request.Number == static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::MemoryMap)) ||
            (request.Number == static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::MemoryUnmap)) ||
            (request.Number == static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::ThreadTerminate)) ||
            (request.Number == static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::WaitSetBind)) ||
            (request.Number == static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::DeviceControl)) ||
            (request.Number == static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::IommuControl)) ||
            (request.Number == static_cast<Foundation::UInt64>(Kernel::System::SystemCallNumber::InterruptControl));
        if ((thread != nullptr) && isWait)
        {
            if (!thread->DispatchWait(request, response))
            {
                return Ia64SystemCallBridgeStatus::ThreadWaitRejected;
            }
        }
        else if ((thread != nullptr) && isExit)
        {
            if (!thread->DispatchExit(request, response))
            {
                return Ia64SystemCallBridgeStatus::ThreadExitRejected;
            }
        }
        else if (isResumableWait)
        {
            const Ia64SystemCallBridgeStatus waitStatus =
                DispatchResumableWait(frame, runLoop, *waitContinuation, *waitSetDispatcher, request, response);
            if (waitStatus != Ia64SystemCallBridgeStatus::None)
            {
                return waitStatus;
            }
        }
        else
        {
            if ((thread != nullptr) && isManagement && !thread->PrepareManagement())
            {
                return Ia64SystemCallBridgeStatus::ManagementPreparationRejected;
            }
            Kernel::System::SystemCallIngressFailure failure{};
            if (!Kernel::System::SystemCallIngress::Dispatch(runLoop, userMemory, scratch, request, response, failure,
                                                             pagerResolution, objectManagement))
            {
                return Ia64SystemCallBridgeStatus::IngressRejected;
            }
        }
    }

    Platform::Synchronization::ServiceThreadRequests(true);
    Ia64SystemCallRegisterCodec::ApplyResponse(response, frame.Registers);
    frame.InterruptedInstructionPointer = resumeTarget.InstructionPointer;
    frame.InterruptedProcessorStatus = resumeTarget.ProcessorStatus;
    return Ia64SystemCallBridgeStatus::None;
}
} // namespace Asteria::Platform::Ia64::System

extern "C" ASTERIA_NOINLINE_NOIPA Asteria::Foundation::UInt64
AsteriaIa64SystemCallBridge(Asteria::Platform::Ia64::System::Ia64SystemCallEntryFrame* const frame,
                            Asteria::Platform::Ia64::System::Ia64SystemCallBridgeContext* const bridgeContext) noexcept
{
    Asteria::Platform::Ia64::Diagnostics::CheckIa64StopPoint();
    if (frame == nullptr)
    {
        return static_cast<Asteria::Foundation::UInt64>(
            Asteria::Platform::Ia64::System::Ia64SystemCallBridgeStatus::InvalidFrameState);
    }
    const Asteria::Foundation::UInt64 bridgeContextAddress =
        static_cast<Asteria::Foundation::UInt64>(reinterpret_cast<Asteria::Foundation::UIntPointer>(bridgeContext));
    if (!Asteria::Platform::Ia64::System::IsIa64SystemCallBridgeContextAddressValid(bridgeContextAddress))
    {
        return static_cast<Asteria::Foundation::UInt64>(
            Asteria::Platform::Ia64::System::Ia64SystemCallBridgeStatus::InvalidBridgeContext);
    }
    const auto& context = *bridgeContext;
    if ((context.RunLoop == nullptr) || (context.UserMemory == nullptr) || (context.Scratch == nullptr) ||
        !Asteria::Platform::Ia64::System::IsIa64SystemCallWaitPairValid(context.WaitContinuation,
                                                                        context.WaitSetDispatcher) ||
        ((context.Thread != nullptr) && (context.WaitContinuation != nullptr)))
    {
        return static_cast<Asteria::Foundation::UInt64>(
            Asteria::Platform::Ia64::System::Ia64SystemCallBridgeStatus::InvalidBridgeContext);
    }
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    return static_cast<Asteria::Foundation::UInt64>(Asteria::Platform::Ia64::System::DispatchIa64SystemCallBridge(
        *frame, *context.RunLoop, *context.UserMemory, *context.Scratch, context.WaitContinuation,
        context.WaitSetDispatcher, context.Thread, context.PagerResolution, context.ObjectManagement));
#else
    static_cast<void>(frame);
    static_cast<void>(context);
    return static_cast<Asteria::Foundation::UInt64>(
        Asteria::Platform::Ia64::System::Ia64SystemCallBridgeStatus::NativeTransportUnavailable);
#endif
}

extern "C" [[noreturn]] ASTERIA_NOINLINE_NOIPA void
AsteriaIa64FailStopUnknownSystemCallBreak(const Asteria::Foundation::UInt64 immediate,
                                          const Asteria::Foundation::UInt64 instructionPointer) noexcept
{
    Asteria::Platform::Ia64::Diagnostics::StopIa64(Asteria::Kernel::Diagnostics::StopCode::KernelTrap, 0x2C00U,
                                                   instructionPointer, immediate);
}
