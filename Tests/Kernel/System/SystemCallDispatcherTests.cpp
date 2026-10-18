// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Kernel/Object/CapabilitySpacePool.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/System/SystemCallDispatcher.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::UInt64;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
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
using Kernel::System::SystemCallDispatcher;
using Kernel::System::SystemCallDeriveChildRevocationDomain;
using Kernel::System::SystemCallError;
using Kernel::System::SystemCallNumber;
using Kernel::System::SystemCallQueryObjectTypeMask;
using Kernel::System::SystemCallQueryRightsShift;
using Kernel::System::SystemCallRequest;
using Kernel::System::SystemCallResponse;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

class TestKernelObject final : public IKernelObject
{
public:
    explicit constexpr TestKernelObject(const ObjectType type = ObjectType::MemoryObject) noexcept : m_type{type}
    {
    }

    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return m_type;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return ResourceDomainId{77U};
    }

private:
    ObjectType m_type;
};

class DispatcherFixture final
{
public:
    explicit DispatcherFixture(const ObjectType objectType = ObjectType::MemoryObject) noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          Space{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL}, Object{objectType}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        const auto object = Issuer.CreateObject(Object);
        const auto revocation = Revocations.CreateRoot();
        if (!object.HasValue() || !revocation.HasValue())
        {
            return false;
        }
        ObjectIdentity = object.GetValue();
        Revocation = revocation.GetValue();
        return true;
    }

    [[nodiscard]] Foundation::Result<CapabilityHandle, CapabilityError>
    Mint(const CapabilityRights rights, const MonotonicTime deadline = MonotonicTime::Infinite()) noexcept
    {
        return Issuer.Mint(Space, ObjectIdentity, rights, Revocation, deadline, 0x1234U);
    }

    [[nodiscard]] bool Finish() noexcept
    {
        if ((Space.GetActiveCount() != 0U) || (Issuer.DestroyObject(ObjectIdentity) != ObjectRegistryError::Ok))
        {
            return false;
        }
        return CollectDestroyedObject();
    }

    [[nodiscard]] bool FinishDestroyedObject() noexcept
    {
        return (Space.GetActiveCount() == 0U) && CollectDestroyedObject();
    }

    [[nodiscard]] bool CollectDestroyedObject() noexcept
    {
        const auto collected = Issuer.CollectDestroyedObject();
        return collected.HasValue() && (collected.GetValue().Object.GetValue() == ObjectIdentity.GetValue()) &&
               (collected.GetValue().Instance == &Object);
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace Space;
    TestKernelObject Object;
    ObjectId ObjectIdentity{};
    RevocationDomainId Revocation{};
};

[[nodiscard]] constexpr CapabilityRights MakeFullRights() noexcept
{
    return CapabilityRights{CapabilityRight::Inspect}
        .With(CapabilityRight::Duplicate)
        .With(CapabilityRight::Derive)
        .With(CapabilityRight::Revoke);
}

[[nodiscard]] constexpr SystemCallRequest MakeRequest(const SystemCallNumber number,
                                                      const CapabilityHandle handle) noexcept
{
    return SystemCallRequest{
        .Number = static_cast<UInt64>(number),
        .Arguments =
            {
                handle.GetSelector(),
                handle.GetAuthenticator(),
                0U,
                0U,
                0U,
                0U,
            },
    };
}

[[nodiscard]] constexpr SystemCallRequest MakeCreationRequest(const SystemCallNumber number,
                                                              const CapabilityHandle handle,
                                                              const CapabilityRights rights,
                                                              const MonotonicTime deadline) noexcept
{
    SystemCallRequest request = MakeRequest(number, handle);
    request.Arguments[2U] = rights.GetRawValue();
    request.Arguments[3U] = deadline.GetTicks();
    return request;
}

[[nodiscard]] constexpr CapabilityHandle DecodeHandle(const SystemCallResponse& response) noexcept
{
    return CapabilityHandle{response.Value0, response.Value1};
}

[[nodiscard]] constexpr bool ResponseIsEmptyError(const SystemCallResponse& response,
                                                  const SystemCallError error) noexcept
{
    return (response.Error == error) && (response.Value0 == 0U) && (response.Value1 == 0U);
}

