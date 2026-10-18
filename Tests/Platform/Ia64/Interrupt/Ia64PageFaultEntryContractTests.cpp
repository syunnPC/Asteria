// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Kernel/Memory/FaultToken.hpp"
#include "Platform/Ia64/Interrupt/Ia64PageFaultEntryContract.hpp"

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
using Platform::Ia64::Interrupt::Ia64PageFaultEntryContract;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryContractError;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryControl;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryControlConfiguration;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryControlError;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryControlState;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryObservation;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryOutput;
using Platform::Ia64::Interrupt::Ia64PageFaultCaptureStage;
using Platform::Ia64::Interrupt::Ia64PageFaultFrame;
using Platform::Ia64::Interrupt::Ia64PageFaultFrameError;
using Platform::Ia64::Interrupt::Ia64PageFaultFrameIsOwned;
using Platform::Ia64::Interrupt::Ia64PageFaultFrameIsPristine;
using Platform::Ia64::Interrupt::Ia64PageFaultFrameState;
using Platform::Ia64::Interrupt::Ia64PageFaultPreCoverObservation;
using Platform::Ia64::Interrupt::AcquireIa64PageFaultToken;
using Platform::Ia64::Interrupt::AcquireIa64PageFaultEntryControlState;
using Platform::Ia64::Interrupt::AcquireIa64PageFaultEntryControlSnapshot;
using Platform::Ia64::Interrupt::AdvanceIa64PageFaultCapture;
using Platform::Ia64::Interrupt::BeginIa64PageFaultCapture;
using Platform::Ia64::Interrupt::ClaimIa64PageFaultEntryControlCapture;
using Platform::Ia64::Interrupt::CommitIa64PageFaultResume;
using Platform::Ia64::Interrupt::CommitIa64PageFaultEntryControlForResume;
using Platform::Ia64::Interrupt::CompleteIa64PageFaultCapture;
using Platform::Ia64::Interrupt::ConfigureIa64PageFaultEntryControl;
using Platform::Ia64::Interrupt::Ia64PageFaultCoordinatorRseBackingStoreSizeBytes;
using Platform::Ia64::Interrupt::Ia64PageFaultCoordinatorStackSizeBytes;
using Platform::Ia64::Interrupt::ModelCompleteIa64PageFaultEntryControlForRfi;
using Platform::Ia64::Interrupt::ModelPublishIa64PageFaultEntryControlCapture;
using Platform::Ia64::Interrupt::ModelReleaseIa64PageFaultFrameForRfi;
using Platform::Ia64::Interrupt::PublishIa64PageFaultCapture;
using Platform::Ia64::Interrupt::ReadIa64PageFaultCapture;
using Platform::Ia64::Interrupt::ValidateConfiguredIa64PageFaultEntryControl;
using Platform::Ia64::Interrupt::ValidateIa64PageFaultCoveredRseState;

constexpr Foundation::UInt64 DataTranslationBit{Foundation::UInt64{1U} << 17U};
constexpr Foundation::UInt64 RseTranslationBit{Foundation::UInt64{1U} << 27U};
constexpr Foundation::UInt64 InterruptionCollectionBit{Foundation::UInt64{1U} << 13U};
constexpr Foundation::UInt64 ProtectionKeyBit{Foundation::UInt64{1U} << 15U};
constexpr Foundation::UInt64 UserPrivilege{Foundation::UInt64{3U} << 32U};
constexpr Foundation::UInt64 InstructionSetBit{Foundation::UInt64{1U} << 34U};
constexpr Foundation::UInt64 InstructionTranslationBit{Foundation::UInt64{1U} << 36U};
constexpr Foundation::UInt64 RestartInstructionSlotShift{41U};
constexpr Foundation::UInt64 RegisterBankBit{Foundation::UInt64{1U} << 44U};
constexpr Foundation::UInt64 ExecuteBit{Foundation::UInt64{1U} << 32U};
constexpr Foundation::UInt64 WriteBit{Foundation::UInt64{1U} << 33U};
constexpr Foundation::UInt64 ReadBit{Foundation::UInt64{1U} << 34U};
constexpr Foundation::UInt64 ExceptionInstructionSlotShift{41U};
constexpr Foundation::UInt64 IfsValidBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 ValidCoveredFunctionState{IfsValidBit | Foundation::UInt64{4U} |
                                                       (Foundation::UInt64{2U} << 7U)};
constexpr Foundation::UInt64 ValidProcessorStatus{InterruptionCollectionBit | DataTranslationBit | RseTranslationBit |
                                                  UserPrivilege | InstructionTranslationBit | RegisterBankBit |
                                                  (Foundation::UInt64{2U} << RestartInstructionSlotShift)};
