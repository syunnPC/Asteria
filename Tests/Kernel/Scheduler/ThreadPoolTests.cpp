// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Scheduler/ThreadPool.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityOperation;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPoolError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationDomainId;
using Kernel::Object::RevocationRegistry;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::IUserThreadBackend;
using Kernel::Scheduler::SchedulingClass;
using Kernel::Scheduler::SchedulingContext;
using Kernel::Scheduler::SchedulingContextConfiguration;
using Kernel::Scheduler::SchedulingContextId;
using Kernel::Scheduler::SchedulingCriticality;
using Kernel::Scheduler::Thread;
using Kernel::Scheduler::ThreadCreateRequest;
using Kernel::Scheduler::ThreadId;
using Kernel::Scheduler::ThreadPool;
using Kernel::Scheduler::ThreadRunDisposition;
using Kernel::Object::ObjectPoolFailure;
using Kernel::Scheduler::ThreadState;
using Kernel::Scheduler::UserThreadBackendError;
using Kernel::Scheduler::UserThreadSchedule;
using Kernel::Scheduler::UserThreadStart;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

constexpr UInt64 NativeSlotBytes{0x1000U};

// Records each start and answers as told; a thread's native slot counts as
// released once the test says so.
class TestBackend final : public IUserThreadBackend
{
public:
    [[nodiscard]] UInt64 GetNativeSlotBytes() const noexcept override
    {
        return NativeSlotBytes;
    }

    [[nodiscard]] UserThreadSchedule GetSchedule() const noexcept override
    {
        return UserThreadSchedule{
            .Configuration =
                SchedulingContextConfiguration{
                    .BudgetTicks = 20U,
                    .PeriodTicks = 100U,
                    .RelativeDeadlineTicks = 80U,
                    .Weight = 10U,
                    .Priority = 8U,
                    .Criticality = SchedulingCriticality::Normal,
                    .CpuAffinityMask = 0x01U,
                    .Class = SchedulingClass::DeadlineSporadic,
                    .Reserved = 0U,
                },
            .Now = MonotonicTime{0U},
        };
    }

    [[nodiscard]] UserThreadBackendError Reserve(Thread& thread, ThreadId, SchedulingContext&, SchedulingContextId,
                                                 const UserThreadStart&) noexcept override
    {
        ReserveHeld = Platform::Synchronization::HeldLockDepth() != 0U;
        ReserveDormant = thread.GetSnapshot().State == ThreadState::Dormant;
        ++Reservations;
        return ReserveAnswer;
    }

    [[nodiscard]] UserThreadBackendError Start(Thread& thread, const ThreadId threadIdentity,
                                               SchedulingContext& context, const SchedulingContextId contextIdentity,
                                               const UserThreadStart& start) noexcept override
    {
        ++Starts;
        LastThread = &thread;
        LastContext = &context;
        LastThreadIdentity = threadIdentity;
        LastContextIdentity = contextIdentity;
        LastStart = start;
        if (DuringStart != nullptr)
        {
            DuringStart(*this);
        }
        if (Answer == UserThreadBackendError::None)
        {
            return thread.MakeReady() == Kernel::Scheduler::ThreadError::None ? Answer
                                                                              : UserThreadBackendError::Faulted;
        }
        return Answer;
    }

    [[nodiscard]] bool IsReleased(const Thread& thread) const noexcept override
    {
        return &thread == Released;
    }

    // Answers as told; on None ends the thread as a platform would once it
    // took it out of its run queue.
    [[nodiscard]] Kernel::Scheduler::UserThreadTerminateError Terminate(Thread& thread,
                                                                        const UInt64 exitCode) noexcept override
    {
        ++Terminations;
        if ((TerminateAnswer == Kernel::Scheduler::UserThreadTerminateError::None) &&
            (thread.Terminate(exitCode) != Kernel::Scheduler::ThreadError::None))
        {
            return Kernel::Scheduler::UserThreadTerminateError::Faulted;
        }
        if (DuringTerminate != nullptr)
        {
            DuringTerminate(*this);
        }
        return TerminateAnswer;
    }

