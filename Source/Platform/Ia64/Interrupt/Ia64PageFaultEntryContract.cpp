// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Interrupt/Ia64PageFaultEntryContract.hpp"

#include "Platform/Ia64/Memory/Ia64PageFaultDecoder.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
namespace
{
constexpr Foundation::UInt64 DataTranslationBit{Foundation::UInt64{1U} << 17U};
constexpr Foundation::UInt64 RseTranslationBit{Foundation::UInt64{1U} << 27U};
constexpr Foundation::UInt64 InstructionTranslationBit{Foundation::UInt64{1U} << 36U};
constexpr Foundation::UInt64 RequiredTranslationBits{DataTranslationBit | RseTranslationBit |
                                                     InstructionTranslationBit};
constexpr Foundation::UInt64 InterruptionCollectionBit{Foundation::UInt64{1U} << 13U};
constexpr Foundation::UInt64 ProtectionKeyBit{Foundation::UInt64{1U} << 15U};
constexpr Foundation::UInt64 PrivilegeMask{Foundation::UInt64{3U} << 32U};
constexpr Foundation::UInt64 UserPrivilege{PrivilegeMask};
constexpr Foundation::UInt64 InstructionSetBit{Foundation::UInt64{1U} << 34U};
constexpr Foundation::UInt64 RegisterBankBit{Foundation::UInt64{1U} << 44U};
constexpr Foundation::UInt64 RestartInstructionSlotMask{Foundation::UInt64{3U} << 41U};
constexpr Foundation::UInt64 RestartInstructionSlotShift{41U};
constexpr Foundation::UInt64 BundleAlignmentMask{0xFU};

constexpr Foundation::UInt64 NonAccessBit{Foundation::UInt64{1U} << 35U};
constexpr Foundation::UInt64 SpeculativeLoadBit{Foundation::UInt64{1U} << 36U};
constexpr Foundation::UInt64 RegisterStackBit{Foundation::UInt64{1U} << 37U};
constexpr Foundation::UInt64 IncompleteRegisterFrameBit{Foundation::UInt64{1U} << 38U};
constexpr Foundation::UInt64 NestedInterruptionBit{Foundation::UInt64{1U} << 39U};
constexpr Foundation::UInt64 PostCoverUnsupportedAccessStateBits{NonAccessBit | SpeculativeLoadBit | RegisterStackBit};
constexpr Foundation::UInt64 ExceptionInstructionSlotMask{Foundation::UInt64{3U} << 41U};
constexpr Foundation::UInt64 ExceptionInstructionSlotShift{41U};
constexpr Foundation::UInt64 InterruptionCodeMask{0xFFFFU};

constexpr Foundation::UInt64 IfsValidBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 CurrentFrameMask{(Foundation::UInt64{1U} << 38U) - 1U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 RegionZeroLimit{Foundation::UInt64{1U} << 61U};

[[nodiscard]] bool TokenOutputAliasesFrame(const Ia64PageFaultFrame& frame,
                                           const Foundation::UInt64& captureToken) noexcept
{
    const Foundation::UIntPointer frameAddress = reinterpret_cast<Foundation::UIntPointer>(&frame);
    const Foundation::UIntPointer outputAddress = reinterpret_cast<Foundation::UIntPointer>(&captureToken);
    if (outputAddress >= frameAddress)
    {
        return (outputAddress - frameAddress) < sizeof(frame);
    }
    return (frameAddress - outputAddress) < sizeof(captureToken);
}

[[nodiscard]] constexpr bool TryAdvanceRsePointer(const Foundation::UInt64 pointer,
                                                  const Foundation::UInt64 registerCount,
                                                  Foundation::UInt64& advanced) noexcept
{
    if ((pointer == 0U) || ((pointer & 7U) != 0U) || (registerCount > 96U))
    {
        return false;
    }
    const Foundation::UInt64 slot = (pointer >> 3U) & 63U;
    if (slot == 63U)
    {
        return false;
    }
    const Foundation::UInt64 slotSum = slot + registerCount;
    const Foundation::UInt64 skippedNatSlots = (slotSum >= 63U ? 1U : 0U) + (slotSum >= 126U ? 1U : 0U);
    const Foundation::UInt64 byteCount = (registerCount + skippedNatSlots) * 8U;
    if (pointer > (MaximumUInt64 - byteCount))
    {
        return false;
    }
    advanced = pointer + byteCount;
    return true;
}

[[nodiscard]] constexpr bool IsRseRegisterSlotPointer(const Foundation::UInt64 pointer) noexcept
{
    return (pointer != 0U) && ((pointer & 7U) == 0U) && (((pointer >> 3U) & 63U) != 63U);
}

[[nodiscard]] constexpr bool TryCountRseRegistersBetween(const Foundation::UInt64 start, const Foundation::UInt64 end,
                                                         Foundation::UInt64& registerCount) noexcept
{
    if (!IsRseRegisterSlotPointer(start) || !IsRseRegisterSlotPointer(end) || (start > end) ||
        ((end - start) >= Ia64PageFaultMaximumRseLoadSizeBytes))
    {
        return false;
    }

    Foundation::UInt64 cursor{start};
    Foundation::UInt64 count{0U};
    while (cursor < end)
    {
        if ((cursor > (MaximumUInt64 - 8U)) || (count >= (Ia64PageFaultMaximumRseLoadSizeBytes / 8U)))
        {
            return false;
        }
        cursor += 8U;
        ++count;
        if (((cursor >> 3U) & 63U) == 63U)
        {
            if (cursor > (MaximumUInt64 - 8U))
            {
                return false;
            }
            cursor += 8U;
        }
    }
    if (cursor != end)
    {
        return false;
    }
    registerCount = count;
    return true;
}

[[nodiscard]] constexpr bool TryCalculatePageAlignedRseSpan(const Foundation::UInt64 registerCount,
                                                            Foundation::UInt64& spanBytes) noexcept
{
    const Foundation::UInt64 skippedNatSlots = registerCount / 63U;
    if (registerCount > ((MaximumUInt64 / 8U) - skippedNatSlots))
    {
        return false;
    }
    spanBytes = (registerCount + skippedNatSlots) * 8U;
    return spanBytes < Ia64PageFaultMaximumRseLoadSizeBytes;
}

/// The vectors an entry observation may name: the four heavyweight vectors
/// of the capture-only checkpoint, and (Decodable only) the Alternate Data
/// TLB vector through which the user-fault entry delivers a ledger miss.
enum class VectorSet : Foundation::UInt8
{
    Heavyweight,
    Decodable,
};

[[nodiscard]] constexpr bool TryGetVector(const Foundation::UInt64 vectorOffset, const VectorSet set,
                                          Memory::Ia64PageFaultVector& vector) noexcept
{
    switch (vectorOffset)
    {
    case Ia64PageFaultAlternateDataTlbVectorOffset:
        if (set != VectorSet::Decodable)
        {
            return false;
        }
        vector = Memory::Ia64PageFaultVector::AlternateDataTlb;
        return true;
    case Ia64PageFaultPageNotPresentVectorOffset:
        vector = Memory::Ia64PageFaultVector::PageNotPresent;
        return true;
    case Ia64PageFaultKeyPermissionVectorOffset:
        vector = Memory::Ia64PageFaultVector::KeyPermission;
        return true;
    case Ia64PageFaultInstructionAccessRightsVectorOffset:
        vector = Memory::Ia64PageFaultVector::InstructionAccessRights;
        return true;
    case Ia64PageFaultDataAccessRightsVectorOffset:
        vector = Memory::Ia64PageFaultVector::DataAccessRights;
        return true;
    }
    return false;
}

[[nodiscard]] constexpr Ia64PageFaultEntryContractError
MapDecodeError(const Memory::Ia64PageFaultDecodeError error) noexcept
{
    switch (error)
    {
    case Memory::Ia64PageFaultDecodeError::None:
        return Ia64PageFaultEntryContractError::None;
    case Memory::Ia64PageFaultDecodeError::InvalidInterruptionStatus:
        return Ia64PageFaultEntryContractError::InvalidInterruptionStatus;
    case Memory::Ia64PageFaultDecodeError::InvalidAccessEncoding:
        return Ia64PageFaultEntryContractError::InvalidAccessEncoding;
    case Memory::Ia64PageFaultDecodeError::UnsupportedInterruptionVector:
    case Memory::Ia64PageFaultDecodeError::UnsupportedInstructionSet:
    case Memory::Ia64PageFaultDecodeError::InvalidInstructionPointer:
        return Ia64PageFaultEntryContractError::DecoderContractViolation;
    }
    return Ia64PageFaultEntryContractError::DecoderContractViolation;
}

[[nodiscard]] constexpr bool FaultRecordsAreEqual(const Kernel::Memory::FaultRecord& left,
                                                  const Kernel::Memory::FaultRecord& right) noexcept
{
    if ((left.VirtualAddress != right.VirtualAddress) || (left.InstructionPointer != right.InstructionPointer) ||
        (left.ArchitectureSyndrome != right.ArchitectureSyndrome) || (left.Access != right.Access) ||
        (left.Cause != right.Cause) || (left.Privilege != right.Privilege))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < 5U; ++index)
    {
        if (left.Reserved[index] != right.Reserved[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool EntryOutputsAreEqual(const Ia64PageFaultEntryOutput& left,
                                                  const Ia64PageFaultEntryOutput& right) noexcept
{
    return FaultRecordsAreEqual(left.Fault, right.Fault) &&
           (left.ResumeTarget.InstructionPointer == right.ResumeTarget.InstructionPointer) &&
           (left.ResumeTarget.InterruptionProcessorStatus == right.ResumeTarget.InterruptionProcessorStatus) &&
           (left.ResumeTarget.CoveredInterruptionFunctionState == right.ResumeTarget.CoveredInterruptionFunctionState);
}

[[nodiscard]] Ia64PageFaultFrameError DecodeFrameEntry(const Ia64PageFaultFrame& frame,
                                                       Ia64PageFaultEntryOutput& output) noexcept
{
    if (frame.InterruptionStatus != frame.PreCoverInterruptionStatus)
    {
        return Ia64PageFaultFrameError::ObservationMismatch;
    }
    if (Ia64PageFaultEntryContract::ValidateAndDecode(
            Ia64PageFaultEntryObservation{
                .FaultAddress = frame.InterruptionFaultingAddress,
                .InstructionPointer = frame.InterruptedInstructionPointer,
                .InterruptionStatus = frame.InterruptionStatus,
                .InterruptionProcessorStatus = frame.InterruptedProcessorStatus,
                .CoveredInterruptionFunctionState = frame.CoveredInterruptionFunctionState,
                .VectorOffset = frame.VectorOffset,
            },
            output) != Ia64PageFaultEntryContractError::None)
    {
        return Ia64PageFaultFrameError::EntryContractRejected;
    }
    return Ia64PageFaultFrameError::None;
}

[[nodiscard]] constexpr Ia64PageFaultCaptureStage NextCaptureStage(const Ia64PageFaultCaptureStage stage) noexcept
{
    switch (stage)
    {
    case Ia64PageFaultCaptureStage::PreCoverValidated:
        return Ia64PageFaultCaptureStage::InterruptionState;
    case Ia64PageFaultCaptureStage::InterruptionState:
        return Ia64PageFaultCaptureStage::IntegerState;
    case Ia64PageFaultCaptureStage::IntegerState:
        return Ia64PageFaultCaptureStage::ApplicationState;
    case Ia64PageFaultCaptureStage::ApplicationState:
        return Ia64PageFaultCaptureStage::FloatingState;
    case Ia64PageFaultCaptureStage::FloatingState:
        return Ia64PageFaultCaptureStage::RseTransition;
    case Ia64PageFaultCaptureStage::None:
    case Ia64PageFaultCaptureStage::RseTransition:
    case Ia64PageFaultCaptureStage::Decoded:
        return Ia64PageFaultCaptureStage::None;
    }
    return Ia64PageFaultCaptureStage::None;
}

[[nodiscard]] constexpr bool IsPowerOfTwo(const Foundation::UInt64 value) noexcept
{
    return (value != 0U) && ((value & (value - 1U)) == 0U);
}

[[nodiscard]] constexpr bool TryCalculateEnd(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes,
                                             Foundation::UInt64& end) noexcept
{
    if ((sizeBytes == 0U) || (start > (MaximumUInt64 - sizeBytes)))
    {
        return false;
    }
    end = start + sizeBytes;
    return true;
}

[[nodiscard]] constexpr bool IsRegionZeroRange(const Foundation::UInt64 start, const Foundation::UInt64 end,
                                               const Foundation::UInt64 alignment) noexcept
{
    return IsPowerOfTwo(alignment) && (start != 0U) && (start < end) && (end <= RegionZeroLimit) &&
           ((start & (alignment - 1U)) == 0U);
}

[[nodiscard]] constexpr bool HasExactSize(const Foundation::UInt64 start, const Foundation::UInt64 end,
                                          const Foundation::UInt64 expectedSizeBytes) noexcept
{
    return (start <= end) && ((end - start) == expectedSizeBytes);
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] Foundation::UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
}

[[nodiscard]] constexpr bool
IsInterruptedRseRangeValid(const Ia64PageFaultEntryControlConfiguration& configuration) noexcept
{
    if ((configuration.InterruptedRseBackingStoreStart == 0U) ||
        (configuration.InterruptedRseBackingStoreStart >= configuration.InterruptedRseBackingStoreEnd) ||
        ((configuration.InterruptedRseBackingStoreStart & (Ia64PageFaultStoragePageSizeBytes - 1U)) != 0U))
    {
        return false;
    }
    const Foundation::UInt64 sizeBytes =
        configuration.InterruptedRseBackingStoreEnd - configuration.InterruptedRseBackingStoreStart;
    return (sizeBytes >= 16U * 1024U) && (sizeBytes <= 64U * 1024U) &&
           ((sizeBytes & (Ia64PageFaultStoragePageSizeBytes - 1U)) == 0U);
}

[[nodiscard]] constexpr bool IsControlConfigurationValid(const Ia64PageFaultEntryControlConfiguration& configuration,
                                                         const Foundation::UInt64 controlAddress) noexcept
{
    Foundation::UInt64 frameEnd{0U};
    Foundation::UInt64 controlEnd{0U};
    if ((configuration.ProcessorIndex >= Ia64PageFaultMaximumProcessorCount) ||
        !TryCalculateEnd(configuration.FrameAddress, sizeof(Ia64PageFaultFrame), frameEnd) ||
        !TryCalculateEnd(controlAddress, sizeof(Ia64PageFaultEntryControl), controlEnd) ||
        !IsRegionZeroRange(configuration.FrameAddress, frameEnd, alignof(Ia64PageFaultFrame)) ||
        !IsInterruptedRseRangeValid(configuration) ||
        !IsRegionZeroRange(configuration.CoordinatorMemoryStackStart, configuration.CoordinatorMemoryStackEnd,
                           Ia64PageFaultStoragePageSizeBytes) ||
        !HasExactSize(configuration.CoordinatorMemoryStackStart, configuration.CoordinatorMemoryStackEnd,
                      Ia64PageFaultCoordinatorStackSizeBytes) ||
        !IsRegionZeroRange(configuration.CoordinatorRseBackingStoreStart, configuration.CoordinatorRseBackingStoreEnd,
                           Ia64PageFaultStoragePageSizeBytes) ||
        !HasExactSize(configuration.CoordinatorRseBackingStoreStart, configuration.CoordinatorRseBackingStoreEnd,
                      Ia64PageFaultCoordinatorRseBackingStoreSizeBytes) ||
        !IsRegionZeroRange(controlAddress, controlEnd, alignof(Ia64PageFaultEntryControl)) ||
        (configuration.CoordinatorEntryPoint == 0U) || ((configuration.CoordinatorEntryPoint & 15U) != 0U) ||
        (configuration.CoordinatorEntryPoint >= RegionZeroLimit) ||
        (configuration.CoordinatorGlobalPointer >= RegionZeroLimit) || (configuration.CoordinatorArgument == 0U) ||
        (configuration.CoordinatorArgument >= RegionZeroLimit))
    {
        return false;
    }

    const Foundation::UInt64 starts[5U]{
        controlAddress,
        configuration.FrameAddress,
        configuration.InterruptedRseBackingStoreStart,
        configuration.CoordinatorMemoryStackStart,
        configuration.CoordinatorRseBackingStoreStart,
    };
    const Foundation::UInt64 ends[5U]{
        controlEnd,
        frameEnd,
        configuration.InterruptedRseBackingStoreEnd,
        configuration.CoordinatorMemoryStackEnd,
        configuration.CoordinatorRseBackingStoreEnd,
    };
    for (Foundation::Size first{0U}; first < 5U; ++first)
    {
        for (Foundation::Size second{first + 1U}; second < 5U; ++second)
        {
            if (RangesOverlap(starts[first], ends[first], starts[second], ends[second]))
            {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] constexpr Ia64PageFaultEntryControlConfiguration
ReadControlConfiguration(const Ia64PageFaultEntryControl& control) noexcept
{
    return Ia64PageFaultEntryControlConfiguration{
        .ProcessorIndex = control.ProcessorIndex,
        .FrameAddress = control.FrameAddress,
        .InterruptedRseBackingStoreStart = control.InterruptedRseBackingStoreStart,
        .InterruptedRseBackingStoreEnd = control.InterruptedRseBackingStoreEnd,
        .CoordinatorMemoryStackStart = control.CoordinatorMemoryStackStart,
        .CoordinatorMemoryStackEnd = control.CoordinatorMemoryStackEnd,
        .CoordinatorRseBackingStoreStart = control.CoordinatorRseBackingStoreStart,
        .CoordinatorRseBackingStoreEnd = control.CoordinatorRseBackingStoreEnd,
        .CoordinatorEntryPoint = control.CoordinatorEntryPoint,
        .CoordinatorGlobalPointer = control.CoordinatorGlobalPointer,
        .CoordinatorArgument = control.CoordinatorArgument,
    };
}

[[nodiscard]] bool ControlMatchesFrame(const Ia64PageFaultEntryControl& control,
                                       const Ia64PageFaultFrame& frame) noexcept
{
    return control.FrameAddress == AddressOf(&frame);
}

[[nodiscard]] Ia64PageFaultEntryControlError FaultControl(Ia64PageFaultEntryControl& control,
                                                          const Ia64PageFaultEntryControlError error) noexcept
{
    control.LastFailure = static_cast<Foundation::UInt64>(error);
    __atomic_store_n(&control.State, static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Faulted),
                     __ATOMIC_RELEASE);
    return error;
}

[[nodiscard]] constexpr bool RsePointerIsInside(const Foundation::UInt64 pointer, const Foundation::UInt64 start,
                                                const Foundation::UInt64 end) noexcept
{
    return IsRseRegisterSlotPointer(pointer) && (pointer >= start) && (pointer < end);
}

[[nodiscard]] constexpr bool RseConvergencePointerIsInside(const Foundation::UInt64 pointer,
                                                           const Foundation::UInt64 start,
                                                           const Foundation::UInt64 end) noexcept
{
    return IsRseRegisterSlotPointer(pointer) && (pointer >= start) && (pointer <= end);
}

[[nodiscard]] bool CapturedRseMatchesControl(const Ia64PageFaultEntryControl& control,
                                             const Ia64PageFaultFrame& frame) noexcept
{
    return RsePointerIsInside(frame.RseBackingStorePointer, control.InterruptedRseBackingStoreStart,
                              control.InterruptedRseBackingStoreEnd) &&
           RsePointerIsInside(frame.RseBackingStoreStorePointer, control.InterruptedRseBackingStoreStart,
                              control.InterruptedRseBackingStoreEnd) &&
           RseConvergencePointerIsInside(frame.CoveredRseBackingStorePointer, control.InterruptedRseBackingStoreStart,
                                         control.InterruptedRseBackingStoreEnd);
}

[[nodiscard]] constexpr bool IsPostClaimControlState(const Ia64PageFaultEntryControlState state) noexcept
{
    return (state == Ia64PageFaultEntryControlState::Capturing) ||
           (state == Ia64PageFaultEntryControlState::Coordinator) ||
           (state == Ia64PageFaultEntryControlState::ResumeCommitted);
}

[[nodiscard]] constexpr Ia64PageFaultEntryControlState DecodeControlState(const Foundation::UInt64 rawState) noexcept
{
    switch (rawState)
    {
    case static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Fresh):
        return Ia64PageFaultEntryControlState::Fresh;
    case static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Configured):
        return Ia64PageFaultEntryControlState::Configured;
    case static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Claiming):
        return Ia64PageFaultEntryControlState::Claiming;
    case static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Capturing):
        return Ia64PageFaultEntryControlState::Capturing;
    case static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Coordinator):
        return Ia64PageFaultEntryControlState::Coordinator;
    case static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::ResumeCommitted):
        return Ia64PageFaultEntryControlState::ResumeCommitted;
    case static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Complete):
        return Ia64PageFaultEntryControlState::Complete;
    case static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Faulted):
        return Ia64PageFaultEntryControlState::Faulted;
    case static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Invalid):
        return Ia64PageFaultEntryControlState::Invalid;
    }
    return Ia64PageFaultEntryControlState::Invalid;
}