void TestValidationAndClose(TestContext& runner) noexcept
{
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "system-call validation fixture initializes", fixture.Initialize());
    const auto minted = fixture.Mint(MakeFullRights());
    if (!minted.HasValue())
    {
        ASTERIA_EXPECT(runner, "validation capability mints", false);
        return;
    }
    const CapabilityHandle handle = minted.GetValue();

    SystemCallRequest unknown = MakeRequest(SystemCallNumber::CapabilityQuery, handle);
    unknown.Number = 0xFFFF'FFFF'FFFF'FFFFULL;
    unknown.Arguments[5U] = 1U;
    ASTERIA_EXPECT(runner, "unknown syscall numbers return one canonical empty error",
                   ResponseIsEmptyError(SystemCallDispatcher::Dispatch(fixture.Space, unknown),
                                        SystemCallError::UnknownSystemCall));

    constexpr SystemCallNumber fixedArityCalls[]{
        SystemCallNumber::CapabilityClose,
        SystemCallNumber::CapabilityQuery,
        SystemCallNumber::CapabilityRevoke,
    };
    bool allUnusedRejected{true};
    for (const SystemCallNumber number : fixedArityCalls)
    {
        for (Foundation::Size index{2U}; index < 6U; ++index)
        {
            SystemCallRequest malformed = MakeRequest(number, handle);
            malformed.Arguments[index] = 1U;
            allUnusedRejected = ResponseIsEmptyError(SystemCallDispatcher::Dispatch(fixture.Space, malformed),
                                                     SystemCallError::InvalidArguments) &&
                                allUnusedRejected;
        }
    }
    ASTERIA_EXPECT(
        runner, "all nonzero unused fixed-arity arguments fail without mutation", allUnusedRejected,
        fixture.Space.GetActiveCount() == 1U,
        SystemCallDispatcher::Dispatch(fixture.Space, MakeRequest(SystemCallNumber::CapabilityQuery, handle)).Error ==
            SystemCallError::None);

    SystemCallRequest malformedCreation = MakeCreationRequest(
        SystemCallNumber::CapabilityDerive, handle, CapabilityRights{CapabilityRight::Inspect}, MonotonicTime{50U});
    // Derive argument 4 defines only bit 0, and argument 5 is unused.
    bool allCreationUnusedRejected{true};
    for (Foundation::Size index{4U}; index < 6U; ++index)
    {
        malformedCreation.Arguments[index] = 2U;
        allCreationUnusedRejected =
            ResponseIsEmptyError(SystemCallDispatcher::Dispatch(fixture.Space, malformedCreation),
                                 SystemCallError::InvalidArguments) &&
            allCreationUnusedRejected;
        malformedCreation.Arguments[index] = 0U;
    }
    SystemCallRequest flaggedDuplicate = MakeCreationRequest(
        SystemCallNumber::CapabilityDuplicate, handle, CapabilityRights{CapabilityRight::Inspect}, MonotonicTime{50U});
    flaggedDuplicate.Arguments[4U] = SystemCallDeriveChildRevocationDomain;
    allCreationUnusedRejected = ResponseIsEmptyError(SystemCallDispatcher::Dispatch(fixture.Space, flaggedDuplicate),
                                                     SystemCallError::InvalidArguments) &&
                                allCreationUnusedRejected;
    malformedCreation.Arguments[2U] = 0U;
    const bool zeroRightsRejected = ResponseIsEmptyError(
        SystemCallDispatcher::Dispatch(fixture.Space, malformedCreation), SystemCallError::InvalidArguments);
    malformedCreation.Arguments[2U] = 0x4000U;
    const bool unknownRightsRejected = ResponseIsEmptyError(
        SystemCallDispatcher::Dispatch(fixture.Space, malformedCreation), SystemCallError::InvalidArguments);
    malformedCreation.Arguments[2U] = 0x1'0001U;
    const bool wideRightsRejected = ResponseIsEmptyError(
        SystemCallDispatcher::Dispatch(fixture.Space, malformedCreation), SystemCallError::InvalidArguments);
    ASTERIA_EXPECT(runner,
                   "creation arguments reject unknown flags, unused arguments, and empty, unknown, and wide rights",
                   allCreationUnusedRejected, zeroRightsRejected, unknownRightsRejected, wideRightsRejected,
                   fixture.Space.GetActiveCount() == 1U);

    CapabilityHandle forged{handle.GetSelector(), handle.GetAuthenticator() ^ 1U};
    ASTERIA_EXPECT(runner, "query authenticates both handle words",
                   ResponseIsEmptyError(SystemCallDispatcher::Dispatch(
                                            fixture.Space, MakeRequest(SystemCallNumber::CapabilityQuery, forged)),
                                        SystemCallError::InvalidHandle));

    const auto nonInspectable = fixture.Mint(CapabilityRights{CapabilityRight::Duplicate});
    bool queryDenied{false};
    if (nonInspectable.HasValue())
    {
        queryDenied = ResponseIsEmptyError(
            SystemCallDispatcher::Dispatch(fixture.Space,
                                           MakeRequest(SystemCallNumber::CapabilityQuery, nonInspectable.GetValue())),
            SystemCallError::RightsDenied);
    }
    ASTERIA_EXPECT(runner, "query requires Inspect without consuming a denied capability", nonInspectable.HasValue(),
                   queryDenied, fixture.Space.GetActiveCount() == 2U);

    fixture.Clock.Set(9U);
    const SystemCallResponse regressed =
        SystemCallDispatcher::Dispatch(fixture.Space, MakeRequest(SystemCallNumber::CapabilityQuery, handle));
    fixture.Clock.Set(10U);
    ASTERIA_EXPECT(runner, "a regressed caller clock fails closed at the dispatcher boundary",
                   ResponseIsEmptyError(regressed, SystemCallError::InvalidConfiguration));

    const SystemCallResponse closed =
        SystemCallDispatcher::Dispatch(fixture.Space, MakeRequest(SystemCallNumber::CapabilityClose, handle));
    const SystemCallResponse stale =
        SystemCallDispatcher::Dispatch(fixture.Space, MakeRequest(SystemCallNumber::CapabilityClose, handle));
    const bool auxiliaryClosed =
        nonInspectable.HasValue() && (fixture.Space.Close(nonInspectable.GetValue()) == CapabilityError::Ok);
    ASTERIA_EXPECT(runner, "close invalidates one exact handle generation and returns no data",
                   ResponseIsEmptyError(closed, SystemCallError::None),
                   ResponseIsEmptyError(stale, SystemCallError::InvalidHandle), auxiliaryClosed, fixture.Finish());
}