constexpr Foundation::UInt64 ValidInterruptionStatus{ReadBit |
                                                     (Foundation::UInt64{2U} << ExceptionInstructionSlotShift)};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 ValidRseBackingStoreStorePointer{0xA000'0000'0000'0100ULL};
constexpr Foundation::UInt64 ValidRseBackingStorePointer{0xA000'0000'0000'0180ULL};
constexpr Foundation::UInt64 ValidCoveredRseBackingStorePointer{0xA000'0000'0000'01A0ULL};
constexpr Foundation::UInt64 ValidRseLoadConfiguration{0xA0U << 16U};

struct alignas(4096U) PageFaultEntryArena
{
    Ia64PageFaultEntryControl Control;
    Ia64PageFaultFrame Frame;
    alignas(4096U) Foundation::UInt8 CoordinatorStack[Ia64PageFaultCoordinatorStackSizeBytes];
    alignas(4096U) Foundation::UInt8 CoordinatorRse[Ia64PageFaultCoordinatorRseBackingStoreSizeBytes];
};

PageFaultEntryArena g_pageFaultEntryArena{};
alignas(64U) Ia64PageFaultFrame g_unrelatedPageFaultFrame{};

[[nodiscard]] Foundation::UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
}

void ResetPageFaultEntryArena() noexcept
{
    g_pageFaultEntryArena.Control = {};
    g_pageFaultEntryArena.Frame = {};
}

[[nodiscard]] Ia64PageFaultEntryControlConfiguration ValidControlConfiguration() noexcept
{
    return Ia64PageFaultEntryControlConfiguration{
        .ProcessorIndex = 0U,
        .FrameAddress = AddressOf(&g_pageFaultEntryArena.Frame),
        .InterruptedRseBackingStoreStart = 0xA000'0000'0000'0000ULL,
        .InterruptedRseBackingStoreEnd = 0xA000'0000'0001'0000ULL,
        .CoordinatorMemoryStackStart = AddressOf(g_pageFaultEntryArena.CoordinatorStack),
        .CoordinatorMemoryStackEnd =
            AddressOf(g_pageFaultEntryArena.CoordinatorStack) + sizeof(g_pageFaultEntryArena.CoordinatorStack),
        .CoordinatorRseBackingStoreStart = AddressOf(g_pageFaultEntryArena.CoordinatorRse),
        .CoordinatorRseBackingStoreEnd =
            AddressOf(g_pageFaultEntryArena.CoordinatorRse) + sizeof(g_pageFaultEntryArena.CoordinatorRse),
        .CoordinatorEntryPoint = 0x10'0000U,
        .CoordinatorGlobalPointer = 0x20'0000U,
        .CoordinatorArgument = AddressOf(&g_pageFaultEntryArena),
    };
}

[[nodiscard]] constexpr Ia64PageFaultEntryObservation
ValidObservation(const Foundation::UInt64 vectorOffset = 0x5000U,
                 const Foundation::UInt64 interruptionStatus = ValidInterruptionStatus) noexcept
{
    return Ia64PageFaultEntryObservation{
        .FaultAddress = 0xA000'0000'0123'4000ULL,
        .InstructionPointer = 0xA000'0000'0000'1230ULL,
        .InterruptionStatus = interruptionStatus,
        .InterruptionProcessorStatus = ValidProcessorStatus,
        .CoveredInterruptionFunctionState = ValidCoveredFunctionState,
        .VectorOffset = vectorOffset,
    };
}

[[nodiscard]] constexpr Ia64PageFaultEntryOutput Sentinel() noexcept
{
    return Ia64PageFaultEntryOutput{
        .Fault =
            FaultRecord{
                .VirtualAddress = 1U,
                .InstructionPointer = 2U,
                .ArchitectureSyndrome = 3U,
                .Access = FaultAccess::Execute,
                .Cause = FaultCause::InvalidMappingEncoding,
                .Privilege = FaultPrivilege::Supervisor,
                .Reserved = {4U, 5U, 6U, 7U, 8U},
            },
        .ResumeTarget =
            {
                .InstructionPointer = 9U,
                .InterruptionProcessorStatus = 10U,
                .CoveredInterruptionFunctionState = 11U,
            },
    };
}

[[nodiscard]] constexpr bool RecordsEqual(const FaultRecord& left, const FaultRecord& right) noexcept
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

[[nodiscard]] constexpr bool OutputsEqual(const Ia64PageFaultEntryOutput& left,
                                          const Ia64PageFaultEntryOutput& right) noexcept
{
    return RecordsEqual(left.Fault, right.Fault) &&
           (left.ResumeTarget.InstructionPointer == right.ResumeTarget.InstructionPointer) &&
           (left.ResumeTarget.InterruptionProcessorStatus == right.ResumeTarget.InterruptionProcessorStatus) &&
           (left.ResumeTarget.CoveredInterruptionFunctionState == right.ResumeTarget.CoveredInterruptionFunctionState);
}

void CopyBytes(const void* const source, Foundation::UInt8* const destination, const Foundation::Size size) noexcept
{
    const auto* const sourceBytes = static_cast<const Foundation::UInt8*>(source);
    for (Foundation::Size index{0U}; index < size; ++index)
    {
        destination[index] = sourceBytes[index];
    }
}

[[nodiscard]] bool BytesAreEqual(const void* const value, const Foundation::UInt8* const expected,
                                 const Foundation::Size size) noexcept
{
    const auto* const valueBytes = static_cast<const Foundation::UInt8*>(value);
    for (Foundation::Size index{0U}; index < size; ++index)
    {
        if (valueBytes[index] != expected[index])
        {
            return false;
        }
    }
    return true;
}

void PopulateCapturedInterruption(Ia64PageFaultFrame& frame, const Ia64PageFaultEntryObservation& observation) noexcept
{
    frame.InterruptedInstructionPointer = observation.InstructionPointer;
    frame.InterruptedProcessorStatus = observation.InterruptionProcessorStatus;
    frame.CoveredInterruptionFunctionState = observation.CoveredInterruptionFunctionState;
    frame.InterruptionStatus = observation.InterruptionStatus;
    frame.InterruptionFaultingAddress = observation.FaultAddress;
    frame.RseBackingStorePointer = ValidRseBackingStorePointer;
    frame.RseBackingStoreStorePointer = ValidRseBackingStoreStorePointer;
    frame.CoveredRseBackingStorePointer = ValidCoveredRseBackingStorePointer;
    frame.RseDirtyPartitionLoadConfiguration = ValidRseLoadConfiguration;
}

[[nodiscard]] bool AdvanceFrameThroughRse(Ia64PageFaultFrame& frame, const Foundation::UInt64 token) noexcept
{
    constexpr Ia64PageFaultCaptureStage stages[]{
        Ia64PageFaultCaptureStage::InterruptionState, Ia64PageFaultCaptureStage::IntegerState,
        Ia64PageFaultCaptureStage::ApplicationState,  Ia64PageFaultCaptureStage::FloatingState,
        Ia64PageFaultCaptureStage::RseTransition,
    };
    for (const Ia64PageFaultCaptureStage stage : stages)
    {
        if (AdvanceIa64PageFaultCapture(frame, token, stage) != Ia64PageFaultFrameError::None)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool PrepareFrameThroughRse(Ia64PageFaultFrame& frame, const Ia64PageFaultEntryObservation& observation,
                                          Foundation::UInt64& token) noexcept
{
    if (BeginIa64PageFaultCapture(frame,
                                  Ia64PageFaultPreCoverObservation{
                                      .InterruptionStatus = observation.InterruptionStatus,
                                      .VectorOffset = observation.VectorOffset,
                                  },
                                  token) != Ia64PageFaultFrameError::None)
    {
        return false;
    }
    PopulateCapturedInterruption(frame, observation);
    return AdvanceFrameThroughRse(frame, token);
}

[[nodiscard]] bool PrepareDecodedFrame(Ia64PageFaultFrame& frame, const Ia64PageFaultEntryObservation& observation,
                                       Foundation::UInt64& token) noexcept
{
    return PrepareFrameThroughRse(frame, observation, token) &&
           (CompleteIa64PageFaultCapture(frame, token) == Ia64PageFaultFrameError::None);
}

[[nodiscard]] bool PrepareEntryControlCoordinator(const Ia64PageFaultEntryObservation& observation) noexcept
{
    ResetPageFaultEntryArena();
    if ((ConfigureIa64PageFaultEntryControl(g_pageFaultEntryArena.Control, ValidControlConfiguration()) !=
         Ia64PageFaultEntryControlError::None) ||
        (ClaimIa64PageFaultEntryControlCapture(g_pageFaultEntryArena.Control, g_pageFaultEntryArena.Frame,
                                               Ia64PageFaultPreCoverObservation{
                                                   .InterruptionStatus = observation.InterruptionStatus,
                                                   .VectorOffset = observation.VectorOffset,
                                               }) != Ia64PageFaultEntryControlError::None))
    {
        return false;
    }
    PopulateCapturedInterruption(g_pageFaultEntryArena.Frame, observation);
    return AdvanceFrameThroughRse(g_pageFaultEntryArena.Frame, 1U) &&
           (CompleteIa64PageFaultCapture(g_pageFaultEntryArena.Frame, 1U) == Ia64PageFaultFrameError::None) &&
           (PublishIa64PageFaultCapture(g_pageFaultEntryArena.Frame, 1U) == Ia64PageFaultFrameError::None) &&
           (ModelPublishIa64PageFaultEntryControlCapture(g_pageFaultEntryArena.Control, g_pageFaultEntryArena.Frame,
                                                         1U) == Ia64PageFaultEntryControlError::None);
}

[[nodiscard]] bool CompletionFailureIsAtomic(Ia64PageFaultFrame& frame, const Foundation::UInt64 token,
                                             const Ia64PageFaultFrameError expectedError) noexcept
{
    Foundation::UInt8 expected[sizeof(frame)]{};
    CopyBytes(&frame, expected, sizeof(frame));
    return (CompleteIa64PageFaultCapture(frame, token) == expectedError) &&
           BytesAreEqual(&frame, expected, sizeof(frame)) && Ia64PageFaultFrameIsOwned(frame) &&
           (frame.State == Ia64PageFaultFrameState::Capturing) &&
           (frame.CaptureStage == Ia64PageFaultCaptureStage::RseTransition) && (AcquireIa64PageFaultToken(frame) == 0U);
}

[[nodiscard]] bool PublicationFailureIsAtomic(Ia64PageFaultFrame& frame, const Foundation::UInt64 token,
                                              const Ia64PageFaultFrameError expectedError) noexcept
{
    Foundation::UInt8 expected[sizeof(frame)]{};
    CopyBytes(&frame, expected, sizeof(frame));
    return (PublishIa64PageFaultCapture(frame, token) == expectedError) &&
           BytesAreEqual(&frame, expected, sizeof(frame)) && Ia64PageFaultFrameIsOwned(frame) &&
           (frame.State == Ia64PageFaultFrameState::Capturing) &&
           (frame.CaptureStage == Ia64PageFaultCaptureStage::Decoded) && (AcquireIa64PageFaultToken(frame) == 0U);
}

void CheckRejected(TestContext& runner, const Ia64PageFaultEntryObservation& observation,
                   const Ia64PageFaultEntryContractError expected, const char* const description) noexcept
{
    const Ia64PageFaultEntryOutput sentinel{Sentinel()};
    Ia64PageFaultEntryOutput output{sentinel};
    ASTERIA_EXPECT(runner, description, Ia64PageFaultEntryContract::ValidateAndDecode(observation, output) == expected,
                   OutputsEqual(output, sentinel));
}

void TestSupportedVectorsAndExactResume(TestContext& runner) noexcept
{
    struct VectorCase
    {
        Foundation::UInt64 Offset;
        Foundation::UInt64 Status;
        FaultAccess Access;
        FaultCause Cause;
    };
    constexpr VectorCase cases[]{
        {0x5000U, ValidInterruptionStatus, FaultAccess::Read, FaultCause::MissingMapping},
        {0x5100U, WriteBit | (Foundation::UInt64{2U} << ExceptionInstructionSlotShift), FaultAccess::Write,
         FaultCause::ProtectionViolation},
        {0x5200U, ExecuteBit | (Foundation::UInt64{2U} << ExceptionInstructionSlotShift), FaultAccess::Execute,
         FaultCause::ProtectionViolation},
        {0x5300U, ValidInterruptionStatus, FaultAccess::Read, FaultCause::ProtectionViolation},
    };

    for (const VectorCase& vectorCase : cases)
    {
        Ia64PageFaultEntryObservation observation{ValidObservation(vectorCase.Offset, vectorCase.Status)};
        if (vectorCase.Offset == 0x5100U)
        {
            observation.InterruptionProcessorStatus |= ProtectionKeyBit;
        }
        Ia64PageFaultEntryOutput output{Sentinel()};
        const Ia64PageFaultEntryContractError error =
            Ia64PageFaultEntryContract::ValidateAndDecode(observation, output);
        ASTERIA_EXPECT(
            runner, "each exact native page-fault vector reaches the common record",
            error == Ia64PageFaultEntryContractError::None, output.Fault.VirtualAddress == observation.FaultAddress,
            output.Fault.InstructionPointer == observation.InstructionPointer, output.Fault.Access == vectorCase.Access,
            output.Fault.Cause == vectorCase.Cause, output.Fault.Privilege == FaultPrivilege::User);
        ASTERIA_EXPECT(runner, "resume retries the same IIP, IPSR RI, and covered IFS",
                       output.ResumeTarget.InstructionPointer == observation.InstructionPointer,
                       output.ResumeTarget.InterruptionProcessorStatus == observation.InterruptionProcessorStatus,
                       output.ResumeTarget.CoveredInterruptionFunctionState ==
                           observation.CoveredInterruptionFunctionState);
    }

    for (Foundation::UInt64 restartSlot{0U}; restartSlot < 3U; ++restartSlot)
    {
        Ia64PageFaultEntryObservation observation{ValidObservation()};
        observation.InterruptionProcessorStatus &= ~(Foundation::UInt64{3U} << RestartInstructionSlotShift);
        observation.InterruptionProcessorStatus |= restartSlot << RestartInstructionSlotShift;
        observation.InterruptionStatus &= ~(Foundation::UInt64{3U} << ExceptionInstructionSlotShift);
        observation.InterruptionStatus |= restartSlot << ExceptionInstructionSlotShift;
        Ia64PageFaultEntryOutput output{Sentinel()};
        ASTERIA_EXPECT(
            runner, "each native restart slot is retained without advancement",
            Ia64PageFaultEntryContract::ValidateAndDecode(observation, output) == Ia64PageFaultEntryContractError::None,
            output.ResumeTarget.InstructionPointer == observation.InstructionPointer,
            ((output.ResumeTarget.InterruptionProcessorStatus >> RestartInstructionSlotShift) & 3U) == restartSlot);
    }
}

void TestPreCoverValidation(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "ordinary collected evidence is safe to cover",
                   Ia64PageFaultEntryContract::ValidateBeforeCover(Ia64PageFaultPreCoverObservation{
                       .InterruptionStatus = ValidInterruptionStatus,
                       .VectorOffset = 0x5000U,
                   }) == Ia64PageFaultEntryContractError::None);

    ASTERIA_EXPECT(runner, "pre-cover validation accepts only explicit page-fault vectors",
                   Ia64PageFaultEntryContract::ValidateBeforeCover(Ia64PageFaultPreCoverObservation{
                       .InterruptionStatus = ValidInterruptionStatus,
                       .VectorOffset = 0x4F00U,
                   }) == Ia64PageFaultEntryContractError::UnsupportedInterruptionVector);

    Ia64PageFaultEntryOutput alternate{Sentinel()};
    ASTERIA_EXPECT(
        runner, "the capture-only lifecycle admits no Alternate Data TLB miss, which only the user-fault entry decodes",
        Ia64PageFaultEntryContract::ValidateBeforeCover(Ia64PageFaultPreCoverObservation{
            .InterruptionStatus = ValidInterruptionStatus,
            .VectorOffset = 0x1000U,
        }) == Ia64PageFaultEntryContractError::UnsupportedInterruptionVector,
        Ia64PageFaultEntryContract::ValidateAndDecode(ValidObservation(0x1000U), alternate) ==
            Ia64PageFaultEntryContractError::None,
        alternate.Fault.Cause == FaultCause::MissingMapping, alternate.Fault.Privilege == FaultPrivilege::User,
        alternate.Fault.Access == FaultAccess::Read,
        alternate.Fault.ArchitectureSyndrome == (ValidInterruptionStatus | (Foundation::UInt64{0x1000U} << 48U)));

    ASTERIA_EXPECT(runner, "nested evidence is rejected before cover",
                   Ia64PageFaultEntryContract::ValidateBeforeCover(Ia64PageFaultPreCoverObservation{
                       .InterruptionStatus = ValidInterruptionStatus | (Foundation::UInt64{1U} << 39U),
                       .VectorOffset = 0x5000U,
                   }) == Ia64PageFaultEntryContractError::NestedInterruption);

    ASTERIA_EXPECT(runner, "an incomplete register frame is rejected before cover",
                   Ia64PageFaultEntryContract::ValidateBeforeCover(Ia64PageFaultPreCoverObservation{
                       .InterruptionStatus = ValidInterruptionStatus | (Foundation::UInt64{1U} << 38U),
                       .VectorOffset = 0x5000U,
                   }) == Ia64PageFaultEntryContractError::UnsupportedAccessState);
}

void TestProcessorStateRejections(TestContext& runner) noexcept
{
    Ia64PageFaultEntryObservation observation{ValidObservation()};
    observation.VectorOffset = 0x4F00U;
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::UnsupportedInterruptionVector,
                  "a vector below the four explicit offsets is rejected");

    observation = ValidObservation();
    observation.VectorOffset = 0x5400U;
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::UnsupportedInterruptionVector,
                  "a vector above the four explicit offsets is rejected");

    observation = ValidObservation();
    observation.VectorOffset = 0x5050U;
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::UnsupportedInterruptionVector,
                  "an offset inside the surrounding range is not accepted by range");

    observation = ValidObservation();
    observation.InterruptionProcessorStatus |= InstructionSetBit;
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::UnsupportedInstructionSet,
                  "IA-32 interruption state is rejected before decoding");

    observation = ValidObservation();
    observation.InterruptionProcessorStatus &= ~UserPrivilege;
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::UnsupportedPrivilegeLevel,
                  "only an interrupted PL3 context can enter the pager path");

    observation = ValidObservation();
    observation.InterruptionProcessorStatus &= ~RegisterBankBit;
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::UnsupportedRegisterBank,
                  "the interrupted context must be bank one");

    constexpr Foundation::UInt64 translationBits[]{
        DataTranslationBit,
        RseTranslationBit,
        InstructionTranslationBit,
    };
    for (const Foundation::UInt64 translationBit : translationBits)
    {
        observation = ValidObservation();
        observation.InterruptionProcessorStatus &= ~translationBit;
        CheckRejected(runner, observation, Ia64PageFaultEntryContractError::UnsupportedTranslationState,
                      "IT, DT, and RT are independently mandatory");
    }

    observation = ValidObservation();
    observation.InterruptionProcessorStatus &= ~InterruptionCollectionBit;
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::InterruptionCollectionDisabled,
                  "only a fully collected interrupted state can be resumed");

    observation = ValidObservation(0x5100U, WriteBit | (Foundation::UInt64{2U} << ExceptionInstructionSlotShift));
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::ProtectionKeyDisabled,
                  "a key-permission vector requires interrupted PSR.pk");

    observation = ValidObservation();
    observation.InstructionPointer |= 1U;
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::InvalidInstructionPointer,
                  "the restart IIP must identify a bundle boundary");

    observation = ValidObservation();
    observation.InterruptionProcessorStatus |= Foundation::UInt64{1U} << RestartInstructionSlotShift;
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::InvalidInstructionSlot,
                  "reserved IPSR RI value three is rejected");

    observation = ValidObservation();
    observation.InterruptionStatus &= ~(Foundation::UInt64{3U} << ExceptionInstructionSlotShift);
    observation.InterruptionStatus |= Foundation::UInt64{1U} << ExceptionInstructionSlotShift;
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::InterruptionSlotMismatch,
                  "ISR EI must equal the valid IPSR RI restart slot");
}

