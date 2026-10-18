// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <initializer_list>

#include "Foundation/Types.hpp"
#include "Kernel/Memory/FaultToken.hpp"
#include "Platform/Ia64/Interrupt/Ia64PageFaultEntryContract.hpp"
#include "Platform/Ia64/Interrupt/Ia64UserFaultEntry.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Memory::FaultAccess;
using Kernel::Memory::FaultCause;
using Kernel::Memory::FaultPrivilege;
using Kernel::Memory::FaultRecord;
using Platform::Ia64::Interrupt::DecodeIa64UserFault;
using Platform::Ia64::Interrupt::IIa64UserFaultHandler;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryContractError;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryOutput;
using Platform::Ia64::Interrupt::Ia64PageFaultFrame;
using Platform::Ia64::Interrupt::Ia64PageFaultFrameState;
using Platform::Ia64::Interrupt::Ia64UserFaultBridgeStatus;
using Platform::Ia64::Interrupt::Ia64UserFaultDisposition;
using Platform::Ia64::Interrupt::Ia64UserFaultInterruptedProcessorStatusValue;
using Platform::Ia64::Interrupt::RunIa64UserFaultBridge;

constexpr Foundation::UInt64 ReadBit{Foundation::UInt64{1U} << 34U};
constexpr Foundation::UInt64 WriteBit{Foundation::UInt64{1U} << 33U};
constexpr Foundation::UInt64 ExecuteBit{Foundation::UInt64{1U} << 32U};
constexpr Foundation::UInt64 SlotShift{41U};
constexpr Foundation::UInt64 FaultSlot{1U};
constexpr Foundation::UInt64 FaultAddress{0x0000'0000'4000'1238ULL};
constexpr Foundation::UInt64 InstructionPointer{0x0000'0000'0010'2340ULL};
constexpr Foundation::UInt64 ProcessorStatus{Ia64UserFaultInterruptedProcessorStatusValue | (FaultSlot << SlotShift)};
constexpr Foundation::UInt64 InterruptionStatus{ReadBit | (FaultSlot << SlotShift)};
// A covered frame of ten registers, three of them locals, with eight
// rotating registers renamed by two and predicates renamed by seven: a CFM
// the system-call entry rejects and a fault may interrupt.
constexpr Foundation::UInt64 CoveredFunctionState{(Foundation::UInt64{1U} << 63U) | 10U | (3U << 7U) | (1U << 14U) |
                                                  (2U << 18U) | (Foundation::UInt64{7U} << 32U)};

/// The frame AsteriaIa64UserFaultEntry leaves before its bridge call.
void Capture(Ia64PageFaultFrame& frame) noexcept
{
    frame = Ia64PageFaultFrame{};
    frame.State = Ia64PageFaultFrameState::Capturing;
    frame.RseConfiguration = Platform::Ia64::System::Ia64SystemCallUserRseConfiguration;
    frame.VectorOffset = 0x1000U;
    frame.PreCoverInterruptionStatus = InterruptionStatus;
    frame.InterruptionStatus = InterruptionStatus;
    frame.InterruptedInstructionPointer = InstructionPointer;
    frame.InterruptedProcessorStatus = ProcessorStatus;
    frame.InterruptionFaultingAddress = FaultAddress;
    frame.CoveredInterruptionFunctionState = CoveredFunctionState;
}

enum class HandlerAction : Foundation::UInt8
{
    None,
    ClearState,
    MoveInstructionPointer,
};

class RecordingHandler final : public IIa64UserFaultHandler
{
public:
    explicit constexpr RecordingHandler(const Ia64UserFaultDisposition disposition,
                                        const HandlerAction action = HandlerAction::None) noexcept
        : m_disposition{disposition}, m_action{action}
    {
    }

    Ia64UserFaultDisposition HandleUserFault(Ia64PageFaultFrame& frame, const FaultRecord& fault) noexcept override
    {
        ++Calls;
        StateAtCall = frame.State;
        Fault = fault;
        switch (m_action)
        {
        case HandlerAction::None:
            break;
        case HandlerAction::ClearState:
            frame.State = Ia64PageFaultFrameState::Inactive;
            break;
        case HandlerAction::MoveInstructionPointer:
            frame.InterruptedInstructionPointer += 16U;
            break;
        }
        return m_disposition;
    }

    Ia64UserFaultDisposition HandleUserInterrupt(Ia64PageFaultFrame& frame) noexcept override
    {
        ++Interrupts;
        StateAtCall = frame.State;
        if (m_action == HandlerAction::MoveInstructionPointer)
        {
            frame.InterruptedInstructionPointer += 16U;
        }
        return m_disposition;
    }

    Foundation::Size Calls{0U};
    Foundation::Size Interrupts{0U};
    Ia64PageFaultFrameState StateAtCall{Ia64PageFaultFrameState::Inactive};
    FaultRecord Fault{};

private:
    Ia64UserFaultDisposition m_disposition;
    HandlerAction m_action;
};

[[nodiscard]] constexpr bool IsExpectedFault(const FaultRecord& fault, const FaultAccess access,
                                             const Foundation::UInt64 interruptionStatus) noexcept
{
    return (fault.VirtualAddress == FaultAddress) && (fault.InstructionPointer == InstructionPointer) &&
           (fault.ArchitectureSyndrome == (interruptionStatus | (Foundation::UInt64{0x1000U} << 48U))) &&
           (fault.Access == access) && (fault.Cause == FaultCause::MissingMapping) &&
           (fault.Privilege == FaultPrivilege::User);
}

alignas(64U) Ia64PageFaultFrame g_frame{};

class DeferredEntryFaultHandler final : public IIa64UserFaultHandler
{
public:
    [[nodiscard]] Ia64UserFaultDisposition HandleUserFault(Ia64PageFaultFrame& frame,
                                                           const FaultRecord& fault) noexcept override
    {
        ++Calls;
        Valid = Valid && frame.State == Ia64PageFaultFrameState::Retained && fault.Cause == FaultCause::Exception;
        return Calls == 1U ? Ia64UserFaultDisposition::Resume : Ia64UserFaultDisposition::FailStop;
    }
    [[nodiscard]] Ia64UserFaultDisposition HandleUserInterrupt(Ia64PageFaultFrame& frame) noexcept override
    {
        ++Interrupts;
        // The interrupt is serviced first, before any fault delivery.
        Valid = Valid && Calls == 0U && frame.State == Ia64PageFaultFrameState::Retained;
        return InterruptDisposition;
    }
    Foundation::Size Calls{0U};
    Foundation::Size Interrupts{0U};
    Ia64UserFaultDisposition InterruptDisposition{Ia64UserFaultDisposition::Resume};
    bool Valid{true};
};

void TestRejectedUserEntryState(TestContext& runner) noexcept
{
    // PL3-writable RSC and CFM violations are thread Exceptions, including an interrupted RSE
    // access.
    for (const Foundation::UInt64 rsc : {0xFU, 0x1CU, 0x1FU})
    {
        Capture(g_frame);
        g_frame.RseConfiguration = rsc;
        Ia64PageFaultEntryOutput decoded{};
        ASTERIA_EXPECT(runner, "rejected user RSC overrides a mapping fault with a thread exception",
                       DecodeIa64UserFault(g_frame, decoded) == Ia64PageFaultEntryContractError::None,
                       decoded.Fault.Cause == FaultCause::Exception, decoded.Fault.VirtualAddress == InstructionPointer,
                       decoded.Fault.Privilege == FaultPrivilege::User);
        g_frame.VectorOffset = 0x2C00U;
        g_frame.InterruptionImmediate = Platform::Ia64::System::Ia64SystemCallBreakImmediate;
        g_frame.InterruptionStatus = FaultSlot << SlotShift;
        g_frame.PreCoverInterruptionStatus = g_frame.InterruptionStatus;
        ASTERIA_EXPECT(runner, "a rejected syscall RSC reaches the existing complete-state fault decoder",
                       DecodeIa64UserFault(g_frame, decoded) == Ia64PageFaultEntryContractError::None,
                       decoded.Fault.Cause == FaultCause::Exception);
    }
    constexpr Foundation::UInt64 badFrames[]{0U, 5U, 7U | (1U << 7U), 8U | (1U << 14U)};
    for (const auto cfm : badFrames)
    {
        Capture(g_frame);
        g_frame.VectorOffset = 0x2C00U;
        g_frame.InterruptionImmediate = Platform::Ia64::System::Ia64SystemCallBreakImmediate;
        g_frame.InterruptionStatus = FaultSlot << SlotShift;
        g_frame.PreCoverInterruptionStatus = g_frame.InterruptionStatus;
        g_frame.CoveredInterruptionFunctionState = (Foundation::UInt64{1U} << 63U) | cfm;
        Ia64PageFaultEntryOutput decoded{};
        ASTERIA_EXPECT(runner, "a caller-controlled syscall frame is an exception before reading its arguments",
                       DecodeIa64UserFault(g_frame, decoded) == Ia64PageFaultEntryContractError::None,
                       decoded.Fault.Cause == FaultCause::Exception);
    }
    Capture(g_frame);
    g_frame.RseConfiguration = 0xFU;
    g_frame.VectorOffset = Platform::Ia64::Interrupt::Ia64UserInterruptVectorOffset;
    g_frame.InterruptionStatus = (FaultSlot << SlotShift) | (Foundation::UInt64{1U} << 38U);
    g_frame.PreCoverInterruptionStatus = g_frame.InterruptionStatus;
    DeferredEntryFaultHandler handler{};
    ASTERIA_EXPECT(runner,
                   "a rejected interrupted RSC services the tick, then faults the thread and retries busy "
                   "delivery without restoring it",
                   RunIa64UserFaultBridge(&g_frame, &handler) == Ia64UserFaultBridgeStatus::HandlerFailStop,
                   handler.Valid, handler.Calls == 2U, handler.Interrupts == 1U,
                   g_frame.State == Ia64PageFaultFrameState::Retained);
    Capture(g_frame);
    g_frame.RseConfiguration = 0xFU;
    g_frame.VectorOffset = Platform::Ia64::Interrupt::Ia64UserInterruptVectorOffset;
    g_frame.InterruptionStatus = FaultSlot << SlotShift;
    g_frame.PreCoverInterruptionStatus = g_frame.InterruptionStatus;
    DeferredEntryFaultHandler stopped{};
    stopped.InterruptDisposition = Ia64UserFaultDisposition::FailStop;
    ASTERIA_EXPECT(runner, "a tick whose own servicing fails still stops before any fault delivery",
                   RunIa64UserFaultBridge(&g_frame, &stopped) == Ia64UserFaultBridgeStatus::HandlerFailStop,
                   stopped.Valid, stopped.Interrupts == 1U, stopped.Calls == 0U);
    Capture(g_frame);
    g_frame.RseConfiguration = 0xFU;
    RecordingHandler changed{Ia64UserFaultDisposition::Resume, HandlerAction::ClearState};
    ASTERIA_EXPECT(runner, "a busy fault handler cannot discard its retained kernel frame",
                   RunIa64UserFaultBridge(&g_frame, &changed) == Ia64UserFaultBridgeStatus::FrameStateChanged,
                   changed.Calls == 1U);
    Capture(g_frame);
    g_frame.RseConfiguration = 0xFU;
    g_frame.InterruptionStatus = Foundation::UInt64{1U} << 39U;
    ASTERIA_EXPECT(runner, "nested interruptions remain kernel failures",
                   !Platform::Ia64::Interrupt::IsIa64UserEntryStateFault(g_frame));
    g_frame.InterruptionStatus = 0U;
    g_frame.InterruptedProcessorStatus &= ~(Foundation::UInt64{3U} << 32U);
    ASTERIA_EXPECT(runner, "PL0 RSC is never classified as a user-state exception",
                   !Platform::Ia64::Interrupt::IsIa64UserEntryStateFault(g_frame));
    Capture(g_frame);
    g_frame.RseConfiguration = 0xFU;
    g_frame.VectorOffset = 0x8000U;
    Ia64PageFaultEntryOutput decoded{};
    ASTERIA_EXPECT(runner, "user entry-state rejection cannot hide a corrupted kernel vector",
                   DecodeIa64UserFault(g_frame, decoded) ==
                       Ia64PageFaultEntryContractError::UnsupportedInterruptionVector);
    g_frame.VectorOffset = 0x1000U;
    g_frame.InterruptionStatus = (FaultSlot + 1U) << SlotShift;
    g_frame.PreCoverInterruptionStatus = g_frame.InterruptionStatus;
    ASTERIA_EXPECT(runner, "user RSC rejection cannot hide a corrupted interruption slot",
                   DecodeIa64UserFault(g_frame, decoded) == Ia64PageFaultEntryContractError::InterruptionSlotMismatch);
    Capture(g_frame);
    g_frame.RseConfiguration = 0xFU;
    g_frame.VectorOffset = 0x5E00U;
    g_frame.InterruptionStatus = ((FaultSlot + 1U) << SlotShift) | (Foundation::UInt64{1U} << 38U);
    g_frame.PreCoverInterruptionStatus = g_frame.InterruptionStatus;
    ASTERIA_EXPECT(runner, "rejected RSC preserves a trap's distinct completed slot and incomplete-frame status",
                   DecodeIa64UserFault(g_frame, decoded) == Ia64PageFaultEntryContractError::None,
                   decoded.Fault.Cause == FaultCause::Exception);
    // PFS.pfm is writable and br.ret copies it to CFM (SDM rev. 1.1
    // Vol. 1 §3.1.8.10; Vol. 3 "mov ar" and "br", Operation).
    constexpr Foundation::UInt64 malformed[]{97U,
                                             6U | (7U << 7U),
                                             8U | (2U << 14U),
                                             Foundation::UInt64{1U} << 18U,
                                             Foundation::UInt64{96U} << 25U,
                                             Foundation::UInt64{48U} << 32U};
    for (const auto cfm : malformed)
    {
        Capture(g_frame);
        g_frame.VectorOffset = 0x2C00U;
        g_frame.InterruptionImmediate = 0U;
        g_frame.CoveredInterruptionFunctionState = (Foundation::UInt64{1U} << 63U) | cfm;
        ASTERIA_EXPECT(runner, "a PL3-writable PFS frame marker faults only its thread",
                       DecodeIa64UserFault(g_frame, decoded) == Ia64PageFaultEntryContractError::None,
                       decoded.Fault.Cause == FaultCause::Exception);
    }
}

void TestResume(TestContext& runner) noexcept
{
    Capture(g_frame);
    RecordingHandler handler{Ia64UserFaultDisposition::Resume};
    const Ia64UserFaultBridgeStatus status = RunIa64UserFaultBridge(&g_frame, &handler);
    ASTERIA_EXPECT(runner, "a captured PL3 read miss is handed over Retained and committed for an unchanged retry",
                   status == Ia64UserFaultBridgeStatus::None, handler.Calls == 1U,
                   handler.StateAtCall == Ia64PageFaultFrameState::Retained,
                   IsExpectedFault(handler.Fault, FaultAccess::Read, InterruptionStatus),
                   g_frame.State == Ia64PageFaultFrameState::ResumeCommitted,
                   IsExpectedFault(g_frame.CapturedEntry.Fault, FaultAccess::Read, InterruptionStatus),
                   g_frame.CapturedEntry.ResumeTarget.InstructionPointer == InstructionPointer,
                   g_frame.CapturedEntry.ResumeTarget.InterruptionProcessorStatus == ProcessorStatus,
                   g_frame.CapturedEntry.ResumeTarget.CoveredInterruptionFunctionState == CoveredFunctionState,
                   g_frame.InterruptedInstructionPointer == InstructionPointer,
                   g_frame.InterruptedProcessorStatus == ProcessorStatus);

    Capture(g_frame);
    g_frame.InterruptionStatus = WriteBit | (FaultSlot << SlotShift);
    g_frame.PreCoverInterruptionStatus = g_frame.InterruptionStatus;
    RecordingHandler writer{Ia64UserFaultDisposition::Resume};
    const Foundation::UInt64 raw = AsteriaIa64UserFaultBridge(&g_frame, static_cast<IIa64UserFaultHandler*>(&writer));
    ASTERIA_EXPECT(runner, "the C bridge reports success as zero and decodes a write", raw == 0U,
                   IsExpectedFault(writer.Fault, FaultAccess::Write, g_frame.InterruptionStatus),
                   g_frame.State == Ia64PageFaultFrameState::ResumeCommitted);
}

void TestInvalidInputs(TestContext& runner) noexcept
{
    RecordingHandler handler{Ia64UserFaultDisposition::Resume};
    alignas(64U) Foundation::UInt8 storage[sizeof(Ia64PageFaultFrame) + 64U]{};
    const Foundation::UIntPointer storageStart = reinterpret_cast<Foundation::UIntPointer>(storage);
    auto* const misaligned = reinterpret_cast<Ia64PageFaultFrame*>(storageStart + 8U);
    auto* const misalignedHandler = reinterpret_cast<IIa64UserFaultHandler*>(storageStart + 4U);
    Capture(g_frame);
    ASTERIA_EXPECT(runner, "a null or misaligned frame or handler is rejected before any frame change",
                   RunIa64UserFaultBridge(nullptr, &handler) == Ia64UserFaultBridgeStatus::InvalidFrame,
                   RunIa64UserFaultBridge(misaligned, &handler) == Ia64UserFaultBridgeStatus::InvalidFrame,
                   RunIa64UserFaultBridge(&g_frame, nullptr) == Ia64UserFaultBridgeStatus::InvalidHandler,
                   RunIa64UserFaultBridge(&g_frame, misalignedHandler) == Ia64UserFaultBridgeStatus::InvalidHandler,
                   AsteriaIa64UserFaultBridge(&g_frame, nullptr) ==
                       static_cast<Foundation::UInt64>(Ia64UserFaultBridgeStatus::InvalidHandler),
                   g_frame.State == Ia64PageFaultFrameState::Capturing, handler.Calls == 0U);

    constexpr Ia64PageFaultFrameState notCapturing[]{
        Ia64PageFaultFrameState::Inactive,
        Ia64PageFaultFrameState::Retained,
        Ia64PageFaultFrameState::ResumeCommitted,
    };
    for (const Ia64PageFaultFrameState state : notCapturing)
    {
        Capture(g_frame);
        g_frame.State = state;
        ASTERIA_EXPECT(runner, "only a frame the entry left Capturing is bridged",
                       RunIa64UserFaultBridge(&g_frame, &handler) == Ia64UserFaultBridgeStatus::InvalidFrameState,
                       g_frame.State == state, handler.Calls == 0U);
    }

    Capture(g_frame);
    g_frame.PublishedToken = 1U;
    ASTERIA_EXPECT(runner, "a frame published by the capture-only lifecycle is not bridged",
                   RunIa64UserFaultBridge(&g_frame, &handler) == Ia64UserFaultBridgeStatus::InvalidFrameState,
                   g_frame.State == Ia64PageFaultFrameState::Capturing, handler.Calls == 0U);
}

void TestDecodeRejections(TestContext& runner) noexcept
{
    // Each rejected capture is decoded and then bridged: the bridge leaves it
    // Retained and never calls the handler.
    struct Rejection
    {
        const char* Description;
        Foundation::UInt64 VectorOffset;
        Foundation::UInt64 InterruptionStatus;
        Foundation::UInt64 PreCoverInterruptionStatus;
        Foundation::UInt64 ProcessorStatus;
        Ia64PageFaultEntryContractError Expected;
    };
    constexpr Foundation::UInt64 slotTwo{Foundation::UInt64{2U} << SlotShift};
    const Rejection rejections[]{
        {"a reserved vector is not a user-fault capture", 0x5800U, InterruptionStatus, InterruptionStatus,
         ProcessorStatus, Ia64PageFaultEntryContractError::UnsupportedInterruptionVector},
        {"an ISR that changed after the pre-cover sample is rejected", 0x1000U, InterruptionStatus,
         InterruptionStatus | WriteBit, ProcessorStatus, Ia64PageFaultEntryContractError::InvalidInterruptionStatus},
        {"an execute access is not an alternate data-TLB miss", 0x1000U, ExecuteBit | (FaultSlot << SlotShift),
         ExecuteBit | (FaultSlot << SlotShift), ProcessorStatus,
         Ia64PageFaultEntryContractError::InvalidAccessEncoding},
        {"a nonzero ISR code (a non-access instruction) is rejected", 0x1000U, InterruptionStatus | 4U,
         InterruptionStatus | 4U, ProcessorStatus, Ia64PageFaultEntryContractError::UnsupportedInterruptionCode},
        {"ISR.ei differing from IPSR.ri cannot restart the instruction", 0x1000U, ReadBit | slotTwo, ReadBit | slotTwo,
         ProcessorStatus, Ia64PageFaultEntryContractError::InterruptionSlotMismatch},
        {"an incomplete data frame without an RSE reference is rejected", 0x1000U,
         InterruptionStatus | (Foundation::UInt64{1U} << 38U), InterruptionStatus | (Foundation::UInt64{1U} << 38U),
         ProcessorStatus, Ia64PageFaultEntryContractError::UnsupportedAccessState},
        {"a kernel-mode access is rejected", 0x1000U, InterruptionStatus, InterruptionStatus,
         ProcessorStatus & ~(Foundation::UInt64{3U} << 32U),
         Ia64PageFaultEntryContractError::UnsupportedPrivilegeLevel},
        {"restart slot three is rejected", 0x1000U, ReadBit | (Foundation::UInt64{3U} << SlotShift),
         ReadBit | (Foundation::UInt64{3U} << SlotShift), ProcessorStatus | (Foundation::UInt64{3U} << SlotShift),
         Ia64PageFaultEntryContractError::InvalidInstructionSlot},
    };
    for (const Rejection& rejection : rejections)
    {
        Capture(g_frame);
        g_frame.VectorOffset = rejection.VectorOffset;
        g_frame.InterruptionStatus = rejection.InterruptionStatus;
        g_frame.PreCoverInterruptionStatus = rejection.PreCoverInterruptionStatus;
        g_frame.InterruptedProcessorStatus = rejection.ProcessorStatus;
        Ia64PageFaultEntryOutput output{};
        output.Fault.VirtualAddress = 7U;
        const Ia64PageFaultEntryContractError decoded = DecodeIa64UserFault(g_frame, output);
        RecordingHandler handler{Ia64UserFaultDisposition::Resume};
        const Ia64UserFaultBridgeStatus status = RunIa64UserFaultBridge(&g_frame, &handler);
        ASTERIA_EXPECT(runner, rejection.Description, decoded == rejection.Expected, output.Fault.VirtualAddress == 7U,
                       status == Ia64UserFaultBridgeStatus::DecodeRejected,
                       g_frame.State == Ia64PageFaultFrameState::Retained, handler.Calls == 0U);
    }
}

void TestHandlerOutcomes(TestContext& runner) noexcept
{
    struct Outcome
    {
        Ia64UserFaultDisposition Disposition;
        HandlerAction Action;
        Ia64UserFaultBridgeStatus Expected;
    };
    const Outcome outcomes[]{
        {Ia64UserFaultDisposition::FailStop, HandlerAction::None, Ia64UserFaultBridgeStatus::HandlerFailStop},
        {static_cast<Ia64UserFaultDisposition>(2U), HandlerAction::None, Ia64UserFaultBridgeStatus::InvalidDisposition},
        {Ia64UserFaultDisposition::Resume, HandlerAction::ClearState, Ia64UserFaultBridgeStatus::FrameStateChanged},
        {Ia64UserFaultDisposition::Resume, HandlerAction::MoveInstructionPointer,
         Ia64UserFaultBridgeStatus::ResumeTargetChanged},
    };
    for (const Outcome& outcome : outcomes)
    {
        Capture(g_frame);
        RecordingHandler handler{outcome.Disposition, outcome.Action};
        const Ia64UserFaultBridgeStatus status = RunIa64UserFaultBridge(&g_frame, &handler);
        ASTERIA_EXPECT(runner, "only an unaltered Resume commits the frame for its rfi", status == outcome.Expected,
                       handler.Calls == 1U, g_frame.State != Ia64PageFaultFrameState::ResumeCommitted);
    }
}
} // Namespace
} // Namespace Asteria::Test