void TestCreationAndQuery(TestContext& runner) noexcept
{
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "system-call creation fixture initializes", fixture.Initialize());
    const CapabilityRights fullRights = MakeFullRights();
    const CapabilityRights plainRights =
        CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Duplicate).With(CapabilityRight::Derive);
    const auto minted = fixture.Mint(fullRights);
    const auto finite = fixture.Mint(fullRights, MonotonicTime{100U});
    const auto mintedPlain = fixture.Mint(plainRights);
    if (!minted.HasValue() || !finite.HasValue() || !mintedPlain.HasValue())
    {
        ASTERIA_EXPECT(runner, "creation source capabilities mint", false);
        return;
    }
    const CapabilityHandle source = minted.GetValue();
    const CapabilityHandle plain = mintedPlain.GetValue();

    const SystemCallResponse queried =
        SystemCallDispatcher::Dispatch(fixture.Space, MakeRequest(SystemCallNumber::CapabilityQuery, source));
    const UInt64 expectedInformation =
        UInt64{7U} | (static_cast<UInt64>(fullRights.GetRawValue()) << SystemCallQueryRightsShift);
    ASTERIA_EXPECT(runner, "query returns only the stable object-type, rights, and deadline ABI",
                   queried.Error == SystemCallError::None, queried.Value0 == expectedInformation,
                   (queried.Value0 & SystemCallQueryObjectTypeMask) == 7U,
                   queried.Value1 == MonotonicTime::Infinite().GetTicks());

    constexpr CapabilityRights inspectRights{CapabilityRight::Inspect};
    const SystemCallResponse derived =
        SystemCallDispatcher::Dispatch(fixture.Space, MakeCreationRequest(SystemCallNumber::CapabilityDerive, source,
                                                                          inspectRights, MonotonicTime{50U}));
    const CapabilityHandle derivedHandle = DecodeHandle(derived);
    const SystemCallResponse derivedQuery =
        SystemCallDispatcher::Dispatch(fixture.Space, MakeRequest(SystemCallNumber::CapabilityQuery, derivedHandle));
    ASTERIA_EXPECT(runner, "derive returns one strictly attenuated handle in two result words",
                   derived.Error == SystemCallError::None, derivedHandle.IsValid(),
                   derivedQuery.Error == SystemCallError::None,
                   derivedQuery.Value0 ==
                       (UInt64{7U} | (static_cast<UInt64>(inspectRights.GetRawValue()) << SystemCallQueryRightsShift)),
                   derivedQuery.Value1 == 50U);

    // A shorter lease with equal rights is no attenuation.
    const SystemCallResponse noAttenuation = SystemCallDispatcher::Dispatch(
        fixture.Space, MakeCreationRequest(SystemCallNumber::CapabilityDerive, plain, plainRights, MonotonicTime{50U}));
    // Revoke over the source's own domain would revoke the source.
    const SystemCallResponse sameDomainRevoke = SystemCallDispatcher::Dispatch(
        fixture.Space, MakeCreationRequest(SystemCallNumber::CapabilityDerive, source,
                                           CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Revoke),
                                           MonotonicTime{50U}));
    const SystemCallResponse duplicatedRevoke =
        SystemCallDispatcher::Dispatch(fixture.Space, MakeCreationRequest(SystemCallNumber::CapabilityDuplicate, source,
                                                                          fullRights, MonotonicTime::Infinite()));
    const CapabilityRights amplified = fullRights.With(CapabilityRight::Invoke);
    const SystemCallResponse amplification = SystemCallDispatcher::Dispatch(
        fixture.Space, MakeCreationRequest(SystemCallNumber::CapabilityDerive, source, amplified, MonotonicTime{50U}));
    const SystemCallResponse expired =
        SystemCallDispatcher::Dispatch(fixture.Space, MakeCreationRequest(SystemCallNumber::CapabilityDerive, source,
                                                                          inspectRights, MonotonicTime{10U}));
    const SystemCallResponse extension = SystemCallDispatcher::Dispatch(
        fixture.Space, MakeCreationRequest(SystemCallNumber::CapabilityDuplicate, finite.GetValue(), plainRights,
                                           MonotonicTime{101U}));
    ASTERIA_EXPECT(runner, "creation errors preserve attenuation, rights, and deadline policy",
                   ResponseIsEmptyError(noAttenuation, SystemCallError::NoAttenuation),
                   ResponseIsEmptyError(sameDomainRevoke, SystemCallError::RightsAmplification),
                   ResponseIsEmptyError(duplicatedRevoke, SystemCallError::RightsAmplification),
                   ResponseIsEmptyError(amplification, SystemCallError::RightsAmplification),
                   ResponseIsEmptyError(expired, SystemCallError::Expired),
                   ResponseIsEmptyError(extension, SystemCallError::LeaseExtension));

    const SystemCallResponse duplicate =
        SystemCallDispatcher::Dispatch(fixture.Space, MakeCreationRequest(SystemCallNumber::CapabilityDuplicate, plain,
                                                                          plainRights, MonotonicTime::Infinite()));
    const CapabilityHandle duplicateHandle = DecodeHandle(duplicate);
    const SystemCallResponse denied = SystemCallDispatcher::Dispatch(
        fixture.Space,
        MakeCreationRequest(SystemCallNumber::CapabilityDuplicate, derivedHandle, inspectRights, MonotonicTime{50U}));
    ASTERIA_EXPECT(runner, "duplicate permits equality while an attenuated source cannot duplicate",
                   duplicate.Error == SystemCallError::None, duplicateHandle.IsValid(),
                   ResponseIsEmptyError(denied, SystemCallError::RightsDenied));

    const bool closed = (fixture.Space.Close(derivedHandle) == CapabilityError::Ok) &&
                        (fixture.Space.Close(duplicateHandle) == CapabilityError::Ok) &&
                        (fixture.Space.Close(finite.GetValue()) == CapabilityError::Ok) &&
                        (fixture.Space.Close(plain) == CapabilityError::Ok) &&
                        (fixture.Space.Close(source) == CapabilityError::Ok);
    ASTERIA_EXPECT(runner, "created syscall handles leave no capability or object behind", closed, fixture.Finish());
}

