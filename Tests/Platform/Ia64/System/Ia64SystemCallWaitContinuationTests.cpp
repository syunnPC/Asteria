// SPDX-License-Identifier: Apache-2.0
#include "Platform/Ia64/System/Ia64SystemCallWaitContinuation.hpp"

#include <atomic>
#include <cstdio>
#include <thread>

#include "Kernel/Object/ObjectRegistry.hpp"
#include "Platform/Ia64/System/Ia64SystemCallEntry.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt64;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::SchedulingContextId;
using Kernel::Scheduler::ThreadId;
using Kernel::System::SystemCallError;
using Kernel::System::SystemCallResponse;
using Platform::Ia64::Scheduler::Ia64KernelRegisterState;
using Platform::Ia64::Scheduler::Ia64UserModeSupervisorContinuation;
using Platform::Ia64::Scheduler::Ia64UserModeSupervisorContinuationOwner;
using Platform::Ia64::Scheduler::Ia64UserModeSupervisorContinuationState;
using Platform::Ia64::Scheduler::Ia64UserModeVirtualRange;
using Platform::Ia64::System::Ia64SystemCallBreakImmediate;
using Platform::Ia64::System::Ia64SystemCallCurrentProcessorStatusValue;
using Platform::Ia64::System::Ia64SystemCallEntryFrame;
using Platform::Ia64::System::Ia64SystemCallEntryFrameState;
using Platform::Ia64::System::Ia64SystemCallIfsValidMask;
using Platform::Ia64::System::Ia64SystemCallInstructionSlotShift;
using Platform::Ia64::System::Ia64SystemCallInterruptedProcessorStatusValue;
using Platform::Ia64::System::Ia64SystemCallKernelRseConfiguration;
using Platform::Ia64::System::Ia64SystemCallMemoryStackSizeBytes;
using Platform::Ia64::System::Ia64SystemCallRseBackingStoreSizeBytes;
using Platform::Ia64::System::Ia64SystemCallUserRseConfiguration;
using Platform::Ia64::System::Ia64SystemCallWaitContinuation;
using Platform::Ia64::System::Ia64SystemCallWaitContinuationConfiguration;
using Platform::Ia64::System::Ia64SystemCallWaitContinuationError;
using Platform::Ia64::System::Ia64SystemCallWaitContinuationSnapshot;
using Platform::Ia64::System::Ia64SystemCallWaitContinuationState;
using Platform::Ia64::System::Ia64SystemCallWaitCorrelation;
using Platform::Ia64::System::Ia64SystemCallWaitEpochPolicy;
using Platform::Ia64::System::Ia64SystemCallWaitResponseResult;
using Platform::Ia64::System::Ia64SystemCallWaitSupervisorReceipt;
using Platform::Ia64::System::Ia64SystemCallWaitSupervisorResumeSite;
using Platform::Synchronization::SpinLock;

constexpr Size SupervisorStorageBytes{16U * 1024U};
constexpr UInt64 ResponseTag{0xA57E'0000'0000'0042ULL};
constexpr UInt64 ResponseRegistrationSequence{17U};

alignas(4096U) UInt8 g_systemCallMemoryStack[Ia64SystemCallMemoryStackSizeBytes]{};
alignas(4096U) UInt8 g_systemCallRseBackingStore[Ia64SystemCallRseBackingStoreSizeBytes]{};
alignas(4096U) UInt8 g_supervisorMemoryStack[SupervisorStorageBytes]{};
alignas(4096U) UInt8 g_supervisorRseBackingStore[SupervisorStorageBytes]{};

std::atomic<Size> g_rawSwitchCount{0U};

[[nodiscard]] UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
}

[[nodiscard]] constexpr Ia64SystemCallEntryFrame MakeActiveFrame() noexcept
{
    Ia64SystemCallEntryFrame frame{};
    frame.State = Ia64SystemCallEntryFrameState::Active;
    frame.CurrentProcessorStatus = Ia64SystemCallCurrentProcessorStatusValue;
    frame.InterruptedInstructionPointer = 0x0040'0000U;
    frame.InterruptedProcessorStatus =
        Ia64SystemCallInterruptedProcessorStatusValue | (UInt64{1U} << Ia64SystemCallInstructionSlotShift);
    frame.BreakImmediate = Ia64SystemCallBreakImmediate;
    frame.CapturedUserInterruptionFunctionState = Ia64SystemCallIfsValidMask | 6U;
    frame.UserRseConfiguration = Ia64SystemCallUserRseConfiguration;
    return frame;
}

class Fixture final
{
public:
    Fixture() noexcept : Registry{RegistryLock}
    {
        Supervisor.MemoryStack = Ia64UserModeVirtualRange{
            .VirtualStart = AddressOf(g_supervisorMemoryStack),
            .SizeBytes = sizeof(g_supervisorMemoryStack),
        };
        Supervisor.RseBackingStore = Ia64UserModeVirtualRange{
            .VirtualStart = AddressOf(g_supervisorRseBackingStore),
            .SizeBytes = sizeof(g_supervisorRseBackingStore),
        };
        Configuration = Ia64SystemCallWaitContinuationConfiguration{
            .EntryFrame = &Frame,
            .SupervisorContinuation = &Supervisor,
            .SystemCallMemoryStack =
                {
                    .VirtualStart = AddressOf(g_systemCallMemoryStack),
                    .SizeBytes = sizeof(g_systemCallMemoryStack),
                },
            .SystemCallRseBackingStore =
                {
                    .VirtualStart = AddressOf(g_systemCallRseBackingStore),
                    .SizeBytes = sizeof(g_systemCallRseBackingStore),
                },
            .Processor = CpuId{0U},
        };
        InitializeError = Continuation.Initialize(Configuration);
    }

