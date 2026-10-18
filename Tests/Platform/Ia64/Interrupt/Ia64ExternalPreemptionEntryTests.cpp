// SPDX-License-Identifier: Apache-2.0
#include "Platform/Ia64/Interrupt/Ia64ExternalPreemptionEntry.hpp"

#include "Support/TestContext.hpp"
#include <cstdio>
#include <cstring>

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::UInt64;
using Platform::Ia64::Interrupt::AcquireIa64ExternalPreemptionControlSnapshot;
using Platform::Ia64::Interrupt::AdvanceIa64ExternalPreemptionCapture;
using Platform::Ia64::Interrupt::ArmIa64ExternalPreemptionControlForSentinel;
using Platform::Ia64::Interrupt::BeginIa64ExternalPreemptionCapture;
using Platform::Ia64::Interrupt::ClaimIa64ExternalPreemptionControlCapture;
using Platform::Ia64::Interrupt::CommitIa64ExternalPreemptionControlForRestore;
using Platform::Ia64::Interrupt::CommitIa64ExternalPreemptionResume;
using Platform::Ia64::Interrupt::CompleteIa64ExternalPreemptionTimerAcknowledgement;
using Platform::Ia64::Interrupt::ConfigureIa64ExternalPreemptionControl;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptInstructionSlotMask;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptMaskedTimerVector;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionCaptureRequest;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionCaptureStage;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionControl;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionControlConfiguration;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionControlError;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionControlState;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionCoveredRseError;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionCoordinatorRseBackingStoreSizeBytes;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionCoordinatorStackSizeBytes;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionFrame;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionFrameError;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionMode;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionSentinelFirstRestartInstructionSlot;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionSentinelLastRestartInstructionSlot;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionSentinelObservation;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionTimerEvidence;
using Platform::Ia64::Interrupt::IsIa64ExternalPreemptionSentinelRestartInstructionSlot;
using Platform::Ia64::Interrupt::ModelCompleteIa64ExternalPreemptionControlForRfi;
using Platform::Ia64::Interrupt::ModelPublishIa64ExternalPreemptionControlCapture;
using Platform::Ia64::Interrupt::ModelReleaseIa64ExternalPreemptionFrameForRfi;
using Platform::Ia64::Interrupt::PrepareIa64ExternalPreemptionSentinelResumeProcessorStatus;
using Platform::Ia64::Interrupt::PublishIa64ExternalPreemptionCapture;
using Platform::Ia64::Interrupt::ValidateIa64ExternalPreemptionCoveredRseState;