[[nodiscard]] SystemCallResponse DeriveBranch(DispatcherFixture& fixture, const CapabilityHandle source,
                                              const CapabilityRights rights = MakeFullRights()) noexcept
{
    SystemCallRequest request =
        MakeCreationRequest(SystemCallNumber::CapabilityDerive, source, rights, MonotonicTime::Infinite());
    request.Arguments[4U] = SystemCallDeriveChildRevocationDomain;
    return SystemCallDispatcher::Dispatch(fixture.Space, request);
}

[[nodiscard]] SystemCallError QueryError(DispatcherFixture& fixture, const CapabilityHandle handle) noexcept
{
    return SystemCallDispatcher::Dispatch(fixture.Space, MakeRequest(SystemCallNumber::CapabilityQuery, handle)).Error;
}

// A branch derived into a child revocation domain keeps every right,
// Revoke included: revoking it leaves its source live, revoking the source
// ends the branch, and branches nest only to the registry's depth.
void TestChildRevocationDomain(TestContext& runner) noexcept
{
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "system-call branch fixture initializes", fixture.Initialize());
    const auto minted = fixture.Mint(MakeFullRights());
    if (!minted.HasValue())
    {
        ASTERIA_EXPECT(runner, "branch source capability mints", false);
        return;
    }
    const CapabilityHandle source = minted.GetValue();

    const SystemCallResponse branched = DeriveBranch(fixture, source);
    const CapabilityHandle branch = DecodeHandle(branched);
    const SystemCallResponse branchQuery =
        SystemCallDispatcher::Dispatch(fixture.Space, MakeRequest(SystemCallNumber::CapabilityQuery, branch));
    const SystemCallResponse branchRevoked =
        SystemCallDispatcher::Dispatch(fixture.Space, MakeRequest(SystemCallNumber::CapabilityRevoke, branch));
    ASTERIA_EXPECT(
        runner, "a branch keeps Revoke and revoking it leaves its source live", branched.Error == SystemCallError::None,
        branchQuery.Value0 ==
            (UInt64{7U} | (static_cast<UInt64>(MakeFullRights().GetRawValue()) << SystemCallQueryRightsShift)),
        branchRevoked.Error == SystemCallError::None, QueryError(fixture, branch) == SystemCallError::Revoked,
        QueryError(fixture, source) == SystemCallError::None, fixture.Space.Close(branch) == CapabilityError::Ok);

    const CapabilityHandle second = DecodeHandle(DeriveBranch(fixture, source));
    const SystemCallResponse sourceRevoked =
        SystemCallDispatcher::Dispatch(fixture.Space, MakeRequest(SystemCallNumber::CapabilityRevoke, source));
    ASTERIA_EXPECT(
        runner, "revoking the source ends every branch derived from it", second.IsValid(),
        sourceRevoked.Error == SystemCallError::None, QueryError(fixture, second) == SystemCallError::Revoked,
        fixture.Space.Close(second) == CapabilityError::Ok, fixture.Space.Close(source) == CapabilityError::Ok);

    // The fixture's root domain has depth 0; the registry nests eight more.
    const auto renewed = fixture.Mint(MakeFullRights());
    CapabilityHandle chain[9U]{};
    chain[0U] = renewed.HasValue() ? renewed.GetValue() : CapabilityHandle{};
    bool nested{renewed.HasValue()};
    for (Foundation::Size depth{1U}; depth < 9U; ++depth)
    {
        const SystemCallResponse response = DeriveBranch(fixture, chain[depth - 1U]);
        nested = nested && (response.Error == SystemCallError::None);
        chain[depth] = DecodeHandle(response);
    }
    const SystemCallResponse tooDeep = DeriveBranch(fixture, chain[8U]);
    bool closed{true};
    for (const CapabilityHandle& handle : chain)
    {
        closed = (fixture.Space.Close(handle) == CapabilityError::Ok) && closed;
    }
    ASTERIA_EXPECT(runner, "branches nest eight deep and a ninth reports an exhausted revocation domain", nested,
                   ResponseIsEmptyError(tooDeep, SystemCallError::RevocationDomainExhausted), closed, fixture.Finish());
}