    void* HookContext{};
    void (*DuringStart)(TestBackend&) noexcept {};
    void (*DuringTerminate)(TestBackend&) noexcept {};
    UserThreadBackendError ReserveAnswer{UserThreadBackendError::None};
    UserThreadBackendError Answer{UserThreadBackendError::None};
    Kernel::Scheduler::UserThreadTerminateError TerminateAnswer{Kernel::Scheduler::UserThreadTerminateError::None};
    Size Reservations{0U};
    bool ReserveHeld{false};
    bool ReserveDormant{false};
    Size Terminations{0U};
    Size Starts{0U};
    Thread* LastThread{nullptr};
    SchedulingContext* LastContext{nullptr};
    ThreadId LastThreadIdentity{};
    SchedulingContextId LastContextIdentity{};
    UserThreadStart LastStart{};
    const Thread* Released{nullptr};
};

class PoolFixture final
{
public:
    PoolFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          Space{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL}, Domain{Ledger},
          Pool{Registry, Issuer}
    {
    }

    [[nodiscard]] bool Initialize(const UInt64 threads = 2U) noexcept
    {
        const ResourceDomainLimits limits{
            .CommittedMemoryPages = 0U,
            .ResidentMemoryPages = 0U,
            .KernelMetadataBytes = 1U << 20U,
            .CapabilityHandles = 0U,
            .IpcQueueBytes = 0U,
            .Threads = threads,
            .ChildDomains = 0U,
        };
        const auto root = Revocations.CreateRoot();
        if ((Domain.InitializeRoot(ResourceDomainId{5U}, limits).Error != ResourceDomainError::None) ||
            !root.HasValue())
        {
            return false;
        }
        const auto registered = Issuer.CreateObject(Domain);
        if (!registered.HasValue())
        {
            return false;
        }
        const auto authority = Issuer.Mint(Space, registered.GetValue(),
                                           CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Inspect),
                                           root.GetValue(), MonotonicTime::Infinite(), 0U);
        const auto inspect = Issuer.Mint(Space, registered.GetValue(), CapabilityRights{CapabilityRight::Inspect},
                                         root.GetValue(), MonotonicTime::Infinite(), 0U);
        Authority = authority.HasValue() ? authority.GetValue() : CapabilityHandle{};
        InspectOnly = inspect.HasValue() ? inspect.GetValue() : CapabilityHandle{};
        return Authority.IsValid() && InspectOnly.IsValid();
    }

    [[nodiscard]] auto Create(const CapabilityHandle authority) noexcept
    {
        return Pool.Create(ThreadCreateRequest{
            .Space = &Space,
            .Authority = authority,
            .Start =
                UserThreadStart{
                    .Entry = 0x4000'1000U, .UserStackTop = 0x4100'0000U, .Argument = 0x42U, .Domain = nullptr},
        });
    }

    // Runs the backend's last thread on CPU 0 and completes it Exited.
    [[nodiscard]] bool Exit(const UInt64 code) noexcept
    {
        return (Backend.LastThread->BeginRun(CpuId{0U}) == Kernel::Scheduler::ThreadError::None) &&
               Backend.LastThread->FinishRun(CpuId{0U}, ThreadRunDisposition::Exited, code).HasValue();
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace Space;
    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    ThreadPool Pool;
    TestBackend Backend;
    CapabilityHandle Authority{};
    CapabilityHandle InspectOnly{};
};

constexpr UInt64 ChargedBytes{sizeof(Thread) + sizeof(SchedulingContext) + NativeSlotBytes};