namespace Asteria::Test
{
namespace
{
/// The frame AsteriaIa64UserInterruptEntry leaves before its bridge call: a
/// claimed timer interrupt at slot 1 of a PL3 bundle with interrupts enabled.
void CaptureInterrupt(Ia64PageFaultFrame& frame) noexcept
{
    Capture(frame);
    frame.VectorOffset = Platform::Ia64::Interrupt::Ia64UserInterruptVectorOffset;
    frame.PreCoverInterruptionStatus = FaultSlot << SlotShift;
    frame.InterruptionStatus = FaultSlot << SlotShift;
    frame.InterruptedProcessorStatus =
        Platform::Ia64::Interrupt::Ia64UserInterruptInterruptedProcessorStatusValue | (FaultSlot << SlotShift);
}

void TestInterrupt(TestContext& runner) noexcept
{
    CaptureInterrupt(g_frame);
    RecordingHandler resumed{Ia64UserFaultDisposition::Resume};
    const Ia64UserFaultBridgeStatus status = RunIa64UserFaultBridge(&g_frame, &resumed);
    ASTERIA_EXPECT(runner, "a claimed PL3 interrupt is handed to the interrupt handler Retained and resumes unchanged",
                   status == Ia64UserFaultBridgeStatus::None, resumed.Interrupts == 1U, resumed.Calls == 0U,
                   resumed.StateAtCall == Ia64PageFaultFrameState::Retained,
                   g_frame.State == Ia64PageFaultFrameState::ResumeCommitted);

    CaptureInterrupt(g_frame);
    RecordingHandler stopped{Ia64UserFaultDisposition::FailStop};
    const Ia64UserFaultBridgeStatus stop = RunIa64UserFaultBridge(&g_frame, &stopped);
    CaptureInterrupt(g_frame);
    RecordingHandler moved{Ia64UserFaultDisposition::Resume, HandlerAction::MoveInstructionPointer};
    const Ia64UserFaultBridgeStatus move = RunIa64UserFaultBridge(&g_frame, &moved);
    ASTERIA_EXPECT(runner, "an interrupt handler's fail-stop and a moved resume target are rejected",
                   stop == Ia64UserFaultBridgeStatus::HandlerFailStop,
                   move == Ia64UserFaultBridgeStatus::ResumeTargetChanged,
                   g_frame.State == Ia64PageFaultFrameState::Retained);
}

class RecordingDispatch final : public Platform::Ia64::Interrupt::IIa64DeviceInterruptDispatch
{
public:
    void Dispatch(const Foundation::UInt64 vector) noexcept override
    {
        ++Calls;
        Vector = vector;
    }