void TestRevocationAndConcurrency(TestContext& runner) noexcept
{
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "system-call revocation fixture initializes", fixture.Initialize());
    const CapabilityRights revokeRights = CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Revoke);
    const auto controller = fixture.Mint(revokeRights);
    constexpr CapabilityRights peerRights{CapabilityRight::Inspect};
    const auto peer = fixture.Mint(peerRights);
    if (!controller.HasValue() || !peer.HasValue())
    {
        ASTERIA_EXPECT(runner, "revocation capabilities mint", false);
        return;
    }

    const SystemCallResponse denied =
        SystemCallDispatcher::Dispatch(fixture.Space, MakeRequest(SystemCallNumber::CapabilityRevoke, peer.GetValue()));
    ASTERIA_EXPECT(runner, "Revoke right is required before any epoch transition",
                   ResponseIsEmptyError(denied, SystemCallError::RightsDenied),
                   SystemCallDispatcher::Dispatch(fixture.Space,
                                                  MakeRequest(SystemCallNumber::CapabilityQuery, controller.GetValue()))
                           .Error == SystemCallError::None);

    std::atomic<bool> start{false};
    SystemCallResponse concurrentQuery{};
    SystemCallResponse concurrentRevoke{};
    std::thread queryThread{[&]() noexcept
                            {
                                while (!start.load(std::memory_order_acquire))
                                {
                                    start.wait(false, std::memory_order_acquire);
                                }
                                concurrentQuery = SystemCallDispatcher::Dispatch(
                                    fixture.Space, MakeRequest(SystemCallNumber::CapabilityQuery, peer.GetValue()));
                            }};
    std::thread revokeThread{[&]() noexcept
                             {
                                 while (!start.load(std::memory_order_acquire))
                                 {
                                     start.wait(false, std::memory_order_acquire);
                                 }
                                 concurrentRevoke = SystemCallDispatcher::Dispatch(
                                     fixture.Space,
                                     MakeRequest(SystemCallNumber::CapabilityRevoke, controller.GetValue()));
                             }};
    start.store(true, std::memory_order_release);
    start.notify_all();
    queryThread.join();
    revokeThread.join();

    const SystemCallResponse peerAfter =
        SystemCallDispatcher::Dispatch(fixture.Space, MakeRequest(SystemCallNumber::CapabilityQuery, peer.GetValue()));
    const SystemCallResponse controllerAfter = SystemCallDispatcher::Dispatch(
        fixture.Space, MakeRequest(SystemCallNumber::CapabilityRevoke, controller.GetValue()));
    const UInt64 peerInformation =
        UInt64{7U} | (static_cast<UInt64>(peerRights.GetRawValue()) << SystemCallQueryRightsShift);
    const bool queryLinearizedBeforeRevoke = (concurrentQuery.Error == SystemCallError::None) &&
                                             (concurrentQuery.Value0 == peerInformation) &&
                                             (concurrentQuery.Value1 == MonotonicTime::Infinite().GetTicks());
    ASTERIA_EXPECT(runner, "query and revoke linearize while all later domain use is revoked",
                   ResponseIsEmptyError(concurrentRevoke, SystemCallError::None),
                   queryLinearizedBeforeRevoke || ResponseIsEmptyError(concurrentQuery, SystemCallError::Revoked),
                   ResponseIsEmptyError(peerAfter, SystemCallError::Revoked),
                   ResponseIsEmptyError(controllerAfter, SystemCallError::Revoked));

    const bool closed = (fixture.Space.Close(peer.GetValue()) == CapabilityError::Ok) &&
                        (fixture.Space.Close(controller.GetValue()) == CapabilityError::Ok);
    ASTERIA_EXPECT(runner, "revoked handles remain explicitly closeable without retaining storage", closed,
                   fixture.Finish());
}