void TestInterruptionStatusRejections(TestContext& runner) noexcept
{
    Ia64PageFaultEntryObservation observation{ValidObservation()};
    observation.InterruptionStatus |= Foundation::UInt64{1U} << 39U;
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::NestedInterruption,
                  "nested interruption evidence is rejected explicitly");

    constexpr Foundation::UInt64 unsupportedAccessBits[]{
        Foundation::UInt64{1U} << 35U,
        Foundation::UInt64{1U} << 36U,
        Foundation::UInt64{1U} << 37U,
        Foundation::UInt64{1U} << 38U,
    };
    for (const Foundation::UInt64 unsupportedAccessBit : unsupportedAccessBits)
    {
        observation = ValidObservation();
        observation.InterruptionStatus |= unsupportedAccessBit;
        CheckRejected(runner, observation, Ia64PageFaultEntryContractError::UnsupportedAccessState,
                      "ISR na, sp, rs, and ir states are independently unsupported");
    }

    observation = ValidObservation();
    observation.InterruptionStatus &= ~(ReadBit | WriteBit | ExecuteBit);
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::InvalidAccessEncoding,
                  "the downstream decoder still validates the access encoding");

    observation = ValidObservation();
    observation.InterruptionProcessorStatus = 0U;
    observation.InstructionPointer |= 1U;
    observation.InterruptionStatus |= Foundation::UInt64{1U} << 39U;
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::NestedInterruption,
                  "nested evidence is classified before stale IIP and IPSR fields");

    observation = ValidObservation();
    observation.InterruptionStatus |= 1U;
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::UnsupportedInterruptionCode,
                  "ordinary access faults require a zero ISR code");

    observation = ValidObservation(0x5200U, ValidInterruptionStatus);
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::InvalidAccessEncoding,
                  "instruction access rights rejects a data access encoding");

    observation = ValidObservation(0x5300U, ExecuteBit | (Foundation::UInt64{2U} << ExceptionInstructionSlotShift));
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::InvalidAccessEncoding,
                  "data access rights rejects an execute access encoding");

    observation = ValidObservation();
    observation.InterruptionStatus |= Foundation::UInt64{1U} << 24U;
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::InvalidInterruptionStatus,
                  "the downstream decoder still rejects reserved ISR fields");
}

void TestCoveredFunctionStateRejections(TestContext& runner) noexcept
{
    Ia64PageFaultEntryObservation observation{ValidObservation()};
    observation.CoveredInterruptionFunctionState &= ~IfsValidBit;
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::InvalidCoveredInterruptionFunctionState,
                  "the covered IFS must carry a valid CFM");

    observation = ValidObservation();
    observation.CoveredInterruptionFunctionState = IfsValidBit | Foundation::UInt64{97U};
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::InvalidCoveredInterruptionFunctionState,
                  "a covered frame cannot exceed the architectural register window");

    observation = ValidObservation();
    observation.CoveredInterruptionFunctionState |= Foundation::UInt64{1U} << 62U;
    CheckRejected(runner, observation, Ia64PageFaultEntryContractError::InvalidCoveredInterruptionFunctionState,
                  "reserved covered-IFS bits are rejected before resume publication");

    constexpr Foundation::UInt64 invalidFunctionStates[]{
        IfsValidBit | Foundation::UInt64{2U} | (Foundation::UInt64{3U} << 7U),
        IfsValidBit | Foundation::UInt64{7U} | (Foundation::UInt64{1U} << 14U),
        IfsValidBit | (Foundation::UInt64{1U} << 18U),
        IfsValidBit | Foundation::UInt64{8U} | (Foundation::UInt64{1U} << 14U) | (Foundation::UInt64{8U} << 18U),
        ValidCoveredFunctionState | (Foundation::UInt64{96U} << 25U),
        ValidCoveredFunctionState | (Foundation::UInt64{48U} << 32U),
    };
    for (const Foundation::UInt64 functionState : invalidFunctionStates)
    {
        observation = ValidObservation();
        observation.CoveredInterruptionFunctionState = functionState;
        CheckRejected(runner, observation, Ia64PageFaultEntryContractError::InvalidCoveredInterruptionFunctionState,
                      "each invalid covered-CFM relation is rejected");
    }
}

void TestPageFaultFramePristineValidation(TestContext& runner) noexcept
{
    Ia64PageFaultFrame frame{};
    ASTERIA_EXPECT(runner, "the exact initial aggregate frame is pristine", frame.NextToken == 0U,
                   Ia64PageFaultFrameIsPristine(frame));

    frame.PublishedToken = 1U;
    const bool publicationRejected = !Ia64PageFaultFrameIsPristine(frame);
    frame = {};
    frame.NextToken = 2U;
    const bool generationRejected = !Ia64PageFaultFrameIsPristine(frame);
    frame = {};
    frame.InterruptionTranslationInformation = 1U;
    const bool scalarRejected = !Ia64PageFaultFrameIsPristine(frame);
    frame = {};
    frame.BranchRegisters[7U] = 1U;
    const bool branchRejected = !Ia64PageFaultFrameIsPristine(frame);
    frame = {};
    frame.GeneralRegisters[30U] = 1U;
    const bool generalRejected = !Ia64PageFaultFrameIsPristine(frame);
    frame = {};
    frame.FloatingRegisters[125U].High = 1U;
    const bool floatingRejected = !Ia64PageFaultFrameIsPristine(frame);
    frame = {};
    frame.CapturedEntry.Fault.Reserved[4U] = 1U;
    const bool outputRejected = !Ia64PageFaultFrameIsPristine(frame);
    ASTERIA_EXPECT(runner,
                   "published, stale-generation, scalar, register, and output residue "
                   "are not pristine",
                   publicationRejected, generationRejected, scalarRejected, branchRejected, generalRejected,
                   floatingRejected, outputRejected);
}

