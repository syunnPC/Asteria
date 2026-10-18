// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Kernel/Memory/FaultTokenFactory.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"

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
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::FaultAccess;
using Kernel::Memory::FaultCause;
using Kernel::Memory::FaultPrivilege;
using Kernel::Memory::FaultRecord;
using Kernel::Memory::FaultResolutionClaimToken;
using Kernel::Memory::FaultResolutionKind;
using Kernel::Memory::FaultTokenCreateRequest;
using Kernel::Memory::FaultTokenCreation;
using Kernel::Memory::FaultTokenError;
using Kernel::Memory::FaultTokenFactory;
using Kernel::Memory::FaultTokenFactoryError;
using Kernel::Memory::FaultTokenFactoryState;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityOperation;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::IKernelObject;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationDomainId;
using Kernel::Object::RevocationRegistry;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Scheduler::ThreadId;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

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

class FactoryFixture final
{
public:
    FactoryFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          Space{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        const auto thread = Issuer.CreateObject(Thread);
        const auto address = Issuer.CreateObject(AddressSpace);
        const auto domain = Revocations.CreateRoot();
        if (!thread.HasValue() || !address.HasValue() || !domain.HasValue())
        {
            return false;
        }
        ThreadObject = thread.GetValue();
        AddressObject = address.GetValue();
        ThreadIdentity = ThreadId{Registry, ThreadObject};
        AddressIdentity = AddressSpaceId{Registry, AddressObject};
        Domain = domain.GetValue();
        return true;
    }

    [[nodiscard]] bool RetireInputs() noexcept
    {
        if ((Issuer.DestroyObject(ThreadObject) != ObjectRegistryError::Ok) ||
            (Issuer.DestroyObject(AddressObject) != ObjectRegistryError::Ok))
        {
            return false;
        }
        const auto first = Issuer.CollectDestroyedObject();
        const auto second = Issuer.CollectDestroyedObject();
        return first.HasValue() && second.HasValue();
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace Space;
    TestKernelObject Thread{ObjectType::Thread, ResourceDomainId{7U}};
    TestKernelObject AddressSpace{ObjectType::AddressSpace, ResourceDomainId{7U}};
    ObjectId ThreadObject{};
    ObjectId AddressObject{};
    ThreadId ThreadIdentity{};
    AddressSpaceId AddressIdentity{};
    RevocationDomainId Domain{};
};

[[nodiscard]] constexpr FaultRecord MakeFault() noexcept
{
    return FaultRecord{
        .VirtualAddress = 0x0000'0001'2345'6000ULL,
        .InstructionPointer = 0x0000'0000'0040'1234ULL,
        .ArchitectureSyndrome = 0xA55AU,
        .Access = FaultAccess::Write,
        .Cause = FaultCause::MissingMapping,
        .Privilege = FaultPrivilege::User,
        .Reserved = {},
    };
}

[[nodiscard]] FaultTokenCreateRequest MakeRequest(FactoryFixture& fixture) noexcept
{
    return FaultTokenCreateRequest{
        .Destination = &fixture.Space,
        .ChargeOwner = ResourceDomainId{7U},
        .Thread = fixture.ThreadIdentity,
        .AddressSpace = fixture.AddressIdentity,
        .Fault = MakeFault(),
        .RevocationDomain = fixture.Domain,
        .LeaseDeadline = MonotonicTime::Infinite(),
    };
}

[[nodiscard]] bool RetireAndReclaim(FaultTokenFactory& factory, FactoryFixture& fixture,
                                    const FaultTokenCreation& creation) noexcept
{
    if (fixture.Space.Close(creation.Capability) != CapabilityError::Ok)
    {
        return false;
    }
    const auto retired = factory.Retire(creation.Token);
    const auto collected = fixture.Issuer.CollectDestroyedObject();
    if (!retired.HasValue() || !collected.HasValue())
    {
        return false;
    }
    const auto reclaimed = factory.Reclaim(collected.GetValue());
    return reclaimed.HasValue() && (reclaimed.GetValue().GetValue() == creation.Token.GetValue());
}

void TestCreateResolveAndReuse(TestContext& runner) noexcept
{
    FactoryFixture fixture;
    ASTERIA_EXPECT(runner, "reuse fixture initializes", fixture.Initialize());
    FaultTokenFactory factory{fixture.Registry, fixture.Issuer};
    const auto created = factory.Create(MakeRequest(fixture));
    if (!created.HasValue())
    {
        ASTERIA_EXPECT(runner, "factory creates its first token", false);
        static_cast<void>(fixture.RetireInputs());
        return;
    }
    const FaultTokenCreation first = created.GetValue();

    bool invokeMatches{false};
    bool transferMatches{false};
    bool inspectDenied{false};
    {
        auto invoke = fixture.Space.Lookup(first.Capability, ObjectType::FaultToken, CapabilityOperation::Invoke);
        constexpr auto expectedRights = Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Invoke}.With(
            Kernel::Object::CapabilityRight::Transfer);
        invokeMatches = invoke.HasValue() && (&invoke.GetValue().GetObject() == first.Storage) &&
                        (invoke.GetValue().GetDescriptor().Object.GetValue() == first.Token.GetValue()) &&
                        invoke.GetValue().GetDescriptor().Rights.IsEqualTo(expectedRights);
    }
    {
        auto transfer = fixture.Space.Lookup(first.Capability, ObjectType::FaultToken, CapabilityOperation::Transfer);
        transferMatches = transfer.HasValue();
    }
    {
        auto inspect = fixture.Space.Lookup(first.Capability, ObjectType::FaultToken, CapabilityOperation::Inspect);
        inspectDenied = !inspect.HasValue() && (inspect.GetError() == CapabilityError::RightsDenied);
    }
    const auto firstClaim = first.Storage->ClaimResolution();
    const FaultResolutionClaimToken staleClaim =
        firstClaim.HasValue() ? firstClaim.GetValue().Token : FaultResolutionClaimToken{};
    const bool firstResolved =
        firstClaim.HasValue() && (first.Storage->CommitResolution(firstClaim.GetValue().Token,
                                                                  FaultResolutionKind::Map) == FaultTokenError::None);
    ASTERIA_EXPECT(runner, "Create publishes one exact Invoke and Transfer capability", invokeMatches, transferMatches,
                   inspectDenied, firstResolved, factory.GetSnapshot().ActiveSlots == 1U);
    ASTERIA_EXPECT(runner, "resolved token retires and returns its fixed slot",
                   RetireAndReclaim(factory, fixture, first),
                   factory.GetSnapshot().FreeSlots == FaultTokenFactory::MaximumTokens);