[[nodiscard]] Ia64PageFaultEntryControlError
RejectUnexpectedControlState(Ia64PageFaultEntryControl& control,
                             const Ia64PageFaultEntryControlState observedState) noexcept
{
    if ((observedState == Ia64PageFaultEntryControlState::Invalid) || IsPostClaimControlState(observedState))
    {
        return FaultControl(control, Ia64PageFaultEntryControlError::InvalidState);
    }
    return Ia64PageFaultEntryControlError::InvalidState;
}

[[nodiscard]] Ia64PageFaultEntryControlError ValidateResumeFrameCorrelation(const Ia64PageFaultEntryControl& control,
                                                                            const Ia64PageFaultFrame& frame) noexcept
{
    if (frame.CaptureStage != Ia64PageFaultCaptureStage::Decoded)
    {
        return Ia64PageFaultEntryControlError::FrameStateMismatch;
    }
    if (frame.VectorOffset != control.CapturedVectorOffset)
    {
        return Ia64PageFaultEntryControlError::VectorMismatch;
    }
    if (ValidateIa64PageFaultCoveredRseState(frame) != Ia64PageFaultFrameError::None)
    {
        return Ia64PageFaultEntryControlError::FrameStateMismatch;
    }
    if (!CapturedRseMatchesControl(control, frame))
    {
        return Ia64PageFaultEntryControlError::RsePointerOutsideInterruptedRange;
    }
    return Ia64PageFaultEntryControlError::None;
}

