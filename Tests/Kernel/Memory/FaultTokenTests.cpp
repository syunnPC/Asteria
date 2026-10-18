// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Kernel/Memory/FaultToken.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::FaultAccess;
using Kernel::Memory::FaultCause;
using Kernel::Memory::FaultClaimSequencePolicy;
using Kernel::Memory::FaultPrivilege;
using Kernel::Memory::FaultRecord;
using Kernel::Memory::FaultResolutionClaimToken;
using Kernel::Memory::FaultResolutionKind;
using Kernel::Memory::FaultToken;
using Kernel::Memory::FaultTokenError;
using Kernel::Memory::FaultTokenSnapshot;
using Kernel::Memory::FaultTokenState;
using Kernel::Object::IKernelObject;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Scheduler::ThreadId;
using Platform::Synchronization::SpinLock;

class TestKernelObject final : public IKernelObject
{
public:
    constexpr TestKernelObject(const ObjectType type, const ResourceDomainId chargeOwner) noexcept
        : m_type{type}, m_chargeOwner{chargeOwner}
    {
    }

    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return m_type;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return m_chargeOwner;
    }

private:
    ObjectType m_type;
    ResourceDomainId m_chargeOwner;
};

class RegistryFixture final
{
public:
    constexpr RegistryFixture() noexcept : Registry{Synchronization}, Issuer{Registry}
    {
    }

    SpinLock Synchronization;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RootCapabilityIssuer Issuer;
};

/// Retires one registered test object after all later-declared pins have been
/// destroyed and before the object's backing storage expires.
class RegisteredObjectCleanup final
{
public:
    constexpr RegisteredObjectCleanup(RegistryFixture& fixture, TestContext& runner, const ObjectId object) noexcept
        : m_fixture{fixture}, m_runner{runner}, m_object{object}
    {
    }

    ~RegisteredObjectCleanup() noexcept
    {
        if (!m_object.IsValid())
        {
            return;
        }
        const ObjectRegistryError destroyError = m_fixture.Issuer.DestroyObject(m_object);
        const auto collected = m_fixture.Issuer.CollectDestroyedObject();
        ASTERIA_EXPECT(m_runner, "registered test object is collected before its storage expires",
                       destroyError == ObjectRegistryError::Ok, collected.HasValue(),
                       collected.GetValue().Object.GetValue() == m_object.GetValue());
    }

    RegisteredObjectCleanup(const RegisteredObjectCleanup&) = delete;
    RegisteredObjectCleanup& operator=(const RegisteredObjectCleanup&) = delete;
    RegisteredObjectCleanup(RegisteredObjectCleanup&&) = delete;
    RegisteredObjectCleanup& operator=(RegisteredObjectCleanup&&) = delete;

private:
    RegistryFixture& m_fixture;
    TestContext& m_runner;
    ObjectId m_object;
};

[[nodiscard]] ObjectId Register(RegistryFixture& fixture, IKernelObject& object, TestContext& runner,
                                const char* const description) noexcept
{
    const auto result = fixture.Issuer.CreateObject(object);
    ASTERIA_EXPECT(runner, description, result.HasValue());
    return result.HasValue() ? result.GetValue() : ObjectId{};
}

[[nodiscard]] constexpr FaultRecord MakeFaultRecord() noexcept
{
    return FaultRecord{
        .VirtualAddress = 0x0000'0001'2345'6789ULL,
        .InstructionPointer = 0x0000'0000'0040'1234ULL,
        .ArchitectureSyndrome = 0xA55AU,
        .Access = FaultAccess::Write,
        .Cause = FaultCause::MissingMapping,
        .Privilege = FaultPrivilege::User,
        .Reserved = {},
    };
}