void TestRetainedFrameLifecycle(TestContext& runner) noexcept
{
    Ia64PageFaultFrame frame{};
    ASTERIA_EXPECT(runner, "a fresh synchronous frame is inactive and unpublished",
                   AcquireIa64PageFaultToken(frame) == 0U, !Ia64PageFaultFrameIsOwned(frame), frame.NextToken == 0U,
                   frame.State == Ia64PageFaultFrameState::Inactive,
                   frame.CaptureStage == Ia64PageFaultCaptureStage::None);

    Foundation::UInt8 expected[sizeof(frame)]{};
    CopyBytes(&frame, expected, sizeof(frame));
    ASTERIA_EXPECT(runner, "a token output inside the frame is rejected before any mutation",
                   BeginIa64PageFaultCapture(frame,
                                             Ia64PageFaultPreCoverObservation{
                                                 .InterruptionStatus = ValidInterruptionStatus,
                                                 .VectorOffset = 0x5000U,
                                             },
                                             frame.NextToken) == Ia64PageFaultFrameError::TokenOutputAliasesFrame,
                   BytesAreEqual(&frame, expected, sizeof(frame)));

    Foundation::UInt64 token{0xABU};
    ASTERIA_EXPECT(runner, "invalid pre-cover evidence changes no frame byte or token output",
                   BeginIa64PageFaultCapture(frame,
                                             Ia64PageFaultPreCoverObservation{
                                                 .InterruptionStatus = ValidInterruptionStatus,
                                                 .VectorOffset = 0x4F00U,
                                             },
                                             token) == Ia64PageFaultFrameError::InvalidPreCoverObservation,
                   token == 0xABU, BytesAreEqual(&frame, expected, sizeof(frame)));

    const Ia64PageFaultEntryObservation observation{ValidObservation()};
    ASTERIA_EXPECT(runner, "begin owns an unpublished frame with a monotonic capture token",
                   BeginIa64PageFaultCapture(frame,
                                             Ia64PageFaultPreCoverObservation{
                                                 .InterruptionStatus = observation.InterruptionStatus,
                                                 .VectorOffset = observation.VectorOffset,
                                             },
                                             token) == Ia64PageFaultFrameError::None,
                   token == 1U, frame.CaptureToken == token, frame.NextToken == 2U,
                   frame.State == Ia64PageFaultFrameState::Capturing,
                   frame.CaptureStage == Ia64PageFaultCaptureStage::PreCoverValidated, Ia64PageFaultFrameIsOwned(frame),
                   AcquireIa64PageFaultToken(frame) == 0U);

    CopyBytes(&frame, expected, sizeof(frame));
    Foundation::UInt64 duplicateToken{0xCDU};
    ASTERIA_EXPECT(runner, "an in-progress frame cannot be captured again",
                   BeginIa64PageFaultCapture(frame,
                                             Ia64PageFaultPreCoverObservation{
                                                 .InterruptionStatus = observation.InterruptionStatus,
                                                 .VectorOffset = observation.VectorOffset,
                                             },
                                             duplicateToken) == Ia64PageFaultFrameError::AlreadyOwned,
                   duplicateToken == 0xCDU, BytesAreEqual(&frame, expected, sizeof(frame)));

    ASTERIA_EXPECT(runner, "capture stages reject skips and stale ownership keys",
                   AdvanceIa64PageFaultCapture(frame, token, Ia64PageFaultCaptureStage::IntegerState) ==
                       Ia64PageFaultFrameError::InvalidCaptureStage,
                   AdvanceIa64PageFaultCapture(frame, token + 1U, Ia64PageFaultCaptureStage::InterruptionState) ==
                       Ia64PageFaultFrameError::TokenMismatch,
                   frame.CaptureStage == Ia64PageFaultCaptureStage::PreCoverValidated);

    PopulateCapturedInterruption(frame, observation);
    ASTERIA_EXPECT(runner,
                   "complete capture decodes the frame-resident fault and exact retry "
                   "state",
                   AdvanceFrameThroughRse(frame, token),
                   CompleteIa64PageFaultCapture(frame, token) == Ia64PageFaultFrameError::None,
                   frame.CaptureStage == Ia64PageFaultCaptureStage::Decoded, AcquireIa64PageFaultToken(frame) == 0U,
                   frame.CapturedEntry.Fault.VirtualAddress == observation.FaultAddress,
                   frame.CapturedEntry.Fault.InstructionPointer == observation.InstructionPointer,
                   frame.CapturedEntry.Fault.Access == FaultAccess::Read,
                   frame.CapturedEntry.Fault.Cause == FaultCause::MissingMapping,
                   frame.CapturedEntry.ResumeTarget.InstructionPointer == observation.InstructionPointer,
                   frame.CapturedEntry.ResumeTarget.InterruptionProcessorStatus ==
                       observation.InterruptionProcessorStatus,
                   frame.CapturedEntry.ResumeTarget.CoveredInterruptionFunctionState ==
                       observation.CoveredInterruptionFunctionState);

    Ia64PageFaultEntryOutput readOutput{Sentinel()};
    const Ia64PageFaultEntryOutput readSentinel{readOutput};
    ASTERIA_EXPECT(runner, "an unpublished capture cannot expose decoded pager state",
                   ReadIa64PageFaultCapture(frame, token, readOutput) == Ia64PageFaultFrameError::TokenMismatch,
                   OutputsEqual(readOutput, readSentinel));

    ASTERIA_EXPECT(runner, "publication release-exposes one complete decoded frame generation",
                   PublishIa64PageFaultCapture(frame, token) == Ia64PageFaultFrameError::None,
                   AcquireIa64PageFaultToken(frame) == token, frame.State == Ia64PageFaultFrameState::Retained,
                   PublishIa64PageFaultCapture(frame, token) == Ia64PageFaultFrameError::InvalidState,
                   ModelReleaseIa64PageFaultFrameForRfi(frame, token) == Ia64PageFaultFrameError::NotCommitted,
                   ReadIa64PageFaultCapture(frame, token, readOutput) == Ia64PageFaultFrameError::None,
                   OutputsEqual(readOutput, frame.CapturedEntry));

    readOutput = Sentinel();
    const Ia64PageFaultEntryOutput committedReadSentinel{readOutput};
    ASTERIA_EXPECT(runner, "commit ends the owning CPU's retained-frame read window",
                   CommitIa64PageFaultResume(frame, token + 1U) == Ia64PageFaultFrameError::TokenMismatch,
                   frame.State == Ia64PageFaultFrameState::Retained,
                   CommitIa64PageFaultResume(frame, token) == Ia64PageFaultFrameError::None,
                   frame.State == Ia64PageFaultFrameState::ResumeCommitted,
                   CommitIa64PageFaultResume(frame, token) == Ia64PageFaultFrameError::NotRetained,
                   ReadIa64PageFaultCapture(frame, token, readOutput) == Ia64PageFaultFrameError::NotRetained,
                   OutputsEqual(readOutput, committedReadSentinel));

    ASTERIA_EXPECT(runner, "the final rfi model release-clears ownership and rejects all replay",
                   ModelReleaseIa64PageFaultFrameForRfi(frame, token) == Ia64PageFaultFrameError::None,
                   AcquireIa64PageFaultToken(frame) == 0U, !Ia64PageFaultFrameIsOwned(frame),
                   frame.State == Ia64PageFaultFrameState::Inactive,
                   frame.CaptureStage == Ia64PageFaultCaptureStage::None,
                   CommitIa64PageFaultResume(frame, token) == Ia64PageFaultFrameError::TokenMismatch,
                   ModelReleaseIa64PageFaultFrameForRfi(frame, token) == Ia64PageFaultFrameError::TokenMismatch,
                   ReadIa64PageFaultCapture(frame, token, readOutput) == Ia64PageFaultFrameError::TokenMismatch,
                   OutputsEqual(readOutput, committedReadSentinel));

    Foundation::UInt64 secondToken{0U};
    ASTERIA_EXPECT(runner, "a released slot never reuses an earlier frame token",
                   BeginIa64PageFaultCapture(frame,
                                             Ia64PageFaultPreCoverObservation{
                                                 .InterruptionStatus = observation.InterruptionStatus,
                                                 .VectorOffset = observation.VectorOffset,
                                             },
                                             secondToken) == Ia64PageFaultFrameError::None,
                   secondToken == 2U);
}

void TestRetainedFrameRseValidation(TestContext& runner) noexcept
{
    const Ia64PageFaultEntryObservation observation{ValidObservation()};
    Ia64PageFaultFrame valid{};
    PopulateCapturedInterruption(valid, observation);
    ASTERIA_EXPECT(runner, "loadrs covers the dirty partition through the post-cover BSP",
                   ValidateIa64PageFaultCoveredRseState(valid) == Ia64PageFaultFrameError::None);

    Ia64PageFaultFrame preCoverOnly{valid};
    preCoverOnly.RseDirtyPartitionLoadConfiguration = Foundation::UInt64{0x80U} << 16U;
    ASTERIA_EXPECT(runner, "loadrs cannot omit the current frame added by cover",
                   ValidateIa64PageFaultCoveredRseState(preCoverOnly) ==
                       Ia64PageFaultFrameError::InvalidRseLoadConfiguration);

    Ia64PageFaultFrame crossing{};
    PopulateCapturedInterruption(crossing, observation);
    crossing.RseBackingStorePointer = 0xA000'0000'0000'01E8ULL;
    crossing.CoveredRseBackingStorePointer = crossing.RseBackingStorePointer + (5U * 8U);
    crossing.RseDirtyPartitionLoadConfiguration = Foundation::UInt64{0x108U} << 16U;
    ASTERIA_EXPECT(runner, "covered BSP advancement counts an intervening RNAT slot",
                   ValidateIa64PageFaultCoveredRseState(crossing) == Ia64PageFaultFrameError::None);

    Ia64PageFaultFrame mismatched{};
    Foundation::UInt64 mismatchToken{0U};
    const bool mismatchPrepared = PrepareFrameThroughRse(mismatched, observation, mismatchToken);
    mismatched.CoveredRseBackingStorePointer -= 8U;
    ASTERIA_EXPECT(
        runner, "a covered BSP that omits the proved advance remains fail-stop", mismatchPrepared,
        CompletionFailureIsAtomic(mismatched, mismatchToken, Ia64PageFaultFrameError::CoveredRsePointerMismatch));

    Ia64PageFaultFrame reservedFunctionState{};
    Foundation::UInt64 reservedToken{0U};
    const bool reservedPrepared = PrepareFrameThroughRse(reservedFunctionState, observation, reservedToken);
    reservedFunctionState.CoveredInterruptionFunctionState |= Foundation::UInt64{1U} << 40U;
    ASTERIA_EXPECT(runner, "reserved covered-CFM bits prevent completion without mutation", reservedPrepared,
                   CompletionFailureIsAtomic(reservedFunctionState, reservedToken,
                                             Ia64PageFaultFrameError::InvalidRseFunctionState));

    Ia64PageFaultFrame malformedBase{};
    Foundation::UInt64 malformedBaseToken{0U};
    const bool malformedBasePrepared = PrepareFrameThroughRse(malformedBase, observation, malformedBaseToken);
    malformedBase.RseBackingStorePointer = 0U;
    ASTERIA_EXPECT(
        runner, "a zero pre-cover BSP cannot complete capture", malformedBasePrepared,
        CompletionFailureIsAtomic(malformedBase, malformedBaseToken, Ia64PageFaultFrameError::InvalidRsePointer));

    Ia64PageFaultFrame malformedCovered{};
    Foundation::UInt64 malformedCoveredToken{0U};
    const bool malformedCoveredPrepared = PrepareFrameThroughRse(malformedCovered, observation, malformedCoveredToken);
    malformedCovered.CoveredRseBackingStorePointer |= 4U;
    ASTERIA_EXPECT(
        runner, "an unaligned covered BSP cannot complete capture", malformedCoveredPrepared,
        CompletionFailureIsAtomic(malformedCovered, malformedCoveredToken, Ia64PageFaultFrameError::InvalidRsePointer));

    Ia64PageFaultFrame malformedStore{};
    Foundation::UInt64 malformedStoreToken{0U};
    const bool malformedStorePrepared = PrepareFrameThroughRse(malformedStore, observation, malformedStoreToken);
    malformedStore.RseBackingStoreStorePointer = malformedStore.RseBackingStorePointer + 8U;
    ASTERIA_EXPECT(
        runner, "an incoherent BSPSTORE relation cannot complete capture", malformedStorePrepared,
        CompletionFailureIsAtomic(malformedStore, malformedStoreToken, Ia64PageFaultFrameError::InvalidRsePointer));

    Ia64PageFaultFrame malformedLoadConfiguration{};
    Foundation::UInt64 malformedLoadToken{0U};
    const bool malformedLoadPrepared =
        PrepareFrameThroughRse(malformedLoadConfiguration, observation, malformedLoadToken);
    malformedLoadConfiguration.RseDirtyPartitionLoadConfiguration = Foundation::UInt64{1U} << 15U;
    ASTERIA_EXPECT(runner, "reserved loadrs encoding cannot complete capture", malformedLoadPrepared,
                   CompletionFailureIsAtomic(malformedLoadConfiguration, malformedLoadToken,
                                             Ia64PageFaultFrameError::InvalidRseLoadConfiguration));

    Ia64PageFaultFrame phaseRebased{};
    PopulateCapturedInterruption(phaseRebased, observation);
    phaseRebased.RseBackingStoreStorePointer = 0xA000'0000'0000'01E8ULL;
    phaseRebased.RseBackingStorePointer = 0xA000'0000'0000'0200ULL;
    phaseRebased.CoveredRseBackingStorePointer = 0xA000'0000'0000'0220ULL;
    phaseRebased.RseDirtyPartitionLoadConfiguration = Foundation::UInt64{0x30U} << 16U;
    ASTERIA_EXPECT(runner,
                   "six dirty registers through cover cross the old RNAT phase and "
                   "normalize to a 48-byte coordinator load",
                   (phaseRebased.RseBackingStorePointer - phaseRebased.RseBackingStoreStorePointer) == 24U,
                   (phaseRebased.CoveredRseBackingStorePointer - phaseRebased.RseBackingStoreStorePointer) == 56U,
                   (phaseRebased.RseDirtyPartitionLoadConfiguration >> 16U) == 48U,
                   ValidateIa64PageFaultCoveredRseState(phaseRebased) == Ia64PageFaultFrameError::None);

    phaseRebased.RseDirtyPartitionLoadConfiguration = Foundation::UInt64{0x38U} << 16U;
    ASTERIA_EXPECT(runner, "loadrs rejects the old raw post-cover span when RNAT phases differ",
                   ValidateIa64PageFaultCoveredRseState(phaseRebased) ==
                       Ia64PageFaultFrameError::InvalidRseLoadConfiguration);

    Ia64PageFaultFrame empty{};
    empty.CoveredInterruptionFunctionState = IfsValidBit;
    empty.RseBackingStoreStorePointer = ValidRseBackingStoreStorePointer;
    empty.RseBackingStorePointer = empty.RseBackingStoreStorePointer;
    empty.CoveredRseBackingStorePointer = empty.RseBackingStorePointer;
    ASTERIA_EXPECT(runner, "an empty current frame and clean RSE admit zero loadrs",
                   ValidateIa64PageFaultCoveredRseState(empty) == Ia64PageFaultFrameError::None);

    Ia64PageFaultFrame limit{};
    limit.CoveredInterruptionFunctionState = IfsValidBit;
    limit.RseBackingStoreStorePointer = 0xA000'0000'0000'0000ULL;
    limit.RseBackingStorePointer = limit.RseBackingStoreStorePointer + 16368U;
    limit.CoveredRseBackingStorePointer = limit.RseBackingStorePointer;
    limit.RseDirtyPartitionLoadConfiguration = Foundation::UInt64{16368U} << 16U;
    ASTERIA_EXPECT(runner, "a dirty partition below the 16 KiB transition limit is accepted",
                   ValidateIa64PageFaultCoveredRseState(limit) == Ia64PageFaultFrameError::None);

    // One current register at slot 62 crosses the trailing RNAT slot. The
    // pre-cover partition fits, but the full covered partition no longer does.
    limit.CoveredInterruptionFunctionState |= 1U;
    limit.CoveredRseBackingStorePointer += 16U;
    limit.RseDirtyPartitionLoadConfiguration = Foundation::UInt64{16384U} << 16U;
    ASTERIA_EXPECT(runner, "cover cannot grow a previously bounded partition to the 16 KiB limit",
                   ValidateIa64PageFaultCoveredRseState(limit) == Ia64PageFaultFrameError::InvalidRsePointer);
}