[[nodiscard]] constexpr Ia64PageFaultEntryContractError
ValidatePreCover(const Ia64PageFaultPreCoverObservation& observation, const VectorSet set) noexcept
{
    Memory::Ia64PageFaultVector vector{Memory::Ia64PageFaultVector::PageNotPresent};
    if (!TryGetVector(observation.VectorOffset, set, vector))
    {
        return Ia64PageFaultEntryContractError::UnsupportedInterruptionVector;
    }
    if ((observation.InterruptionStatus & NestedInterruptionBit) != 0U)
    {
        return Ia64PageFaultEntryContractError::NestedInterruption;
    }
    if ((observation.InterruptionStatus & IncompleteRegisterFrameBit) != 0U)
    {
        return Ia64PageFaultEntryContractError::UnsupportedAccessState;
    }
    return Ia64PageFaultEntryContractError::None;
}
} // namespace

bool Ia64PageFaultEntryContract::IsCoveredFunctionStateValid(const Foundation::UInt64 functionState) noexcept
{
    if ((functionState & ~(IfsValidBit | CurrentFrameMask)) != 0U || (functionState & IfsValidBit) == 0U)
    {
        return false;
    }

    const Foundation::UInt64 sizeOfFrame = functionState & 0x7FU;
    const Foundation::UInt64 sizeOfLocals = (functionState >> 7U) & 0x7FU;
    const Foundation::UInt64 sizeOfRotating = ((functionState >> 14U) & 0xFU) * 8U;
    const Foundation::UInt64 rotatingGeneralRegisterBase = (functionState >> 18U) & 0x7FU;
    const Foundation::UInt64 rotatingFloatingRegisterBase = (functionState >> 25U) & 0x7FU;
    const Foundation::UInt64 rotatingPredicateBase = (functionState >> 32U) & 0x3FU;

    return (sizeOfFrame <= 96U) && (sizeOfLocals <= sizeOfFrame) && (sizeOfRotating <= sizeOfFrame) &&
           ((sizeOfRotating == 0U) ? (rotatingGeneralRegisterBase == 0U)
                                   : (rotatingGeneralRegisterBase < sizeOfRotating)) &&
           (rotatingFloatingRegisterBase < 96U) && (rotatingPredicateBase < 48U);
}