    void ActivateAndRetainSupervisor() noexcept
    {
        Frame = MakeActiveFrame();
        Supervisor.Registers = {};
        Supervisor.Registers.InstructionPointer = 0x2000U;
        Supervisor.Registers.StackPointer =
            Supervisor.MemoryStack.VirtualStart + Supervisor.MemoryStack.SizeBytes - 32U;
        Supervisor.Registers.RseBackingStorePointer = Supervisor.RseBackingStore.VirtualStart;
        Supervisor.State = Ia64UserModeSupervisorContinuationState::Retained;
    }

    [[nodiscard]] Ia64SystemCallWaitCorrelation Correlation(const UInt64 runSequence = 1U) noexcept
    {
        return Ia64SystemCallWaitCorrelation{
            .Thread = ThreadId{Registry, ObjectId{0x101U}},
            .SchedulingContext = SchedulingContextId{Registry, ObjectId{0x102U}},
            .RunSequence = runSequence,
            .Processor = CpuId{0U},
        };
    }

    SpinLock RegistryLock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    alignas(16U) Ia64UserModeSupervisorContinuation Supervisor{};
    alignas(16U) Ia64SystemCallEntryFrame Frame{};
    Ia64SystemCallWaitContinuation Continuation;
    Ia64SystemCallWaitContinuationConfiguration Configuration{};
    Ia64SystemCallWaitContinuationError InitializeError{Ia64SystemCallWaitContinuationError::None};
};