void TestRetainedFramePublicationRevalidation(TestContext& runner) noexcept
{
    const Ia64PageFaultEntryObservation observation{ValidObservation()};

    Ia64PageFaultFrame changedAddress{};
    Foundation::UInt64 changedAddressToken{0U};
    const bool addressPrepared = PrepareDecodedFrame(changedAddress, observation, changedAddressToken);
    changedAddress.InterruptionFaultingAddress ^= 0x1000U;
    ASTERIA_EXPECT(runner, "publication rejects decoded interruption input mutation", addressPrepared,
                   PublicationFailureIsAtomic(changedAddress, changedAddressToken,
                                              Ia64PageFaultFrameError::CapturedEntryMismatch));

    Ia64PageFaultFrame changedCapturedEntry{};
    Foundation::UInt64 changedEntryToken{0U};
    const bool entryPrepared = PrepareDecodedFrame(changedCapturedEntry, observation, changedEntryToken);
    changedCapturedEntry.CapturedEntry.Fault.VirtualAddress ^= 0x1000U;
    ASTERIA_EXPECT(runner, "publication rejects mutation of the retained decoded output", entryPrepared,
                   PublicationFailureIsAtomic(changedCapturedEntry, changedEntryToken,
                                              Ia64PageFaultFrameError::CapturedEntryMismatch));

    Ia64PageFaultFrame invalidProcessorStatus{};
    Foundation::UInt64 invalidProcessorToken{0U};
    const bool invalidProcessorPrepared =
        PrepareDecodedFrame(invalidProcessorStatus, observation, invalidProcessorToken);
    invalidProcessorStatus.InterruptedProcessorStatus &= ~UserPrivilege;
    ASTERIA_EXPECT(runner, "publication revalidates frame-resident interruption controls", invalidProcessorPrepared,
                   PublicationFailureIsAtomic(invalidProcessorStatus, invalidProcessorToken,
                                              Ia64PageFaultFrameError::EntryContractRejected));

    Ia64PageFaultFrame changedPreCoverEvidence{};
    Foundation::UInt64 changedEvidenceToken{0U};
    const bool evidencePrepared = PrepareDecodedFrame(changedPreCoverEvidence, observation, changedEvidenceToken);
    changedPreCoverEvidence.InterruptionStatus = WriteBit | (Foundation::UInt64{2U} << ExceptionInstructionSlotShift);
    ASTERIA_EXPECT(runner, "publication rechecks the accepted pre-cover ISR observation", evidencePrepared,
                   PublicationFailureIsAtomic(changedPreCoverEvidence, changedEvidenceToken,
                                              Ia64PageFaultFrameError::ObservationMismatch));

    Ia64PageFaultFrame changedRse{};
    Foundation::UInt64 changedRseToken{0U};
    const bool rsePrepared = PrepareDecodedFrame(changedRse, observation, changedRseToken);
    changedRse.CoveredRseBackingStorePointer -= 8U;
    ASTERIA_EXPECT(
        runner, "publication repeats the covered-RSE coherence proof", rsePrepared,
        PublicationFailureIsAtomic(changedRse, changedRseToken, Ia64PageFaultFrameError::CoveredRsePointerMismatch));
}

void TestRetainedFrameTerminalFailures(TestContext& runner) noexcept
{
    const Ia64PageFaultEntryObservation observation{ValidObservation()};
    Ia64PageFaultFrame mismatched{};
    Foundation::UInt64 mismatchToken{0U};
    const bool mismatchStarted = BeginIa64PageFaultCapture(mismatched,
                                                           Ia64PageFaultPreCoverObservation{
                                                               .InterruptionStatus = observation.InterruptionStatus,
                                                               .VectorOffset = observation.VectorOffset,
                                                           },
                                                           mismatchToken) == Ia64PageFaultFrameError::None;
    PopulateCapturedInterruption(mismatched, observation);
    mismatched.InterruptionStatus |= 1U;
    ASTERIA_EXPECT(runner, "changed pre/post-cover ISR evidence remains fail-stop unpublished", mismatchStarted,
                   AdvanceFrameThroughRse(mismatched, mismatchToken),
                   CompleteIa64PageFaultCapture(mismatched, mismatchToken) ==
                       Ia64PageFaultFrameError::ObservationMismatch,
                   Ia64PageFaultFrameIsOwned(mismatched), AcquireIa64PageFaultToken(mismatched) == 0U,
                   mismatched.CaptureStage == Ia64PageFaultCaptureStage::RseTransition);

    Ia64PageFaultFrame invalidEntry{};
    Foundation::UInt64 invalidToken{0U};
    const bool invalidStarted = BeginIa64PageFaultCapture(invalidEntry,
                                                          Ia64PageFaultPreCoverObservation{
                                                              .InterruptionStatus = observation.InterruptionStatus,
                                                              .VectorOffset = observation.VectorOffset,
                                                          },
                                                          invalidToken) == Ia64PageFaultFrameError::None;
    PopulateCapturedInterruption(invalidEntry, observation);
    invalidEntry.InterruptedProcessorStatus &= ~UserPrivilege;
    ASTERIA_EXPECT(
        runner, "a post-capture contract failure cannot publish a partial frame", invalidStarted,
        AdvanceFrameThroughRse(invalidEntry, invalidToken),
        CompleteIa64PageFaultCapture(invalidEntry, invalidToken) == Ia64PageFaultFrameError::EntryContractRejected,
        Ia64PageFaultFrameIsOwned(invalidEntry),
        PublishIa64PageFaultCapture(invalidEntry, invalidToken) == Ia64PageFaultFrameError::InvalidCaptureStage);

    Ia64PageFaultFrame exhausted{};
    exhausted.CaptureToken = 1U;
    Foundation::UInt8 expected[sizeof(exhausted)]{};
    CopyBytes(&exhausted, expected, sizeof(exhausted));
    Foundation::UInt64 exhaustedToken{0xEFU};
    ASTERIA_EXPECT(runner, "token exhaustion is permanent and failure-atomic",
                   BeginIa64PageFaultCapture(exhausted,
                                             Ia64PageFaultPreCoverObservation{
                                                 .InterruptionStatus = observation.InterruptionStatus,
                                                 .VectorOffset = observation.VectorOffset,
                                             },
                                             exhaustedToken) == Ia64PageFaultFrameError::TokenExhausted,
                   exhaustedToken == 0xEFU, BytesAreEqual(&exhausted, expected, sizeof(exhausted)));

    Ia64PageFaultFrame lastGeneration{};
    lastGeneration.NextToken = MaximumUInt64;
    Foundation::UInt64 lastToken{0U};
    const bool lastStarted = BeginIa64PageFaultCapture(lastGeneration,
                                                       Ia64PageFaultPreCoverObservation{
                                                           .InterruptionStatus = observation.InterruptionStatus,
                                                           .VectorOffset = observation.VectorOffset,
                                                       },
                                                       lastToken) == Ia64PageFaultFrameError::None;
    PopulateCapturedInterruption(lastGeneration, observation);
    ASTERIA_EXPECT(runner, "the maximum token completes and releases its final generation", lastStarted,
                   lastToken == MaximumUInt64, lastGeneration.NextToken == 0U,
                   AdvanceFrameThroughRse(lastGeneration, lastToken),
                   CompleteIa64PageFaultCapture(lastGeneration, lastToken) == Ia64PageFaultFrameError::None,
                   PublishIa64PageFaultCapture(lastGeneration, lastToken) == Ia64PageFaultFrameError::None,
                   CommitIa64PageFaultResume(lastGeneration, lastToken) == Ia64PageFaultFrameError::None,
                   ModelReleaseIa64PageFaultFrameForRfi(lastGeneration, lastToken) == Ia64PageFaultFrameError::None,
                   AcquireIa64PageFaultToken(lastGeneration) == 0U, !Ia64PageFaultFrameIsOwned(lastGeneration));

    Foundation::UInt8 exhaustedGeneration[sizeof(lastGeneration)]{};
    CopyBytes(&lastGeneration, exhaustedGeneration, sizeof(lastGeneration));
    Foundation::UInt64 replayToken{0x1357U};
    ASTERIA_EXPECT(runner, "maximum-token release is followed by permanent atomic exhaustion",
                   BeginIa64PageFaultCapture(lastGeneration,
                                             Ia64PageFaultPreCoverObservation{
                                                 .InterruptionStatus = observation.InterruptionStatus,
                                                 .VectorOffset = observation.VectorOffset,
                                             },
                                             replayToken) == Ia64PageFaultFrameError::TokenExhausted,
                   replayToken == 0x1357U, BytesAreEqual(&lastGeneration, exhaustedGeneration, sizeof(lastGeneration)));

    Ia64PageFaultFrame corruptedStage{};
    Foundation::UInt64 corruptedStageToken{0U};
    const bool corruptedStagePublished =
        PrepareDecodedFrame(corruptedStage, observation, corruptedStageToken) &&
        (PublishIa64PageFaultCapture(corruptedStage, corruptedStageToken) == Ia64PageFaultFrameError::None);
    corruptedStage.CaptureStage = Ia64PageFaultCaptureStage::RseTransition;
    const bool commitRejected = (CommitIa64PageFaultResume(corruptedStage, corruptedStageToken) ==
                                 Ia64PageFaultFrameError::InvalidCaptureStage) &&
                                (corruptedStage.State == Ia64PageFaultFrameState::Retained) &&
                                (AcquireIa64PageFaultToken(corruptedStage) == corruptedStageToken);
    corruptedStage.CaptureStage = Ia64PageFaultCaptureStage::Decoded;
    const bool committed =
        CommitIa64PageFaultResume(corruptedStage, corruptedStageToken) == Ia64PageFaultFrameError::None;
    corruptedStage.CaptureStage = Ia64PageFaultCaptureStage::RseTransition;
    ASTERIA_EXPECT(runner,
                   "resume commit and final release retain ownership unless the frame "
                   "remains decoded",
                   corruptedStagePublished, commitRejected, committed,
                   ModelReleaseIa64PageFaultFrameForRfi(corruptedStage, corruptedStageToken) ==
                       Ia64PageFaultFrameError::InvalidCaptureStage,
                   corruptedStage.State == Ia64PageFaultFrameState::ResumeCommitted,
                   AcquireIa64PageFaultToken(corruptedStage) == corruptedStageToken);
}