Ia64PageFaultEntryContractError
Ia64PageFaultEntryContract::ValidateBeforeCover(const Ia64PageFaultPreCoverObservation& observation) noexcept
{
    return ValidatePreCover(observation, VectorSet::Heavyweight);
}

Ia64PageFaultEntryContractError
Ia64PageFaultEntryContract::ValidateAndDecode(const Ia64PageFaultEntryObservation& observation,
                                              Ia64PageFaultEntryOutput& output) noexcept
{
    const Ia64PageFaultEntryContractError preCoverError = ValidatePreCover(
        Ia64PageFaultPreCoverObservation{
            .InterruptionStatus = observation.InterruptionStatus,
            .VectorOffset = observation.VectorOffset,
        },
        VectorSet::Decodable);
    if (preCoverError != Ia64PageFaultEntryContractError::None)
    {
        return preCoverError;
    }

    Memory::Ia64PageFaultVector vector{Memory::Ia64PageFaultVector::PageNotPresent};
    if (!TryGetVector(observation.VectorOffset, VectorSet::Decodable, vector))
    {
        return Ia64PageFaultEntryContractError::DecoderContractViolation;
    }
    if ((observation.InterruptionProcessorStatus & InstructionSetBit) != 0U)
    {
        return Ia64PageFaultEntryContractError::UnsupportedInstructionSet;
    }
    if ((observation.InterruptionProcessorStatus & PrivilegeMask) != UserPrivilege)
    {
        return Ia64PageFaultEntryContractError::UnsupportedPrivilegeLevel;
    }
    if ((observation.InterruptionProcessorStatus & RegisterBankBit) == 0U)
    {
        return Ia64PageFaultEntryContractError::UnsupportedRegisterBank;
    }
    if ((observation.InterruptionProcessorStatus & RequiredTranslationBits) != RequiredTranslationBits)
    {
        return Ia64PageFaultEntryContractError::UnsupportedTranslationState;
    }
    if ((observation.InterruptionProcessorStatus & InterruptionCollectionBit) == 0U)
    {
        return Ia64PageFaultEntryContractError::InterruptionCollectionDisabled;
    }
    if ((vector == Memory::Ia64PageFaultVector::KeyPermission) &&
        ((observation.InterruptionProcessorStatus & ProtectionKeyBit) == 0U))
    {
        return Ia64PageFaultEntryContractError::ProtectionKeyDisabled;
    }
    if ((observation.InstructionPointer & BundleAlignmentMask) != 0U)
    {
        return Ia64PageFaultEntryContractError::InvalidInstructionPointer;
    }

    const Foundation::UInt64 restartInstructionSlot =
        (observation.InterruptionProcessorStatus & RestartInstructionSlotMask) >> RestartInstructionSlotShift;
    if (restartInstructionSlot == 3U)
    {
        return Ia64PageFaultEntryContractError::InvalidInstructionSlot;
    }
    if ((observation.InterruptionStatus & PostCoverUnsupportedAccessStateBits) != 0U)
    {
        return Ia64PageFaultEntryContractError::UnsupportedAccessState;
    }
    if ((observation.InterruptionStatus & InterruptionCodeMask) != 0U)
    {
        return Ia64PageFaultEntryContractError::UnsupportedInterruptionCode;
    }

    const Foundation::UInt64 exceptionInstructionSlot =
        (observation.InterruptionStatus & ExceptionInstructionSlotMask) >> ExceptionInstructionSlotShift;
    if (exceptionInstructionSlot != restartInstructionSlot)
    {
        return Ia64PageFaultEntryContractError::InterruptionSlotMismatch;
    }
    if (!Ia64PageFaultEntryContract::IsCoveredFunctionStateValid(observation.CoveredInterruptionFunctionState))
    {
        return Ia64PageFaultEntryContractError::InvalidCoveredInterruptionFunctionState;
    }

    Kernel::Memory::FaultRecord decodedFault{};
    const Memory::Ia64PageFaultDecodeError decodeError = Memory::Ia64PageFaultDecoder::Decode(
        Memory::Ia64PageFaultState{
            .FaultAddress = observation.FaultAddress,
            .InstructionPointer = observation.InstructionPointer,
            .InterruptionStatus = observation.InterruptionStatus,
            .InterruptionProcessorStatus = observation.InterruptionProcessorStatus,
            .Vector = vector,
        },
        decodedFault);
    if (decodeError != Memory::Ia64PageFaultDecodeError::None)
    {
        return MapDecodeError(decodeError);
    }

    output = Ia64PageFaultEntryOutput{
        .Fault = decodedFault,
        .ResumeTarget =
            Ia64PageFaultResumeTarget{
                .InstructionPointer = observation.InstructionPointer,
                .InterruptionProcessorStatus = observation.InterruptionProcessorStatus,
                .CoveredInterruptionFunctionState = observation.CoveredInterruptionFunctionState,
            },
    };
    return Ia64PageFaultEntryContractError::None;
}