void TestRegistryAndCapacityErrors(TestContext& runner) noexcept
{
    {
        DispatcherFixture fixture;
        ASTERIA_EXPECT(runner, "object-loss error fixture initializes", fixture.Initialize());
        const auto minted = fixture.Mint(MakeFullRights());
        if (!minted.HasValue())
        {
            ASTERIA_EXPECT(runner, "object-loss capability mints", false);
            return;
        }

        const bool destroyed = fixture.Issuer.DestroyObject(fixture.ObjectIdentity) == ObjectRegistryError::Ok;
        const SystemCallResponse query = SystemCallDispatcher::Dispatch(
            fixture.Space, MakeRequest(SystemCallNumber::CapabilityQuery, minted.GetValue()));
        ASTERIA_EXPECT(runner, "query maps a destroyed capability target to ObjectNotFound", destroyed,
                       ResponseIsEmptyError(query, SystemCallError::ObjectNotFound));
        ASTERIA_EXPECT(runner, "object-loss fixture closes and collects the exact generation",
                       fixture.Space.Close(minted.GetValue()) == CapabilityError::Ok, fixture.FinishDestroyedObject());
    }

    {
        DispatcherFixture fixture;
        ASTERIA_EXPECT(runner, "space-exhaustion error fixture initializes", fixture.Initialize());
        // A duplicate may not carry Revoke, so the source has none.
        const CapabilityRights fullRights =
            CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Duplicate).With(CapabilityRight::Derive);
        const auto minted = fixture.Mint(fullRights);
        if (!minted.HasValue())
        {
            ASTERIA_EXPECT(runner, "space-exhaustion source capability mints", false);
            return;
        }

        CapabilityHandle handles[CapabilitySpace::MaximumSlots]{};
        handles[0U] = minted.GetValue();
        Foundation::Size createdCount{1U};
        bool filled{true};
        for (Foundation::Size index{1U}; index < CapabilitySpace::MaximumSlots; ++index)
        {
            const SystemCallResponse duplicate = SystemCallDispatcher::Dispatch(
                fixture.Space, MakeCreationRequest(SystemCallNumber::CapabilityDuplicate, minted.GetValue(), fullRights,
                                                   MonotonicTime::Infinite()));
            if (duplicate.Error != SystemCallError::None)
            {
                filled = false;
                break;
            }
            handles[createdCount] = DecodeHandle(duplicate);
            ++createdCount;
        }

        const SystemCallResponse overflow = SystemCallDispatcher::Dispatch(
            fixture.Space, MakeCreationRequest(SystemCallNumber::CapabilityDuplicate, minted.GetValue(), fullRights,
                                               MonotonicTime::Infinite()));
        ASTERIA_EXPECT(runner, "duplicate maps fixed capability-slot exhaustion exactly", filled,
                       createdCount == CapabilitySpace::MaximumSlots,
                       fixture.Space.GetActiveCount() == CapabilitySpace::MaximumSlots,
                       ResponseIsEmptyError(overflow, SystemCallError::SpaceExhausted),
                       ResponseIsEmptyError(DeriveBranch(fixture, minted.GetValue(), fullRights),
                                            SystemCallError::SpaceExhausted));

        bool closed{true};
        for (Foundation::Size index{0U}; index < createdCount; ++index)
        {
            closed = (fixture.Space.Close(handles[index]) == CapabilityError::Ok) && closed;
        }
        ASTERIA_EXPECT(runner, "space-exhaustion fixture releases every returned handle", closed, fixture.Finish());
    }
}