[[nodiscard]] bool RecordIsEqual(const FaultRecord& left, const FaultRecord& right) noexcept
{
    if ((left.VirtualAddress != right.VirtualAddress) || (left.InstructionPointer != right.InstructionPointer) ||
        (left.ArchitectureSyndrome != right.ArchitectureSyndrome) || (left.Access != right.Access) ||
        (left.Cause != right.Cause) || (left.Privilege != right.Privilege))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < sizeof(left.Reserved); ++index)
    {
        if ((left.Reserved[index] != 0U) || (right.Reserved[index] != 0U))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool SnapshotReservedIsZero(const FaultTokenSnapshot& snapshot) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(snapshot.Reserved); ++index)
    {
        if (snapshot.Reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool SnapshotsEqual(const FaultTokenSnapshot& left, const FaultTokenSnapshot& right) noexcept
{
    return left.Thread.IsEqualTo(right.Thread) && left.AddressSpace.IsEqualTo(right.AddressSpace) &&
           RecordIsEqual(left.Fault, right.Fault) && (left.ClaimSequence == right.ClaimSequence) &&
           (left.State == right.State) && (left.Resolution == right.Resolution) && SnapshotReservedIsZero(left) &&
           SnapshotReservedIsZero(right);
}

struct RegisteredFaultInputs
{
    ObjectId Thread;
    ObjectId AddressSpace;
};

[[nodiscard]] RegisteredFaultInputs RegisterFaultInputs(RegistryFixture& fixture, TestKernelObject& thread,
                                                        TestKernelObject& addressSpace, TestContext& runner) noexcept
{
    return RegisteredFaultInputs{
        .Thread = Register(fixture, thread, runner, "fault fixture registers its Thread"),
        .AddressSpace = Register(fixture, addressSpace, runner, "fault fixture registers its AddressSpace"),
    };
}

void TestInitializationAndRegistration(TestContext& runner) noexcept
{
    RegistryFixture fixture;
    RegistryFixture otherFixture;
    TestKernelObject thread{ObjectType::Thread, ResourceDomainId{10U}};
    TestKernelObject addressSpace{ObjectType::AddressSpace, ResourceDomainId{11U}};
    TestKernelObject endpoint{ObjectType::Endpoint, ResourceDomainId{12U}};
    TestKernelObject otherThread{ObjectType::Thread, ResourceDomainId{13U}};
    TestKernelObject otherAddressSpace{ObjectType::AddressSpace, ResourceDomainId{14U}};
    const RegisteredFaultInputs inputs = RegisterFaultInputs(fixture, thread, addressSpace, runner);
    const ObjectId endpointId = Register(fixture, endpoint, runner, "validation fixture registers an Endpoint");
    const ObjectId otherThreadId =
        Register(otherFixture, otherThread, runner, "validation fixture registers a Thread in another registry");
    const ObjectId otherAddressSpaceId = Register(otherFixture, otherAddressSpace, runner,
                                                  "validation fixture registers an AddressSpace in another registry");
    RegisteredObjectCleanup threadCleanup{fixture, runner, inputs.Thread};
    RegisteredObjectCleanup addressSpaceCleanup{fixture, runner, inputs.AddressSpace};
    RegisteredObjectCleanup endpointCleanup{fixture, runner, endpointId};
    RegisteredObjectCleanup otherThreadCleanup{otherFixture, runner, otherThreadId};
    RegisteredObjectCleanup otherAddressSpaceCleanup{otherFixture, runner, otherAddressSpaceId};
    if (!inputs.Thread.IsValid() || !inputs.AddressSpace.IsValid() || !endpointId.IsValid() ||
        !otherThreadId.IsValid() || !otherAddressSpaceId.IsValid())
    {
        return;
    }

    FaultToken token;
    const FaultTokenSnapshot initial = token.GetSnapshot();
    ASTERIA_EXPECT(runner, "default FaultToken is inert and exposes no registered identity", !token.IsInitialized(),
                   token.GetObjectType() == ObjectType::FaultToken, token.GetChargeOwner().GetValue() == 0U,
                   initial.State == FaultTokenState::Uninitialized, initial.Resolution == FaultResolutionKind::None,
                   initial.ClaimSequence == 0U, !initial.Thread.IsValid(), !initial.AddressSpace.IsValid(),
                   SnapshotReservedIsZero(initial));
    ASTERIA_EXPECT(runner, "uninitialized resolution operations are failure-atomic",
                   !token.ClaimResolution().HasValue(),
                   token.ClaimResolution().GetError() == FaultTokenError::NotInitialized,
                   token.CancelResolution({}) == FaultTokenError::NotInitialized,
                   token.CommitResolution({}, FaultResolutionKind::Map) == FaultTokenError::NotInitialized,
                   !token.GetResolutionClaim({}).HasValue(),
                   token.GetResolutionClaim({}).GetError() == FaultTokenError::NotInitialized,
                   SnapshotsEqual(initial, token.GetSnapshot()));

    FaultRecord invalidFault = MakeFaultRecord();
    invalidFault.Reserved[3] = 1U;
    FaultRecord invalidAccess = MakeFaultRecord();
    invalidAccess.Access = static_cast<FaultAccess>(0xFFU);
    FaultRecord invalidCause = MakeFaultRecord();
    invalidCause.Cause = static_cast<FaultCause>(0xFFU);
    FaultRecord invalidPrivilege = MakeFaultRecord();
    invalidPrivilege.Privilege = static_cast<FaultPrivilege>(0xFFU);
    const ThreadId threadId{fixture.Registry, inputs.Thread};
    const AddressSpaceId addressSpaceId{fixture.Registry, inputs.AddressSpace};
    ASTERIA_EXPECT(runner, "configuration and normalized fault validation are retryable",
                   token.Initialize(fixture.Registry, ResourceDomainId{}, threadId, addressSpaceId,
                                    MakeFaultRecord()) == FaultTokenError::InvalidConfiguration,
                   token.Initialize(fixture.Registry, ResourceDomainId{20U}, ThreadId{}, addressSpaceId,
                                    MakeFaultRecord()) == FaultTokenError::InvalidThread,
                   token.Initialize(fixture.Registry, ResourceDomainId{20U}, threadId, AddressSpaceId{},
                                    MakeFaultRecord()) == FaultTokenError::InvalidAddressSpace,
                   token.Initialize(fixture.Registry, ResourceDomainId{20U}, threadId, addressSpaceId, invalidFault) ==
                       FaultTokenError::InvalidFaultRecord,
                   token.Initialize(fixture.Registry, ResourceDomainId{20U}, threadId, addressSpaceId, invalidAccess) ==
                       FaultTokenError::InvalidFaultRecord,
                   token.Initialize(fixture.Registry, ResourceDomainId{20U}, threadId, addressSpaceId, invalidCause) ==
                       FaultTokenError::InvalidFaultRecord,
                   token.Initialize(fixture.Registry, ResourceDomainId{20U}, threadId, addressSpaceId,
                                    invalidPrivilege) == FaultTokenError::InvalidFaultRecord,
                   SnapshotsEqual(initial, token.GetSnapshot()), token.GetChargeOwner().GetValue() == 0U);

    ASTERIA_EXPECT(runner, "cross-registry identities cannot alias equal raw object values",
                   token.Initialize(fixture.Registry, ResourceDomainId{20U},
                                    ThreadId{otherFixture.Registry, otherThreadId}, addressSpaceId,
                                    MakeFaultRecord()) == FaultTokenError::ThreadRegistryMismatch,
                   token.Initialize(fixture.Registry, ResourceDomainId{20U}, threadId,
                                    AddressSpaceId{otherFixture.Registry, otherAddressSpaceId},
                                    MakeFaultRecord()) == FaultTokenError::AddressSpaceRegistryMismatch,
                   SnapshotsEqual(initial, token.GetSnapshot()));

    ASTERIA_EXPECT(runner, "registry pinning enforces the exact Thread and AddressSpace types",
                   token.Initialize(fixture.Registry, ResourceDomainId{20U}, ThreadId{fixture.Registry, endpointId},
                                    addressSpaceId, MakeFaultRecord()) == FaultTokenError::ThreadTypeMismatch,
                   token.Initialize(fixture.Registry, ResourceDomainId{20U}, threadId,
                                    AddressSpaceId{fixture.Registry, endpointId},
                                    MakeFaultRecord()) == FaultTokenError::AddressSpaceTypeMismatch,
                   SnapshotsEqual(initial, token.GetSnapshot()));

    const FaultRecord fault = MakeFaultRecord();
    ASTERIA_EXPECT(runner, "valid initialization publishes one pending fault",
                   token.Initialize(fixture.Registry, ResourceDomainId{20U}, threadId, addressSpaceId, fault) ==
                       FaultTokenError::None,
                   token.IsInitialized(), token.GetChargeOwner().GetValue() == 20U);
    const FaultTokenSnapshot pending = token.GetSnapshot();
    ASTERIA_EXPECT(runner, "pending snapshot retains the exact normalized fault and generations",
                   pending.State == FaultTokenState::Pending, pending.Resolution == FaultResolutionKind::None,
                   pending.ClaimSequence == 0U, pending.Thread.IsEqualTo(threadId),
                   pending.AddressSpace.IsEqualTo(addressSpaceId), RecordIsEqual(pending.Fault, fault),
                   SnapshotReservedIsZero(pending));
    ASTERIA_EXPECT(runner, "successful initialization is single-use",
                   token.Initialize(fixture.Registry, ResourceDomainId{21U}, threadId, addressSpaceId, fault) ==
                       FaultTokenError::AlreadyInitialized,
                   SnapshotsEqual(pending, token.GetSnapshot()), token.GetChargeOwner().GetValue() == 20U);

    const auto registered = fixture.Issuer.CreateObject(token);
    ASTERIA_EXPECT(runner, "initialized FaultToken registers as its own kernel object type", registered.HasValue(),
                   fixture.Registry.GetActiveCount() == 4U);
    if (registered.HasValue())
    {
        const ObjectId tokenId = registered.GetValue();
        const auto destroyed = fixture.Issuer.DestroyObject(tokenId);
        const auto collected = fixture.Issuer.CollectDestroyedObject();
        ASTERIA_EXPECT(runner, "unleased FaultToken completes ordinary registry destruction",
                       destroyed == ObjectRegistryError::Ok, collected.HasValue(),
                       collected.GetValue().Object.GetValue() == tokenId.GetValue(),
                       collected.GetValue().Instance == &token);
    }
}

void TestPinRollbackAndLifetime(TestContext& runner) noexcept
{
    RegistryFixture rollbackFixture;
    TestKernelObject rollbackThread{ObjectType::Thread, ResourceDomainId{30U}};
    TestKernelObject rollbackAddressSpace{ObjectType::AddressSpace, ResourceDomainId{31U}};
    const RegisteredFaultInputs rollbackInputs =
        RegisterFaultInputs(rollbackFixture, rollbackThread, rollbackAddressSpace, runner);
    if (!rollbackInputs.Thread.IsValid() || !rollbackInputs.AddressSpace.IsValid())
    {
        return;
    }

    ASTERIA_EXPECT(runner, "rollback fixture quiesces the AddressSpace before initialization",
                   rollbackFixture.Issuer.DestroyObject(rollbackInputs.AddressSpace) == ObjectRegistryError::Ok);
    FaultToken rollbackToken;
    ASTERIA_EXPECT(runner, "second pin failure rolls back the provisional Thread pin",
                   rollbackToken.Initialize(rollbackFixture.Registry, ResourceDomainId{32U},
                                            ThreadId{rollbackFixture.Registry, rollbackInputs.Thread},
                                            AddressSpaceId{rollbackFixture.Registry, rollbackInputs.AddressSpace},
                                            MakeFaultRecord()) == FaultTokenError::AddressSpaceNotFound,
                   !rollbackToken.IsInitialized(), rollbackToken.GetChargeOwner().GetValue() == 0U);
    ASTERIA_EXPECT(runner, "rollback fixture can quiesce the Thread after failed initialization",
                   rollbackFixture.Issuer.DestroyObject(rollbackInputs.Thread) == ObjectRegistryError::Ok);
    const auto rollbackFirst = rollbackFixture.Issuer.CollectDestroyedObject();
    const auto rollbackSecond = rollbackFixture.Issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "failed initialization leaves neither input generation pinned", rollbackFirst.HasValue(),
                   rollbackSecond.HasValue());

    RegistryFixture lifetimeFixture;
    TestKernelObject lifetimeThread{ObjectType::Thread, ResourceDomainId{33U}};
    TestKernelObject lifetimeAddressSpace{ObjectType::AddressSpace, ResourceDomainId{34U}};
    const RegisteredFaultInputs lifetimeInputs =
        RegisterFaultInputs(lifetimeFixture, lifetimeThread, lifetimeAddressSpace, runner);
    if (!lifetimeInputs.Thread.IsValid() || !lifetimeInputs.AddressSpace.IsValid())
    {
        return;
    }

    {
        FaultToken token;
        ASTERIA_EXPECT(runner, "lifetime fixture initializes its token",
                       token.Initialize(lifetimeFixture.Registry, ResourceDomainId{35U},
                                        ThreadId{lifetimeFixture.Registry, lifetimeInputs.Thread},
                                        AddressSpaceId{lifetimeFixture.Registry, lifetimeInputs.AddressSpace},
                                        MakeFaultRecord()) == FaultTokenError::None);
        ASTERIA_EXPECT(runner, "FaultToken pins both quiescing generations until destruction",
                       lifetimeFixture.Issuer.DestroyObject(lifetimeInputs.Thread) == ObjectRegistryError::Ok,
                       lifetimeFixture.Issuer.DestroyObject(lifetimeInputs.AddressSpace) == ObjectRegistryError::Ok,
                       !lifetimeFixture.Issuer.CollectDestroyedObject().HasValue());
    }
    const auto lifetimeFirst = lifetimeFixture.Issuer.CollectDestroyedObject();
    const auto lifetimeSecond = lifetimeFixture.Issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "FaultToken destruction releases both retained generations", lifetimeFirst.HasValue(),
                   lifetimeSecond.HasValue());
}