Ia64PageFaultFrameError BeginIa64PageFaultCapture(Ia64PageFaultFrame& frame,
                                                  const Ia64PageFaultPreCoverObservation& observation,
                                                  Foundation::UInt64& captureToken) noexcept
{
    if (TokenOutputAliasesFrame(frame, captureToken))
    {
        return Ia64PageFaultFrameError::TokenOutputAliasesFrame;
    }
    if (Ia64PageFaultEntryContract::ValidateBeforeCover(observation) != Ia64PageFaultEntryContractError::None)
    {
        return Ia64PageFaultFrameError::InvalidPreCoverObservation;
    }
    if ((__atomic_load_n(&frame.PublishedToken, __ATOMIC_ACQUIRE) != 0U) ||
        (frame.State != Ia64PageFaultFrameState::Inactive))
    {
        return Ia64PageFaultFrameError::AlreadyOwned;
    }
    Foundation::UInt64 selectedToken{frame.NextToken};
    if (selectedToken == 0U)
    {
        if (!Ia64PageFaultFrameIsPristine(frame))
        {
            return Ia64PageFaultFrameError::TokenExhausted;
        }
        selectedToken = 1U;
    }

    frame.CaptureToken = selectedToken;
    frame.NextToken = selectedToken == MaximumUInt64 ? 0U : selectedToken + 1U;
    frame.State = Ia64PageFaultFrameState::Capturing;
    frame.CaptureStage = Ia64PageFaultCaptureStage::PreCoverValidated;
    frame.PreCoverInterruptionStatus = observation.InterruptionStatus;
    frame.VectorOffset = observation.VectorOffset;
    captureToken = selectedToken;
    return Ia64PageFaultFrameError::None;
}

Ia64PageFaultFrameError AdvanceIa64PageFaultCapture(Ia64PageFaultFrame& frame, const Foundation::UInt64 captureToken,
                                                    const Ia64PageFaultCaptureStage completedStage) noexcept
{
    if (frame.State != Ia64PageFaultFrameState::Capturing)
    {
        return Ia64PageFaultFrameError::InvalidState;
    }
    if ((captureToken == 0U) || (captureToken != frame.CaptureToken))
    {
        return Ia64PageFaultFrameError::TokenMismatch;
    }
    const Ia64PageFaultCaptureStage nextStage = NextCaptureStage(frame.CaptureStage);
    if ((nextStage == Ia64PageFaultCaptureStage::None) || (completedStage != nextStage))
    {
        return Ia64PageFaultFrameError::InvalidCaptureStage;
    }

    frame.CaptureStage = completedStage;
    return Ia64PageFaultFrameError::None;
}

Ia64PageFaultFrameError ValidateIa64PageFaultCoveredRseState(const Ia64PageFaultFrame& frame) noexcept
{
    if (!Ia64PageFaultEntryContract::IsCoveredFunctionStateValid(frame.CoveredInterruptionFunctionState))
    {
        return Ia64PageFaultFrameError::InvalidRseFunctionState;
    }

    const Foundation::UInt64 sizeOfFrame = frame.CoveredInterruptionFunctionState & 0x7FU;
    Foundation::UInt64 expectedCoveredPointer{0U};
    if (!TryAdvanceRsePointer(frame.RseBackingStorePointer, sizeOfFrame, expectedCoveredPointer) ||
        !IsRseRegisterSlotPointer(frame.RseBackingStoreStorePointer) ||
        !IsRseRegisterSlotPointer(frame.CoveredRseBackingStorePointer) ||
        (frame.RseBackingStoreStorePointer > frame.RseBackingStorePointer))
    {
        return Ia64PageFaultFrameError::InvalidRsePointer;
    }
    if (frame.CoveredRseBackingStorePointer != expectedCoveredPointer)
    {
        return Ia64PageFaultFrameError::CoveredRsePointerMismatch;
    }
    Foundation::UInt64 dirtyRegisterCount{0U};
    if ((frame.RseBackingStoreStorePointer > frame.CoveredRseBackingStorePointer) ||
        ((frame.CoveredRseBackingStorePointer - frame.RseBackingStoreStorePointer) >=
         Ia64PageFaultMaximumRseLoadSizeBytes) ||
        !TryCountRseRegistersBetween(frame.RseBackingStoreStorePointer, frame.CoveredRseBackingStorePointer,
                                     dirtyRegisterCount))
    {
        return Ia64PageFaultFrameError::InvalidRsePointer;
    }

    const Foundation::UInt64 encoded = frame.RseDirtyPartitionLoadConfiguration;
    const Foundation::UInt64 loadBytes = encoded >> 16U;
    Foundation::UInt64 normalizedDirtyPartitionBytes{0U};
    if (((encoded & 0xFFFFU) != 0U) || ((loadBytes & 7U) != 0U) ||
        (loadBytes >= Ia64PageFaultMaximumRseLoadSizeBytes) ||
        !TryCalculatePageAlignedRseSpan(dirtyRegisterCount, normalizedDirtyPartitionBytes) ||
        (loadBytes != normalizedDirtyPartitionBytes))
    {
        return Ia64PageFaultFrameError::InvalidRseLoadConfiguration;
    }
    return Ia64PageFaultFrameError::None;
}