constexpr UInt64 Deadline{0x1000'2000U};
constexpr UInt64 CurrentProcessorStatus{0x0000'0010'0802'2000ULL};
constexpr UInt64 InterruptedProcessorStatusBase{0x0000'1010'0802'6000ULL};

[[nodiscard]] constexpr UInt64 MakeInterruptedProcessorStatus(const UInt64 instructionSlot) noexcept
{
    return (InterruptedProcessorStatusBase & ~Ia64ExternalInterruptInstructionSlotMask) | (instructionSlot << 41U);
}

constexpr UInt64 InterruptedProcessorStatus{
    MakeInterruptedProcessorStatus(Ia64ExternalPreemptionSentinelLastRestartInstructionSlot)};

struct alignas(4096U) Arena
{
    Ia64ExternalPreemptionControl Control;
    Ia64ExternalPreemptionFrame Frame;
    alignas(4096U) Foundation::UInt8 WorkerStack[64U * 1024U];
    alignas(4096U) Foundation::UInt8 WorkerRbs[64U * 1024U];
    alignas(4096U) Foundation::UInt8 CoordinatorStack[Ia64ExternalPreemptionCoordinatorStackSizeBytes];
    alignas(4096U) Foundation::UInt8 CoordinatorRbs[Ia64ExternalPreemptionCoordinatorRseBackingStoreSizeBytes];
};

Arena g_arena{};

[[nodiscard]] UInt64 AddressOf(const void* const storage) noexcept
{
    return static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(storage));
}

[[nodiscard]] Ia64ExternalPreemptionControlConfiguration ValidConfiguration() noexcept
{
    return Ia64ExternalPreemptionControlConfiguration{
        .ProcessorIndex = 0U,
        .TimerVector = Ia64ExternalInterruptTimerVector,
        .FrameAddress = AddressOf(&g_arena.Frame),
        .WorkerMemoryStackStart = AddressOf(g_arena.WorkerStack),
        .WorkerMemoryStackEnd = AddressOf(g_arena.WorkerStack) + sizeof(g_arena.WorkerStack),
        .WorkerRseBackingStoreStart = AddressOf(g_arena.WorkerRbs),
        .WorkerRseBackingStoreEnd = AddressOf(g_arena.WorkerRbs) + sizeof(g_arena.WorkerRbs),
        .CoordinatorMemoryStackStart = AddressOf(g_arena.CoordinatorStack),
        .CoordinatorMemoryStackEnd = AddressOf(g_arena.CoordinatorStack) + sizeof(g_arena.CoordinatorStack),
        .CoordinatorRseBackingStoreStart = AddressOf(g_arena.CoordinatorRbs),
        .CoordinatorRseBackingStoreEnd = AddressOf(g_arena.CoordinatorRbs) + sizeof(g_arena.CoordinatorRbs),
        .CoordinatorEntryPoint = 0x10'0000U,
        .CoordinatorGlobalPointer = 0x20'0000U,
        .CoordinatorArgument = AddressOf(&g_arena),
        .SentinelEntryPoint = 0x30'0000U,
        .SentinelEnd = 0x30'0010U,
    };
}

[[nodiscard]] Ia64ExternalPreemptionSentinelObservation ValidSentinelObservation() noexcept
{
    return Ia64ExternalPreemptionSentinelObservation{
        .InstructionPointer = 0x30'0000U,
        .CurrentProcessorStatus = CurrentProcessorStatus,
        .StackPointer = AddressOf(g_arena.WorkerStack) + sizeof(g_arena.WorkerStack) - 16U,
        .RseConfiguration = 0U,
        .RseBackingStorePointer = AddressOf(g_arena.WorkerRbs) + 0x180U,
        .RseBackingStoreStorePointer = AddressOf(g_arena.WorkerRbs),
        .ProgrammedDeadlineTicks = Deadline,
    };
}

[[nodiscard]] UInt64 CompleteFrame(Ia64ExternalPreemptionFrame& frame) noexcept
{
    UInt64 token{0U};
    if (BeginIa64ExternalPreemptionCapture(frame,
                                           Ia64ExternalPreemptionCaptureRequest{
                                               .ProcessorIndex = 0U,
                                               .TimerVector = Ia64ExternalInterruptTimerVector,
                                               .ProgrammedDeadlineTicks = Deadline,
                                               .Reserved = 0U,
                                           },
                                           token) != Ia64ExternalPreemptionFrameError::None)
    {
        return 0U;
    }
    frame.RseBackingStorePointer = AddressOf(g_arena.WorkerRbs) + 0x180U;
    frame.InterruptedInstructionPointer = ValidConfiguration().SentinelEntryPoint;
    frame.InterruptedProcessorStatus = InterruptedProcessorStatus;
    frame.CoveredInterruptionFunctionState = UInt64{1U} << 63U;
    frame.CoveredRseBackingStorePointer = frame.RseBackingStorePointer;
    frame.RseDirtyPartitionLoadConfiguration = 0U;
    constexpr Ia64ExternalPreemptionCaptureStage stages[]{
        Ia64ExternalPreemptionCaptureStage::InterruptionState, Ia64ExternalPreemptionCaptureStage::IntegerState,
        Ia64ExternalPreemptionCaptureStage::ApplicationState,  Ia64ExternalPreemptionCaptureStage::FloatingState,
        Ia64ExternalPreemptionCaptureStage::RseTransition,
    };
    for (const Ia64ExternalPreemptionCaptureStage stage : stages)
    {
        if (AdvanceIa64ExternalPreemptionCapture(frame, token, stage) != Ia64ExternalPreemptionFrameError::None)
        {
            return 0U;
        }
    }
    if (CompleteIa64ExternalPreemptionTimerAcknowledgement(
            frame, token,
            Ia64ExternalPreemptionTimerEvidence{
                .ClaimedVector = Ia64ExternalInterruptTimerVector,
                .ObservedTicks = Deadline + 1U,
                .MaskedTimerVector = Ia64ExternalInterruptMaskedTimerVector,
                .EoiCount = 1U,
            }) != Ia64ExternalPreemptionFrameError::None ||
        PublishIa64ExternalPreemptionCapture(frame, token) != Ia64ExternalPreemptionFrameError::None)
    {
        return 0U;
    }
    return token;
}

void ResetArena() noexcept
{
    g_arena = {};
}

void TestConfigurationIsFailureAtomic(TestContext& runner) noexcept
{
    ResetArena();
    Ia64ExternalPreemptionControlConfiguration invalid = ValidConfiguration();
    invalid.ProcessorIndex = 1U;
    Ia64ExternalPreemptionControl before{};
    static_cast<void>(std::memcpy(&before, &g_arena.Control, sizeof(before)));
    const bool invalidProcessor = ConfigureIa64ExternalPreemptionControl(g_arena.Control, invalid) ==
                                  Ia64ExternalPreemptionControlError::InvalidConfiguration;
    invalid = ValidConfiguration();
    invalid.CoordinatorMemoryStackEnd -= 4096U;
    const bool invalidStack = ConfigureIa64ExternalPreemptionControl(g_arena.Control, invalid) ==
                              Ia64ExternalPreemptionControlError::InvalidConfiguration;
    invalid = ValidConfiguration();
    invalid.CoordinatorRseBackingStoreStart = invalid.CoordinatorMemoryStackStart;
    invalid.CoordinatorRseBackingStoreEnd =
        invalid.CoordinatorRseBackingStoreStart + Ia64ExternalPreemptionCoordinatorRseBackingStoreSizeBytes;
    const bool overlap = ConfigureIa64ExternalPreemptionControl(g_arena.Control, invalid) ==
                         Ia64ExternalPreemptionControlError::InvalidConfiguration;
    invalid = ValidConfiguration();
    invalid.CoordinatorMemoryStackStart = invalid.WorkerMemoryStackStart;
    invalid.CoordinatorMemoryStackEnd =
        invalid.CoordinatorMemoryStackStart + Ia64ExternalPreemptionCoordinatorStackSizeBytes;
    const bool workerStackOverlap = ConfigureIa64ExternalPreemptionControl(g_arena.Control, invalid) ==
                                    Ia64ExternalPreemptionControlError::InvalidConfiguration;
    invalid = ValidConfiguration();
    invalid.SentinelEnd = invalid.SentinelEntryPoint;
    const bool invalidSentinel = ConfigureIa64ExternalPreemptionControl(g_arena.Control, invalid) ==
                                 Ia64ExternalPreemptionControlError::InvalidConfiguration;
    invalid = ValidConfiguration();
    invalid.SentinelEnd += 16U;
    const bool oversizedSentinel = ConfigureIa64ExternalPreemptionControl(g_arena.Control, invalid) ==
                                   Ia64ExternalPreemptionControlError::InvalidConfiguration;
    ASTERIA_EXPECT(runner, "invalid BSP, extents, and storage overlap leave control bytes unchanged", invalidProcessor,
                   invalidStack, overlap, workerStackOverlap, invalidSentinel, oversizedSentinel,
                   std::memcmp(&before, &g_arena.Control, sizeof(before)) == 0);

    ASTERIA_EXPECT(runner, "one fresh configuration retains immutable processor-local geometry",
                   ConfigureIa64ExternalPreemptionControl(g_arena.Control, ValidConfiguration()) ==
                       Ia64ExternalPreemptionControlError::None,
                   ConfigureIa64ExternalPreemptionControl(g_arena.Control, ValidConfiguration()) ==
                       Ia64ExternalPreemptionControlError::AlreadyConfigured);
}

void TestSentinelAdmissionChecksDirtyHeadroom(TestContext& runner) noexcept
{
    ResetArena();
    static_cast<void>(ConfigureIa64ExternalPreemptionControl(g_arena.Control, ValidConfiguration()));
    Ia64ExternalPreemptionSentinelObservation invalid = ValidSentinelObservation();
    invalid.CurrentProcessorStatus |= UInt64{1U} << 14U;
    const bool deliveryAlreadyEnabled =
        ArmIa64ExternalPreemptionControlForSentinel(g_arena.Control, g_arena.Frame, invalid) ==
        Ia64ExternalPreemptionControlError::InvalidSentinelState;
    invalid = ValidSentinelObservation();
    invalid.RseConfiguration = 3U;
    const bool wrongRsc = ArmIa64ExternalPreemptionControlForSentinel(g_arena.Control, g_arena.Frame, invalid) ==
                          Ia64ExternalPreemptionControlError::InvalidSentinelState;
    invalid = ValidSentinelObservation();
    invalid.InstructionPointer += 16U;
    const bool outsideSentinel = ArmIa64ExternalPreemptionControlForSentinel(g_arena.Control, g_arena.Frame, invalid) ==
                                 Ia64ExternalPreemptionControlError::InvalidSentinelState;
    invalid = ValidSentinelObservation();
    invalid.StackPointer = AddressOf(g_arena.WorkerStack) - 16U;
    const bool outsideWorkerStack =
        ArmIa64ExternalPreemptionControlForSentinel(g_arena.Control, g_arena.Frame, invalid) ==
        Ia64ExternalPreemptionControlError::InvalidSentinelState;
    invalid = ValidSentinelObservation();
    invalid.RseBackingStorePointer = invalid.RseBackingStoreStorePointer + 16U * 1024U;
    const bool dirtyTooLarge = ArmIa64ExternalPreemptionControlForSentinel(g_arena.Control, g_arena.Frame, invalid) ==
                               Ia64ExternalPreemptionControlError::DirtyPartitionTooLarge;
    const auto unchanged = AcquireIa64ExternalPreemptionControlSnapshot(g_arena.Control);
    ASTERIA_EXPECT(runner, "sentinel rejects wrong PSR, IIP, SP, RSC, and dirty partition before arm",
                   deliveryAlreadyEnabled, wrongRsc, outsideSentinel, outsideWorkerStack, dirtyTooLarge,
                   unchanged.Mode == Ia64ExternalPreemptionMode::LegacyEvidence,
                   unchanged.State == Ia64ExternalPreemptionControlState::Configured,
                   unchanged.ArmedDeadlineTicks == 0U);

    ASTERIA_EXPECT(
        runner, "exact no-lock sentinel release-selects native one-shot mode",
        ArmIa64ExternalPreemptionControlForSentinel(g_arena.Control, g_arena.Frame, ValidSentinelObservation()) ==
            Ia64ExternalPreemptionControlError::None);
    const auto armed = AcquireIa64ExternalPreemptionControlSnapshot(g_arena.Control);
    ASTERIA_EXPECT(runner, "Armed and deadline are complete before native mode is observable",
                   armed.Mode == Ia64ExternalPreemptionMode::BspOneShotPreemption,
                   armed.State == Ia64ExternalPreemptionControlState::Armed, armed.ArmedDeadlineTicks == Deadline,
                   armed.CapturedFrameToken == 0U);
}

void TestSentinelRestartBoundary(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "sentinel restart helper admits exactly RI0 and RI1",
                   Ia64ExternalPreemptionSentinelFirstRestartInstructionSlot == 0U,
                   Ia64ExternalPreemptionSentinelLastRestartInstructionSlot == 1U,
                   IsIa64ExternalPreemptionSentinelRestartInstructionSlot(0U),
                   IsIa64ExternalPreemptionSentinelRestartInstructionSlot(1U),
                   !IsIa64ExternalPreemptionSentinelRestartInstructionSlot(2U),
                   !IsIa64ExternalPreemptionSentinelRestartInstructionSlot(3U));

    ResetArena();
    static_cast<void>(ConfigureIa64ExternalPreemptionControl(g_arena.Control, ValidConfiguration()));
    static_cast<void>(
        ArmIa64ExternalPreemptionControlForSentinel(g_arena.Control, g_arena.Frame, ValidSentinelObservation()));
    Ia64ExternalPreemptionControl beforeRejections{};
    static_cast<void>(std::memcpy(&beforeRejections, &g_arena.Control, sizeof(beforeRejections)));
    const bool landingEndRejected =
        ClaimIa64ExternalPreemptionControlCapture(g_arena.Control, ValidConfiguration().SentinelEnd,
                                                  MakeInterruptedProcessorStatus(0U)) ==
        Ia64ExternalPreemptionControlError::InvalidSentinelState;
    const bool slotTwoRejected =
        ClaimIa64ExternalPreemptionControlCapture(g_arena.Control, ValidConfiguration().SentinelEntryPoint,
                                                  MakeInterruptedProcessorStatus(2U)) ==
        Ia64ExternalPreemptionControlError::InvalidSentinelState;
    const bool slotThreeRejected =
        ClaimIa64ExternalPreemptionControlCapture(g_arena.Control, ValidConfiguration().SentinelEntryPoint,
                                                  MakeInterruptedProcessorStatus(3U)) ==
        Ia64ExternalPreemptionControlError::InvalidSentinelState;
    const bool rejectionsWereAtomic = std::memcmp(&beforeRejections, &g_arena.Control, sizeof(beforeRejections)) == 0;
    const bool slotZeroAccepted = ClaimIa64ExternalPreemptionControlCapture(
                                      g_arena.Control, ValidConfiguration().SentinelEntryPoint,
                                      MakeInterruptedProcessorStatus(0U)) == Ia64ExternalPreemptionControlError::None;

    ResetArena();
    static_cast<void>(ConfigureIa64ExternalPreemptionControl(g_arena.Control, ValidConfiguration()));
    static_cast<void>(
        ArmIa64ExternalPreemptionControlForSentinel(g_arena.Control, g_arena.Frame, ValidSentinelObservation()));
    const bool slotOneAccepted = ClaimIa64ExternalPreemptionControlCapture(
                                     g_arena.Control, ValidConfiguration().SentinelEntryPoint,
                                     MakeInterruptedProcessorStatus(1U)) == Ia64ExternalPreemptionControlError::None;
    ASTERIA_EXPECT(runner,
                   "capture claims LandingStart RI0 or RI1 and rejects RI2, RI3, and "
                   "LandingEnd RI0",
                   landingEndRejected, slotTwoRejected, slotThreeRejected, rejectionsWereAtomic, slotZeroAccepted,
                   slotOneAccepted);
}

void TestCaptureCommitAndRfiLifecycle(TestContext& runner) noexcept
{
    ResetArena();
    static_cast<void>(ConfigureIa64ExternalPreemptionControl(g_arena.Control, ValidConfiguration()));
    static_cast<void>(
        ArmIa64ExternalPreemptionControlForSentinel(g_arena.Control, g_arena.Frame, ValidSentinelObservation()));
    ASTERIA_EXPECT(runner, "entry claims one admitted sentinel restart slot exactly once",
                   ClaimIa64ExternalPreemptionControlCapture(g_arena.Control, ValidConfiguration().SentinelEntryPoint,
                                                             InterruptedProcessorStatus) ==
                       Ia64ExternalPreemptionControlError::None,
                   ClaimIa64ExternalPreemptionControlCapture(g_arena.Control, ValidConfiguration().SentinelEntryPoint,
                                                             InterruptedProcessorStatus) ==
                       Ia64ExternalPreemptionControlError::InvalidState);
    const UInt64 token = CompleteFrame(g_arena.Frame);
    ASTERIA_EXPECT(runner, "entry publication accepts only a complete retained frame", token != 0U,
                   ModelPublishIa64ExternalPreemptionControlCapture(g_arena.Control, g_arena.Frame) ==
                       Ia64ExternalPreemptionControlError::None);
    const auto captured = AcquireIa64ExternalPreemptionControlSnapshot(g_arena.Control);
    ASTERIA_EXPECT(runner, "coordinator state release-follows exact frame token publication",
                   captured.State == Ia64ExternalPreemptionControlState::Coordinator,
                   captured.Mode == Ia64ExternalPreemptionMode::BspOneShotPreemption,
                   captured.CapturedFrameToken == token, captured.CaptureCount == 1U);

    Ia64ExternalPreemptionControl before{};
    static_cast<void>(std::memcpy(&before, &g_arena.Control, sizeof(before)));
    ASTERIA_EXPECT(runner, "wrong restore token retains native mode and coordinator ownership",
                   CommitIa64ExternalPreemptionControlForRestore(g_arena.Control, g_arena.Frame, token + 1U, 7U) ==
                       Ia64ExternalPreemptionControlError::TokenMismatch,
                   std::memcmp(&before, &g_arena.Control, sizeof(before)) == 0);

    ASTERIA_EXPECT(runner, "frame commit precedes exact control restore commit",
                   CommitIa64ExternalPreemptionResume(g_arena.Frame, token) == Ia64ExternalPreemptionFrameError::None,
                   CommitIa64ExternalPreemptionControlForRestore(g_arena.Control, g_arena.Frame, token, 7U) ==
                       Ia64ExternalPreemptionControlError::None);
    const auto committed = AcquireIa64ExternalPreemptionControlSnapshot(g_arena.Control);
    ASTERIA_EXPECT(runner, "one-shot mode is release-disarmed before restore becomes observable",
                   committed.Mode == Ia64ExternalPreemptionMode::LegacyEvidence,
                   committed.State == Ia64ExternalPreemptionControlState::ResumeCommitted,
                   committed.IdleContinuationGeneration == 7U);

    Ia64ExternalPreemptionFrame retainedFrame{};
    static_cast<void>(std::memcpy(&retainedFrame, &g_arena.Frame, sizeof(retainedFrame)));
    constexpr UInt64 admittedRestartSlots[]{
        Ia64ExternalPreemptionSentinelFirstRestartInstructionSlot,
        Ia64ExternalPreemptionSentinelLastRestartInstructionSlot,
    };
    bool admittedSlotsPreserved{true};
    for (const UInt64 instructionSlot : admittedRestartSlots)
    {
        static_cast<void>(std::memcpy(&g_arena.Frame, &retainedFrame, sizeof(g_arena.Frame)));
        g_arena.Frame.InterruptedProcessorStatus = MakeInterruptedProcessorStatus(instructionSlot);
        Ia64ExternalPreemptionFrame frameBeforeNormalization{};
        static_cast<void>(std::memcpy(&frameBeforeNormalization, &g_arena.Frame, sizeof(frameBeforeNormalization)));
        UInt64 normalizedProcessorStatus{0xCAFE'BABE'CAFE'BABEU};
        admittedSlotsPreserved =
            admittedSlotsPreserved &&
            (PrepareIa64ExternalPreemptionSentinelResumeProcessorStatus(g_arena.Control, g_arena.Frame,
                                                                        normalizedProcessorStatus) ==
             Ia64ExternalPreemptionControlError::None) &&
            (normalizedProcessorStatus == (g_arena.Frame.InterruptedProcessorStatus & ~(UInt64{1U} << 14U))) &&
            (std::memcmp(&frameBeforeNormalization, &g_arena.Frame, sizeof(frameBeforeNormalization)) == 0);
    }
    static_cast<void>(std::memcpy(&g_arena.Frame, &retainedFrame, sizeof(g_arena.Frame)));
    ASTERIA_EXPECT(runner, "RI0 and RI1 resume clear only outgoing IPSR.i and retain exact frame bytes",
                   admittedSlotsPreserved);

    UInt64 normalizedProcessorStatus{0xCAFE'BABE'CAFE'BABEU};
    g_arena.Frame.InterruptedInstructionPointer = ValidConfiguration().SentinelEnd;
    g_arena.Frame.InterruptedProcessorStatus = MakeInterruptedProcessorStatus(0U);
    const bool landingEndSlotZeroRejected = (PrepareIa64ExternalPreemptionSentinelResumeProcessorStatus(
                                                 g_arena.Control, g_arena.Frame, normalizedProcessorStatus) ==
                                             Ia64ExternalPreemptionControlError::InvalidSentinelState);
    static_cast<void>(std::memcpy(&g_arena.Frame, &retainedFrame, sizeof(g_arena.Frame)));
    g_arena.Frame.InterruptedProcessorStatus &= ~(UInt64{1U} << 14U);
    const bool disabledInterruptRejected = PrepareIa64ExternalPreemptionSentinelResumeProcessorStatus(
                                               g_arena.Control, g_arena.Frame, normalizedProcessorStatus) ==
                                           Ia64ExternalPreemptionControlError::InvalidSentinelState;
    g_arena.Frame.InterruptedProcessorStatus = MakeInterruptedProcessorStatus(2U);
    const bool slotTwoRejected = PrepareIa64ExternalPreemptionSentinelResumeProcessorStatus(
                                     g_arena.Control, g_arena.Frame, normalizedProcessorStatus) ==
                                 Ia64ExternalPreemptionControlError::InvalidSentinelState;
    g_arena.Frame.InterruptedProcessorStatus = MakeInterruptedProcessorStatus(3U);
    const bool slotThreeRejected = PrepareIa64ExternalPreemptionSentinelResumeProcessorStatus(
                                       g_arena.Control, g_arena.Frame, normalizedProcessorStatus) ==
                                   Ia64ExternalPreemptionControlError::InvalidSentinelState;
    static_cast<void>(std::memcpy(&g_arena.Frame, &retainedFrame, sizeof(g_arena.Frame)));
    ASTERIA_EXPECT(runner, "resume rejects RI2, RI3, disabled delivery, and LandingEnd RI0 atomically",
                   landingEndSlotZeroRejected, disabledInterruptRejected, slotTwoRejected, slotThreeRejected,
                   normalizedProcessorStatus == 0xCAFE'BABE'CAFE'BABEU,
                   std::memcmp(&retainedFrame, &g_arena.Frame, sizeof(retainedFrame)) == 0);

    ASTERIA_EXPECT(runner, "final leaf consumes exact continuation generation and frame token",
                   ModelCompleteIa64ExternalPreemptionControlForRfi(g_arena.Control, token, 8U) ==
                       Ia64ExternalPreemptionControlError::TokenMismatch,
                   ModelCompleteIa64ExternalPreemptionControlForRfi(g_arena.Control, token, 7U) ==
                       Ia64ExternalPreemptionControlError::None,
                   ModelReleaseIa64ExternalPreemptionFrameForRfi(g_arena.Frame, token) ==
                       Ia64ExternalPreemptionFrameError::None);
    const auto complete = AcquireIa64ExternalPreemptionControlSnapshot(g_arena.Control);
    ASTERIA_EXPECT(runner, "rfi completion leaves the reusable corridor disarmed in legacy mode",
                   complete.State == Ia64ExternalPreemptionControlState::Complete,
                   complete.Mode == Ia64ExternalPreemptionMode::LegacyEvidence, complete.ArmedDeadlineTicks == 0U,
                   complete.RestoreCount == 1U);
}

void TestFrameOwnershipAndPublicationRejections(TestContext& runner) noexcept
{
    ResetArena();
    static_cast<void>(ConfigureIa64ExternalPreemptionControl(g_arena.Control, ValidConfiguration()));
    UInt64 token{0U};
    static_cast<void>(BeginIa64ExternalPreemptionCapture(g_arena.Frame,
                                                         Ia64ExternalPreemptionCaptureRequest{
                                                             .ProcessorIndex = 0U,
                                                             .TimerVector = Ia64ExternalInterruptTimerVector,
                                                             .ProgrammedDeadlineTicks = Deadline,
                                                             .Reserved = 0U,
                                                         },
                                                         token));
    ASTERIA_EXPECT(
        runner, "capture ownership prevents arming over an incomplete frame",
        ArmIa64ExternalPreemptionControlForSentinel(g_arena.Control, g_arena.Frame, ValidSentinelObservation()) ==
            Ia64ExternalPreemptionControlError::FrameOwned);

    ResetArena();
    static_cast<void>(ConfigureIa64ExternalPreemptionControl(g_arena.Control, ValidConfiguration()));
    static_cast<void>(
        ArmIa64ExternalPreemptionControlForSentinel(g_arena.Control, g_arena.Frame, ValidSentinelObservation()));
    static_cast<void>(ClaimIa64ExternalPreemptionControlCapture(
        g_arena.Control, ValidSentinelObservation().InstructionPointer, InterruptedProcessorStatus));
    Ia64ExternalPreemptionControl before{};
    static_cast<void>(std::memcpy(&before, &g_arena.Control, sizeof(before)));
    ASTERIA_EXPECT(runner, "control cannot publish before the complete frame token exists",
                   ModelPublishIa64ExternalPreemptionControlCapture(g_arena.Control, g_arena.Frame) ==
                       Ia64ExternalPreemptionControlError::FrameStateMismatch,
                   std::memcmp(&before, &g_arena.Control, sizeof(before)) == 0);
}

void TestCoveredRsePayloadMustBeOverwrittenOnReuse(TestContext& runner) noexcept
{
    ResetArena();
    UInt64 firstToken = CompleteFrame(g_arena.Frame);
    const bool firstReleased =
        (firstToken != 0U) &&
        (CommitIa64ExternalPreemptionResume(g_arena.Frame, firstToken) == Ia64ExternalPreemptionFrameError::None) &&
        (ModelReleaseIa64ExternalPreemptionFrameForRfi(g_arena.Frame, firstToken) ==
         Ia64ExternalPreemptionFrameError::None);
    g_arena.Frame.CoveredInterruptionFunctionState = (UInt64{1U} << 63U) | 4U;
    g_arena.Frame.RseBackingStorePointer = AddressOf(g_arena.WorkerRbs) + 0x180U;
    g_arena.Frame.CoveredRseBackingStorePointer = 0xDEAD'BEEF'0000U;
    g_arena.Frame.RseDirtyPartitionLoadConfiguration = 0U;
    Ia64ExternalPreemptionFrame beforeFailedBegin{};
    static_cast<void>(std::memcpy(&beforeFailedBegin, &g_arena.Frame, sizeof(beforeFailedBegin)));
    UInt64 rejectedToken{0U};
    const bool failedBeginUnchanged =
        (BeginIa64ExternalPreemptionCapture(
             g_arena.Frame,
             Ia64ExternalPreemptionCaptureRequest{
                 .ProcessorIndex = Platform::Ia64::Interrupt::Ia64ExternalInterruptMaximumProcessorCount,
                 .TimerVector = Ia64ExternalInterruptTimerVector,
                 .ProgrammedDeadlineTicks = Deadline,
                 .Reserved = 0U,
             },
             rejectedToken) == Ia64ExternalPreemptionFrameError::InvalidRequest) &&
        (rejectedToken == 0U) && (std::memcmp(&beforeFailedBegin, &g_arena.Frame, sizeof(beforeFailedBegin)) == 0);

    static_cast<void>(ConfigureIa64ExternalPreemptionControl(g_arena.Control, ValidConfiguration()));
    static_cast<void>(
        ArmIa64ExternalPreemptionControlForSentinel(g_arena.Control, g_arena.Frame, ValidSentinelObservation()));
    static_cast<void>(ClaimIa64ExternalPreemptionControlCapture(
        g_arena.Control, ValidSentinelObservation().InstructionPointer, InterruptedProcessorStatus));
    UInt64 secondToken{0U};
    const bool secondBegan = BeginIa64ExternalPreemptionCapture(g_arena.Frame,
                                                                Ia64ExternalPreemptionCaptureRequest{
                                                                    .ProcessorIndex = 0U,
                                                                    .TimerVector = Ia64ExternalInterruptTimerVector,
                                                                    .ProgrammedDeadlineTicks = Deadline,
                                                                    .Reserved = 0U,
                                                                },
                                                                secondToken) == Ia64ExternalPreemptionFrameError::None;
    constexpr Ia64ExternalPreemptionCaptureStage stages[]{
        Ia64ExternalPreemptionCaptureStage::InterruptionState, Ia64ExternalPreemptionCaptureStage::IntegerState,
        Ia64ExternalPreemptionCaptureStage::ApplicationState,  Ia64ExternalPreemptionCaptureStage::FloatingState,
        Ia64ExternalPreemptionCaptureStage::RseTransition,
    };
    bool advanced{secondBegan};
    for (const Ia64ExternalPreemptionCaptureStage stage : stages)
    {
        advanced = advanced && (AdvanceIa64ExternalPreemptionCapture(g_arena.Frame, secondToken, stage) ==
                                Ia64ExternalPreemptionFrameError::None);
    }
    advanced =
        advanced &&
        (CompleteIa64ExternalPreemptionTimerAcknowledgement(
             g_arena.Frame, secondToken,
             Ia64ExternalPreemptionTimerEvidence{
                 .ClaimedVector = Ia64ExternalInterruptTimerVector,
                 .ObservedTicks = Deadline + 1U,
                 .MaskedTimerVector = Ia64ExternalInterruptMaskedTimerVector,
                 .EoiCount = 1U,
             }) == Ia64ExternalPreemptionFrameError::None) &&
        (PublishIa64ExternalPreemptionCapture(g_arena.Frame, secondToken) == Ia64ExternalPreemptionFrameError::None);
    Ia64ExternalPreemptionControl beforeRejectedPublish{};
    static_cast<void>(std::memcpy(&beforeRejectedPublish, &g_arena.Control, sizeof(beforeRejectedPublish)));
    const bool staleRejected = ModelPublishIa64ExternalPreemptionControlCapture(g_arena.Control, g_arena.Frame) ==
                               Ia64ExternalPreemptionControlError::FrameStateMismatch;
    const bool controlUnchanged =
        std::memcmp(&beforeRejectedPublish, &g_arena.Control, sizeof(beforeRejectedPublish)) == 0;

    g_arena.Frame.CoveredInterruptionFunctionState = UInt64{1U} << 63U;
    g_arena.Frame.CoveredRseBackingStorePointer = g_arena.Frame.RseBackingStorePointer;
    ASTERIA_EXPECT(runner, "a reused frame cannot publish stale covered-BSP payload", firstReleased,
                   failedBeginUnchanged, secondToken > firstToken, advanced, staleRejected, controlUnchanged,
                   ModelPublishIa64ExternalPreemptionControlCapture(g_arena.Control, g_arena.Frame) ==
                       Ia64ExternalPreemptionControlError::None);
}

[[nodiscard]] constexpr UInt64 MakeCoveredIfs(const UInt64 sof, const UInt64 sol, const UInt64 sor) noexcept
{
    return (UInt64{1U} << 63U) | sof | (sol << 7U) | ((sor / 8U) << 14U);
}

void TestCoveredRseValidation(TestContext& runner) noexcept
{
    ResetArena();
    static_cast<void>(ConfigureIa64ExternalPreemptionControl(g_arena.Control, ValidConfiguration()));

    Ia64ExternalPreemptionFrame frame{};
    frame.RseBackingStorePointer = AddressOf(g_arena.WorkerRbs) + 0x180U;
    frame.CoveredInterruptionFunctionState = MakeCoveredIfs(8U, 4U, 8U);
    frame.CoveredRseBackingStorePointer = frame.RseBackingStorePointer + 64U;
    frame.RseDirtyPartitionLoadConfiguration = 0x80U << 16U;
    ASTERIA_EXPECT(runner, "a legal covered CFM advances BSP without an RNAT crossing",
                   ValidateIa64ExternalPreemptionCoveredRseState(g_arena.Control, frame) ==
                       Ia64ExternalPreemptionCoveredRseError::None);

    Ia64ExternalPreemptionFrame crossing = frame;
    crossing.RseBackingStorePointer = AddressOf(g_arena.WorkerRbs) + (61U * 8U);
    crossing.CoveredInterruptionFunctionState = MakeCoveredIfs(4U, 4U, 0U);
    crossing.CoveredRseBackingStorePointer = crossing.RseBackingStorePointer + (5U * 8U);
    ASTERIA_EXPECT(runner, "covered BSP advancement counts an intervening RNAT slot",
                   ValidateIa64ExternalPreemptionCoveredRseState(g_arena.Control, crossing) ==
                       Ia64ExternalPreemptionCoveredRseError::None);

    Ia64ExternalPreemptionFrame invalid = crossing;
    invalid.CoveredRseBackingStorePointer -= 8U;
    const bool missingRnatSkip = ValidateIa64ExternalPreemptionCoveredRseState(g_arena.Control, invalid) ==
                                 Ia64ExternalPreemptionCoveredRseError::CoveredPointerMismatch;
    invalid = frame;
    invalid.CoveredInterruptionFunctionState |= UInt64{1U} << 40U;
    const bool reservedIfs = ValidateIa64ExternalPreemptionCoveredRseState(g_arena.Control, invalid) ==
                             Ia64ExternalPreemptionCoveredRseError::InvalidFunctionState;
    invalid = frame;
    invalid.CoveredInterruptionFunctionState = MakeCoveredIfs(97U, 0U, 0U);
    const bool oversizedFrame = ValidateIa64ExternalPreemptionCoveredRseState(g_arena.Control, invalid) ==
                                Ia64ExternalPreemptionCoveredRseError::InvalidFunctionState;
    invalid = frame;
    invalid.RseDirtyPartitionLoadConfiguration = UInt64{1U} << 15U;
    const bool malformedLoadrs = ValidateIa64ExternalPreemptionCoveredRseState(g_arena.Control, invalid) ==
                                 Ia64ExternalPreemptionCoveredRseError::InvalidLoadConfiguration;
    ASTERIA_EXPECT(runner, "reserved CFM, wrong RNAT advance, and malformed loadrs are rejected", missingRnatSkip,
                   reservedIfs, oversizedFrame, malformedLoadrs);

    invalid = frame;
    invalid.RseBackingStorePointer = AddressOf(g_arena.WorkerRbs) + sizeof(g_arena.WorkerRbs) - 24U;
    invalid.CoveredInterruptionFunctionState = MakeCoveredIfs(3U, 0U, 0U);
    invalid.CoveredRseBackingStorePointer = invalid.RseBackingStorePointer + 32U;
    ASTERIA_EXPECT(runner, "cover cannot advance a near-end BSP beyond the worker RBS",
                   ValidateIa64ExternalPreemptionCoveredRseState(g_arena.Control, invalid) ==
                       Ia64ExternalPreemptionCoveredRseError::CoveredPointerOutsideWorkerRange);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64ExternalPreemptionEntryTests"};
    Asteria::Test::TestConfigurationIsFailureAtomic(runner);
    Asteria::Test::TestSentinelAdmissionChecksDirtyHeadroom(runner);
    Asteria::Test::TestSentinelRestartBoundary(runner);
    Asteria::Test::TestCaptureCommitAndRfiLifecycle(runner);
    Asteria::Test::TestFrameOwnershipAndPublicationRejections(runner);
    Asteria::Test::TestCoveredRsePayloadMustBeOverwrittenOnReuse(runner);
    Asteria::Test::TestCoveredRseValidation(runner);
    return runner.Finish();
}