    const auto recreated = factory.Create(MakeRequest(fixture));
    if (!recreated.HasValue())
    {
        ASTERIA_EXPECT(runner, "reclaimed slot creates a replacement token", false);
        static_cast<void>(fixture.RetireInputs());
        return;
    }
    const FaultTokenCreation second = recreated.GetValue();
    const auto secondClaim = second.Storage->ClaimResolution();
    const bool staleRejected = secondClaim.HasValue() && (secondClaim.GetValue().Token.GetSequence() == 2U) &&
                               (second.Storage->CancelResolution(staleClaim) == FaultTokenError::InvalidClaim);
    const bool secondResolved =
        secondClaim.HasValue() &&
        (second.Storage->CommitResolution(secondClaim.GetValue().Token, FaultResolutionKind::Redirect) ==
         FaultTokenError::None);
    ASTERIA_EXPECT(runner, "slot reuse advances both object and resolver generations without ABA",
                   second.Storage == first.Storage, second.Token.GetValue() != first.Token.GetValue(), staleRejected,
                   secondResolved);
    ASTERIA_EXPECT(runner, "replacement token retires cleanly", RetireAndReclaim(factory, fixture, second));
    ASTERIA_EXPECT(runner, "reuse fixture retires retained inputs after token reclamation", fixture.RetireInputs());
}