Ia64PageFaultFrameError CompleteIa64PageFaultCapture(Ia64PageFaultFrame& frame,
                                                     const Foundation::UInt64 captureToken) noexcept
{
    if (frame.State != Ia64PageFaultFrameState::Capturing)
    {
        return Ia64PageFaultFrameError::InvalidState;
    }
    if ((captureToken == 0U) || (captureToken != frame.CaptureToken))
    {
        return Ia64PageFaultFrameError::TokenMismatch;
    }
    if (frame.CaptureStage != Ia64PageFaultCaptureStage::RseTransition)
    {
        return Ia64PageFaultFrameError::InvalidCaptureStage;
    }
    const Ia64PageFaultFrameError rseError = ValidateIa64PageFaultCoveredRseState(frame);
    if (rseError != Ia64PageFaultFrameError::None)
    {
        return rseError;
    }

    Ia64PageFaultEntryOutput decoded{};
    const Ia64PageFaultFrameError decodeError = DecodeFrameEntry(frame, decoded);
    if (decodeError != Ia64PageFaultFrameError::None)
    {
        return decodeError;
    }

    frame.CapturedEntry = decoded;
    frame.CaptureStage = Ia64PageFaultCaptureStage::Decoded;
    return Ia64PageFaultFrameError::None;
}

Ia64PageFaultFrameError PublishIa64PageFaultCapture(Ia64PageFaultFrame& frame,
                                                    const Foundation::UInt64 captureToken) noexcept
{
    if (frame.State != Ia64PageFaultFrameState::Capturing)
    {
        return Ia64PageFaultFrameError::InvalidState;
    }
    if ((captureToken == 0U) || (captureToken != frame.CaptureToken))
    {
        return Ia64PageFaultFrameError::TokenMismatch;
    }
    if (frame.CaptureStage != Ia64PageFaultCaptureStage::Decoded)
    {
        return Ia64PageFaultFrameError::InvalidCaptureStage;
    }

    const Ia64PageFaultFrameError rseError = ValidateIa64PageFaultCoveredRseState(frame);
    if (rseError != Ia64PageFaultFrameError::None)
    {
        return rseError;
    }
    Ia64PageFaultEntryOutput decoded{};
    const Ia64PageFaultFrameError decodeError = DecodeFrameEntry(frame, decoded);
    if (decodeError != Ia64PageFaultFrameError::None)
    {
        return decodeError;
    }
    if (!EntryOutputsAreEqual(frame.CapturedEntry, decoded))
    {
        return Ia64PageFaultFrameError::CapturedEntryMismatch;
    }

    frame.State = Ia64PageFaultFrameState::Retained;
    __atomic_store_n(&frame.PublishedToken, captureToken, __ATOMIC_RELEASE);
    return Ia64PageFaultFrameError::None;
}

Foundation::UInt64 AcquireIa64PageFaultToken(const Ia64PageFaultFrame& frame) noexcept
{
    return __atomic_load_n(&frame.PublishedToken, __ATOMIC_ACQUIRE);
}

bool Ia64PageFaultFrameIsPristine(const Ia64PageFaultFrame& frame) noexcept
{
    const Ia64PageFaultEntryOutput pristineOutput{};
    if ((AcquireIa64PageFaultToken(frame) != 0U) || (frame.CaptureToken != 0U) || (frame.NextToken != 0U) ||
        (frame.State != Ia64PageFaultFrameState::Inactive) || (frame.CaptureStage != Ia64PageFaultCaptureStage::None) ||
        (frame.PreCoverInterruptionStatus != 0U) || (frame.VectorOffset != 0U) ||
        (frame.CurrentProcessorStatus != 0U) || (frame.InterruptedInstructionPointer != 0U) ||
        (frame.InterruptedProcessorStatus != 0U) || (frame.HardwareInterruptionFunctionState != 0U) ||
        (frame.CoveredInterruptionFunctionState != 0U) || (frame.InterruptionStatus != 0U) ||
        (frame.InterruptionFaultingAddress != 0U) || (frame.InterruptionTranslationInformation != 0U) ||
        (frame.InterruptionPreviousInstructionPointer != 0U) || (frame.InterruptionImmediate != 0U) ||
        (frame.PreviousFunctionState != 0U) || (frame.RseConfiguration != 0U) || (frame.RseBackingStorePointer != 0U) ||
        (frame.RseBackingStoreStorePointer != 0U) || (frame.RseNatCollection != 0U) ||
        (frame.RseDirtyPartitionLoadConfiguration != 0U) || (frame.OriginalNatCollection != 0U) ||
        (frame.StaticGeneralRegisterSpillNatCollection != 0U) ||
        (frame.BankedGeneralRegisterSpillNatCollection != 0U) || (frame.LoopCounter != 0U) ||
        (frame.EpilogCounter != 0U) || (frame.CompareAndStoreData != 0U) || (frame.CompareExchangeValue != 0U) ||
        (frame.FloatingPointStatus != 0U) || (frame.Predicates != 0U) || (frame.CoveredRseBackingStorePointer != 0U) ||
        !EntryOutputsAreEqual(frame.CapturedEntry, pristineOutput))
    {
        return false;
    }
    for (const Foundation::UInt64 value : frame.BranchRegisters)
    {
        if (value != 0U)
        {
            return false;
        }
    }
    for (const Foundation::UInt64 value : frame.GeneralRegisters)
    {
        if (value != 0U)
        {
            return false;
        }
    }
    for (const Ia64PageFaultFloatingRegister& value : frame.FloatingRegisters)
    {
        if ((value.Low != 0U) || (value.High != 0U))
        {
            return false;
        }
    }
    return true;
}

Ia64PageFaultFrameError ReadIa64PageFaultCapture(const Ia64PageFaultFrame& frame,
                                                 const Foundation::UInt64 publishedToken,
                                                 Ia64PageFaultEntryOutput& output) noexcept
{
    if ((publishedToken == 0U) || (AcquireIa64PageFaultToken(frame) != publishedToken) ||
        (frame.CaptureToken != publishedToken))
    {
        return Ia64PageFaultFrameError::TokenMismatch;
    }
    if ((frame.State != Ia64PageFaultFrameState::Retained) ||
        (frame.CaptureStage != Ia64PageFaultCaptureStage::Decoded))
    {
        return Ia64PageFaultFrameError::NotRetained;
    }

    output = frame.CapturedEntry;
    return Ia64PageFaultFrameError::None;
}

Ia64PageFaultFrameError CommitIa64PageFaultResume(Ia64PageFaultFrame& frame,
                                                  const Foundation::UInt64 publishedToken) noexcept
{
    if ((publishedToken == 0U) || (AcquireIa64PageFaultToken(frame) != publishedToken) ||
        (frame.CaptureToken != publishedToken))
    {
        return Ia64PageFaultFrameError::TokenMismatch;
    }
    if (frame.State != Ia64PageFaultFrameState::Retained)
    {
        return Ia64PageFaultFrameError::NotRetained;
    }
    if (frame.CaptureStage != Ia64PageFaultCaptureStage::Decoded)
    {
        return Ia64PageFaultFrameError::InvalidCaptureStage;
    }

    frame.State = Ia64PageFaultFrameState::ResumeCommitted;
    return Ia64PageFaultFrameError::None;
}

Ia64PageFaultFrameError ModelReleaseIa64PageFaultFrameForRfi(Ia64PageFaultFrame& frame,
                                                             const Foundation::UInt64 publishedToken) noexcept
{
    if ((publishedToken == 0U) || (AcquireIa64PageFaultToken(frame) != publishedToken) ||
        (frame.CaptureToken != publishedToken))
    {
        return Ia64PageFaultFrameError::TokenMismatch;
    }
    if (frame.State != Ia64PageFaultFrameState::ResumeCommitted)
    {
        return Ia64PageFaultFrameError::NotCommitted;
    }
    if (frame.CaptureStage != Ia64PageFaultCaptureStage::Decoded)
    {
        return Ia64PageFaultFrameError::InvalidCaptureStage;
    }

    frame.CaptureStage = Ia64PageFaultCaptureStage::None;
    frame.State = Ia64PageFaultFrameState::Inactive;
    __atomic_store_n(&frame.PublishedToken, 0U, __ATOMIC_RELEASE);
    return Ia64PageFaultFrameError::None;
}