void TestEntryControlConfigurationAndVectorClaim(TestContext& runner) noexcept
{
    ResetPageFaultEntryArena();
    Foundation::UInt8 expectedControl[sizeof(Ia64PageFaultEntryControl)]{};
    CopyBytes(&g_pageFaultEntryArena.Control, expectedControl, sizeof(expectedControl));

    Ia64PageFaultEntryControlConfiguration invalid = ValidControlConfiguration();
    invalid.ProcessorIndex = 4U;
    const bool processorRejected = ConfigureIa64PageFaultEntryControl(g_pageFaultEntryArena.Control, invalid) ==
                                   Ia64PageFaultEntryControlError::InvalidConfiguration;
    invalid = ValidControlConfiguration();
    invalid.CoordinatorMemoryStackEnd -= 4096U;
    const bool stackRejected = ConfigureIa64PageFaultEntryControl(g_pageFaultEntryArena.Control, invalid) ==
                               Ia64PageFaultEntryControlError::InvalidConfiguration;
    invalid = ValidControlConfiguration();
    invalid.InterruptedRseBackingStoreStart += 8U;
    const bool interruptedRseRejected = ConfigureIa64PageFaultEntryControl(g_pageFaultEntryArena.Control, invalid) ==
                                        Ia64PageFaultEntryControlError::InvalidConfiguration;
    ASTERIA_EXPECT(runner, "invalid processor and storage geometry leave a fresh control unchanged", processorRejected,
                   stackRejected, interruptedRseRejected,
                   BytesAreEqual(&g_pageFaultEntryArena.Control, expectedControl, sizeof(expectedControl)));

    ASTERIA_EXPECT(runner, "one immutable processor-local geometry is configured exactly once",
                   ConfigureIa64PageFaultEntryControl(g_pageFaultEntryArena.Control, ValidControlConfiguration()) ==
                       Ia64PageFaultEntryControlError::None,
                   ValidateConfiguredIa64PageFaultEntryControl(g_pageFaultEntryArena.Control) ==
                       Ia64PageFaultEntryControlError::None,
                   ConfigureIa64PageFaultEntryControl(g_pageFaultEntryArena.Control, ValidControlConfiguration()) ==
                       Ia64PageFaultEntryControlError::AlreadyConfigured);

    Foundation::UInt8 configuredControl[sizeof(Ia64PageFaultEntryControl)]{};
    Foundation::UInt8 freshFrame[sizeof(Ia64PageFaultFrame)]{};
    CopyBytes(&g_pageFaultEntryArena.Control, configuredControl, sizeof(configuredControl));
    CopyBytes(&g_pageFaultEntryArena.Frame, freshFrame, sizeof(freshFrame));
    ASTERIA_EXPECT(runner, "an unsupported vector is rejected before control or frame ownership",
                   ClaimIa64PageFaultEntryControlCapture(g_pageFaultEntryArena.Control, g_pageFaultEntryArena.Frame,
                                                         Ia64PageFaultPreCoverObservation{
                                                             .InterruptionStatus = ValidInterruptionStatus,
                                                             .VectorOffset = 0x5400U,
                                                         }) ==
                       Ia64PageFaultEntryControlError::InvalidPreCoverObservation,
                   BytesAreEqual(&g_pageFaultEntryArena.Control, configuredControl, sizeof(configuredControl)),
                   BytesAreEqual(&g_pageFaultEntryArena.Frame, freshFrame, sizeof(freshFrame)));

    g_pageFaultEntryArena.Frame.CaptureToken = 9U;
    Foundation::UInt8 staleFrame[sizeof(Ia64PageFaultFrame)]{};
    CopyBytes(&g_pageFaultEntryArena.Frame, staleFrame, sizeof(staleFrame));
    ASTERIA_EXPECT(runner, "one-shot claim rejects changed frame storage before reservation",
                   ClaimIa64PageFaultEntryControlCapture(g_pageFaultEntryArena.Control, g_pageFaultEntryArena.Frame,
                                                         Ia64PageFaultPreCoverObservation{
                                                             .InterruptionStatus = ValidInterruptionStatus,
                                                             .VectorOffset = 0x5000U,
                                                         }) == Ia64PageFaultEntryControlError::FrameCaptureRejected,
                   BytesAreEqual(&g_pageFaultEntryArena.Control, configuredControl, sizeof(configuredControl)),
                   BytesAreEqual(&g_pageFaultEntryArena.Frame, staleFrame, sizeof(staleFrame)));

    constexpr Foundation::UInt64 vectors[]{
        0x5000U,
        0x5100U,
        0x5200U,
        0x5300U,
    };
    bool allVectorsClaimed{true};
    for (const Foundation::UInt64 vector : vectors)
    {
        ResetPageFaultEntryArena();
        allVectorsClaimed =
            allVectorsClaimed &&
            (ConfigureIa64PageFaultEntryControl(g_pageFaultEntryArena.Control, ValidControlConfiguration()) ==
             Ia64PageFaultEntryControlError::None) &&
            (ClaimIa64PageFaultEntryControlCapture(g_pageFaultEntryArena.Control, g_pageFaultEntryArena.Frame,
                                                   Ia64PageFaultPreCoverObservation{
                                                       .InterruptionStatus = ValidInterruptionStatus,
                                                       .VectorOffset = vector,
                                                   }) == Ia64PageFaultEntryControlError::None);
        const auto snapshot = AcquireIa64PageFaultEntryControlSnapshot(g_pageFaultEntryArena.Control);
        allVectorsClaimed = allVectorsClaimed && (snapshot.State == Ia64PageFaultEntryControlState::Capturing) &&
                            (snapshot.EntryCaptureToken == 1U) && (snapshot.CapturedFrameToken == 1U) &&
                            (snapshot.CapturedVectorOffset == vector) &&
                            (g_pageFaultEntryArena.Frame.VectorOffset == vector);
    }
    ASTERIA_EXPECT(runner, "the control retains the exact identity of each supported IVT vector", allVectorsClaimed);

    ResetPageFaultEntryArena();
    Ia64PageFaultEntryObservation boundaryObservation{ValidObservation()};
    boundaryObservation.CoveredInterruptionFunctionState =
        (boundaryObservation.CoveredInterruptionFunctionState & ~Foundation::UInt64{0x7FU}) | 3U;
    const Ia64PageFaultEntryControlConfiguration boundaryConfiguration = ValidControlConfiguration();
    const bool boundaryClaimed =
        (ConfigureIa64PageFaultEntryControl(g_pageFaultEntryArena.Control, boundaryConfiguration) ==
         Ia64PageFaultEntryControlError::None) &&
        (ClaimIa64PageFaultEntryControlCapture(g_pageFaultEntryArena.Control, g_pageFaultEntryArena.Frame,
                                               Ia64PageFaultPreCoverObservation{
                                                   .InterruptionStatus = boundaryObservation.InterruptionStatus,
                                                   .VectorOffset = boundaryObservation.VectorOffset,
                                               }) == Ia64PageFaultEntryControlError::None);
    PopulateCapturedInterruption(g_pageFaultEntryArena.Frame, boundaryObservation);
    g_pageFaultEntryArena.Frame.RseBackingStorePointer = boundaryConfiguration.InterruptedRseBackingStoreEnd - 32U;
    g_pageFaultEntryArena.Frame.RseBackingStoreStorePointer = g_pageFaultEntryArena.Frame.RseBackingStorePointer;
    g_pageFaultEntryArena.Frame.CoveredRseBackingStorePointer = boundaryConfiguration.InterruptedRseBackingStoreEnd;
    g_pageFaultEntryArena.Frame.RseDirtyPartitionLoadConfiguration = Foundation::UInt64{0x18U} << 16U;
    ASTERIA_EXPECT(runner, "covered BSP may converge exactly one-past the prepared user RBS", boundaryClaimed,
                   AdvanceFrameThroughRse(g_pageFaultEntryArena.Frame, 1U),
                   CompleteIa64PageFaultCapture(g_pageFaultEntryArena.Frame, 1U) == Ia64PageFaultFrameError::None,
                   PublishIa64PageFaultCapture(g_pageFaultEntryArena.Frame, 1U) == Ia64PageFaultFrameError::None,
                   ModelPublishIa64PageFaultEntryControlCapture(g_pageFaultEntryArena.Control,
                                                                g_pageFaultEntryArena.Frame,
                                                                1U) == Ia64PageFaultEntryControlError::None);
}