void TestClaimLifecycle(TestContext& runner) noexcept
{
    RegistryFixture fixture;
    TestKernelObject thread{ObjectType::Thread, ResourceDomainId{40U}};
    TestKernelObject addressSpace{ObjectType::AddressSpace, ResourceDomainId{41U}};
    const RegisteredFaultInputs inputs = RegisterFaultInputs(fixture, thread, addressSpace, runner);
    RegisteredObjectCleanup threadCleanup{fixture, runner, inputs.Thread};
    RegisteredObjectCleanup addressSpaceCleanup{fixture, runner, inputs.AddressSpace};
    if (!inputs.Thread.IsValid() || !inputs.AddressSpace.IsValid())
    {
        return;
    }

    const ThreadId threadId{fixture.Registry, inputs.Thread};
    const AddressSpaceId addressSpaceId{fixture.Registry, inputs.AddressSpace};
    FaultToken token;
    FaultToken otherToken;
    static_cast<void>(
        token.Initialize(fixture.Registry, ResourceDomainId{42U}, threadId, addressSpaceId, MakeFaultRecord()));
    static_cast<void>(
        otherToken.Initialize(fixture.Registry, ResourceDomainId{43U}, threadId, addressSpaceId, MakeFaultRecord()));

    const auto firstClaim = token.ClaimResolution();
    const auto authoritativeFirstClaim =
        firstClaim.HasValue() ? token.GetResolutionClaim(firstClaim.GetValue().Token) : token.GetResolutionClaim({});
    ASTERIA_EXPECT(runner, "first claim acquires the complete fault context with generation one", firstClaim.HasValue(),
                   authoritativeFirstClaim.HasValue(), firstClaim.GetValue().Token.IsValid(),
                   firstClaim.GetValue().Token.GetSequence() == 1U, firstClaim.GetValue().Thread.IsEqualTo(threadId),
                   firstClaim.GetValue().AddressSpace.IsEqualTo(addressSpaceId),
                   RecordIsEqual(firstClaim.GetValue().Fault, MakeFaultRecord()),
                   authoritativeFirstClaim.GetValue().Token.IsEqualTo(firstClaim.GetValue().Token),
                   authoritativeFirstClaim.GetValue().Thread.IsEqualTo(threadId),
                   authoritativeFirstClaim.GetValue().AddressSpace.IsEqualTo(addressSpaceId),
                   RecordIsEqual(authoritativeFirstClaim.GetValue().Fault, MakeFaultRecord()));
    const FaultTokenSnapshot claimed = token.GetSnapshot();
    ASTERIA_EXPECT(runner, "a claimed fault rejects a second resolver", claimed.State == FaultTokenState::Claimed,
                   claimed.ClaimSequence == 1U, !token.ClaimResolution().HasValue(),
                   token.ClaimResolution().GetError() == FaultTokenError::AlreadyClaimed);
    if (!firstClaim.HasValue())
    {
        return;
    }

    const auto foreignClaim = otherToken.ClaimResolution();
    ASTERIA_EXPECT(runner, "default and foreign claim tokens cannot alter the active resolver", foreignClaim.HasValue(),
                   token.CancelResolution({}) == FaultTokenError::InvalidClaim,
                   token.CancelResolution(foreignClaim.GetValue().Token) == FaultTokenError::InvalidClaim,
                   token.GetResolutionClaim({}).GetError() == FaultTokenError::InvalidClaim,
                   token.GetResolutionClaim(foreignClaim.GetValue().Token).GetError() == FaultTokenError::InvalidClaim,
                   SnapshotsEqual(claimed, token.GetSnapshot()));
    ASTERIA_EXPECT(runner, "cancelling the exact claim preserves its generation and retries pending",
                   token.CancelResolution(firstClaim.GetValue().Token) == FaultTokenError::None,
                   token.GetResolutionClaim(firstClaim.GetValue().Token).GetError() == FaultTokenError::NotClaimed,
                   token.GetSnapshot().State == FaultTokenState::Pending, token.GetSnapshot().ClaimSequence == 1U);

    const auto secondClaim = token.ClaimResolution();
    ASTERIA_EXPECT(runner, "cancelled claim generations can never resolve a later claim", secondClaim.HasValue(),
                   secondClaim.GetValue().Token.GetSequence() == 2U,
                   token.CancelResolution(firstClaim.GetValue().Token) == FaultTokenError::InvalidClaim,
                   token.CommitResolution(firstClaim.GetValue().Token, FaultResolutionKind::Map) ==
                       FaultTokenError::InvalidClaim);
    if (!secondClaim.HasValue())
    {
        return;
    }

    const FaultTokenSnapshot beforeInvalidCommit = token.GetSnapshot();
    ASTERIA_EXPECT(runner, "unknown resolution kinds are rejected without consuming the claim",
                   token.CommitResolution(secondClaim.GetValue().Token, FaultResolutionKind::None) ==
                       FaultTokenError::InvalidResolution,
                   token.CommitResolution(secondClaim.GetValue().Token, static_cast<FaultResolutionKind>(0xFFU)) ==
                       FaultTokenError::InvalidResolution,
                   SnapshotsEqual(beforeInvalidCommit, token.GetSnapshot()));
    ASTERIA_EXPECT(runner, "successful mapping consumes the exact active claim",
                   token.CommitResolution(secondClaim.GetValue().Token, FaultResolutionKind::Map) ==
                       FaultTokenError::None);
    const FaultTokenSnapshot resolved = token.GetSnapshot();
    ASTERIA_EXPECT(runner, "resolved FaultToken is terminal and retains its first resolution",
                   resolved.State == FaultTokenState::Resolved, resolved.Resolution == FaultResolutionKind::Map,
                   resolved.ClaimSequence == 2U, !token.ClaimResolution().HasValue(),
                   token.ClaimResolution().GetError() == FaultTokenError::AlreadyResolved,
                   token.CancelResolution(secondClaim.GetValue().Token) == FaultTokenError::AlreadyResolved,
                   token.GetResolutionClaim(secondClaim.GetValue().Token).GetError() ==
                       FaultTokenError::AlreadyResolved,
                   token.CommitResolution(secondClaim.GetValue().Token, FaultResolutionKind::Terminate) ==
                       FaultTokenError::AlreadyResolved);

    if (foreignClaim.HasValue())
    {
        static_cast<void>(otherToken.CancelResolution(foreignClaim.GetValue().Token));
    }
}