Ia64PageFaultEntryControlError
ValidateConfiguredIa64PageFaultEntryControl(const Ia64PageFaultEntryControl& control) noexcept
{
    if (AcquireIa64PageFaultEntryControlState(control) != Ia64PageFaultEntryControlState::Configured)
    {
        return Ia64PageFaultEntryControlError::InvalidState;
    }
    return IsControlConfigurationValid(ReadControlConfiguration(control), AddressOf(&control))
               ? Ia64PageFaultEntryControlError::None
               : Ia64PageFaultEntryControlError::InvalidConfiguration;
}

Ia64PageFaultEntryControlError
ConfigureIa64PageFaultEntryControl(Ia64PageFaultEntryControl& control,
                                   const Ia64PageFaultEntryControlConfiguration& configuration) noexcept
{
    const Ia64PageFaultEntryControlState observedState = AcquireIa64PageFaultEntryControlState(control);
    if ((observedState == Ia64PageFaultEntryControlState::Invalid) || IsPostClaimControlState(observedState))
    {
        return FaultControl(control, Ia64PageFaultEntryControlError::InvalidState);
    }
    if (observedState != Ia64PageFaultEntryControlState::Fresh)
    {
        return Ia64PageFaultEntryControlError::AlreadyConfigured;
    }
    const Foundation::UInt64 controlAddress = AddressOf(&control);
    if (!IsControlConfigurationValid(configuration, controlAddress))
    {
        return Ia64PageFaultEntryControlError::InvalidConfiguration;
    }

    control.ProcessorIndex = configuration.ProcessorIndex;
    control.FrameAddress = configuration.FrameAddress;
    control.InterruptedRseBackingStoreStart = configuration.InterruptedRseBackingStoreStart;
    control.InterruptedRseBackingStoreEnd = configuration.InterruptedRseBackingStoreEnd;
    control.CoordinatorMemoryStackStart = configuration.CoordinatorMemoryStackStart;
    control.CoordinatorMemoryStackEnd = configuration.CoordinatorMemoryStackEnd;
    control.CoordinatorRseBackingStoreStart = configuration.CoordinatorRseBackingStoreStart;
    control.CoordinatorRseBackingStoreEnd = configuration.CoordinatorRseBackingStoreEnd;
    control.CoordinatorEntryPoint = configuration.CoordinatorEntryPoint;
    control.CoordinatorGlobalPointer = configuration.CoordinatorGlobalPointer;
    control.CoordinatorArgument = configuration.CoordinatorArgument;
    control.EntryCaptureToken = 0U;
    control.NextEntryCaptureToken = 1U;
    control.CapturedFrameToken = 0U;
    control.CapturedVectorOffset = 0U;
    control.ResumeGeneration = 0U;
    control.CaptureCount = 0U;
    control.RestoreCount = 0U;
    control.LastFailure = 0U;
    __atomic_store_n(&control.State, static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Configured),
                     __ATOMIC_RELEASE);
    return Ia64PageFaultEntryControlError::None;
}

Ia64PageFaultEntryControlError
ClaimIa64PageFaultEntryControlCapture(Ia64PageFaultEntryControl& control, Ia64PageFaultFrame& frame,
                                      const Ia64PageFaultPreCoverObservation& observation) noexcept
{
    const Ia64PageFaultEntryControlState observedState = AcquireIa64PageFaultEntryControlState(control);
    if (observedState != Ia64PageFaultEntryControlState::Configured)
    {
        return RejectUnexpectedControlState(control, observedState);
    }
    if (Ia64PageFaultEntryContract::ValidateBeforeCover(observation) != Ia64PageFaultEntryContractError::None)
    {
        return Ia64PageFaultEntryControlError::InvalidPreCoverObservation;
    }
    if (!ControlMatchesFrame(control, frame) ||
        !IsControlConfigurationValid(ReadControlConfiguration(control), AddressOf(&control)))
    {
        return Ia64PageFaultEntryControlError::InvalidConfiguration;
    }
    if (Ia64PageFaultFrameIsOwned(frame))
    {
        return Ia64PageFaultEntryControlError::FrameOwned;
    }
    if (control.NextEntryCaptureToken == 0U)
    {
        return Ia64PageFaultEntryControlError::EntryCaptureTokenExhausted;
    }
    if (!Ia64PageFaultFrameIsPristine(frame))
    {
        return Ia64PageFaultEntryControlError::FrameCaptureRejected;
    }

    Foundation::UInt64 expected = static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Configured);
    const Foundation::UInt64 desired = static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Claiming);
    if (!__atomic_compare_exchange_n(&control.State, &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        return RejectUnexpectedControlState(control, DecodeControlState(expected));
    }

    const Foundation::UInt64 entryCaptureToken = control.NextEntryCaptureToken;
    Foundation::UInt64 frameToken{0U};
    const Ia64PageFaultFrameError frameError = BeginIa64PageFaultCapture(frame, observation, frameToken);
    if (frameError != Ia64PageFaultFrameError::None)
    {
        return FaultControl(control, Ia64PageFaultEntryControlError::FrameCaptureRejected);
    }

    control.EntryCaptureToken = entryCaptureToken;
    control.NextEntryCaptureToken = entryCaptureToken == MaximumUInt64 ? 0U : entryCaptureToken + 1U;
    control.CapturedFrameToken = frameToken;
    control.CapturedVectorOffset = observation.VectorOffset;
    __atomic_store_n(&control.State, static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Capturing),
                     __ATOMIC_RELEASE);
    return Ia64PageFaultEntryControlError::None;
}

Ia64PageFaultEntryControlError
ModelPublishIa64PageFaultEntryControlCapture(Ia64PageFaultEntryControl& control, const Ia64PageFaultFrame& frame,
                                             const Foundation::UInt64 entryCaptureToken) noexcept
{
    const Ia64PageFaultEntryControlState observedState = AcquireIa64PageFaultEntryControlState(control);
    if (observedState != Ia64PageFaultEntryControlState::Capturing)
    {
        return RejectUnexpectedControlState(control, observedState);
    }
    if ((entryCaptureToken == 0U) || (entryCaptureToken != control.EntryCaptureToken))
    {
        return FaultControl(control, Ia64PageFaultEntryControlError::EntryCaptureTokenMismatch);
    }
    if (!ControlMatchesFrame(control, frame) ||
        !IsControlConfigurationValid(ReadControlConfiguration(control), AddressOf(&control)))
    {
        return FaultControl(control, Ia64PageFaultEntryControlError::InvalidConfiguration);
    }
    const Foundation::UInt64 frameToken = AcquireIa64PageFaultToken(frame);
    if ((frameToken == 0U) || (control.CapturedFrameToken != frameToken) || (frame.CaptureToken != frameToken) ||
        (frame.State != Ia64PageFaultFrameState::Retained) ||
        (frame.CaptureStage != Ia64PageFaultCaptureStage::Decoded))
    {
        return FaultControl(control, Ia64PageFaultEntryControlError::FrameStateMismatch);
    }
    if (frame.VectorOffset != control.CapturedVectorOffset)
    {
        return FaultControl(control, Ia64PageFaultEntryControlError::VectorMismatch);
    }
    if ((ValidateIa64PageFaultCoveredRseState(frame) != Ia64PageFaultFrameError::None) ||
        !CapturedRseMatchesControl(control, frame))
    {
        return FaultControl(control, Ia64PageFaultEntryControlError::RsePointerOutsideInterruptedRange);
    }
    if (control.CaptureCount != 0U)
    {
        return FaultControl(control, Ia64PageFaultEntryControlError::FrameStateMismatch);
    }

    control.CaptureCount = 1U;
    __atomic_store_n(&control.State, static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Coordinator),
                     __ATOMIC_RELEASE);
    return Ia64PageFaultEntryControlError::None;
}