void TestFailureRollback(TestContext& runner) noexcept
{
    FactoryFixture fixture;
    FactoryFixture foreign;
    ASTERIA_EXPECT(runner, "rollback fixtures initialize", fixture.Initialize(), foreign.Initialize());
    FaultTokenFactory factory{fixture.Registry, fixture.Issuer};

    FaultTokenCreateRequest invalid = MakeRequest(fixture);
    invalid.Fault.Reserved[2U] = 1U;
    const auto rejected = factory.Create(invalid);
    ASTERIA_EXPECT(runner, "pre-publication rejection leaves every slot and registry unchanged", !rejected.HasValue(),
                   rejected.GetError().Error == FaultTokenFactoryError::TokenRejected,
                   rejected.GetError().TokenError == FaultTokenError::InvalidFaultRecord,
                   factory.GetSnapshot().FreeSlots == FaultTokenFactory::MaximumTokens,
                   fixture.Registry.GetActiveCount() == 2U, fixture.Space.GetActiveCount() == 0U);

    FaultTokenCreateRequest wrongSpace = MakeRequest(fixture);
    wrongSpace.Destination = &foreign.Space;
    wrongSpace.RevocationDomain = foreign.Domain;
    const auto mintRejected = factory.Create(wrongSpace);
    const auto beforeCollection = factory.GetSnapshot();
    const auto collected = fixture.Issuer.CollectDestroyedObject();
    const auto reclaimed =
        collected.HasValue() ? factory.Reclaim(collected.GetValue()) : decltype(factory.Reclaim({}))::Failure({});
    ASTERIA_EXPECT(runner, "mint rejection starts destruction and the exact record restores capacity",
                   !mintRejected.HasValue(),
                   mintRejected.GetError().Error == FaultTokenFactoryError::CapabilityRejected,
                   mintRejected.GetError().CapabilityError == CapabilityError::InvalidConfiguration,
                   beforeCollection.QuiescingSlots == 1U, fixture.Registry.GetActiveCount() == 2U, collected.HasValue(),
                   reclaimed.HasValue(), factory.GetSnapshot().FreeSlots == FaultTokenFactory::MaximumTokens);

    const auto valid = factory.Create(MakeRequest(fixture));
    const bool validRetired = valid.HasValue() && RetireAndReclaim(factory, fixture, valid.GetValue());
    ASTERIA_EXPECT(runner, "factory remains usable after a fully reclaimed mint rollback", validRetired);
    ASTERIA_EXPECT(runner, "rollback fixtures retire all input objects", fixture.RetireInputs(),
                   foreign.RetireInputs());
}

void TestFixedCapacity(TestContext& runner) noexcept
{
    FactoryFixture fixture;
    ASTERIA_EXPECT(runner, "capacity fixture initializes", fixture.Initialize());
    FaultTokenFactory factory{fixture.Registry, fixture.Issuer};
    FaultTokenCreation creations[FaultTokenFactory::MaximumTokens]{};
    bool filled{true};
    for (Size index{0U}; index < FaultTokenFactory::MaximumTokens; ++index)
    {
        const auto created = factory.Create(MakeRequest(fixture));
        if (!created.HasValue())
        {
            filled = false;
            break;
        }
        creations[index] = created.GetValue();
    }
    const auto overflow = factory.Create(MakeRequest(fixture));
    ASTERIA_EXPECT(runner, "fixed capacity rejects the seventeenth live token without side effects", filled,
                   !overflow.HasValue(), overflow.GetError().Error == FaultTokenFactoryError::CapacityExceeded,
                   factory.GetSnapshot().ActiveSlots == FaultTokenFactory::MaximumTokens);

    bool retiredAll{filled};
    if (filled)
    {
        for (const FaultTokenCreation& creation : creations)
        {
            if ((fixture.Space.Close(creation.Capability) != CapabilityError::Ok) ||
                !factory.Retire(creation.Token).HasValue())
            {
                retiredAll = false;
                break;
            }
        }
        for (Size index{0U}; retiredAll && (index < FaultTokenFactory::MaximumTokens); ++index)
        {
            const auto collected = fixture.Issuer.CollectDestroyedObject();
            if (!collected.HasValue() || !factory.Reclaim(collected.GetValue()).HasValue())
            {
                retiredAll = false;
            }
        }
    }
    const auto final = factory.GetSnapshot();
    ASTERIA_EXPECT(runner, "all fixed slots return to the ready pool after exact collection", retiredAll,
                   final.State == FaultTokenFactoryState::Ready, final.FreeSlots == FaultTokenFactory::MaximumTokens,
                   final.ActiveSlots == 0U, final.QuiescingSlots == 0U, final.RetiredSlots == 0U,
                   final.FaultedSlots == 0U);
    ASTERIA_EXPECT(runner, "capacity fixture retires retained inputs", fixture.RetireInputs());
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"FaultTokenFactoryTests"};
    Asteria::Test::TestCreateResolveAndReuse(runner);
    Asteria::Test::TestFailureRollback(runner);
    Asteria::Test::TestFixedCapacity(runner);
    return runner.Finish();
}