void TestEntryControlCheckedStatesAndSnapshots(TestContext& runner) noexcept
{
    ResetPageFaultEntryArena();
    Ia64PageFaultEntryControl& control = g_pageFaultEntryArena.Control;
    control.FrameAddress = 41U;
    const auto fresh = AcquireIa64PageFaultEntryControlSnapshot(control);
    ASTERIA_EXPECT(runner, "a Fresh snapshot does not race configuration initialization",
                   fresh.State == Ia64PageFaultEntryControlState::Fresh, fresh.Configuration.FrameAddress == 0U,
                   fresh.EntryCaptureToken == 0U, fresh.CaptureCount == 0U, fresh.LastFailure == 0U);

    ResetPageFaultEntryArena();
    const Ia64PageFaultEntryControlConfiguration configuration = ValidControlConfiguration();
    const bool configured =
        ConfigureIa64PageFaultEntryControl(control, configuration) == Ia64PageFaultEntryControlError::None;
    const auto configuredSnapshot = AcquireIa64PageFaultEntryControlSnapshot(control);
    control.EntryCaptureToken = 11U;
    control.NextEntryCaptureToken = 12U;
    control.CapturedFrameToken = 13U;
    control.CapturedVectorOffset = 0x5000U;
    control.CaptureCount = 14U;
    control.ResumeGeneration = 15U;
    control.RestoreCount = 16U;
    control.LastFailure = 17U;
    __atomic_store_n(&control.State, static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Claiming),
                     __ATOMIC_RELEASE);
    const Ia64PageFaultEntryControlError claimingConfigure = ConfigureIa64PageFaultEntryControl(control, configuration);
    const Ia64PageFaultEntryControlError claimingTransition =
        ModelPublishIa64PageFaultEntryControlCapture(control, g_pageFaultEntryArena.Frame, 11U);
    const auto claiming = AcquireIa64PageFaultEntryControlSnapshot(control);
    ASTERIA_EXPECT(runner,
                   "Configured and Claiming expose only release-stable fields, and a "
                   "competing API cannot corrupt the reservation",
                   configured, configuredSnapshot.State == Ia64PageFaultEntryControlState::Configured,
                   configuredSnapshot.Configuration.FrameAddress == configuration.FrameAddress,
                   configuredSnapshot.NextEntryCaptureToken == 0U,
                   claimingConfigure == Ia64PageFaultEntryControlError::AlreadyConfigured,
                   claimingTransition == Ia64PageFaultEntryControlError::InvalidState,
                   claiming.State == Ia64PageFaultEntryControlState::Claiming,
                   claiming.Configuration.FrameAddress == configuration.FrameAddress, claiming.EntryCaptureToken == 0U,
                   claiming.CaptureCount == 0U, claiming.LastFailure == 0U, control.LastFailure == 17U);

    __atomic_store_n(&control.State, static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Capturing),
                     __ATOMIC_RELEASE);
    const auto capturing = AcquireIa64PageFaultEntryControlSnapshot(control);
    __atomic_store_n(&control.State, static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Coordinator),
                     __ATOMIC_RELEASE);
    const auto coordinator = AcquireIa64PageFaultEntryControlSnapshot(control);
    __atomic_store_n(&control.State, static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::ResumeCommitted),
                     __ATOMIC_RELEASE);
    const auto resume = AcquireIa64PageFaultEntryControlSnapshot(control);
    __atomic_store_n(&control.State, static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Complete),
                     __ATOMIC_RELEASE);
    const auto complete = AcquireIa64PageFaultEntryControlSnapshot(control);
    __atomic_store_n(&control.State, static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Faulted),
                     __ATOMIC_RELEASE);
    const auto faulted = AcquireIa64PageFaultEntryControlSnapshot(control);
    ASTERIA_EXPECT(runner,
                   "snapshots expose each lifecycle field only after its publishing "
                   "state and preserve complete Faulted diagnostics",
                   capturing.EntryCaptureToken == 11U, capturing.NextEntryCaptureToken == 12U,
                   capturing.CapturedFrameToken == 13U, capturing.CapturedVectorOffset == 0x5000U,
                   capturing.CaptureCount == 0U, capturing.ResumeGeneration == 0U, coordinator.CaptureCount == 14U,
                   coordinator.ResumeGeneration == 0U, resume.ResumeGeneration == 15U, resume.RestoreCount == 0U,
                   complete.RestoreCount == 16U, complete.LastFailure == 0U,
                   faulted.Configuration.FrameAddress == configuration.FrameAddress, faulted.EntryCaptureToken == 11U,
                   faulted.CaptureCount == 14U, faulted.ResumeGeneration == 15U, faulted.RestoreCount == 16U,
                   faulted.LastFailure == 17U);

    constexpr Ia64PageFaultEntryControlState liveStates[]{
        Ia64PageFaultEntryControlState::Capturing,
        Ia64PageFaultEntryControlState::Coordinator,
        Ia64PageFaultEntryControlState::ResumeCommitted,
    };
    bool liveConfigureFaults{true};
    for (const Ia64PageFaultEntryControlState liveState : liveStates)
    {
        ResetPageFaultEntryArena();
        liveConfigureFaults = liveConfigureFaults && (ConfigureIa64PageFaultEntryControl(control, configuration) ==
                                                      Ia64PageFaultEntryControlError::None);
        __atomic_store_n(&control.State, static_cast<Foundation::UInt64>(liveState), __ATOMIC_RELEASE);
        liveConfigureFaults =
            liveConfigureFaults &&
            (ConfigureIa64PageFaultEntryControl(control, configuration) ==
             Ia64PageFaultEntryControlError::InvalidState) &&
            (AcquireIa64PageFaultEntryControlState(control) == Ia64PageFaultEntryControlState::Faulted) &&
            (control.LastFailure == static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlError::InvalidState));
    }
    ASTERIA_EXPECT(runner, "Configure terminally faults every release-published live owner", liveConfigureFaults);

    ResetPageFaultEntryArena();
    control.FrameAddress = 51U;
    control.EntryCaptureToken = 52U;
    control.CaptureCount = 53U;
    control.LastFailure = 54U;
    __atomic_store_n(&control.State, Foundation::UInt64{99U}, __ATOMIC_RELEASE);
    const auto invalid = AcquireIa64PageFaultEntryControlSnapshot(control);
    const Ia64PageFaultEntryControlError invalidConfigure = ConfigureIa64PageFaultEntryControl(control, configuration);
    const auto invalidFault = AcquireIa64PageFaultEntryControlSnapshot(control);
    ASTERIA_EXPECT(
        runner, "unknown raw states decode safely and mutable use fail-stops them",
        invalid.State == Ia64PageFaultEntryControlState::Invalid, invalid.Configuration.FrameAddress == 0U,
        invalid.EntryCaptureToken == 0U, invalid.CaptureCount == 0U, invalid.LastFailure == 0U,
        invalidConfigure == Ia64PageFaultEntryControlError::InvalidState,
        invalidFault.State == Ia64PageFaultEntryControlState::Faulted, invalidFault.Configuration.FrameAddress == 51U,
        invalidFault.EntryCaptureToken == 52U, invalidFault.CaptureCount == 53U,
        invalidFault.LastFailure == static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlError::InvalidState));

    ResetPageFaultEntryArena();
    const bool terminalConfigured =
        ConfigureIa64PageFaultEntryControl(control, configuration) == Ia64PageFaultEntryControlError::None;
    __atomic_store_n(&control.State, static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Complete),
                     __ATOMIC_RELEASE);
    const Ia64PageFaultEntryControlError completeConfigure = ConfigureIa64PageFaultEntryControl(control, configuration);
    const Ia64PageFaultEntryControlState stateAfterCompleteConfigure = AcquireIa64PageFaultEntryControlState(control);
    control.LastFailure = static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlError::VectorMismatch);
    __atomic_store_n(&control.State, static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlState::Faulted),
                     __ATOMIC_RELEASE);
    const Ia64PageFaultEntryControlError faultedConfigure = ConfigureIa64PageFaultEntryControl(control, configuration);
    ASTERIA_EXPECT(runner, "Configure cannot overwrite Complete or Faulted terminal controls", terminalConfigured,
                   completeConfigure == Ia64PageFaultEntryControlError::AlreadyConfigured,
                   stateAfterCompleteConfigure == Ia64PageFaultEntryControlState::Complete,
                   faultedConfigure == Ia64PageFaultEntryControlError::AlreadyConfigured,
                   AcquireIa64PageFaultEntryControlState(control) == Ia64PageFaultEntryControlState::Faulted,
                   control.LastFailure ==
                       static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlError::VectorMismatch));
}

void TestEntryControlFrameAndResumeLifecycle(TestContext& runner) noexcept
{
    ResetPageFaultEntryArena();
    const Ia64PageFaultEntryObservation observation{ValidObservation()};
    const bool claimed =
        (ConfigureIa64PageFaultEntryControl(g_pageFaultEntryArena.Control, ValidControlConfiguration()) ==
         Ia64PageFaultEntryControlError::None) &&
        (ClaimIa64PageFaultEntryControlCapture(g_pageFaultEntryArena.Control, g_pageFaultEntryArena.Frame,
                                               Ia64PageFaultPreCoverObservation{
                                                   .InterruptionStatus = observation.InterruptionStatus,
                                                   .VectorOffset = observation.VectorOffset,
                                               }) == Ia64PageFaultEntryControlError::None);
    const auto claimedSnapshot = AcquireIa64PageFaultEntryControlSnapshot(g_pageFaultEntryArena.Control);
    PopulateCapturedInterruption(g_pageFaultEntryArena.Frame, observation);
    const bool framePublished =
        claimed && (claimedSnapshot.EntryCaptureToken == 1U) && (claimedSnapshot.CapturedFrameToken == 1U) &&
        AdvanceFrameThroughRse(g_pageFaultEntryArena.Frame, 1U) &&
        (CompleteIa64PageFaultCapture(g_pageFaultEntryArena.Frame, 1U) == Ia64PageFaultFrameError::None) &&
        (PublishIa64PageFaultCapture(g_pageFaultEntryArena.Frame, 1U) == Ia64PageFaultFrameError::None);
    ASTERIA_EXPECT(runner, "coordinator publication follows the exact retained frame generation", framePublished,
                   ModelPublishIa64PageFaultEntryControlCapture(g_pageFaultEntryArena.Control,
                                                                g_pageFaultEntryArena.Frame,
                                                                1U) == Ia64PageFaultEntryControlError::None);

    const auto coordinator = AcquireIa64PageFaultEntryControlSnapshot(g_pageFaultEntryArena.Control);
    ASTERIA_EXPECT(runner, "control, frame, and future resume identities remain distinct fields",
                   coordinator.State == Ia64PageFaultEntryControlState::Coordinator,
                   coordinator.EntryCaptureToken == 1U, coordinator.CapturedFrameToken == 1U,
                   coordinator.ResumeGeneration == 0U, coordinator.CaptureCount == 1U);

    constexpr Foundation::UInt64 resumeGeneration{19U};
    ASTERIA_EXPECT(
        runner,
        "frame commit, distinct resume generation, control completion, and "
        "final frame release occur in order",
        CommitIa64PageFaultResume(g_pageFaultEntryArena.Frame, 1U) == Ia64PageFaultFrameError::None,
        CommitIa64PageFaultEntryControlForResume(g_pageFaultEntryArena.Control, g_pageFaultEntryArena.Frame, 1U, 1U,
                                                 resumeGeneration) == Ia64PageFaultEntryControlError::None,
        ModelCompleteIa64PageFaultEntryControlForRfi(g_pageFaultEntryArena.Control, g_pageFaultEntryArena.Frame, 1U, 1U,
                                                     resumeGeneration) == Ia64PageFaultEntryControlError::None,
        ModelReleaseIa64PageFaultFrameForRfi(g_pageFaultEntryArena.Frame, 1U) == Ia64PageFaultFrameError::None);
    const auto complete = AcquireIa64PageFaultEntryControlSnapshot(g_pageFaultEntryArena.Control);
    ASTERIA_EXPECT(runner, "the one-shot owner completes without making its control reusable",
                   complete.State == Ia64PageFaultEntryControlState::Complete,
                   complete.ResumeGeneration == resumeGeneration, complete.RestoreCount == 1U,
                   !Ia64PageFaultFrameIsOwned(g_pageFaultEntryArena.Frame));
    const Ia64PageFaultEntryControlError staleComplete = ModelCompleteIa64PageFaultEntryControlForRfi(
        g_pageFaultEntryArena.Control, g_pageFaultEntryArena.Frame, 1U, 1U, resumeGeneration);
    const auto terminalComplete = AcquireIa64PageFaultEntryControlSnapshot(g_pageFaultEntryArena.Control);
    ASTERIA_EXPECT(runner, "stale replay cannot overwrite a terminal complete control",
                   staleComplete == Ia64PageFaultEntryControlError::InvalidState,
                   terminalComplete.State == Ia64PageFaultEntryControlState::Complete,
                   terminalComplete.RestoreCount == 1U, terminalComplete.LastFailure == 0U);
}