void TestLifecycle(TestContext& runner) noexcept
{
    PoolFixture fixture;
    const bool ready = fixture.Initialize();
    const auto unconfigured = fixture.Create(fixture.Authority);
    const bool configured = fixture.Pool.Configure(fixture.Backend) && !fixture.Pool.Configure(fixture.Backend);
    const Size registered = fixture.Registry.GetActiveCount();
    const auto created = fixture.Create(fixture.Authority);
    const auto query = created.HasValue()
                           ? fixture.Space.Query(created.GetValue().Capability)
                           : decltype(fixture.Space.Query(CapabilityHandle{}))::Failure(CapabilityError::InvalidHandle);
    const auto usage = fixture.Domain.GetSnapshot().Usage;
    ASTERIA_EXPECT(
        runner, "a created thread is charged, registered with its context, minted and started Ready", ready,
        !unconfigured.HasValue() && (unconfigured.GetError().Error == ObjectPoolError::CapacityExceeded), configured,
        created.HasValue(), fixture.Backend.Starts == 1U, fixture.Backend.LastStart.Entry == 0x4000'1000U,
        fixture.Backend.LastStart.UserStackTop == 0x4100'0000U, fixture.Backend.LastStart.Argument == 0x42U,
        fixture.Backend.LastThread->GetSnapshot().State == ThreadState::Ready,
        fixture.Backend.LastContext->GetSnapshot().BaseThread.IsEqualTo(fixture.Backend.LastThreadIdentity),
        fixture.Registry.GetActiveCount() == registered + 2U,
        query.HasValue() && (query.GetValue().Type == ObjectType::Thread),
        query.HasValue() && query.GetValue().Rights.IsEqualTo(ThreadPool::CreatedRights), usage.Threads == 1U,
        usage.KernelMetadataBytes == ChargedBytes, fixture.Pool.GetSnapshot().ActiveSlots == 1U);
    if (!created.HasValue())
    {
        return;
    }

    const CapabilityHandle handle = created.GetValue().Capability;
    const auto early = fixture.Pool.Destroy(fixture.Space, handle);
    const bool exited = fixture.Exit(0x1234U);
    const auto destroyed = fixture.Pool.Destroy(fixture.Space, handle);
    const auto stale = fixture.Space.Lookup(handle, ObjectType::Thread, CapabilityOperation::Inspect);
    const auto bound = fixture.Pool.Collect();
    const auto afterBound = fixture.Pool.GetSnapshot();
    fixture.Backend.Released = fixture.Backend.LastThread;
    const auto collected = fixture.Pool.Collect();
    const auto finalUsage = fixture.Domain.GetSnapshot().Usage;
    ASTERIA_EXPECT(runner, "only an exited thread is reclaimed, and only once its native slot is released",
                   !early.HasValue() && (early.GetError().Error == ObjectPoolError::Busy), exited,
                   destroyed.HasValue() && (destroyed.GetValue() == 0x1234U),
                   !stale.HasValue() && (stale.GetError() == CapabilityError::ObjectNotFound),
                   bound.HasValue() && (bound.GetValue() == 0U), afterBound.QuiescingSlots == 1U,
                   collected.HasValue() && (collected.GetValue() == 1U),
                   fixture.Registry.GetActiveCount() == registered, finalUsage.Threads == 0U,
                   finalUsage.KernelMetadataBytes == 0U, fixture.Pool.GetSnapshot().FreeSlots == ThreadPool::Capacity,
                   fixture.Pool.GetSnapshot().Reclaimed == 1U, fixture.Pool.GetSnapshot().LastExitCode == 0x1234U,
                   fixture.Space.Close(handle) == CapabilityError::Ok);
}