void TestResolutionKindsAndConcurrency(TestContext& runner) noexcept
{
    for (const FaultResolutionKind resolution : {FaultResolutionKind::Terminate, FaultResolutionKind::Redirect})
    {
        RegistryFixture fixture;
        TestKernelObject thread{ObjectType::Thread, ResourceDomainId{50U}};
        TestKernelObject addressSpace{ObjectType::AddressSpace, ResourceDomainId{51U}};
        const RegisteredFaultInputs inputs = RegisterFaultInputs(fixture, thread, addressSpace, runner);
        RegisteredObjectCleanup threadCleanup{fixture, runner, inputs.Thread};
        RegisteredObjectCleanup addressSpaceCleanup{fixture, runner, inputs.AddressSpace};
        if (!inputs.Thread.IsValid() || !inputs.AddressSpace.IsValid())
        {
            continue;
        }
        FaultToken token;
        static_cast<void>(token.Initialize(fixture.Registry, ResourceDomainId{52U},
                                           ThreadId{fixture.Registry, inputs.Thread},
                                           AddressSpaceId{fixture.Registry, inputs.AddressSpace}, MakeFaultRecord()));
        const auto claim = token.ClaimResolution();
        ASTERIA_EXPECT(runner, "terminate and redirect are valid terminal fault resolutions", claim.HasValue(),
                       token.CommitResolution(claim.GetValue().Token, resolution) == FaultTokenError::None,
                       token.GetSnapshot().Resolution == resolution);
    }

    RegistryFixture fixture;
    TestKernelObject thread{ObjectType::Thread, ResourceDomainId{53U}};
    TestKernelObject addressSpace{ObjectType::AddressSpace, ResourceDomainId{54U}};
    const RegisteredFaultInputs inputs = RegisterFaultInputs(fixture, thread, addressSpace, runner);
    RegisteredObjectCleanup threadCleanup{fixture, runner, inputs.Thread};
    RegisteredObjectCleanup addressSpaceCleanup{fixture, runner, inputs.AddressSpace};
    if (!inputs.Thread.IsValid() || !inputs.AddressSpace.IsValid())
    {
        return;
    }
    FaultToken token;
    static_cast<void>(token.Initialize(fixture.Registry, ResourceDomainId{55U},
                                       ThreadId{fixture.Registry, inputs.Thread},
                                       AddressSpaceId{fixture.Registry, inputs.AddressSpace}, MakeFaultRecord()));

    std::atomic<bool> start{false};
    bool succeeded[2]{};
    FaultTokenError errors[2]{};
    FaultResolutionClaimToken claims[2]{};
    auto resolver = [&](const Foundation::Size index) noexcept
    {
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        const auto result = token.ClaimResolution();
        succeeded[index] = result.HasValue();
        errors[index] = result.GetError();
        if (result.HasValue())
        {
            claims[index] = result.GetValue().Token;
        }
    };

    std::thread first{resolver, Foundation::Size{0U}};
    std::thread second{resolver, Foundation::Size{1U}};
    start.store(true, std::memory_order_release);
    first.join();
    second.join();

    const Foundation::Size successCount =
        static_cast<Foundation::Size>(succeeded[0] ? 1U : 0U) + static_cast<Foundation::Size>(succeeded[1] ? 1U : 0U);
    const bool loserIsClaimed =
        succeeded[0] ? (errors[1] == FaultTokenError::AlreadyClaimed) : (errors[0] == FaultTokenError::AlreadyClaimed);
    ASTERIA_EXPECT(runner, "concurrent resolvers publish exactly one active claim", successCount == 1U, loserIsClaimed,
                   token.GetSnapshot().State == FaultTokenState::Claimed);
    const FaultResolutionClaimToken winner = succeeded[0] ? claims[0] : claims[1];
    ASTERIA_EXPECT(runner, "the concurrent winning claim remains an ordinary cancellable claim",
                   token.CancelResolution(winner) == FaultTokenError::None);
}

void TestClaimSequenceBoundary(TestContext& runner) noexcept
{
    Foundation::UInt64 next{0U};
    ASTERIA_EXPECT(runner, "claim sequence starts at one", FaultClaimSequencePolicy::TryAdvance(0U, next), next == 1U);
    next = 99U;
    ASTERIA_EXPECT(runner, "claim sequence admits its final nonwrapping generation",
                   FaultClaimSequencePolicy::TryAdvance(FaultClaimSequencePolicy::Maximum - 1U, next),
                   next == FaultClaimSequencePolicy::Maximum);
    next = 77U;
    ASTERIA_EXPECT(runner, "claim sequence exhaustion is failure-atomic",
                   !FaultClaimSequencePolicy::TryAdvance(FaultClaimSequencePolicy::Maximum, next), next == 77U);
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"FaultTokenTests"};
    Asteria::Test::TestInitializationAndRegistration(runner);
    Asteria::Test::TestPinRollbackAndLifetime(runner);
    Asteria::Test::TestClaimLifecycle(runner);
    Asteria::Test::TestResolutionKindsAndConcurrency(runner);
    Asteria::Test::TestClaimSequenceBoundary(runner);
    return runner.Finish();
}