Ia64PageFaultEntryControlError CommitIa64PageFaultEntryControlForResume(
    Ia64PageFaultEntryControl& control, const Ia64PageFaultFrame& frame, const Foundation::UInt64 entryCaptureToken,
    const Foundation::UInt64 frameToken, const Foundation::UInt64 resumeGeneration) noexcept
{
    const Ia64PageFaultEntryControlState observedState = AcquireIa64PageFaultEntryControlState(control);
    if (observedState != Ia64PageFaultEntryControlState::Coordinator)
    {
        return RejectUnexpectedControlState(control, observedState);
    }
    if ((entryCaptureToken == 0U) || (entryCaptureToken != control.EntryCaptureToken))
    {
        return FaultControl(control, Ia64PageFaultEntryControlError::EntryCaptureTokenMismatch);
    }
    if ((frameToken == 0U) || (frameToken != control.CapturedFrameToken) ||
        (AcquireIa64PageFaultToken(frame) != frameToken) || (frame.CaptureToken != frameToken) ||
        !ControlMatchesFrame(control, frame) || (frame.State != Ia64PageFaultFrameState::ResumeCommitted))
    {
        return FaultControl(control, Ia64PageFaultEntryControlError::FrameStateMismatch);
    }
    const Ia64PageFaultEntryControlError frameCorrelationError = ValidateResumeFrameCorrelation(control, frame);
    if (frameCorrelationError != Ia64PageFaultEntryControlError::None)
    {
        return FaultControl(control, frameCorrelationError);
    }
    if (resumeGeneration == 0U)
    {
        return FaultControl(control, Ia64PageFaultEntryControlError::ResumeGenerationMismatch);
    }

    control.ResumeGeneration = resumeGeneration;
    __atomic_store_n(&control.State, static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::ResumeCommitted),
                     __ATOMIC_RELEASE);
    return Ia64PageFaultEntryControlError::None;
}

Ia64PageFaultEntryControlError ModelCompleteIa64PageFaultEntryControlForRfi(
    Ia64PageFaultEntryControl& control, const Ia64PageFaultFrame& frame, const Foundation::UInt64 entryCaptureToken,
    const Foundation::UInt64 frameToken, const Foundation::UInt64 resumeGeneration) noexcept
{
    const Ia64PageFaultEntryControlState observedState = AcquireIa64PageFaultEntryControlState(control);
    if (observedState != Ia64PageFaultEntryControlState::ResumeCommitted)
    {
        return RejectUnexpectedControlState(control, observedState);
    }
    if ((entryCaptureToken == 0U) || (entryCaptureToken != control.EntryCaptureToken))
    {
        return FaultControl(control, Ia64PageFaultEntryControlError::EntryCaptureTokenMismatch);
    }
    if ((frameToken == 0U) || (frameToken != control.CapturedFrameToken) ||
        (AcquireIa64PageFaultToken(frame) != frameToken) || (frame.CaptureToken != frameToken) ||
        !ControlMatchesFrame(control, frame) || (frame.State != Ia64PageFaultFrameState::ResumeCommitted))
    {
        return FaultControl(control, Ia64PageFaultEntryControlError::FrameStateMismatch);
    }
    const Ia64PageFaultEntryControlError frameCorrelationError = ValidateResumeFrameCorrelation(control, frame);
    if (frameCorrelationError != Ia64PageFaultEntryControlError::None)
    {
        return FaultControl(control, frameCorrelationError);
    }
    if ((resumeGeneration == 0U) || (resumeGeneration != control.ResumeGeneration))
    {
        return FaultControl(control, Ia64PageFaultEntryControlError::ResumeGenerationMismatch);
    }
    if ((control.CaptureCount != 1U) || (control.RestoreCount != 0U))
    {
        return FaultControl(control, Ia64PageFaultEntryControlError::FrameStateMismatch);
    }

    control.RestoreCount = 1U;
    __atomic_store_n(&control.State, static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Complete),
                     __ATOMIC_RELEASE);
    return Ia64PageFaultEntryControlError::None;
}

Ia64PageFaultEntryControlState AcquireIa64PageFaultEntryControlState(const Ia64PageFaultEntryControl& control) noexcept
{
    return DecodeControlState(__atomic_load_n(&control.State, __ATOMIC_ACQUIRE));
}

Ia64PageFaultEntryControlSnapshot
AcquireIa64PageFaultEntryControlSnapshot(const Ia64PageFaultEntryControl& control) noexcept
{
    const Ia64PageFaultEntryControlState state = AcquireIa64PageFaultEntryControlState(control);
    Ia64PageFaultEntryControlSnapshot snapshot{};
    snapshot.State = state;
    switch (state)
    {
    case Ia64PageFaultEntryControlState::Faulted:
        snapshot.LastFailure = control.LastFailure;
        [[fallthrough]];
    case Ia64PageFaultEntryControlState::Complete:
        snapshot.RestoreCount = control.RestoreCount;
        [[fallthrough]];
    case Ia64PageFaultEntryControlState::ResumeCommitted:
        snapshot.ResumeGeneration = control.ResumeGeneration;
        [[fallthrough]];
    case Ia64PageFaultEntryControlState::Coordinator:
        snapshot.CaptureCount = control.CaptureCount;
        [[fallthrough]];
    case Ia64PageFaultEntryControlState::Capturing:
        snapshot.EntryCaptureToken = control.EntryCaptureToken;
        snapshot.NextEntryCaptureToken = control.NextEntryCaptureToken;
        snapshot.CapturedFrameToken = control.CapturedFrameToken;
        snapshot.CapturedVectorOffset = control.CapturedVectorOffset;
        [[fallthrough]];
    case Ia64PageFaultEntryControlState::Configured:
    case Ia64PageFaultEntryControlState::Claiming:
        snapshot.Configuration = ReadControlConfiguration(control);
        return snapshot;
    case Ia64PageFaultEntryControlState::Fresh:
    case Ia64PageFaultEntryControlState::Invalid:
        return snapshot;
    }
    return snapshot;
}
} // namespace Asteria::Platform::Ia64::Interrupt

// The target entry is in Ia64PageFaultEntryContract.S.