void TestRefusals(TestContext& runner) noexcept
{
    PoolFixture fixture;
    const bool ready = fixture.Initialize(1U) && fixture.Pool.Configure(fixture.Backend);
    const Size registered = fixture.Registry.GetActiveCount();
    const auto unauthorized = fixture.Create(fixture.InspectOnly);
    fixture.Backend.Answer = UserThreadBackendError::Exhausted;
    const auto exhausted = fixture.Create(fixture.Authority);
    fixture.Backend.Answer = UserThreadBackendError::Rejected;
    const auto rejected = fixture.Create(fixture.Authority);
    const auto afterRefusals = fixture.Domain.GetSnapshot().Usage;
    const Size registeredAfterRefusals = fixture.Registry.GetActiveCount();
    const auto poolAfterRefusals = fixture.Pool.GetSnapshot();
    fixture.Backend.Answer = UserThreadBackendError::None;
    const auto first = fixture.Create(fixture.Authority);
    const auto overLimit = fixture.Create(fixture.Authority);
    ASTERIA_EXPECT(runner, "refused creations leave no charge, registration or slot behind", ready,
                   !unauthorized.HasValue() && (unauthorized.GetError().Error == ObjectPoolError::CapabilityRejected),
                   !unauthorized.HasValue() && (unauthorized.GetError().Capability == CapabilityError::RightsDenied),
                   !exhausted.HasValue() && (exhausted.GetError().Error == ObjectPoolError::CapacityExceeded),
                   !rejected.HasValue() && (rejected.GetError().Error == ObjectPoolError::InvalidRequest),
                   afterRefusals.Threads == 0U, afterRefusals.KernelMetadataBytes == 0U,
                   registeredAfterRefusals == registered, poolAfterRefusals.FreeSlots == ThreadPool::Capacity,
                   poolAfterRefusals.State == Kernel::Scheduler::ThreadPoolState::Ready, first.HasValue(),
                   !overLimit.HasValue() && (overLimit.GetError().Error == ObjectPoolError::ChargeRejected));

    // A capability to another object of the same type is not this pool's.
    Thread foreign;
    const bool foreignReady = foreign.Initialize(ResourceDomainId{5U}) == Kernel::Scheduler::ThreadError::None;
    const auto foreignObject = fixture.Issuer.CreateObject(foreign);
    const auto root = fixture.Revocations.CreateRoot();
    const auto foreignHandle =
        (foreignObject.HasValue() && root.HasValue())
            ? fixture.Issuer.Mint(fixture.Space, foreignObject.GetValue(), CapabilityRights{CapabilityRight::Manage},
                                  root.GetValue(), MonotonicTime::Infinite(), 0U)
            : decltype(fixture.Issuer.Mint(fixture.Space, ObjectId{}, CapabilityRights{}, RevocationDomainId{},
                                           MonotonicTime::Infinite(), 0U))::Failure(CapabilityError::InvalidHandle);
    const auto notOwned = foreignHandle.HasValue()
                              ? fixture.Pool.Destroy(fixture.Space, foreignHandle.GetValue())
                              : decltype(fixture.Pool.Destroy(fixture.Space, CapabilityHandle{}))::Failure(
                                    Kernel::Object::MakeObjectPoolFailure(ObjectPoolError::Faulted));
    ASTERIA_EXPECT(runner, "a foreign thread is not the pool's to destroy", foreignReady, foreignHandle.HasValue(),
                   !notOwned.HasValue() && (notOwned.GetError().Error == ObjectPoolError::NotOwned));
    if (foreignObject.HasValue())
    {
        static_cast<void>(fixture.Issuer.DestroyObject(foreignObject.GetValue()));
        static_cast<void>(fixture.Issuer.CollectDestroyedObject(foreignObject.GetValue()));
    }
}
// ThreadTerminate: the backend decides whether a live thread can end now; an ended or exited one is
// reclaimed with the code the kernel chose.
void TestTerminate(TestContext& runner) noexcept
{
    using Kernel::Scheduler::UserThreadTerminateError;
    constexpr UInt64 Killed{0x8000'0000'0000'0003ULL};
    PoolFixture fixture;
    const bool ready = fixture.Initialize() && fixture.Pool.Configure(fixture.Backend);
    const auto created = fixture.Create(fixture.Authority);
    const CapabilityHandle handle = created.HasValue() ? created.GetValue().Capability : CapabilityHandle{};
    const auto inspect =
        fixture.Space.Derive(handle, CapabilityRights{CapabilityRight::Inspect}, MonotonicTime::Infinite());
    const ObjectPoolFailure unauthorized =
        fixture.Pool.Terminate(fixture.Space, inspect.HasValue() ? inspect.GetValue() : CapabilityHandle{}, Killed);
    fixture.Backend.TerminateAnswer = UserThreadTerminateError::Blocked;
    const ObjectPoolFailure blocked = fixture.Pool.Terminate(fixture.Space, handle, Killed);
    fixture.Backend.TerminateAnswer = UserThreadTerminateError::Running;
    const ObjectPoolFailure running = fixture.Pool.Terminate(fixture.Space, handle, Killed);
    fixture.Backend.TerminateAnswer = UserThreadTerminateError::Unsupported;
    const ObjectPoolFailure unsupported = fixture.Pool.Terminate(fixture.Space, handle, Killed);
    const ThreadState alive = fixture.Backend.LastThread->GetSnapshot().State;
    fixture.Backend.TerminateAnswer = UserThreadTerminateError::None;
    const ObjectPoolFailure terminated = fixture.Pool.Terminate(fixture.Space, handle, Killed);
    const ObjectPoolFailure again = fixture.Pool.Terminate(fixture.Space, handle, Killed);
    const auto destroyed = fixture.Pool.Destroy(fixture.Space, handle);
    ASTERIA_EXPECT(runner, "a live thread ends only when the backend can take it out of its run queue", ready,
                   created.HasValue(), inspect.HasValue(), unauthorized.Error == ObjectPoolError::CapabilityRejected,
                   unauthorized.Capability == CapabilityError::RightsDenied, blocked.Error == ObjectPoolError::Busy,
                   running.Error == ObjectPoolError::InvalidRequest,
                   unsupported.Error == ObjectPoolError::InvalidConfiguration, alive == ThreadState::Ready,
                   terminated.Error == ObjectPoolError::None, again.Error == ObjectPoolError::None,
                   fixture.Backend.Terminations == 4U, destroyed.HasValue() && (destroyed.GetValue() == Killed),
                   fixture.Pool.GetSnapshot().Terminated == 1U,
                   fixture.Pool.GetSnapshot().State == Kernel::Scheduler::ThreadPoolState::Ready);

    PoolFixture faulted;
    const bool faultedReady = faulted.Initialize() && faulted.Pool.Configure(faulted.Backend);
    const auto victim = faulted.Create(faulted.Authority);
    faulted.Backend.TerminateAnswer = UserThreadTerminateError::Faulted;
    const ObjectPoolFailure broken = faulted.Pool.Terminate(
        faulted.Space, victim.HasValue() ? victim.GetValue().Capability : CapabilityHandle{}, Killed);
    ASTERIA_EXPECT(runner, "a backend that cannot complete or undo the removal faults the pool", faultedReady,
                   victim.HasValue(), broken.Error == ObjectPoolError::InvariantViolation,
                   faulted.Pool.GetSnapshot().State == Kernel::Scheduler::ThreadPoolState::Faulted);
}
// Backend reentry observes reserved storage without a held pool lock;
// Destroy cannot reclaim a call's live arguments.
void TestUnlockedBackend(TestContext& runner) noexcept
{
    struct Probe
    {
        PoolFixture Fixture;
        CapabilityHandle Handle{};
        Thread* Outer{};
        bool StartUnlocked{false};
        bool Reserved{false};
        bool NestedDistinct{false};
        bool TerminateUnlocked{false};
        bool DestroyBusy{false};
        bool CollectEmpty{false};
    } probe;
    const bool ready = probe.Fixture.Initialize(3U) && probe.Fixture.Pool.Configure(probe.Fixture.Backend);
    auto& backend = probe.Fixture.Backend;
    backend.HookContext = &probe;
    backend.DuringStart = [](TestBackend& current) noexcept
    {
        auto& state = *static_cast<Probe*>(current.HookContext);
        state.Outer = current.LastThread;
        state.StartUnlocked = Platform::Synchronization::HeldLockDepth() == 0U;
        const auto snapshot = state.Fixture.Pool.GetSnapshot();
        state.Reserved = snapshot.FreeSlots == ThreadPool::Capacity - 1U && snapshot.Created == 0U &&
                         state.Fixture.Domain.GetSnapshot().Usage.Threads == 1U;
        current.DuringStart = nullptr;
        const auto nested = state.Fixture.Create(state.Fixture.Authority);
        state.NestedDistinct = nested.HasValue() && current.LastThread != state.Outer;
    };
    const auto created = probe.Fixture.Create(probe.Fixture.Authority);
    probe.Handle = created.HasValue() ? created.GetValue().Capability : CapabilityHandle{};
    backend.DuringTerminate = [](TestBackend& current) noexcept
    {
        auto& state = *static_cast<Probe*>(current.HookContext);
        state.TerminateUnlocked = Platform::Synchronization::HeldLockDepth() == 0U;
        current.Released = state.Outer;
        const auto destroyed = state.Fixture.Pool.Destroy(state.Fixture.Space, state.Handle);
        state.DestroyBusy = !destroyed.HasValue() && destroyed.GetError().Error == ObjectPoolError::Busy;
        const auto collected = state.Fixture.Pool.Collect();
        state.CollectEmpty = collected.HasValue() && collected.GetValue() == 0U;
    };
    const auto ended = probe.Fixture.Pool.Terminate(probe.Fixture.Space, probe.Handle, 0x55U);
    const auto destroyed = probe.Fixture.Pool.Destroy(probe.Fixture.Space, probe.Handle);
    const auto collected = probe.Fixture.Pool.Collect();
    ASTERIA_EXPECT(runner, "reserve, release, backend and re-lock preserve create/terminate storage across reentry",
                   ready, created.HasValue(), backend.ReserveHeld, backend.ReserveDormant, backend.Reservations == 2U,
                   probe.StartUnlocked, probe.Reserved, probe.NestedDistinct, probe.TerminateUnlocked,
                   probe.DestroyBusy, probe.CollectEmpty, ended.Error == ObjectPoolError::None,
                   destroyed.HasValue() && destroyed.GetValue() == 0x55U,
                   collected.HasValue() && collected.GetValue() == 1U, probe.Fixture.Pool.GetSnapshot().Created == 2U,
                   probe.Fixture.Pool.GetSnapshot().Terminated == 1U);
}

void TestReserveRefusal(TestContext& runner) noexcept
{
    PoolFixture fixture;
    const bool ready = fixture.Initialize() && fixture.Pool.Configure(fixture.Backend);
    fixture.Backend.ReserveAnswer = UserThreadBackendError::Exhausted;
    const auto exhausted = fixture.Create(fixture.Authority);
    fixture.Backend.ReserveAnswer = UserThreadBackendError::Rejected;
    const auto rejected = fixture.Create(fixture.Authority);
    const auto usage = fixture.Domain.GetSnapshot().Usage;
    ASTERIA_EXPECT(runner, "reserve refusal rolls back registrations, the private capability and charges before start",
                   ready, !exhausted.HasValue() && exhausted.GetError().Error == ObjectPoolError::CapacityExceeded,
                   !rejected.HasValue() && rejected.GetError().Error == ObjectPoolError::InvalidRequest,
                   fixture.Backend.Reservations == 2U, fixture.Backend.Starts == 0U, usage.Threads == 0U,
                   usage.KernelMetadataBytes == 0U, fixture.Pool.GetSnapshot().FreeSlots == ThreadPool::Capacity);
}

void TestPendingTerminate(TestContext& runner) noexcept
{
    PoolFixture fixture;
    const bool ready = fixture.Initialize() && fixture.Pool.Configure(fixture.Backend);
    const auto created = fixture.Create(fixture.Authority);
    fixture.Backend.TerminateAnswer = Kernel::Scheduler::UserThreadTerminateError::Pending;
    const auto posted = fixture.Pool.Terminate(
        fixture.Space, created.HasValue() ? created.GetValue().Capability : CapabilityHandle{}, 0x88U);
    fixture.Backend.TerminateAnswer = Kernel::Scheduler::UserThreadTerminateError::None;
    class Subtree final : public Kernel::Resource::IResourceDomainSubtree
    {
    public:
        [[nodiscard]] bool Contains(const ResourceDomain* domain) const noexcept override
        {
            return domain != nullptr;
        }
    };
    struct Probe
    {
        PoolFixture* Fixture;
        bool ExitBusy{false};
        bool DestroyBusy{false};
    } probe{&fixture};
    fixture.Backend.HookContext = &probe;
    fixture.Backend.DuringTerminate = [](TestBackend& current) noexcept
    {
        auto& state = *static_cast<Probe*>(current.HookContext);
        const Subtree subtree;
        // Thread is already Exited here, but the backend still owns its storage.
        state.ExitBusy = !state.Fixture->Pool.HaveChargedExited(subtree);
        state.DestroyBusy = state.Fixture->Pool.DestroyCharged(subtree).Error == ObjectPoolError::Busy;
    };
    const auto completed = fixture.Pool.Terminate(
        fixture.Space, created.HasValue() ? created.GetValue().Capability : CapabilityHandle{}, 0x88U);
    const Subtree subtree;
    ASTERIA_EXPECT(runner, "posted termination returns Busy for the ABI WouldBlock retry, without faulting the pool",
                   ready, created.HasValue(), posted.Error == ObjectPoolError::Busy,
                   completed.Error == ObjectPoolError::None, fixture.Pool.GetSnapshot().Terminated == 1U,
                   probe.ExitBusy, probe.DestroyBusy, fixture.Pool.HaveChargedExited(subtree));
}

} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"ThreadPoolTests"};
    Asteria::Test::TestLifecycle(runner);
    Asteria::Test::TestRefusals(runner);
    Asteria::Test::TestTerminate(runner);
    Asteria::Test::TestUnlockedBackend(runner);
    Asteria::Test::TestPendingTerminate(runner);
    Asteria::Test::TestReserveRefusal(runner);
    return runner.Finish();
}