    Foundation::Size Calls{0U};
    Foundation::UInt64 Vector{0U};
};

// The claim leaves a device vector in the low byte of the interrupt vector offset; the bridge hands
// it to the configured dispatch and resumes the thread without its interrupt handler. Before a
// dispatch is configured, and for a vector outside the device range, it stops.
void TestDeviceInterrupt(TestContext& runner) noexcept
{
    using Platform::Ia64::Interrupt::Ia64UserInterruptVectorOffset;
    CaptureInterrupt(g_frame);
    g_frame.VectorOffset = Ia64UserInterruptVectorOffset | 0x41U;
    RecordingHandler early{Ia64UserFaultDisposition::Resume};
    const Ia64UserFaultBridgeStatus unconfigured = RunIa64UserFaultBridge(&g_frame, &early);
    RecordingDispatch dispatch;
    RecordingDispatch other;
    const bool configured = Platform::Ia64::Interrupt::ConfigureIa64UserDeviceInterrupts(dispatch);
    const bool again = Platform::Ia64::Interrupt::ConfigureIa64UserDeviceInterrupts(other);
    CaptureInterrupt(g_frame);
    g_frame.VectorOffset = Ia64UserInterruptVectorOffset | 0x41U;
    RecordingHandler handler{Ia64UserFaultDisposition::FailStop};
    const Ia64UserFaultBridgeStatus status = RunIa64UserFaultBridge(&g_frame, &handler);
    const Ia64PageFaultFrameState state = g_frame.State;
    CaptureInterrupt(g_frame);
    g_frame.VectorOffset = Ia64UserInterruptVectorOffset | 0x50U;
    const Ia64UserFaultBridgeStatus outside = RunIa64UserFaultBridge(&g_frame, &handler);
    ASTERIA_EXPECT(runner,
                   "a device vector reaches the configured dispatch once and resumes without the interrupt handler",
                   unconfigured == Ia64UserFaultBridgeStatus::HandlerFailStop, early.Interrupts == 0U, configured,
                   !again, status == Ia64UserFaultBridgeStatus::None, dispatch.Calls == 1U, dispatch.Vector == 0x41U,
                   other.Calls == 0U, handler.Interrupts == 0U, state == Ia64PageFaultFrameState::ResumeCommitted,
                   outside == Ia64UserFaultBridgeStatus::DecodeRejected);
    // Rejected PL3 state never swallows a device Interrupt; it is dispatched before the Exception.
    CaptureInterrupt(g_frame);
    g_frame.VectorOffset = Ia64UserInterruptVectorOffset | 0x42U;
    g_frame.RseConfiguration = 0xFU;
    DeferredEntryFaultHandler rejected{};
    const Ia64UserFaultBridgeStatus faulted = RunIa64UserFaultBridge(&g_frame, &rejected);
    ASTERIA_EXPECT(runner,
                   "a device vector with rejected user RSC is dispatched, then supervised as the thread's exception",
                   faulted == Ia64UserFaultBridgeStatus::HandlerFailStop, dispatch.Calls == 2U,
                   dispatch.Vector == 0x42U, rejected.Valid, rejected.Interrupts == 0U, rejected.Calls == 2U,
                   g_frame.State == Ia64PageFaultFrameState::Retained);
}

/// The frame AsteriaIa64UserExceptionEntry leaves before its bridge call: a
/// privileged operation (General Exception, ISR.code 0x10) at slot 1.
void CaptureException(Ia64PageFaultFrame& frame) noexcept
{
    Capture(frame);
    frame.VectorOffset = Platform::Ia64::Interrupt::Ia64UserGeneralExceptionVectorOffset;
    frame.InterruptionPreviousInstructionPointer = InstructionPointer;
    frame.PreCoverInterruptionStatus = (FaultSlot << SlotShift) | 0x10U;
    frame.InterruptionStatus = (FaultSlot << SlotShift) | 0x10U;
}

void TestException(TestContext& runner) noexcept
{
    CaptureException(g_frame);
    RecordingHandler handler{Ia64UserFaultDisposition::Resume};
    const Ia64UserFaultBridgeStatus status = RunIa64UserFaultBridge(&g_frame, &handler);
    ASTERIA_EXPECT(
        runner,
        "a PL3 General Exception reaches the fault handler as an Exception at the bundle, with the vector and the low ISR word",
        status == Ia64UserFaultBridgeStatus::None, handler.Calls == 1U, handler.Interrupts == 0U,
        handler.Fault.Cause == FaultCause::Exception, handler.Fault.Access == FaultAccess::Execute,
        handler.Fault.Privilege == FaultPrivilege::User, handler.Fault.VirtualAddress == InstructionPointer,
        handler.Fault.InstructionPointer == InstructionPointer,
        handler.Fault.ArchitectureSyndrome == ((Foundation::UInt64{0x5400U} << 48U) | (FaultSlot << SlotShift) | 0x10U),
        g_frame.State == Ia64PageFaultFrameState::ResumeCommitted);

    CaptureException(g_frame);
    g_frame.InterruptionStatus |= WriteBit;
    Ia64PageFaultEntryOutput output{};
    ASTERIA_EXPECT(runner, "an exception whose ISR changed after the pre-cover sample is rejected",
                   DecodeIa64UserFault(g_frame, output) == Ia64PageFaultEntryContractError::InvalidInterruptionStatus);

    // The Floating-point Fault and Trap vectors share the exception entry and reach the supervisor
    // the same way, each with its own offset; an offset the entry never records is refused.
    Ia64PageFaultEntryOutput fault{};
    Ia64PageFaultEntryOutput trap{};
    Ia64PageFaultEntryOutput unknown{};
    CaptureException(g_frame);
    g_frame.VectorOffset = Platform::Ia64::Interrupt::Ia64UserFloatingPointFaultVectorOffset;
    const Ia64PageFaultEntryContractError faulted = DecodeIa64UserFault(g_frame, fault);
    g_frame.VectorOffset = Platform::Ia64::Interrupt::Ia64UserFloatingPointTrapVectorOffset;
    const Ia64PageFaultEntryContractError trapped = DecodeIa64UserFault(g_frame, trap);
    g_frame.VectorOffset = 0x5800U;
    const Ia64PageFaultEntryContractError disabled = DecodeIa64UserFault(g_frame, unknown);
    ASTERIA_EXPECT(
        runner, "floating-point faults and traps decode as exceptions of their vectors; another vector does not",
        faulted == Ia64PageFaultEntryContractError::None,
        fault.Fault.ArchitectureSyndrome == ((Foundation::UInt64{0x5C00U} << 48U) | (FaultSlot << SlotShift) | 0x10U),
        fault.Fault.Cause == FaultCause::Exception, trapped == Ia64PageFaultEntryContractError::None,
        trap.Fault.ArchitectureSyndrome == ((Foundation::UInt64{0x5D00U} << 48U) | (FaultSlot << SlotShift) | 0x10U),
        trap.ResumeTarget.InstructionPointer == InstructionPointer,
        disabled == Ia64PageFaultEntryContractError::UnsupportedInterruptionVector);
}
// Expectations come from SDM Vol. 2 tables 8-1/8-2, not the decoder's
// branching structure. Each case exercises a newly reachable vector.
void TestNativeVectorInventory(TestContext& runner) noexcept
{
    struct VectorCase
    {
        Foundation::UInt64 Vector;
        Foundation::UInt64 AccessBits;
        FaultCause Cause;
        bool HasAddress;
        bool Trap;
        Foundation::UInt64 Code;
    };
    static constexpr VectorCase cases[]{
        {0x0400U, ExecuteBit, FaultCause::MissingMapping, true, false, 0U},
        {0x0800U, ReadBit, FaultCause::MissingMapping, true, false, 0U},
        {0x0C00U, ExecuteBit, FaultCause::MissingMapping, true, false, 0U},
        {0x1000U, WriteBit, FaultCause::MissingMapping, true, false, 0U},
        {0x1800U, ExecuteBit, FaultCause::ProtectionViolation, true, false, 0U},
        {0x1C00U, ReadBit, FaultCause::ProtectionViolation, true, false, 0U},
        {0x2000U, WriteBit, FaultCause::ProtectionViolation, true, false, 0U},
        {0x2400U, ExecuteBit, FaultCause::ProtectionViolation, true, false, 0U},
        {0x2800U, ReadBit, FaultCause::ProtectionViolation, true, false, 0U},
        {0x2C00U, 0U, FaultCause::Exception, false, false, 0U},
        {0x5000U, ExecuteBit, FaultCause::MissingMapping, true, false, 0U},
        {0x5100U, WriteBit, FaultCause::ProtectionViolation, true, false, 0U},
        {0x5200U, ExecuteBit, FaultCause::ProtectionViolation, true, false, 0U},
        {0x5300U, ReadBit, FaultCause::ProtectionViolation, true, false, 0U},
        {0x5400U, 0U, FaultCause::Exception, false, false, 0x80U},
        {0x5500U, 0U, FaultCause::Exception, false, false, 2U},
        {0x5600U, ReadBit, FaultCause::Exception, true, false, 0x20U},
        {0x5600U, ReadBit, FaultCause::Exception, false, false, 0x10U},
        {0x5700U, 0U, FaultCause::Exception, false, false, 4U},
        {0x5900U, ReadBit, FaultCause::Exception, true, false, 0U},
        {0x5A00U, WriteBit, FaultCause::Exception, true, false, 0U},
        {0x5B00U, ReadBit, FaultCause::Exception, true, false, 0U},
        {0x5C00U, 0U, FaultCause::Exception, false, false, 0x10U},
        {0x5D00U, 0U, FaultCause::Exception, false, true, 1U},
        {0x5E00U, ExecuteBit, FaultCause::Exception, true, false, 0x10U},
        {0x5E00U, 0U, FaultCause::Exception, false, true, 0x10U},
        {0x5F00U, 0U, FaultCause::Exception, false, true, 4U},
        {0x6000U, 0U, FaultCause::Exception, false, true, 8U},
    };
    for (const VectorCase& item : cases)
    {
        for (Foundation::UInt64 slot{0U}; slot < 3U; ++slot)
        {
            Capture(g_frame);
            g_frame.VectorOffset = item.Vector;
            g_frame.InterruptionStatus = item.AccessBits | item.Code | (slot << SlotShift);
            g_frame.PreCoverInterruptionStatus = g_frame.InterruptionStatus;
            const Foundation::UInt64 restartSlot = item.Trap ? ((slot + 1U) % 3U) : slot;
            g_frame.InterruptedProcessorStatus = Ia64UserFaultInterruptedProcessorStatusValue |
                                                 (restartSlot << SlotShift) | (Foundation::UInt64{1U} << 15U);
            g_frame.InterruptionPreviousInstructionPointer = InstructionPointer - 16U;
            g_frame.InterruptionImmediate = 0x1F'FFFFU;
            Ia64PageFaultEntryOutput output{};
            const Foundation::UInt64 instruction = item.Trap ? InstructionPointer - 16U : InstructionPointer;
            const Foundation::UInt64 address = item.HasAddress ? FaultAddress : instruction;
            Foundation::UInt64 syndrome = (item.Vector << 48U) | g_frame.InterruptionStatus;
            if (item.Vector == 0x2C00U)
            {
                syndrome |= g_frame.InterruptionImmediate;
            }
            ASTERIA_EXPECT(
                runner,
                "native vectors deliver defined addresses, cause, ISR and source slot; traps retain their next slot",
                DecodeIa64UserFault(g_frame, output) == Ia64PageFaultEntryContractError::None,
                output.Fault.Cause == item.Cause, output.Fault.VirtualAddress == address,
                output.Fault.InstructionPointer == instruction, output.Fault.ArchitectureSyndrome == syndrome,
                output.ResumeTarget.InstructionPointer == InstructionPointer,
                output.ResumeTarget.InterruptionProcessorStatus == g_frame.InterruptedProcessorStatus,
                output.ResumeTarget.CoveredInterruptionFunctionState == CoveredFunctionState);
        }
    }
    constexpr Foundation::UInt64 rseVectors[]{0x0800U, 0x1000U, 0x1C00U, 0x2800U, 0x5000U, 0x5100U, 0x5300U};
    for (const Foundation::UInt64 vector : rseVectors)
    {
        Capture(g_frame);
        g_frame.VectorOffset = vector;
        g_frame.InterruptionStatus |= Foundation::UInt64{3U} << 37U;
        g_frame.PreCoverInterruptionStatus = g_frame.InterruptionStatus;
        Ia64PageFaultEntryOutput output{};
        ASTERIA_EXPECT(runner, "mandatory RSE fills of incomplete frames preserve the backing-store IFA and ISR.rs/ir",
                       DecodeIa64UserFault(g_frame, output) == Ia64PageFaultEntryContractError::None,
                       output.Fault.VirtualAddress == FaultAddress,
                       (output.Fault.ArchitectureSyndrome & (Foundation::UInt64{3U} << 37U)) ==
                           (Foundation::UInt64{3U} << 37U));
    }
    Capture(g_frame);
    g_frame.CoveredInterruptionFunctionState = CoveredFunctionState | (Foundation::UInt64{1U} << 38U);
    Ia64PageFaultEntryOutput output{};
    ASTERIA_EXPECT(runner, "the hardware-owned covered IFS header remains a kernel invariant",
                   DecodeIa64UserFault(g_frame, output) ==
                       Ia64PageFaultEntryContractError::InvalidCoveredInterruptionFunctionState);
    Capture(g_frame);
    g_frame.InterruptionStatus |= Foundation::UInt64{1U} << 39U;
    g_frame.PreCoverInterruptionStatus = g_frame.InterruptionStatus;
    ASTERIA_EXPECT(runner, "nested faults lack an independent tuple and never reach the supervisor",
                   DecodeIa64UserFault(g_frame, output) == Ia64PageFaultEntryContractError::NestedInterruption);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64UserFaultEntryTests"};
    Asteria::Test::TestRejectedUserEntryState(runner);
    Asteria::Test::TestResume(runner);
    Asteria::Test::TestInvalidInputs(runner);
    Asteria::Test::TestDecodeRejections(runner);
    Asteria::Test::TestHandlerOutcomes(runner);
    Asteria::Test::TestInterrupt(runner);
    Asteria::Test::TestDeviceInterrupt(runner);
    Asteria::Test::TestException(runner);
    Asteria::Test::TestNativeVectorInventory(runner);
    return runner.Finish();
}