void TestEntryControlPostClaimContradictionsFault(TestContext& runner) noexcept
{
    ResetPageFaultEntryArena();
    const Ia64PageFaultEntryObservation observation{ValidObservation()};
    const bool claimed =
        (ConfigureIa64PageFaultEntryControl(g_pageFaultEntryArena.Control, ValidControlConfiguration()) ==
         Ia64PageFaultEntryControlError::None) &&
        (ClaimIa64PageFaultEntryControlCapture(g_pageFaultEntryArena.Control, g_pageFaultEntryArena.Frame,
                                               Ia64PageFaultPreCoverObservation{
                                                   .InterruptionStatus = observation.InterruptionStatus,
                                                   .VectorOffset = observation.VectorOffset,
                                               }) == Ia64PageFaultEntryControlError::None);
    PopulateCapturedInterruption(g_pageFaultEntryArena.Frame, observation);
    const bool framePublished =
        claimed && AdvanceFrameThroughRse(g_pageFaultEntryArena.Frame, 1U) &&
        (CompleteIa64PageFaultCapture(g_pageFaultEntryArena.Frame, 1U) == Ia64PageFaultFrameError::None) &&
        (PublishIa64PageFaultCapture(g_pageFaultEntryArena.Frame, 1U) == Ia64PageFaultFrameError::None);
    const Ia64PageFaultEntryControlError error =
        framePublished ? ModelPublishIa64PageFaultEntryControlCapture(g_pageFaultEntryArena.Control,
                                                                      g_pageFaultEntryArena.Frame, 2U)
                       : Ia64PageFaultEntryControlError::None;
    const auto faulted = AcquireIa64PageFaultEntryControlSnapshot(g_pageFaultEntryArena.Control);
    ASTERIA_EXPECT(runner, "a post-capture token contradiction fail-stops and retains the frame", framePublished,
                   error == Ia64PageFaultEntryControlError::EntryCaptureTokenMismatch,
                   faulted.State == Ia64PageFaultEntryControlState::Faulted,
                   faulted.LastFailure == static_cast<Foundation::UInt64>(error),
                   Ia64PageFaultFrameIsOwned(g_pageFaultEntryArena.Frame));
    const Ia64PageFaultEntryControlError faultedReplay = CommitIa64PageFaultEntryControlForResume(
        g_pageFaultEntryArena.Control, g_pageFaultEntryArena.Frame, 1U, 1U, 3U);
    const auto replayedFault = AcquireIa64PageFaultEntryControlSnapshot(g_pageFaultEntryArena.Control);
    ASTERIA_EXPECT(runner, "stale replay cannot overwrite the first terminal fault",
                   faultedReplay == Ia64PageFaultEntryControlError::InvalidState,
                   replayedFault.State == Ia64PageFaultEntryControlState::Faulted,
                   replayedFault.LastFailure ==
                       static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlError::EntryCaptureTokenMismatch));

    ResetPageFaultEntryArena();
    const bool captureClaimed =
        (ConfigureIa64PageFaultEntryControl(g_pageFaultEntryArena.Control, ValidControlConfiguration()) ==
         Ia64PageFaultEntryControlError::None) &&
        (ClaimIa64PageFaultEntryControlCapture(g_pageFaultEntryArena.Control, g_pageFaultEntryArena.Frame,
                                               Ia64PageFaultPreCoverObservation{
                                                   .InterruptionStatus = observation.InterruptionStatus,
                                                   .VectorOffset = observation.VectorOffset,
                                               }) == Ia64PageFaultEntryControlError::None);
    g_pageFaultEntryArena.Frame.State = Ia64PageFaultFrameState::Inactive;
    const bool frameUnexpectedlyUnowned = !Ia64PageFaultFrameIsOwned(g_pageFaultEntryArena.Frame);
    const Ia64PageFaultEntryControlError outOfOrderError =
        captureClaimed ? CommitIa64PageFaultEntryControlForResume(g_pageFaultEntryArena.Control,
                                                                  g_pageFaultEntryArena.Frame, 1U, 1U, 5U)
                       : Ia64PageFaultEntryControlError::None;
    const auto outOfOrderFault = AcquireIa64PageFaultEntryControlSnapshot(g_pageFaultEntryArena.Control);
    ASTERIA_EXPECT(
        runner,
        "out-of-order post-claim transition terminally faults its control "
        "even when frame ownership was also corrupted",
        captureClaimed, frameUnexpectedlyUnowned, outOfOrderError == Ia64PageFaultEntryControlError::InvalidState,
        outOfOrderFault.State == Ia64PageFaultEntryControlState::Faulted,
        outOfOrderFault.LastFailure == static_cast<Foundation::UInt64>(Ia64PageFaultEntryControlError::InvalidState));

    ResetPageFaultEntryArena();
    g_unrelatedPageFaultFrame = {};
    const bool wrongFrameClaimed =
        (ConfigureIa64PageFaultEntryControl(g_pageFaultEntryArena.Control, ValidControlConfiguration()) ==
         Ia64PageFaultEntryControlError::None) &&
        (ClaimIa64PageFaultEntryControlCapture(g_pageFaultEntryArena.Control, g_pageFaultEntryArena.Frame,
                                               Ia64PageFaultPreCoverObservation{
                                                   .InterruptionStatus = observation.InterruptionStatus,
                                                   .VectorOffset = observation.VectorOffset,
                                               }) == Ia64PageFaultEntryControlError::None);
    const Ia64PageFaultEntryControlError wrongFrameError =
        wrongFrameClaimed
            ? ModelPublishIa64PageFaultEntryControlCapture(g_pageFaultEntryArena.Control, g_unrelatedPageFaultFrame, 1U)
            : Ia64PageFaultEntryControlError::None;
    const auto wrongFrameFault = AcquireIa64PageFaultEntryControlSnapshot(g_pageFaultEntryArena.Control);
    ASTERIA_EXPECT(runner,
                   "a substituted post-claim frame faults the exact owner while "
                   "preserving captured ownership and unrelated-frame bytes",
                   wrongFrameClaimed, wrongFrameError == Ia64PageFaultEntryControlError::InvalidConfiguration,
                   wrongFrameFault.State == Ia64PageFaultEntryControlState::Faulted,
                   wrongFrameFault.LastFailure == static_cast<Foundation::UInt64>(wrongFrameError),
                   Ia64PageFaultFrameIsOwned(g_pageFaultEntryArena.Frame),
                   Ia64PageFaultFrameIsPristine(g_unrelatedPageFaultFrame));

    const bool stagePrepared =
        PrepareEntryControlCoordinator(observation) &&
        (CommitIa64PageFaultResume(g_pageFaultEntryArena.Frame, 1U) == Ia64PageFaultFrameError::None);
    g_pageFaultEntryArena.Frame.CaptureStage = Ia64PageFaultCaptureStage::RseTransition;
    const Ia64PageFaultEntryControlError stageError =
        stagePrepared ? CommitIa64PageFaultEntryControlForResume(g_pageFaultEntryArena.Control,
                                                                 g_pageFaultEntryArena.Frame, 1U, 1U, 11U)
                      : Ia64PageFaultEntryControlError::None;
    ASTERIA_EXPECT(runner, "resume control rejects a frame whose decoded stage was corrupted", stagePrepared,
                   stageError == Ia64PageFaultEntryControlError::FrameStateMismatch,
                   AcquireIa64PageFaultEntryControlSnapshot(g_pageFaultEntryArena.Control).State ==
                       Ia64PageFaultEntryControlState::Faulted,
                   Ia64PageFaultFrameIsOwned(g_pageFaultEntryArena.Frame));

    const bool rsePrepared =
        PrepareEntryControlCoordinator(observation) &&
        (CommitIa64PageFaultResume(g_pageFaultEntryArena.Frame, 1U) == Ia64PageFaultFrameError::None);
    g_pageFaultEntryArena.Frame.RseDirtyPartitionLoadConfiguration = 0U;
    const Ia64PageFaultEntryControlError rseError =
        rsePrepared ? CommitIa64PageFaultEntryControlForResume(g_pageFaultEntryArena.Control,
                                                               g_pageFaultEntryArena.Frame, 1U, 1U, 13U)
                    : Ia64PageFaultEntryControlError::None;
    ASTERIA_EXPECT(runner, "resume control repeats the exact captured RSE proof", rsePrepared,
                   rseError == Ia64PageFaultEntryControlError::FrameStateMismatch,
                   AcquireIa64PageFaultEntryControlSnapshot(g_pageFaultEntryArena.Control).State ==
                       Ia64PageFaultEntryControlState::Faulted,
                   Ia64PageFaultFrameIsOwned(g_pageFaultEntryArena.Frame));

    constexpr Foundation::UInt64 resumeGeneration{17U};
    const bool resumePrepared =
        PrepareEntryControlCoordinator(observation) &&
        (CommitIa64PageFaultResume(g_pageFaultEntryArena.Frame, 1U) == Ia64PageFaultFrameError::None) &&
        (CommitIa64PageFaultEntryControlForResume(g_pageFaultEntryArena.Control, g_pageFaultEntryArena.Frame, 1U, 1U,
                                                  resumeGeneration) == Ia64PageFaultEntryControlError::None);
    g_pageFaultEntryArena.Frame.VectorOffset = Platform::Ia64::Interrupt::Ia64PageFaultKeyPermissionVectorOffset;
    const Ia64PageFaultEntryControlError vectorError =
        resumePrepared ? ModelCompleteIa64PageFaultEntryControlForRfi(
                             g_pageFaultEntryArena.Control, g_pageFaultEntryArena.Frame, 1U, 1U, resumeGeneration)
                       : Ia64PageFaultEntryControlError::None;
    ASTERIA_EXPECT(runner, "final control completion rejects a changed vector identity", resumePrepared,
                   vectorError == Ia64PageFaultEntryControlError::VectorMismatch,
                   AcquireIa64PageFaultEntryControlSnapshot(g_pageFaultEntryArena.Control).State ==
                       Ia64PageFaultEntryControlState::Faulted,
                   Ia64PageFaultFrameIsOwned(g_pageFaultEntryArena.Frame));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64PageFaultEntryContractTests"};
    Asteria::Test::TestSupportedVectorsAndExactResume(runner);
    Asteria::Test::TestPreCoverValidation(runner);
    Asteria::Test::TestProcessorStateRejections(runner);
    Asteria::Test::TestInterruptionStatusRejections(runner);
    Asteria::Test::TestCoveredFunctionStateRejections(runner);
    Asteria::Test::TestPageFaultFramePristineValidation(runner);
    Asteria::Test::TestRetainedFrameLifecycle(runner);
    Asteria::Test::TestRetainedFrameRseValidation(runner);
    Asteria::Test::TestRetainedFramePublicationRevalidation(runner);
    Asteria::Test::TestRetainedFrameTerminalFailures(runner);
    Asteria::Test::TestEntryControlConfigurationAndVectorClaim(runner);
    Asteria::Test::TestEntryControlCheckedStatesAndSnapshots(runner);
    Asteria::Test::TestEntryControlFrameAndResumeLifecycle(runner);
    Asteria::Test::TestEntryControlPostClaimContradictionsFault(runner);
    return runner.Finish();
}
