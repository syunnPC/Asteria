// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Interrupt/Ia64UserFaultEntry.hpp"

// The target entry point is in Ia64UserFaultEntry.S.
#if !defined(__ia64__)
extern "C" [[noreturn]] void AsteriaIa64UserBreakEntry() noexcept
{
    __builtin_trap();
}

extern "C" [[noreturn]] void AsteriaIa64UserFaultEntry() noexcept
{
    __builtin_trap();
}

extern "C" [[noreturn]] void AsteriaIa64UserInterruptClaim() noexcept
{
    __builtin_trap();
}

extern "C" [[noreturn]] void AsteriaIa64UserInterruptEntry() noexcept
{
    __builtin_trap();
}

extern "C" [[noreturn]] void AsteriaIa64UserExceptionEntry() noexcept
{
    __builtin_trap();
}
#endif

namespace Asteria::Platform::Ia64::Interrupt
{
namespace
{
constexpr Foundation::UInt64 FrameAlignmentMask{alignof(Ia64PageFaultFrame) - 1U};
constexpr Foundation::UInt64 DeviceVectorMask{0xFFU};

// Where the bridge sends device vectors: set once at boot, read on the BSP
// with PSR.i clear.
constinit IIa64DeviceInterruptDispatch* g_deviceInterrupts{nullptr};

[[nodiscard]] constexpr bool IsDeviceVectorOffset(const Foundation::UInt64 offset) noexcept
{
    const Foundation::UInt64 vector = offset & DeviceVectorMask;
    return ((offset & ~DeviceVectorMask) == Ia64UserInterruptVectorOffset) &&
           (vector >= Ia64UserInterruptFirstDeviceVector) &&
           (vector < (Ia64UserInterruptFirstDeviceVector + Ia64UserInterruptDeviceVectorCount));
}
constexpr Foundation::UInt64 HandlerAlignmentMask{alignof(Foundation::UInt64) - 1U};

[[nodiscard]] Foundation::UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
}

[[nodiscard]] Ia64PageFaultFrameState LoadState(const Ia64PageFaultFrame& frame) noexcept
{
    return __atomic_load_n(&frame.State, __ATOMIC_ACQUIRE);
}

void StoreState(Ia64PageFaultFrame& frame, const Ia64PageFaultFrameState state) noexcept
{
    __atomic_store_n(&frame.State, state, __ATOMIC_RELEASE);
}

[[nodiscard]] bool ResumeTargetIsUnchanged(const Ia64PageFaultFrame& frame,
                                           const Ia64PageFaultResumeTarget& target) noexcept
{
    return (frame.InterruptedInstructionPointer == target.InstructionPointer) &&
           (frame.InterruptedProcessorStatus == target.InterruptionProcessorStatus) &&
           (frame.CoveredInterruptionFunctionState == target.CoveredInterruptionFunctionState);
}

// The interruption fields the decoder reads (Intel SDM rev. 2.3 Vol. 2,
// §3.3.5 and tables 8-1/8-2).
constexpr Foundation::UInt64 IsrCodeMask{0xFFFFU};
constexpr Foundation::UInt64 IsrExecuteBit{Foundation::UInt64{1U} << 32U};
constexpr Foundation::UInt64 IsrWriteBit{Foundation::UInt64{1U} << 33U};
constexpr Foundation::UInt64 IsrReadBit{Foundation::UInt64{1U} << 34U};
constexpr Foundation::UInt64 IsrNonAccessBit{Foundation::UInt64{1U} << 35U};
constexpr Foundation::UInt64 IsrRseBit{Foundation::UInt64{1U} << 37U};
constexpr Foundation::UInt64 IsrIncompleteFrameBit{Foundation::UInt64{1U} << 38U};
constexpr Foundation::UInt64 IsrNestedBit{Foundation::UInt64{1U} << 39U};
/// ISR.code, x, w, r, na, sp, rs, ir, ni, ei and ed: every field but the
/// IA-32 vector (bits 16-23) and so (bit 40), and the reserved bits.
constexpr Foundation::UInt64 NativeIsrMask{IsrCodeMask | (Foundation::UInt64{0xEFFU} << 32U)};
/// ISR.ei and IPSR.ri: the instruction slot.
constexpr Foundation::UInt64 SlotShift{41U};
constexpr Foundation::UInt64 SlotMask{3U};
constexpr Foundation::UInt64 InvalidSlot{3U};
constexpr Foundation::UInt64 PsrInstructionSetBit{Foundation::UInt64{1U} << 34U};
constexpr Foundation::UInt64 PsrPrivilegeShift{32U};
constexpr Foundation::UInt64 PsrPrivilegeMask{3U};
constexpr Foundation::UInt64 UserPrivilegeLevel{3U};
constexpr Foundation::UInt64 BundleMask{15U};
/// ISR.code{7:4} of a NaT Consumption fault: 1 a NaT register, 2 a NaT
/// page, whose IFA is defined (SDM Vol. 2, NaT Consumption vector).
constexpr Foundation::UInt64 NatCodeShift{4U};
constexpr Foundation::UInt64 NatCodeMask{0xFU};
constexpr Foundation::UInt64 NatPageConsumption{2U};
/// IIM of a break: a zero-extended 21-bit immediate.
constexpr Foundation::UInt64 BreakImmediateMask{0x1F'FFFFU};
/// Every IVT offset lies in the 32 KiB table (SDM Vol. 2, §5.7).
constexpr Foundation::UInt64 InterruptVectorTableBytes{0x8000U};
constexpr Foundation::UInt64 SyndromeVectorShift{48U};
constexpr Foundation::UInt64 SyndromeLowWordMask{0xFFFF'FFFFU};

/// Which accesses a memory vector reports in ISR.x, w and r.
enum class AccessOrigin : Foundation::UInt8
{
    Either,
    Instruction,
    Data,
};

/// What a native vector delivers.
struct VectorClass
{
    Kernel::Memory::FaultCause Cause{Kernel::Memory::FaultCause::Exception};
    bool HasAddress{false};
    AccessOrigin Origin{AccessOrigin::Either};
    bool Trap{false};
};

[[nodiscard]] constexpr Foundation::UInt64 VectorOffset(const Ia64UserFaultVector vector) noexcept
{
    return static_cast<Foundation::UInt64>(vector);
}

[[nodiscard]] constexpr bool Classify(const Foundation::UInt64 offset, const Foundation::UInt64 isr,
                                      VectorClass& shape) noexcept
{
    using Kernel::Memory::FaultCause;
    if (offset >= InterruptVectorTableBytes)
    {
        return false;
    }
    const bool execute = (isr & IsrExecuteBit) != 0U;
    switch (static_cast<Ia64UserFaultVector>(offset))
    {
    case Ia64UserFaultVector::InstructionTlb:
    case Ia64UserFaultVector::AlternateInstructionTlb:
        shape = {FaultCause::MissingMapping, true, AccessOrigin::Instruction, false};
        return true;
    case Ia64UserFaultVector::DataTlb:
    case Ia64UserFaultVector::AlternateDataTlb:
        shape = {FaultCause::MissingMapping, true, AccessOrigin::Data, false};
        return true;
    case Ia64UserFaultVector::PageNotPresent:
        shape = {FaultCause::MissingMapping, true, AccessOrigin::Either, false};
        return true;
    case Ia64UserFaultVector::InstructionKeyMiss:
    case Ia64UserFaultVector::InstructionAccessBit:
    case Ia64UserFaultVector::InstructionAccessRights:
        shape = {FaultCause::ProtectionViolation, true, AccessOrigin::Instruction, false};
        return true;
    case Ia64UserFaultVector::DataKeyMiss:
    case Ia64UserFaultVector::DirtyBit:
    case Ia64UserFaultVector::DataAccessBit:
    case Ia64UserFaultVector::DataAccessRights:
        shape = {FaultCause::ProtectionViolation, true, AccessOrigin::Data, false};
        return true;
    case Ia64UserFaultVector::KeyPermission:
        shape = {FaultCause::ProtectionViolation, true, AccessOrigin::Either, false};
        return true;
    case Ia64UserFaultVector::Debug:
        shape = {FaultCause::Exception, true, AccessOrigin::Either, false};
        return true;
    case Ia64UserFaultVector::UnalignedReference:
    case Ia64UserFaultVector::UnsupportedDataReference:
        shape = {FaultCause::Exception, true, AccessOrigin::Data, false};
        return true;
    case Ia64UserFaultVector::NatConsumption:
        shape = {FaultCause::Exception, ((isr >> NatCodeShift) & NatCodeMask) == NatPageConsumption,
                 AccessOrigin::Either, false};
        return true;
    case Ia64UserFaultVector::LowerPrivilegeTransfer:
        // Only the Unimplemented Instruction Address fault (ISR.x) defines
        // IFA; the other forms are traps.
        shape = {FaultCause::Exception, execute, AccessOrigin::Either, !execute};
        return true;
    case Ia64UserFaultVector::FloatingPointTrap:
    case Ia64UserFaultVector::TakenBranch:
    case Ia64UserFaultVector::SingleStep:
        shape = {FaultCause::Exception, false, AccessOrigin::Either, true};
        return true;
    case Ia64UserFaultVector::Break:
    case Ia64UserFaultVector::GeneralException:
    case Ia64UserFaultVector::DisabledFloatingPointRegister:
    case Ia64UserFaultVector::Speculation:
    case Ia64UserFaultVector::FloatingPointFault:
        shape = {FaultCause::Exception, false, AccessOrigin::Either, false};
        return true;
    }
    return false;
}
} // Namespace

bool IsIa64UserEntryStateFault(const Ia64PageFaultFrame& frame) noexcept
{
    if (((frame.InterruptedProcessorStatus >> PsrPrivilegeShift) & PsrPrivilegeMask) != UserPrivilegeLevel ||
        (frame.InterruptionStatus & IsrNestedBit) != 0U)
    {
        return false;
    }
    if ((frame.RseConfiguration & Ia64UserFaultRseConfigurationMask) != System::Ia64SystemCallUserRseConfiguration)
    {
        return true;
    }
    // PFS.pfm is PL3-writable and br.ret copies it to CFM. Only the
    // hardware-owned covered-IFS header remains a kernel invariant.
    // SDM rev. 1.1 Vol. 1 §3.1.8.10; Vol. 3 "mov ar" / "br" Operation.
    if ((frame.CoveredInterruptionFunctionState & ~System::Ia64SystemCallFrameMarkerMask) ==
            System::Ia64SystemCallIfsValidMask &&
        !Ia64PageFaultEntryContract::IsCoveredFunctionStateValid(frame.CoveredInterruptionFunctionState))
    {
        return true;
    }
    return frame.VectorOffset == VectorOffset(Ia64UserFaultVector::Break) &&
           frame.InterruptionImmediate == System::Ia64SystemCallBreakImmediate &&
           (frame.RseConfiguration != System::Ia64SystemCallUserRseConfiguration ||
            System::ValidateIa64SystemCallFrameConfiguration(frame.CoveredInterruptionFunctionState) !=
                System::Ia64SystemCallFrameConfigurationError::None);
}

Ia64PageFaultEntryContractError DecodeIa64UserFault(const Ia64PageFaultFrame& frame,
                                                    Ia64PageFaultEntryOutput& output) noexcept
{
    using Kernel::Memory::FaultAccess;
    using Kernel::Memory::FaultCause;
    using Kernel::Memory::FaultPrivilege;
    const Foundation::UInt64 isr = frame.InterruptionStatus;
    const Foundation::UInt64 psr = frame.InterruptedProcessorStatus;
    const Foundation::UInt64 vector = frame.VectorOffset;
    const Foundation::UInt64 slot = (isr >> SlotShift) & SlotMask;
    const Foundation::UInt64 restartSlot = (psr >> SlotShift) & SlotMask;
    const bool execute = (isr & IsrExecuteBit) != 0U;
    const bool write = (isr & IsrWriteBit) != 0U;
    const bool read = (isr & IsrReadBit) != 0U;
    const bool entryStateFault = IsIa64UserEntryStateFault(frame);
    VectorClass shape{};
    if (!Classify(vector, isr, shape) &&
        !(entryStateFault && (vector == Ia64UserInterruptVectorOffset || IsDeviceVectorOffset(vector))))
    {
        return Ia64PageFaultEntryContractError::UnsupportedInterruptionVector;
    }
    const bool interruptedTrap = shape.Trap;
    const bool interruptedExternal = vector == Ia64UserInterruptVectorOffset || IsDeviceVectorOffset(vector);
    if (entryStateFault)
    {
        shape = {FaultCause::Exception, false, AccessOrigin::Either, false};
    }
    if ((isr != frame.PreCoverInterruptionStatus) || ((isr & ~NativeIsrMask) != 0U))
    {
        return Ia64PageFaultEntryContractError::InvalidInterruptionStatus;
    }
    if ((isr & IsrNestedBit) != 0U)
    {
        return Ia64PageFaultEntryContractError::NestedInterruption;
    }
    // External interrupts and lower-privilege traps can interrupt a fill
    // with ir=1, rs=0 (SDM rev. 1.1 Vol. 2 table 8-2, note i).
    if (((isr & IsrIncompleteFrameBit) != 0U) && ((isr & IsrRseBit) == 0U) && !interruptedTrap &&
        !interruptedExternal && !execute)
    {
        return Ia64PageFaultEntryContractError::UnsupportedAccessState;
    }
    if ((psr & PsrInstructionSetBit) != 0U)
    {
        return Ia64PageFaultEntryContractError::UnsupportedInstructionSet;
    }
    if (((psr >> PsrPrivilegeShift) & PsrPrivilegeMask) != UserPrivilegeLevel)
    {
        return Ia64PageFaultEntryContractError::UnsupportedPrivilegeLevel;
    }
    if ((psr & Ia64UserFaultInterruptedProcessorStatusMask) != Ia64UserFaultInterruptedProcessorStatusValue)
    {
        return Ia64PageFaultEntryContractError::UnsupportedTranslationState;
    }
    if (((frame.InterruptedInstructionPointer & BundleMask) != 0U) ||
        (shape.Trap && ((frame.InterruptionPreviousInstructionPointer & BundleMask) != 0U)))
    {
        return Ia64PageFaultEntryContractError::InvalidInstructionPointer;
    }
    if ((slot == InvalidSlot) || (restartSlot == InvalidSlot))
    {
        return Ia64PageFaultEntryContractError::InvalidInstructionSlot;
    }
    // A fault restarts its own slot; a trap's ISR.ei names the completed
    // instruction and IPSR.ri the next one.
    if (!interruptedTrap && (slot != restartSlot))
    {
        return Ia64PageFaultEntryContractError::InterruptionSlotMismatch;
    }
    if ((frame.CoveredInterruptionFunctionState & ~System::Ia64SystemCallFrameMarkerMask) !=
            System::Ia64SystemCallIfsValidMask ||
        (!entryStateFault &&
         !Ia64PageFaultEntryContract::IsCoveredFunctionStateValid(frame.CoveredInterruptionFunctionState)))
    {
        return Ia64PageFaultEntryContractError::InvalidCoveredInterruptionFunctionState;
    }
    if (shape.HasAddress &&
        ((execute && (read || write || (shape.Origin == AccessOrigin::Data))) || (!execute && !read && !write) ||
         (!execute && (shape.Origin == AccessOrigin::Instruction))))
    {
        return Ia64PageFaultEntryContractError::InvalidAccessEncoding;
    }
    // A memory-management fault carries ISR.code only for a non-access
    // instruction (ISR.na: probe, fc, lfetch, tpa, tak).
    if ((shape.Cause != FaultCause::Exception) && ((isr & IsrCodeMask) != 0U) && ((isr & IsrNonAccessBit) == 0U))
    {
        return Ia64PageFaultEntryContractError::UnsupportedInterruptionCode;
    }
    Foundation::UInt64 syndrome = (vector << SyndromeVectorShift) | isr;
    if (vector == VectorOffset(Ia64UserFaultVector::Break))
    {
        // A rejected syscall's admission uses this same complete-state
        // path; an admitted syscall must still use its dedicated bridge.
        if ((frame.InterruptionImmediate > BreakImmediateMask) ||
            (!entryStateFault && frame.InterruptionImmediate == System::Ia64SystemCallBreakImmediate))
        {
            return Ia64PageFaultEntryContractError::UnsupportedInterruptionCode;
        }
        syndrome = (syndrome & ~SyndromeLowWordMask) | frame.InterruptionImmediate;
    }
    FaultAccess access{FaultAccess::Execute};
    if (shape.HasAddress && !execute)
    {
        access = write ? FaultAccess::Write : FaultAccess::Read;
    }
    const Foundation::UInt64 instruction =
        shape.Trap ? frame.InterruptionPreviousInstructionPointer : frame.InterruptedInstructionPointer;
    output = Ia64PageFaultEntryOutput{
        .Fault =
            Kernel::Memory::FaultRecord{
                .VirtualAddress = shape.HasAddress ? frame.InterruptionFaultingAddress : instruction,
                .InstructionPointer = instruction,
                .ArchitectureSyndrome = syndrome,
                .Access = access,
                .Cause = shape.Cause,
                .Privilege = FaultPrivilege::User,
                .Reserved = {},
            },
        .ResumeTarget =
            Ia64PageFaultResumeTarget{
                .InstructionPointer = frame.InterruptedInstructionPointer,
                .InterruptionProcessorStatus = psr,
                .CoveredInterruptionFunctionState = frame.CoveredInterruptionFunctionState,
            },
    };
    return Ia64PageFaultEntryContractError::None;
}

bool ConfigureIa64UserDeviceInterrupts(IIa64DeviceInterruptDispatch& dispatch) noexcept
{
    if (g_deviceInterrupts != nullptr)
    {
        return false;
    }
    g_deviceInterrupts = &dispatch;
    return true;
}

Ia64UserFaultBridgeStatus RunIa64UserFaultBridge(Ia64PageFaultFrame* const frame,
                                                 IIa64UserFaultHandler* const handler) noexcept
{
    if ((frame == nullptr) || ((AddressOf(frame) & FrameAlignmentMask) != 0U))
    {
        return Ia64UserFaultBridgeStatus::InvalidFrame;
    }
    if ((handler == nullptr) || ((AddressOf(handler) & HandlerAlignmentMask) != 0U))
    {
        return Ia64UserFaultBridgeStatus::InvalidHandler;
    }
    if ((LoadState(*frame) != Ia64PageFaultFrameState::Capturing) ||
        (__atomic_load_n(&frame->PublishedToken, __ATOMIC_ACQUIRE) != 0U))
    {
        return Ia64UserFaultBridgeStatus::InvalidFrameState;
    }

    // From here the thread may block: Retained is the one frame state in which Ia64KernelContext:
    // SwitchTo leaves and re-enters the thread.
    StoreState(*frame, Ia64PageFaultFrameState::Retained);
    Ia64PageFaultResumeTarget target{frame->InterruptedInstructionPointer, frame->InterruptedProcessorStatus,
                                     frame->CoveredInterruptionFunctionState};
    Ia64UserFaultDisposition disposition{Ia64UserFaultDisposition::FailStop};
    const bool entryStateFault = IsIa64UserEntryStateFault(*frame);
    const bool device = IsDeviceVectorOffset(frame->VectorOffset);
    const bool external = device || (frame->VectorOffset == Ia64UserInterruptVectorOffset);
    Ia64PageFaultEntryOutput decoded{};
    if (!external || entryStateFault)
    {
        // Failure-atomic: a corrupted frame stops before any scheduling work.
        if (DecodeIa64UserFault(*frame, decoded) != Ia64PageFaultEntryContractError::None)
        {
            return Ia64UserFaultBridgeStatus::DecodeRejected;
        }
        frame->CapturedEntry = decoded;
        target = decoded.ResumeTarget;
    }
    if (device)
    {
        // The claim relinquished the vector; the interrupted thread resumes
        // at once, and one the Interrupt woke runs when the scheduler picks it.
        if (g_deviceInterrupts != nullptr)
        {
            g_deviceInterrupts->Dispatch(frame->VectorOffset & DeviceVectorMask);
            disposition = Ia64UserFaultDisposition::Resume;
        }
    }
    else if (external)
    {
        disposition = handler->HandleUserInterrupt(*frame);
    }
    // An interrupt that finds rejected PL3 state is first serviced as an interrupt (the timer's
    // deadline acknowledged and its scheduling point taken, a device Interrupt dispatched), so no
    // other thread loses it; only then does the thread's Exception reach its supervisor, before any
    // return to PL3.
    if (external && entryStateFault && (disposition == Ia64UserFaultDisposition::Resume))
    {
        if (LoadState(*frame) != Ia64PageFaultFrameState::Retained)
        {
            return Ia64UserFaultBridgeStatus::FrameStateChanged;
        }
        if (!ResumeTargetIsUnchanged(*frame, target))
        {
            return Ia64UserFaultBridgeStatus::ResumeTargetChanged;
        }
    }
    if (!external || (entryStateFault && (disposition == Ia64UserFaultDisposition::Resume)))
    {
        // A busy supervisor yields through the handler. Retry in the retained kernel continuation:
        // never restore the rejected RSC/CFM merely to fault again.
        do
        {
            disposition = handler->HandleUserFault(*frame, decoded.Fault);
            if (entryStateFault && disposition == Ia64UserFaultDisposition::Resume)
            {
                if (LoadState(*frame) != Ia64PageFaultFrameState::Retained)
                {
                    return Ia64UserFaultBridgeStatus::FrameStateChanged;
                }
                if (!ResumeTargetIsUnchanged(*frame, target))
                {
                    return Ia64UserFaultBridgeStatus::ResumeTargetChanged;
                }
            }
        } while (entryStateFault && disposition == Ia64UserFaultDisposition::Resume);
    }
    if (disposition == Ia64UserFaultDisposition::FailStop)
    {
        return Ia64UserFaultBridgeStatus::HandlerFailStop;
    }
    if (disposition != Ia64UserFaultDisposition::Resume)
    {
        return Ia64UserFaultBridgeStatus::InvalidDisposition;
    }
    if (LoadState(*frame) != Ia64PageFaultFrameState::Retained)
    {
        return Ia64UserFaultBridgeStatus::FrameStateChanged;
    }
    if (!ResumeTargetIsUnchanged(*frame, target))
    {
        return Ia64UserFaultBridgeStatus::ResumeTargetChanged;
    }
    StoreState(*frame, Ia64PageFaultFrameState::ResumeCommitted);
    return Ia64UserFaultBridgeStatus::None;
}
} // Namespace Asteria::Platform::Ia64::Interrupt

extern "C" ASTERIA_NOINLINE_NOIPA Asteria::Foundation::UInt64
AsteriaIa64UserFaultBridge(Asteria::Platform::Ia64::Interrupt::Ia64PageFaultFrame* const frame,
                           void* const handler) noexcept
{
    return static_cast<Asteria::Foundation::UInt64>(Asteria::Platform::Ia64::Interrupt::RunIa64UserFaultBridge(
        frame, static_cast<Asteria::Platform::Ia64::Interrupt::IIa64UserFaultHandler*>(handler)));
}