void TestNonMemoryObjectQuery(TestContext& runner) noexcept
{
    DispatcherFixture fixture{ObjectType::Frame};
    ASTERIA_EXPECT(runner, "non-memory query fixture initializes", fixture.Initialize());
    const CapabilityRights rights = MakeFullRights();
    const auto minted = fixture.Mint(rights);
    if (!minted.HasValue())
    {
        ASTERIA_EXPECT(runner, "non-memory query capability mints", false);
        return;
    }

    const SystemCallResponse query = SystemCallDispatcher::Dispatch(
        fixture.Space, MakeRequest(SystemCallNumber::CapabilityQuery, minted.GetValue()));
    const UInt64 expectedInformation =
        UInt64{8U} | (static_cast<UInt64>(rights.GetRawValue()) << SystemCallQueryRightsShift);
    ASTERIA_EXPECT(runner, "query encodes a non-MemoryObject registry type without substitution",
                   query.Error == SystemCallError::None, query.Value0 == expectedInformation,
                   query.Value1 == MonotonicTime::Infinite().GetTicks());
    ASTERIA_EXPECT(runner, "non-memory query fixture releases its capability and object",
                   fixture.Space.Close(minted.GetValue()) == CapabilityError::Ok, fixture.Finish());
}

// ThreadExit carries a code with bit 63 clear and nothing else; Dispatch never completes it. domain
// creation ABI: CapabilityGrant moves or copies one capability into a CapabilitySpace the caller
// manages and returns its handle there.
void TestCapabilityGrant(TestContext& runner) noexcept
{
    DispatcherFixture fixture;
    CapabilitySpace child{fixture.Registry, fixture.Revocations, fixture.Clock, 0x1357'9BDF'2468'ACE0ULL,
                          0x0ECA'8642'FDB9'7531ULL};
    Kernel::Object::CapabilitySpaceObject childObject{child, ResourceDomainId{77U}};
    const bool ready = fixture.Initialize();
    const auto childRegistered = fixture.Issuer.CreateObject(childObject);
    const auto manage =
        childRegistered.HasValue()
            ? fixture.Issuer.Mint(fixture.Space, childRegistered.GetValue(), CapabilityRights{CapabilityRight::Manage},
                                  fixture.Revocation, MonotonicTime::Infinite(), 0U)
            : decltype(fixture.Mint(CapabilityRights{}))::Failure(CapabilityError::InvalidHandle);
    const auto inspectChild =
        childRegistered.HasValue()
            ? fixture.Issuer.Mint(fixture.Space, childRegistered.GetValue(), CapabilityRights{CapabilityRight::Inspect},
                                  fixture.Revocation, MonotonicTime::Infinite(), 0U)
            : decltype(fixture.Mint(CapabilityRights{}))::Failure(CapabilityError::InvalidHandle);
    const auto source = fixture.Mint(CapabilityRights{CapabilityRight::Inspect}
                                         .With(CapabilityRight::Duplicate)
                                         .With(CapabilityRight::Transfer)
                                         .With(CapabilityRight::Revoke),
                                     MonotonicTime{5'000U});
    const CapabilityHandle destination = manage.HasValue() ? manage.GetValue() : CapabilityHandle{};
    const CapabilityHandle sourceHandle = source.HasValue() ? source.GetValue() : CapabilityHandle{};
    const auto grant =
        [&](const CapabilityHandle into, const UInt64 rights, const UInt64 type, const UInt64 disposition) noexcept
    {
        SystemCallRequest request = MakeRequest(SystemCallNumber::CapabilityGrant, sourceHandle);
        request.Arguments[2U] = into.GetSelector();
        request.Arguments[3U] = into.GetAuthenticator();
        request.Arguments[4U] = rights | (type << Asteria::Abi::CapabilityGrantTypeShift);
        request.Arguments[5U] = disposition;
        return SystemCallDispatcher::Dispatch(fixture.Space, request);
    };
    const UInt64 inspect = CapabilityRights{CapabilityRight::Inspect}.GetRawValue();
    const UInt64 memoryType = static_cast<UInt64>(ObjectType::MemoryObject);
    const auto copied = grant(destination, inspect, memoryType, Asteria::Abi::CapabilityGrantCopy);
    const auto inChild = child.Query(DecodeHandle(copied));
    const auto withRevoke =
        grant(destination, CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Revoke).GetRawValue(),
              memoryType, Asteria::Abi::CapabilityGrantCopy);
    const auto wrongType =
        grant(destination, inspect, static_cast<UInt64>(ObjectType::Frame), Asteria::Abi::CapabilityGrantCopy);
    const auto withoutManage = grant(inspectChild.HasValue() ? inspectChild.GetValue() : CapabilityHandle{}, inspect,
                                     memoryType, Asteria::Abi::CapabilityGrantCopy);
    const auto managedObject = fixture.Mint(CapabilityRights{CapabilityRight::Manage});
    const auto notASpace = grant(managedObject.HasValue() ? managedObject.GetValue() : CapabilityHandle{}, inspect,
                                 memoryType, Asteria::Abi::CapabilityGrantCopy);
    const auto badDisposition = grant(destination, inspect, memoryType, 3U);
    const auto reservedBits =
        grant(destination, inspect | (UInt64{1U} << 24U), memoryType, Asteria::Abi::CapabilityGrantCopy);
    const auto noType = grant(destination, inspect, 0U, Asteria::Abi::CapabilityGrantCopy);
    ASTERIA_EXPECT(
        runner, "a copy lands in the managed space with the source's lease, and every bad request fails", ready,
        childRegistered.HasValue(), source.HasValue(), copied.Error == SystemCallError::None, inChild.HasValue(),
        inChild.HasValue() && (inChild.GetValue().Object.GetValue() == fixture.ObjectIdentity.GetValue()),
        inChild.HasValue() && inChild.GetValue().Rights.IsEqualTo(CapabilityRights{CapabilityRight::Inspect}),
        inChild.HasValue() && (inChild.GetValue().LeaseDeadline.GetTicks() == 5'000U),
        !fixture.Space.Query(DecodeHandle(copied)).HasValue(),
        ResponseIsEmptyError(withRevoke, SystemCallError::RightsAmplification),
        ResponseIsEmptyError(wrongType, SystemCallError::TypeMismatch),
        ResponseIsEmptyError(withoutManage, SystemCallError::RightsDenied),
        ResponseIsEmptyError(notASpace, SystemCallError::TypeMismatch),
        ResponseIsEmptyError(badDisposition, SystemCallError::InvalidArguments),
        ResponseIsEmptyError(reservedBits, SystemCallError::InvalidArguments),
        ResponseIsEmptyError(noType, SystemCallError::InvalidArguments));

    const auto moved = grant(destination, inspect, memoryType, Asteria::Abi::CapabilityGrantMove);
    ASTERIA_EXPECT(runner, "a move takes the source out of the caller's space", moved.Error == SystemCallError::None,
                   child.Query(DecodeHandle(moved)).HasValue(), !fixture.Space.Query(sourceHandle).HasValue(),
                   child.GetActiveCount() == 2U);
}

void TestThreadExitDecoding(TestContext& runner) noexcept
{
    const auto request = [](const UInt64 code, const UInt64 extra) noexcept
    {
        return SystemCallRequest{
            .Number = static_cast<UInt64>(SystemCallNumber::ThreadExit),
            .Arguments = {code, 0U, 0U, 0U, 0U, extra},
        };
    };
    UInt64 code{1U};
    const bool valid = SystemCallDispatcher::DecodeThreadExit(request(0x7FFF'FFFF'FFFF'FFFFULL, 0U), code);
    const UInt64 decoded = code;
    UInt64 untouched{5U};
    const bool kernelCode = SystemCallDispatcher::DecodeThreadExit(request(UInt64{1U} << 63U, 0U), untouched);
    const bool extraArgument = SystemCallDispatcher::DecodeThreadExit(request(0U, 1U), untouched);
    SystemCallRequest other = request(0U, 0U);
    other.Number = static_cast<UInt64>(SystemCallNumber::MemoryUnmap);
    const bool otherNumber = SystemCallDispatcher::DecodeThreadExit(other, untouched);
    DispatcherFixture fixture;
    const bool ready = fixture.Initialize();
    const auto dispatched = SystemCallDispatcher::Dispatch(fixture.Space, request(0U, 0U));
    ASTERIA_EXPECT(runner, "ThreadExit decodes only a non-kernel code with zero arguments and is not dispatched", valid,
                   decoded == 0x7FFF'FFFF'FFFF'FFFFULL, !kernelCode, !extraArgument, !otherNumber, untouched == 5U,
                   ready, dispatched.Error == SystemCallError::UnknownSystemCall, fixture.Finish());
}
} // Namespace

int RunSystemCallDispatcherTests() noexcept
{
    TestContext runner{"SystemCallDispatcherTests"};
    TestValidationAndClose(runner);
    TestCreationAndQuery(runner);
    TestChildRevocationDomain(runner);
    TestRevocationAndConcurrency(runner);
    TestRegistryAndCapacityErrors(runner);
    TestNonMemoryObjectQuery(runner);
    TestCapabilityGrant(runner);
    TestThreadExitDecoding(runner);
    return runner.Finish();
}
} // Namespace Asteria::Test

int main()
{
    return Asteria::Test::RunSystemCallDispatcherTests();
}