[[nodiscard]] bool RegisterStateIsZero(const Ia64KernelRegisterState& registers) noexcept
{
    const auto* const bytes = reinterpret_cast<const UInt8*>(&registers);
    for (Size index{0U}; index < sizeof(registers); ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

enum class SystemCallBoundary : UInt8
{
    WithinStorage,
    BelowMemoryStack,
};

enum class RawSwitchSide : Foundation::UInt32
{
    SystemCall,
    Supervisor,
    Detached,
};

class RawSwitchModel;

RawSwitchModel* g_rawSwitchModel{nullptr};

/// Hosted model of the native P64 raw switch between one fixture's syscall
/// record and its retained supervisor record. The syscall side is the test
/// thread. The first switch to the supervisor record starts one host thread
/// that runs the scenario's supervisor script from the LaunchResumable return
/// site; later switches alternate strictly between the two threads through
/// one atomic turn word. A switch saves a modeled call boundary into current,
/// as the native switch does, and returns once a later switch resumes
/// current. When the supervisor script ends, or the model is destroyed, the
/// suspended side returns without a resume, which the continuation must
/// reject.
class RawSwitchModel final
{
public:
    template <typename TScript>
    RawSwitchModel(Fixture& fixture, TScript& script,
                   const SystemCallBoundary boundary = SystemCallBoundary::WithinStorage) noexcept
        : m_fixture{fixture}, m_script{&InvokeScript<TScript>}, m_scriptContext{&script}, m_boundary{boundary}
    {
        g_rawSwitchModel = this;
    }

    ~RawSwitchModel() noexcept
    {
        m_turn.store(RawSwitchSide::Detached);
        m_turn.notify_all();
        if (m_supervisorStarted)
        {
            m_supervisor.join();
        }
        g_rawSwitchModel = nullptr;
    }

    RawSwitchModel(const RawSwitchModel&) = delete;
    RawSwitchModel& operator=(const RawSwitchModel&) = delete;
    RawSwitchModel(RawSwitchModel&&) = delete;
    RawSwitchModel& operator=(RawSwitchModel&&) = delete;

    void Switch(Ia64KernelRegisterState& current) noexcept
    {
        const bool fromSupervisor = &current == &m_fixture.Supervisor.Registers;
        if (fromSupervisor)
        {
            SaveSupervisorBoundary(current);
        }
        else
        {
            m_systemCallRecord = &current;
            SaveSystemCallBoundary(current);
        }

        const RawSwitchSide self = fromSupervisor ? RawSwitchSide::Supervisor : RawSwitchSide::SystemCall;
        RawSwitchSide expected = self;
        if (!m_turn.compare_exchange_strong(expected,
                                            fromSupervisor ? RawSwitchSide::SystemCall : RawSwitchSide::Supervisor))
        {
            return;
        }
        m_turn.notify_all();
        if (!fromSupervisor && !m_supervisorStarted)
        {
            m_supervisorStarted = true;
            m_supervisor = std::thread{&RawSwitchModel::RunSupervisor, this};
        }
        WaitForTurn(self);
    }

    /// The exact syscall record that the continuation handed to the switch.
    [[nodiscard]] Ia64KernelRegisterState* GetSystemCallRecord() const noexcept
    {
        return m_systemCallRecord;
    }

private:
    template <typename TScript> static void InvokeScript(void* const script) noexcept
    {
        (*static_cast<TScript*>(script))();
    }

    void WaitForTurn(const RawSwitchSide side) noexcept
    {
        RawSwitchSide observed = m_turn.load();
        while ((observed != side) && (observed != RawSwitchSide::Detached))
        {
            m_turn.wait(observed);
            observed = m_turn.load();
        }
    }

    void RunSupervisor() noexcept
    {
        WaitForTurn(RawSwitchSide::Supervisor);
        m_script(m_scriptContext);
        m_turn.store(RawSwitchSide::Detached);
        m_turn.notify_all();
    }

    void SaveSystemCallBoundary(Ia64KernelRegisterState& record) noexcept
    {
        const Ia64UserModeVirtualRange& stack = m_fixture.Configuration.SystemCallMemoryStack;
        record = {};
        record.InstructionPointer = 0x1000U;
        record.StackPointer = m_boundary == SystemCallBoundary::WithinStorage
                                  ? stack.VirtualStart + stack.SizeBytes - 32U
                                  : stack.VirtualStart - 16U;
        record.RseBackingStorePointer = m_fixture.Configuration.SystemCallRseBackingStore.VirtualStart;
        record.RseConfiguration = Ia64SystemCallKernelRseConfiguration;
    }

    void SaveSupervisorBoundary(Ia64KernelRegisterState& record) noexcept
    {
        const Ia64UserModeSupervisorContinuation& supervisor = m_fixture.Supervisor;
        record = {};
        record.InstructionPointer = 0x2000U;
        record.StackPointer = supervisor.MemoryStack.VirtualStart + supervisor.MemoryStack.SizeBytes - 32U;
        record.RseBackingStorePointer = supervisor.RseBackingStore.VirtualStart;
    }

    Fixture& m_fixture;
    void (*m_script)(void*) noexcept;
    void* m_scriptContext;
    SystemCallBoundary m_boundary;
    std::atomic<RawSwitchSide> m_turn{RawSwitchSide::SystemCall};
    std::thread m_supervisor{};
    bool m_supervisorStarted{false};
    Ia64KernelRegisterState* m_systemCallRecord{nullptr};
};

/// Reserves and commits one wait for runSequence, then suspends the syscall
/// through the raw switch. The result is the response returned when the
/// supervisor resumes the syscall, or the rejection of a spurious return.
[[nodiscard]] Ia64SystemCallWaitResponseResult SuspendCommittedWait(Fixture& fixture, const UInt64 runSequence,
                                                                    bool& committed) noexcept
{
    const auto correlation = fixture.Correlation(runSequence);
    const auto reserved = fixture.Continuation.ReserveEntry(fixture.Frame, correlation);
    const auto commit = fixture.Continuation.CommitPrepared(fixture.Frame, correlation);
    committed = reserved.HasValue() && commit.HasValue();
    return fixture.Continuation.SuspendToSupervisor(fixture.Frame);
}

[[nodiscard]] bool SupervisorContinuationsEqual(const Ia64UserModeSupervisorContinuation& left,
                                                const Ia64UserModeSupervisorContinuation& right) noexcept
{
    const auto* const leftBytes = reinterpret_cast<const UInt8*>(&left);
    const auto* const rightBytes = reinterpret_cast<const UInt8*>(&right);
    for (Size index{0U}; index < sizeof(left); ++index)
    {
        if (leftBytes[index] != rightBytes[index])
        {
            return false;
        }
    }
    return true;
}

void TestInitialization(TestContext& runner) noexcept
{
    Fixture fixture;
    const auto snapshot = fixture.Continuation.GetSnapshot();
    ASTERIA_EXPECT(runner, "initialization publishes one idle slot without actionable state",
                   fixture.InitializeError == Ia64SystemCallWaitContinuationError::None,
                   snapshot.State == Ia64SystemCallWaitContinuationState::Idle, snapshot.Epoch == 0U,
                   snapshot.EntryFrameRetained == 0U, snapshot.SystemCallRecordActionable == 0U,
                   fixture.Supervisor.Owner == Ia64UserModeSupervisorContinuationOwner::None);
    ASTERIA_EXPECT(runner, "the exact storage configuration is single-publication",
                   fixture.Continuation.Initialize(fixture.Configuration) ==
                       Ia64SystemCallWaitContinuationError::AlreadyInitialized);

    Fixture invalidProcessor;
    Ia64SystemCallWaitContinuation uninitialized;
    auto configuration = invalidProcessor.Configuration;
    configuration.Processor = CpuId{};
    ASTERIA_EXPECT(runner, "an invalid owning processor is rejected atomically",
                   uninitialized.Initialize(configuration) == Ia64SystemCallWaitContinuationError::InvalidProcessor);

    Ia64SystemCallWaitContinuation overlapping;
    configuration = invalidProcessor.Configuration;
    configuration.SystemCallRseBackingStore = configuration.SystemCallMemoryStack;
    configuration.SystemCallRseBackingStore.SizeBytes = Ia64SystemCallRseBackingStoreSizeBytes;
    ASTERIA_EXPECT(runner, "syscall stack and RSE storage may not overlap",
                   overlapping.Initialize(configuration) != Ia64SystemCallWaitContinuationError::None);

    Ia64SystemCallWaitContinuation activeFrameOwner;
    invalidProcessor.Frame = MakeActiveFrame();
    ASTERIA_EXPECT(runner, "configuration binds an inactive exact frame before publication",
                   activeFrameOwner.Initialize(invalidProcessor.Configuration) ==
                       Ia64SystemCallWaitContinuationError::InvalidEntryFrame);

    Ia64SystemCallWaitContinuation foreignOwner;
    invalidProcessor.Frame = {};
    invalidProcessor.Supervisor.Owner = Ia64UserModeSupervisorContinuationOwner::PageFault;
    ASTERIA_EXPECT(runner, "configuration rejects a supervisor continuation owned by page fault",
                   foreignOwner.Initialize(invalidProcessor.Configuration) ==
                       Ia64SystemCallWaitContinuationError::InvalidSupervisorContinuation);
}

void TestReservationAndCancellation(TestContext& runner) noexcept
{
    Fixture fixture;
    fixture.Frame = MakeActiveFrame();
    const auto correlation = fixture.Correlation();
    const auto beforeLaunch = fixture.Continuation.ReserveEntry(fixture.Frame, correlation);
    ASTERIA_EXPECT(runner, "a syscall cannot reserve the slot before launch retained a supervisor",
                   !beforeLaunch.HasValue(),
                   beforeLaunch.GetError().Error == Ia64SystemCallWaitContinuationError::InvalidSupervisorState,
                   fixture.Continuation.GetSnapshot().State == Ia64SystemCallWaitContinuationState::Idle);

    fixture.ActivateAndRetainSupervisor();
    fixture.Supervisor.Owner = Ia64UserModeSupervisorContinuationOwner::PageFault;
    const auto foreignOwner = fixture.Continuation.ReserveEntry(fixture.Frame, correlation);
    ASTERIA_EXPECT(runner, "reservation cannot steal a page-fault-owned supervisor continuation",
                   !foreignOwner.HasValue(),
                   foreignOwner.GetError().Error == Ia64SystemCallWaitContinuationError::InvalidSupervisorState,
                   fixture.Supervisor.Owner == Ia64UserModeSupervisorContinuationOwner::PageFault);
    fixture.Supervisor.Owner = Ia64UserModeSupervisorContinuationOwner::None;
    Ia64SystemCallEntryFrame foreignFrame{fixture.Frame};
    const auto foreign = fixture.Continuation.ReserveEntry(foreignFrame, correlation);
    ASTERIA_EXPECT(runner, "same-content foreign frame storage is rejected by exact identity", !foreign.HasValue(),
                   foreign.GetError().Error == Ia64SystemCallWaitContinuationError::InvalidEntryFrame);

    auto invalidCorrelation = correlation;
    invalidCorrelation.Thread = ThreadId{};
    const auto invalid = fixture.Continuation.ReserveEntry(fixture.Frame, invalidCorrelation);
    ASTERIA_EXPECT(runner, "invalid scheduling identity cannot reserve the slot", !invalid.HasValue(),
                   invalid.GetError().Error == Ia64SystemCallWaitContinuationError::InvalidCorrelation);

    const auto reserved = fixture.Continuation.ReserveEntry(fixture.Frame, correlation);
    ASTERIA_EXPECT(runner, "the first reservation preflights epoch one without consuming it", reserved.HasValue(),
                   reserved.GetValue().PendingEpoch == 1U,
                   fixture.Continuation.GetSnapshot().State == Ia64SystemCallWaitContinuationState::EntryReserved,
                   fixture.Supervisor.Owner == Ia64UserModeSupervisorContinuationOwner::SystemCallWait);

    auto foreignCorrelation = correlation;
    foreignCorrelation.RunSequence = 2U;
    ASTERIA_EXPECT(runner, "foreign cancellation changes no retained reservation byte",
                   fixture.Continuation.CancelReservedEntry(fixture.Frame, foreignCorrelation) ==
                       Ia64SystemCallWaitContinuationError::InvalidCorrelation,
                   fixture.Continuation.GetSnapshot().PendingEpoch == 1U);
    ASTERIA_EXPECT(runner, "the exact reservation cancels before WaitSet preparation commits",
                   fixture.Continuation.CancelReservedEntry(fixture.Frame, correlation) ==
                       Ia64SystemCallWaitContinuationError::None,
                   fixture.Supervisor.Owner == Ia64UserModeSupervisorContinuationOwner::None);

    const auto retried = fixture.Continuation.ReserveEntry(fixture.Frame, correlation);
    ASTERIA_EXPECT(runner, "cancellation does not burn the nonwrapping suspension epoch", retried.HasValue(),
                   retried.GetValue().PendingEpoch == 1U, fixture.Continuation.GetSnapshot().Epoch == 0U);

    const auto wrongCommit = fixture.Continuation.CommitPrepared(fixture.Frame, foreignCorrelation);
    ASTERIA_EXPECT(runner, "foreign WaitSet correlation cannot commit the reserved epoch", !wrongCommit.HasValue(),
                   fixture.Continuation.GetSnapshot().State == Ia64SystemCallWaitContinuationState::EntryReserved);
    const auto committed = fixture.Continuation.CommitPrepared(fixture.Frame, correlation);
    ASTERIA_EXPECT(runner, "exact WaitSet preparation commits epoch one once", committed.HasValue(),
                   committed.GetValue().Epoch == 1U,
                   fixture.Continuation.GetSnapshot().State == Ia64SystemCallWaitContinuationState::WaitPrepared);
}

void TestInitialAndRepeatedRoundTrip(TestContext& runner) noexcept
{
    Fixture fixture;
    fixture.ActivateAndRetainSupervisor();
    const SystemCallResponse response{
        .Error = SystemCallError::None,
        .Value0 = ResponseTag,
        .Value1 = ResponseRegistrationSequence,
    };
    bool initialAccepted{false};
    Ia64SystemCallWaitSupervisorReceipt initial{};
    Ia64SystemCallWaitContinuationSnapshot active{};
    bool supervisorRecordZero{false};
    Ia64SystemCallWaitContinuationError waiting{Ia64SystemCallWaitContinuationError::Faulted};
    Ia64SystemCallWaitContinuationError resumePrepared{Ia64SystemCallWaitContinuationError::Faulted};
    bool repeatedAccepted{false};
    Ia64SystemCallWaitSupervisorReceipt repeated{};
    auto supervisor = [&]() noexcept
    {
        const auto accepted = fixture.Continuation.AcceptInitialSupervisorResume();
        initialAccepted = accepted.HasValue();
        if (initialAccepted)
        {
            initial = accepted.GetValue();
        }
        active = fixture.Continuation.GetSnapshot();
        supervisorRecordZero = RegisterStateIsZero(fixture.Supervisor.Registers);
        waiting = fixture.Continuation.BeginWaitingForReadmission();
        resumePrepared = fixture.Continuation.PrepareSystemCallResume(fixture.Frame, fixture.Correlation(2U), response);
        const auto resumed = fixture.Continuation.ResumeSystemCall();
        repeatedAccepted = resumed.HasValue();
        if (repeatedAccepted)
        {
            repeated = resumed.GetValue();
        }
    };
    RawSwitchModel model{fixture, supervisor};

    bool firstCommitted{false};
    const auto returned = SuspendCommittedWait(fixture, 1U, firstCommitted);
    const Ia64SystemCallWaitContinuationSnapshot snapshot = fixture.Continuation.GetSnapshot();
    const Ia64KernelRegisterState* const systemCallRecord = model.GetSystemCallRecord();
    const bool systemCallRecordZero = (systemCallRecord != nullptr) && RegisterStateIsZero(*systemCallRecord);
    const Ia64UserModeSupervisorContinuationOwner ownerAfterReturn = fixture.Supervisor.Owner;
    const Ia64SystemCallEntryFrameState frameAfterReturn = fixture.Frame.State;

    const auto secondCorrelation = fixture.Correlation(2U);
    const auto secondReservation = fixture.Continuation.ReserveEntry(fixture.Frame, secondCorrelation);
    const auto secondCommit = fixture.Continuation.CommitPrepared(fixture.Frame, secondCorrelation);
    // The supervisor script ends at the repeated site, so this suspended
    // syscall observes a spurious return that the continuation rejects.
    static_cast<void>(fixture.Continuation.SuspendToSupervisor(fixture.Frame));

    ASTERIA_EXPECT(runner, "the modeled first switch accepts launch as the supervisor return site", firstCommitted,
                   initialAccepted, initial.Epoch == 1U,
                   initial.Site == Ia64SystemCallWaitSupervisorResumeSite::InitialLaunch);
    ASTERIA_EXPECT(runner, "restored supervisor ownership clears its consumed raw record",
                   active.State == Ia64SystemCallWaitContinuationState::SupervisorActive,
                   active.SupervisorState == Ia64UserModeSupervisorContinuationState::Active,
                   active.SystemCallRecordActionable == 1U, supervisorRecordZero);
    ASTERIA_EXPECT(runner, "the active supervisor explicitly enters the blocked readmission window",
                   waiting == Ia64SystemCallWaitContinuationError::None);
    ASTERIA_EXPECT(runner, "the exact next Thread run retains the completed syscall response",
                   resumePrepared == Ia64SystemCallWaitContinuationError::None);
    ASTERIA_EXPECT(runner, "reverse switch clears the syscall record but leaves the native frame active",
                   returned.HasValue(), returned.GetValue().Error == SystemCallError::None,
                   returned.GetValue().Value0 == ResponseTag,
                   returned.GetValue().Value1 == ResponseRegistrationSequence,
                   snapshot.State == Ia64SystemCallWaitContinuationState::Idle, snapshot.Epoch == 1U,
                   snapshot.EntryFrameRetained == 0U, snapshot.SystemCallRecordActionable == 0U,
                   snapshot.SupervisorState == Ia64UserModeSupervisorContinuationState::Retained,
                   ownerAfterReturn == Ia64UserModeSupervisorContinuationOwner::None, systemCallRecordZero,
                   frameAfterReturn == Ia64SystemCallEntryFrameState::Active);
    ASTERIA_EXPECT(runner, "a second blocked syscall resumes at the repeated supervisor switch site",
                   secondReservation.HasValue(), secondReservation.GetValue().PendingEpoch == 2U,
                   secondCommit.HasValue(), secondCommit.GetValue().Epoch == 2U, repeatedAccepted, repeated.Epoch == 2U,
                   repeated.Site == Ia64SystemCallWaitSupervisorResumeSite::RepeatedSwitch);
}

void TestForeignOwnerIsolation(TestContext& runner) noexcept
{
    Fixture cancellation;
    cancellation.ActivateAndRetainSupervisor();
    const auto cancellationCorrelation = cancellation.Correlation();
    const auto reserved = cancellation.Continuation.ReserveEntry(cancellation.Frame, cancellationCorrelation);
    cancellation.Supervisor.Owner = Ia64UserModeSupervisorContinuationOwner::PageFault;
    const Ia64UserModeSupervisorContinuation beforeCancellation{cancellation.Supervisor};
    ASTERIA_EXPECT(runner, "cancellation cannot alter a page-fault-owned continuation", reserved.HasValue(),
                   cancellation.Continuation.CancelReservedEntry(cancellation.Frame, cancellationCorrelation) ==
                       Ia64SystemCallWaitContinuationError::InvalidSupervisorState,
                   cancellation.Continuation.GetSnapshot().State == Ia64SystemCallWaitContinuationState::Faulted,
                   SupervisorContinuationsEqual(cancellation.Supervisor, beforeCancellation));

    Fixture commit;
    commit.ActivateAndRetainSupervisor();
    const auto commitCorrelation = commit.Correlation();
    const auto commitReservation = commit.Continuation.ReserveEntry(commit.Frame, commitCorrelation);
    commit.Supervisor.Owner = Ia64UserModeSupervisorContinuationOwner::PageFault;
    const Ia64UserModeSupervisorContinuation beforeCommit{commit.Supervisor};
    const auto rejectedCommit = commit.Continuation.CommitPrepared(commit.Frame, commitCorrelation);
    ASTERIA_EXPECT(runner, "commit cannot alter a page-fault-owned continuation", commitReservation.HasValue(),
                   !rejectedCommit.HasValue(),
                   rejectedCommit.GetError().Error == Ia64SystemCallWaitContinuationError::InvalidSupervisorState,
                   commit.Continuation.GetSnapshot().State == Ia64SystemCallWaitContinuationState::Faulted,
                   SupervisorContinuationsEqual(commit.Supervisor, beforeCommit));

    {
        Fixture retained;
        retained.ActivateAndRetainSupervisor();
        const auto retainedCorrelation = retained.Correlation();
        const auto retainedReservation = retained.Continuation.ReserveEntry(retained.Frame, retainedCorrelation);
        const auto retainedCommit = retained.Continuation.CommitPrepared(retained.Frame, retainedCorrelation);
        retained.Supervisor.Owner = Ia64UserModeSupervisorContinuationOwner::PageFault;
        const Ia64UserModeSupervisorContinuation beforeRetainedFailure{retained.Supervisor};
        const auto retainedSuspend = retained.Continuation.SuspendToSupervisor(retained.Frame);
        ASTERIA_EXPECT(runner, "a retained page-fault-owned continuation survives syscall failure",
                       retainedReservation.HasValue(), retainedCommit.HasValue(), !retainedSuspend.HasValue(),
                       retained.Continuation.GetSnapshot().State == Ia64SystemCallWaitContinuationState::Faulted,
                       SupervisorContinuationsEqual(retained.Supervisor, beforeRetainedFailure));
    }

    {
        Fixture resumePending;
        resumePending.ActivateAndRetainSupervisor();
        bool beganSwitch{false};
        bool pendingResumeAccepted{true};
        Ia64UserModeSupervisorContinuation beforePendingFailure{};
        auto supervisor = [&]() noexcept
        {
            beganSwitch = (resumePending.Supervisor.State == Ia64UserModeSupervisorContinuationState::ResumePending) &&
                          (resumePending.Continuation.GetSnapshot().State ==
                           Ia64SystemCallWaitContinuationState::SwitchingToSupervisor);
            resumePending.Supervisor.Owner = Ia64UserModeSupervisorContinuationOwner::PageFault;
            beforePendingFailure = resumePending.Supervisor;
            pendingResumeAccepted = resumePending.Continuation.AcceptInitialSupervisorResume().HasValue();
        };
        RawSwitchModel model{resumePending, supervisor};
        bool pendingCommitted{false};
        static_cast<void>(SuspendCommittedWait(resumePending, 1U, pendingCommitted));
        ASTERIA_EXPECT(runner, "a resume-pending page-fault owner retains every continuation byte", pendingCommitted,
                       beganSwitch, !pendingResumeAccepted,
                       resumePending.Continuation.GetSnapshot().State == Ia64SystemCallWaitContinuationState::Faulted,
                       SupervisorContinuationsEqual(resumePending.Supervisor, beforePendingFailure));
    }

    {
        Fixture active;
        active.ActivateAndRetainSupervisor();
        bool prepared{false};
        Ia64SystemCallWaitContinuationError waiting{Ia64SystemCallWaitContinuationError::None};
        Ia64UserModeSupervisorContinuation beforeActiveFailure{};
        auto supervisor = [&]() noexcept
        {
            prepared = active.Continuation.AcceptInitialSupervisorResume().HasValue();
            active.Supervisor.Owner = Ia64UserModeSupervisorContinuationOwner::PageFault;
            beforeActiveFailure = active.Supervisor;
            waiting = active.Continuation.BeginWaitingForReadmission();
        };
        RawSwitchModel model{active, supervisor};
        bool committed{false};
        static_cast<void>(SuspendCommittedWait(active, 1U, committed));
        ASTERIA_EXPECT(runner, "an active page-fault owner retains every continuation byte", committed, prepared,
                       waiting == Ia64SystemCallWaitContinuationError::InvalidSupervisorState,
                       active.Continuation.GetSnapshot().State == Ia64SystemCallWaitContinuationState::Faulted,
                       SupervisorContinuationsEqual(active.Supervisor, beforeActiveFailure));
    }
}

void TestReadmissionRejection(TestContext& runner) noexcept
{
    enum class RejectionCase : UInt8
    {
        Thread,
        SchedulingContext,
        Processor,
        RunSequence,
    };
    const auto checkRejected = [&runner](const RejectionCase rejection, const char* const description) noexcept
    {
        Fixture fixture;
        fixture.ActivateAndRetainSupervisor();
        Ia64SystemCallWaitCorrelation candidate = fixture.Correlation(2U);
        if (rejection == RejectionCase::Thread)
        {
            candidate.Thread = ThreadId{fixture.Registry, ObjectId{0x201U}};
        }
        else if (rejection == RejectionCase::SchedulingContext)
        {
            candidate.SchedulingContext = SchedulingContextId{fixture.Registry, ObjectId{0x202U}};
        }
        else if (rejection == RejectionCase::Processor)
        {
            candidate.Processor = CpuId{1U};
        }
        else
        {
            candidate.RunSequence = 3U;
        }
        bool accepted{false};
        Ia64SystemCallWaitContinuationError waiting{Ia64SystemCallWaitContinuationError::Faulted};
        Ia64SystemCallWaitContinuationError error{Ia64SystemCallWaitContinuationError::None};
        Ia64SystemCallWaitContinuationSnapshot snapshot{};
        auto supervisor = [&]() noexcept
        {
            accepted = fixture.Continuation.AcceptInitialSupervisorResume().HasValue();
            waiting = fixture.Continuation.BeginWaitingForReadmission();
            error = fixture.Continuation.PrepareSystemCallResume(fixture.Frame, candidate, SystemCallResponse{});
            snapshot = fixture.Continuation.GetSnapshot();
        };
        RawSwitchModel model{fixture, supervisor};
        bool committed{false};
        static_cast<void>(SuspendCommittedWait(fixture, 1U, committed));
        ASTERIA_EXPECT(runner, description, committed, accepted, waiting == Ia64SystemCallWaitContinuationError::None,
                       error == Ia64SystemCallWaitContinuationError::InvalidCorrelation,
                       snapshot.State == Ia64SystemCallWaitContinuationState::Faulted,
                       snapshot.TerminalError == Ia64SystemCallWaitContinuationError::InvalidCorrelation,
                       snapshot.SystemCallRecordActionable == 0U, snapshot.EntryFrameRetained == 0U);
    };

    checkRejected(RejectionCase::Thread, "foreign Thread generation is terminal after block");
    checkRejected(RejectionCase::SchedulingContext, "foreign SchedulingContext generation is terminal after block");
    checkRejected(RejectionCase::Processor, "foreign CPU readmission is terminal after block");
    checkRejected(RejectionCase::RunSequence, "a skipped Thread run generation is terminal after block");
}

void TestTerminalAndUnavailablePaths(TestContext& runner) noexcept
{
    {
        Fixture unownedCorruption;
        unownedCorruption.ActivateAndRetainSupervisor();
        auto supervisor = [&]() noexcept
        {
            static_cast<void>(unownedCorruption.Continuation.AcceptInitialSupervisorResume());
            static_cast<void>(unownedCorruption.Continuation.BeginWaitingForReadmission());
            static_cast<void>(unownedCorruption.Continuation.PrepareSystemCallResume(
                unownedCorruption.Frame, unownedCorruption.Correlation(2U), SystemCallResponse{}));
            static_cast<void>(unownedCorruption.Continuation.ResumeSystemCall());
        };
        RawSwitchModel model{unownedCorruption, supervisor};
        bool committed{false};
        const auto completed = SuspendCommittedWait(unownedCorruption, 1U, committed);
        // Models a stray write into the idle syscall record at the exact
        // address that the continuation hands to the native switch.
        Ia64KernelRegisterState* const record = model.GetSystemCallRecord();
        if (record != nullptr)
        {
            record->InstructionPointer = 1U;
        }
        const Ia64UserModeSupervisorContinuation beforeUnownedFailure{unownedCorruption.Supervisor};
        const auto corruptReservation =
            unownedCorruption.Continuation.ReserveEntry(unownedCorruption.Frame, unownedCorruption.Correlation());
        const Ia64SystemCallWaitContinuationSnapshot afterCorruption = unownedCorruption.Continuation.GetSnapshot();
        ASTERIA_EXPECT(runner, "a pre-claim terminal failure leaves the unowned continuation intact", committed,
                       completed.HasValue(), record != nullptr, !corruptReservation.HasValue(),
                       afterCorruption.State == Ia64SystemCallWaitContinuationState::Faulted,
                       afterCorruption.TerminalError ==
                           Ia64SystemCallWaitContinuationError::InvalidSystemCallRegisterState,
                       SupervisorContinuationsEqual(unownedCorruption.Supervisor, beforeUnownedFailure));
    }

    {
        Fixture invalidRecord;
        invalidRecord.ActivateAndRetainSupervisor();
        bool accepted{true};
        Ia64SystemCallWaitContinuationSnapshot rejected{};
        auto supervisor = [&]() noexcept
        {
            accepted = invalidRecord.Continuation.AcceptInitialSupervisorResume().HasValue();
            rejected = invalidRecord.Continuation.GetSnapshot();
        };
        RawSwitchModel model{invalidRecord, supervisor, SystemCallBoundary::BelowMemoryStack};
        bool committed{false};
        static_cast<void>(SuspendCommittedWait(invalidRecord, 1U, committed));
        ASTERIA_EXPECT(runner, "a syscall record outside its dedicated stack is terminal before block", committed,
                       !accepted, rejected.State == Ia64SystemCallWaitContinuationState::Faulted,
                       rejected.TerminalError == Ia64SystemCallWaitContinuationError::InvalidSystemCallRegisterState);
    }

    // ReserveEntry preflights the next epoch with this policy and faults
    // before claiming the supervisor when it is exhausted; 2^64 waits cannot
    // be reached, so the boundary is exercised on the policy itself.
    UInt64 maximumEpoch{0xA5A5U};
    const bool reachesMaximum =
        Ia64SystemCallWaitEpochPolicy::TryAdvance(Ia64SystemCallWaitEpochPolicy::Maximum - 1U, maximumEpoch);
    UInt64 wrappedEpoch{0xA5A5U};
    const bool wraps = Ia64SystemCallWaitEpochPolicy::TryAdvance(Ia64SystemCallWaitEpochPolicy::Maximum, wrappedEpoch);
    ASTERIA_EXPECT(runner, "the suspension epoch never wraps or aliases a prior owner", reachesMaximum,
                   maximumEpoch == Ia64SystemCallWaitEpochPolicy::Maximum, !wraps, wrappedEpoch == 0xA5A5U);

    Fixture unavailable;
    unavailable.ActivateAndRetainSupervisor();
    const auto unavailableCorrelation = unavailable.Correlation();
    static_cast<void>(unavailable.Continuation.ReserveEntry(unavailable.Frame, unavailableCorrelation));
    static_cast<void>(unavailable.Continuation.CommitPrepared(unavailable.Frame, unavailableCorrelation));
    const Size beforeSwitches = g_rawSwitchCount.load();
    const auto suspended = unavailable.Continuation.SuspendToSupervisor(unavailable.Frame);
    ASTERIA_EXPECT(runner, "an immediate hosted raw-switch return cannot impersonate a resume", !suspended.HasValue(),
                   suspended.GetError().Error == Ia64SystemCallWaitContinuationError::NativeTransitionUnavailable,
                   g_rawSwitchCount.load() == (beforeSwitches + 1U),
                   unavailable.Continuation.GetSnapshot().State == Ia64SystemCallWaitContinuationState::Faulted);

    {
        Fixture maximumRun;
        maximumRun.ActivateAndRetainSupervisor();
        bool accepted{false};
        Ia64SystemCallWaitContinuationError waiting{Ia64SystemCallWaitContinuationError::Faulted};
        Ia64SystemCallWaitContinuationError exhaustedRun{Ia64SystemCallWaitContinuationError::None};
        Ia64SystemCallWaitContinuationSnapshot afterExhaustion{};
        auto supervisor = [&]() noexcept
        {
            accepted = maximumRun.Continuation.AcceptInitialSupervisorResume().HasValue();
            waiting = maximumRun.Continuation.BeginWaitingForReadmission();
            exhaustedRun = maximumRun.Continuation.PrepareSystemCallResume(
                maximumRun.Frame, maximumRun.Correlation(Kernel::Scheduler::ThreadRunSequencePolicy::Maximum),
                SystemCallResponse{});
            afterExhaustion = maximumRun.Continuation.GetSnapshot();
        };
        RawSwitchModel model{maximumRun, supervisor};
        bool committed{false};
        static_cast<void>(
            SuspendCommittedWait(maximumRun, Kernel::Scheduler::ThreadRunSequencePolicy::Maximum, committed));
        ASTERIA_EXPECT(runner, "readmission cannot wrap the Thread run sequence", committed, accepted,
                       waiting == Ia64SystemCallWaitContinuationError::None,
                       exhaustedRun == Ia64SystemCallWaitContinuationError::RunSequenceExhausted,
                       afterExhaustion.TerminalError == Ia64SystemCallWaitContinuationError::RunSequenceExhausted);
    }
}
} // namespace
} // namespace Asteria::Test

extern "C" void
AsteriaIa64SwitchKernelContext(Asteria::Platform::Ia64::Scheduler::Ia64KernelRegisterState* const current,
                               const Asteria::Platform::Ia64::Scheduler::Ia64KernelRegisterState* const next) noexcept
{
    static_cast<void>(next);
    Asteria::Test::g_rawSwitchCount.fetch_add(1U);
    Asteria::Test::RawSwitchModel* const model = Asteria::Test::g_rawSwitchModel;
    if (model != nullptr)
    {
        model->Switch(*current);
    }
}

int main()
{
    TestContext runner{"Ia64SystemCallWaitContinuationTests"};
    Asteria::Test::TestInitialization(runner);
    Asteria::Test::TestReservationAndCancellation(runner);
    Asteria::Test::TestInitialAndRepeatedRoundTrip(runner);
    Asteria::Test::TestForeignOwnerIsolation(runner);
    Asteria::Test::TestReadmissionRejection(runner);
    Asteria::Test::TestTerminalAndUnavailablePaths(runner);
    return runner.Finish();
}
