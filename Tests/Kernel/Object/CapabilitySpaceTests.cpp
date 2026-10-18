// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "Kernel/Object/CapabilitySpace.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Foundation::UInt64;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityGenerationPolicy;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityOperation;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::CapabilityTransferBatch;
using Kernel::Object::CapabilityTransferDisposition;
using Kernel::Object::CapabilityTransferRequest;
using Kernel::Object::CapabilityTransferRequestView;
using Kernel::Object::IKernelObject;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationDomainId;
using Kernel::Object::RevocationError;
using Kernel::Object::RevocationRegistry;
using Kernel::Object::RootCapabilityIssuer;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

class TestKernelObject final : public IKernelObject
{
public:
    void Configure(const ObjectType type, const ResourceDomainId chargeOwner) noexcept
    {
        m_type = type;
        m_chargeOwner = chargeOwner;
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
    ObjectType m_type{ObjectType::Invalid};
    ResourceDomainId m_chargeOwner{};
};

class CapabilityFixture final
{
public:
    explicit CapabilityFixture(const UInt64 validationKey0 = 0x0123'4567'89AB'CDEFULL,
                               const UInt64 validationKey1 = 0xFEDC'BA98'7654'3210ULL) noexcept
        : m_clock{10U}, m_objects{m_synchronization}, m_revocations{m_synchronization}, m_issuer{m_objects},
          m_space{m_objects, m_revocations, m_clock, validationKey0, validationKey1}
    {
    }

    [[nodiscard]] ManualClock& GetClock() noexcept
    {
        return m_clock;
    }

    [[nodiscard]] ObjectRegistry& GetObjects() noexcept
    {
        return m_objects;
    }

    [[nodiscard]] RevocationRegistry& GetRevocations() noexcept
    {
        return m_revocations;
    }

    [[nodiscard]] RootCapabilityIssuer& GetIssuer() noexcept
    {
        return m_issuer;
    }

    [[nodiscard]] CapabilitySpace& GetSpace() noexcept
    {
        return m_space;
    }

    /// Preconditions:
    /// - index is less than the number of successfully created objects.
    [[nodiscard]] TestKernelObject& GetKernelObject(const Foundation::Size index) noexcept
    {
        return m_kernelObjects[index];
    }

    [[nodiscard]] Foundation::Result<ObjectId, ObjectRegistryError>
    CreateObject(const ObjectType type, const ResourceDomainId chargeOwner) noexcept
    {
        if (m_nextKernelObject >= MaximumTestObjects)
        {
            return Foundation::Result<ObjectId, ObjectRegistryError>::Failure(ObjectRegistryError::RegistryExhausted);
        }

        TestKernelObject& instance = m_kernelObjects[m_nextKernelObject];
        instance.Configure(type, chargeOwner);
        const auto result = m_issuer.CreateObject(instance);
        if (result.HasValue())
        {
            ++m_nextKernelObject;
        }
        return result;
    }

private:
    static constexpr Foundation::Size MaximumTestObjects{Asteria::TestSupport::TestObjectRegistry::EntryCapacity + 1U};

    TestKernelObject m_kernelObjects[MaximumTestObjects]{};
    Foundation::Size m_nextKernelObject{0U};
    SpinLock m_synchronization;
    ManualClock m_clock;
    Asteria::TestSupport::TestObjectRegistry m_objects;
    RevocationRegistry m_revocations;
    RootCapabilityIssuer m_issuer;
    CapabilitySpace m_space;
};

[[nodiscard]] CapabilityRights MakeRights() noexcept
{
    return CapabilityRights{CapabilityRight::Inspect}
        .With(CapabilityRight::Invoke)
        .With(CapabilityRight::Read)
        .With(CapabilityRight::Duplicate)
        .With(CapabilityRight::Derive);
}

[[nodiscard]] CapabilityRights MakeTransferRights() noexcept
{
    return CapabilityRights{CapabilityRight::Inspect}
        .With(CapabilityRight::Invoke)
        .With(CapabilityRight::Read)
        .With(CapabilityRight::Transfer)
        .With(CapabilityRight::Duplicate);
}

[[nodiscard]] ObjectId CreateEndpoint(CapabilityFixture& fixture, TestContext& runner) noexcept
{
    const auto objectResult = fixture.CreateObject(ObjectType::Endpoint, ResourceDomainId{7U});
    ASTERIA_EXPECT(runner, "endpoint object is registered", objectResult.HasValue());
    if (!objectResult.HasValue())
    {
        return ObjectId{};
    }
    return objectResult.GetValue();
}

[[nodiscard]] RevocationDomainId CreateRootDomain(CapabilityFixture& fixture, TestContext& runner) noexcept
{
    const auto domainResult = fixture.GetRevocations().CreateRoot();
    ASTERIA_EXPECT(runner, "root revocation domain is created", domainResult.HasValue());
    if (!domainResult.HasValue())
    {
        return RevocationDomainId{};
    }
    return domainResult.GetValue();
}

[[nodiscard]] CapabilityHandle MintEndpoint(CapabilityFixture& fixture, TestContext& runner, const ObjectId object,
                                            const RevocationDomainId domain,
                                            const CapabilityRights rights = MakeRights(),
                                            const MonotonicTime deadline = MonotonicTime::Infinite()) noexcept
{
    const auto handleResult =
        fixture.GetIssuer().Mint(fixture.GetSpace(), object, rights, domain, deadline, 0x1020'3040U);
    ASTERIA_EXPECT(runner, "root issuer mints endpoint capability", handleResult.HasValue());
    if (!handleResult.HasValue())
    {
        return CapabilityHandle{};
    }
    return handleResult.GetValue();
}

[[nodiscard]] CapabilityHandle MintTransferCapability(CapabilityFixture& fixture, CapabilitySpace& destination,
                                                      const ObjectId object, const RevocationDomainId domain,
                                                      const CapabilityRights rights = MakeTransferRights(),
                                                      const MonotonicTime deadline = MonotonicTime::Infinite(),
                                                      const UInt64 sealedObjectSelector = 0x1020'3040U) noexcept
{
    const auto result = fixture.GetIssuer().Mint(destination, object, rights, domain, deadline, sealedObjectSelector);
    return result.HasValue() ? result.GetValue() : CapabilityHandle{};
}

[[nodiscard]] CapabilityTransferRequest
MakeTransferRequest(const CapabilityHandle source, const CapabilityTransferDisposition disposition,
                    const CapabilityRights requestedRights = CapabilityRights{CapabilityRight::Invoke},
                    const MonotonicTime requestedDeadline = MonotonicTime::Infinite(),
                    const ObjectType expectedType = ObjectType::Endpoint) noexcept
{
    return CapabilityTransferRequest{
        .Source = source,
        .RequestedDeadline = requestedDeadline,
        .RequestedRights = requestedRights,
        .ExpectedType = expectedType,
        .Disposition = disposition,
        .Reserved = 0U,
    };
}

void TestMintAndLookup(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    CapabilityFixture foreignFixture;
    ASTERIA_EXPECT(runner, "capability space configuration is valid", fixture.GetSpace().IsReady());
    ASTERIA_EXPECT(runner, "the first capability space receives the first boot-unique identity",
                   fixture.GetSpace().GetIdentifier().GetValue() == 1U);
    ASTERIA_EXPECT(runner, "capability space reports exact registry provenance",
                   fixture.GetSpace().IsFromRegistry(fixture.GetObjects()),
                   fixture.GetSpace().IsFromRevocationRegistry(fixture.GetRevocations()),
                   !fixture.GetSpace().IsFromRegistry(foreignFixture.GetObjects()),
                   !fixture.GetSpace().IsFromRevocationRegistry(foreignFixture.GetRevocations()));
    const ObjectId object = CreateEndpoint(fixture, runner);
    const RevocationDomainId domain = CreateRootDomain(fixture, runner);
    if (!object.IsValid() || !domain.IsValid())
    {
        return;
    }

    const CapabilityHandle handle = MintEndpoint(fixture, runner, object, domain);
    if (!handle.IsValid())
    {
        return;
    }

    const auto lookupResult = fixture.GetSpace().Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Invoke);
    ASTERIA_EXPECT(runner, "matching type and operation resolve", lookupResult.HasValue());
    if (lookupResult.HasValue())
    {
        ASTERIA_EXPECT(runner, "lookup returns the registered object identity",
                       lookupResult.GetValue().GetDescriptor().Object.GetValue() == object.GetValue());
        ASTERIA_EXPECT(runner, "lookup returns a lease over the registered object instance",
                       &lookupResult.GetValue().GetObject() == &fixture.GetKernelObject(0U));
    }
    ASTERIA_EXPECT(runner, "fixed handle-authentication vector remains stable",
                   handle.GetSelector() == 0x0000'0000'0000'0100ULL,
                   handle.GetAuthenticator() == 0x1E5C'DFA1'5CF5'1FADULL);
    ASTERIA_EXPECT(runner, "one capability slot is active", fixture.GetSpace().GetActiveCount() == 1U);
}

void TestTypeAndRightsChecks(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    const ObjectId object = CreateEndpoint(fixture, runner);
    const RevocationDomainId domain = CreateRootDomain(fixture, runner);
    if (!object.IsValid() || !domain.IsValid())
    {
        return;
    }

    const CapabilityHandle handle = MintEndpoint(fixture, runner, object, domain);
    const auto wrongType = fixture.GetSpace().Lookup(handle, ObjectType::MemoryObject, CapabilityOperation::Inspect);
    ASTERIA_EXPECT(runner, "object type mismatch is rejected", !wrongType.HasValue(),
                   wrongType.GetError() == CapabilityError::TypeMismatch);

    const auto missingRight = fixture.GetSpace().Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Write);
    ASTERIA_EXPECT(runner, "operation-specific right is enforced", !missingRight.HasValue(),
                   missingRight.GetError() == CapabilityError::RightsDenied);

    const CapabilityHandle inspectOnly =
        MintEndpoint(fixture, runner, object, domain, CapabilityRights{CapabilityRight::Inspect});
    const auto unauthorizedDerive =
        fixture.GetSpace().Derive(inspectOnly, CapabilityRights{CapabilityRight::Inspect}, MonotonicTime::Infinite());
    ASTERIA_EXPECT(runner, "derivation requires the derive right", !unauthorizedDerive.HasValue(),
                   unauthorizedDerive.GetError() == CapabilityError::RightsDenied);
    const auto unauthorizedDuplicate = fixture.GetSpace().Duplicate(
        inspectOnly, CapabilityRights{CapabilityRight::Inspect}, MonotonicTime::Infinite());
    ASTERIA_EXPECT(runner, "duplication requires the duplicate right", !unauthorizedDuplicate.HasValue(),
                   unauthorizedDuplicate.GetError() == CapabilityError::RightsDenied);
}

void TestAttenuationAndDuplication(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    const ObjectId object = CreateEndpoint(fixture, runner);
    const RevocationDomainId domain = CreateRootDomain(fixture, runner);
    if (!object.IsValid() || !domain.IsValid())
    {
        return;
    }

    const CapabilityHandle source = MintEndpoint(fixture, runner, object, domain, MakeRights(), MonotonicTime{100U});
    if (!source.IsValid())
    {
        return;
    }

    const CapabilityRights readOnly{CapabilityRight::Read};
    const auto derivedResult = fixture.GetSpace().Derive(source, readOnly, MonotonicTime{80U});
    ASTERIA_EXPECT(runner, "derivation attenuates rights and lease", derivedResult.HasValue());
    if (derivedResult.HasValue())
    {
        fixture.GetClock().Set(79U);
        const auto readLookup =
            fixture.GetSpace().Lookup(derivedResult.GetValue(), ObjectType::Endpoint, CapabilityOperation::Read);
        ASTERIA_EXPECT(runner, "derived read right remains usable", readLookup.HasValue());

        const auto invokeLookup =
            fixture.GetSpace().Lookup(derivedResult.GetValue(), ObjectType::Endpoint, CapabilityOperation::Invoke);
        ASTERIA_EXPECT(runner, "removed invoke right cannot be recovered", !invokeLookup.HasValue(),
                       invokeLookup.GetError() == CapabilityError::RightsDenied);

        fixture.GetClock().Set(80U);
        const auto expiredLookup =
            fixture.GetSpace().Lookup(derivedResult.GetValue(), ObjectType::Endpoint, CapabilityOperation::Read);
        ASTERIA_EXPECT(runner, "lease expires at its deadline", !expiredLookup.HasValue(),
                       expiredLookup.GetError() == CapabilityError::Expired);
    }

    fixture.GetClock().Set(80U);
    const CapabilityRights amplified = MakeRights().With(CapabilityRight::Write);
    const auto amplificationResult = fixture.GetSpace().Derive(source, amplified, MonotonicTime{90U});
    ASTERIA_EXPECT(runner, "derivation cannot amplify rights", !amplificationResult.HasValue(),
                   amplificationResult.GetError() == CapabilityError::RightsAmplification);

    const auto unattenuatedResult = fixture.GetSpace().Derive(source, MakeRights(), MonotonicTime{100U});
    ASTERIA_EXPECT(runner, "derive right alone cannot duplicate an unchanged capability",
                   !unattenuatedResult.HasValue(), unattenuatedResult.GetError() == CapabilityError::NoAttenuation);

    const auto duplicateResult = fixture.GetSpace().Duplicate(source, MakeRights(), MonotonicTime{100U});
    ASTERIA_EXPECT(runner, "duplicate right permits an exact duplicate", duplicateResult.HasValue());

    const auto leaseExtensionResult = fixture.GetSpace().Duplicate(source, readOnly, MonotonicTime{101U});
    ASTERIA_EXPECT(runner, "duplication cannot extend a lease", !leaseExtensionResult.HasValue(),
                   leaseExtensionResult.GetError() == CapabilityError::LeaseExtension);
}

void TestHierarchicalRevocation(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    const ObjectId object = CreateEndpoint(fixture, runner);
    const RevocationDomainId root = CreateRootDomain(fixture, runner);
    if (!object.IsValid() || !root.IsValid())
    {
        return;
    }

    const auto childResult = fixture.GetRevocations().CreateChild(root);
    ASTERIA_EXPECT(runner, "child revocation domain is created", childResult.HasValue());
    if (!childResult.HasValue())
    {
        return;
    }

    const CapabilityHandle rootHandle = MintEndpoint(fixture, runner, object, root);
    const CapabilityHandle childHandle = MintEndpoint(fixture, runner, object, childResult.GetValue());
    ASTERIA_EXPECT(runner, "child revocation succeeds",
                   fixture.GetRevocations().Revoke(childResult.GetValue()) == RevocationError::Ok);

    const auto revokedChild =
        fixture.GetSpace().Lookup(childHandle, ObjectType::Endpoint, CapabilityOperation::Inspect);
    ASTERIA_EXPECT(runner, "child revocation invalidates child capability", !revokedChild.HasValue(),
                   revokedChild.GetError() == CapabilityError::Revoked);

    const auto liveRoot = fixture.GetSpace().Lookup(rootHandle, ObjectType::Endpoint, CapabilityOperation::Inspect);
    ASTERIA_EXPECT(runner, "child revocation leaves parent capability valid", liveRoot.HasValue());

    const auto secondChildResult = fixture.GetRevocations().CreateChild(root);
    ASTERIA_EXPECT(runner, "second child domain is created", secondChildResult.HasValue());
    if (!secondChildResult.HasValue())
    {
        return;
    }

    const CapabilityHandle descendant = MintEndpoint(fixture, runner, object, secondChildResult.GetValue());
    ASTERIA_EXPECT(runner, "parent revocation succeeds", fixture.GetRevocations().Revoke(root) == RevocationError::Ok);
    const auto revokedDescendant =
        fixture.GetSpace().Lookup(descendant, ObjectType::Endpoint, CapabilityOperation::Inspect);
    ASTERIA_EXPECT(runner, "parent revocation invalidates descendants", !revokedDescendant.HasValue(),
                   revokedDescendant.GetError() == CapabilityError::Revoked);

    const auto rejectedChild = fixture.GetRevocations().CreateChild(secondChildResult.GetValue());
    ASTERIA_EXPECT(runner, "a revoked lineage cannot gain a new child domain", !rejectedChild.HasValue(),
                   rejectedChild.GetError() == RevocationError::Revoked);
}

void TestRevocationRegistryContract(TestContext& runner) noexcept
{
    SpinLock synchronization;
    RevocationRegistry registry{synchronization};
    const RevocationDomainId invalidZero{};
    const auto rootResult = registry.CreateRoot();
    ASTERIA_EXPECT(runner, "revocation contract creates a root domain", rootResult.HasValue());
    if (!rootResult.HasValue())
    {
        return;
    }
    const RevocationDomainId root = rootResult.GetValue();
    const Foundation::UInt64 registryBits = root.GetValue() & 0xFFFF'FFFF'0000'0000ULL;
    const RevocationDomainId invalidGeneration{registryBits | 0x0000'0001ULL};
    const RevocationDomainId invalidSlot{registryBits | 0x0001'0041ULL};
    const auto invalidChild = registry.CreateChild(invalidZero);
    const auto invalidGenerationEpoch = registry.GetCurrentEpoch(invalidGeneration);
    const auto invalidSlotEpoch = registry.GetCurrentEpoch(invalidSlot);

    ASTERIA_EXPECT(runner, "child creation rejects an invalid parent domain", !invalidChild.HasValue(),
                   invalidChild.GetError() == RevocationError::InvalidDomain);
    ASTERIA_EXPECT(runner, "epoch lookup rejects a zero-generation domain identity", !invalidGenerationEpoch.HasValue(),
                   invalidGenerationEpoch.GetError() == RevocationError::InvalidDomain);
    ASTERIA_EXPECT(runner, "epoch lookup rejects a slot outside the registry", !invalidSlotEpoch.HasValue(),
                   invalidSlotEpoch.GetError() == RevocationError::InvalidDomain);
    ASTERIA_EXPECT(runner, "revocation and currency checks reject malformed domains",
                   registry.Revoke(invalidZero) == RevocationError::InvalidDomain,
                   !registry.IsCurrent(invalidSlot, 1U));

    const RevocationDomainId staleRoot{root.GetValue() + 0x0001'0000U};
    const auto initialEpoch = registry.GetCurrentEpoch(root);
    ASTERIA_EXPECT(runner, "new revocation domains expose epoch one", initialEpoch.HasValue(),
                   initialEpoch.GetValue() == 1U);
    ASTERIA_EXPECT(runner, "currency checks compare the expected epoch", registry.IsCurrent(root, 1U),
                   !registry.IsCurrent(root, 2U));
    ASTERIA_EXPECT(runner, "epoch lookup rejects a stale domain generation",
                   !registry.GetCurrentEpoch(staleRoot).HasValue(),
                   registry.GetCurrentEpoch(staleRoot).GetError() == RevocationError::InvalidDomain);

    RevocationDomainId deepest = root;
    for (Foundation::UInt8 depth{0U}; depth < RevocationRegistry::MaximumDepth; ++depth)
    {
        const auto child = registry.CreateChild(deepest);
        ASTERIA_EXPECT(runner, "revocation contract creates an in-range descendant", child.HasValue());
        if (!child.HasValue())
        {
            return;
        }
        deepest = child.GetValue();
    }
    ASTERIA_EXPECT(runner, "deepest supported descendant initially has a current lineage",
                   registry.IsCurrent(deepest, 1U));
    ASTERIA_EXPECT(runner, "root epoch advancement succeeds", registry.Revoke(root) == RevocationError::Ok);
    const auto advancedRootEpoch = registry.GetCurrentEpoch(root);
    ASTERIA_EXPECT(runner, "root revocation advances the epoch without retiring the domain",
                   advancedRootEpoch.HasValue(), advancedRootEpoch.GetValue() == 2U, !registry.IsCurrent(root, 1U),
                   registry.IsCurrent(root, 2U));
    const auto revokedDescendantEpoch = registry.GetCurrentEpoch(deepest);
    ASTERIA_EXPECT(runner, "epoch lookup rejects a descendant of an advanced ancestor",
                   !revokedDescendantEpoch.HasValue(), revokedDescendantEpoch.GetError() == RevocationError::Revoked);
    ASTERIA_EXPECT(runner, "currency and revoke operations reject a revoked lineage", !registry.IsCurrent(deepest, 1U),
                   registry.Revoke(deepest) == RevocationError::Revoked);
    const auto rejectedDescendant = registry.CreateChild(deepest);
    ASTERIA_EXPECT(runner, "a revoked lineage cannot create another descendant", !rejectedDescendant.HasValue(),
                   rejectedDescendant.GetError() == RevocationError::Revoked);
    ASTERIA_EXPECT(runner, "an advanced root can create a child bound to its new epoch",
                   registry.CreateChild(root).HasValue());
}

// A destroyed domain's slot is reused under the next generation until that
// 16-bit generation is exhausted; then the slot retires, so no identifier is
// ever issued twice. The model test cannot reach 65,535 reuses of one slot.
void TestRevocationDomainSlotRetirement(TestContext& runner) noexcept
{
    SpinLock synchronization;
    RevocationRegistry registry{synchronization};
    bool cycled{true};
    RevocationDomainId last{};
    for (UInt64 generation{1U}; generation <= 0xFFFFU; ++generation)
    {
        const auto created = registry.CreateRoot();
        cycled = cycled && created.HasValue() &&
                 ((created.GetValue().GetValue() & 0xFFFF'FFFFULL) == ((generation << 16U) | 1U));
        last = created.HasValue() ? created.GetValue() : RevocationDomainId{};
        cycled = cycled && (registry.Destroy(last) == RevocationError::Ok);
    }
    const auto next = registry.CreateRoot();
    const auto stale = registry.GetCurrentEpoch(last);
    ASTERIA_EXPECT(runner, "a domain slot is reused under each generation and then retired", cycled, next.HasValue(),
                   (next.GetValue().GetValue() & 0xFFFF'FFFFULL) == 0x0001'0002ULL, !stale.HasValue(),
                   stale.GetError() == RevocationError::InvalidDomain,
                   registry.Destroy(last) == RevocationError::InvalidDomain);
}

void TestRevocationRegistryProvenance(TestContext& runner) noexcept
{
    SpinLock synchronization;
    ManualClock clock{10U};
    Asteria::TestSupport::TestObjectRegistry objects{synchronization};
    RevocationRegistry localRevocations{synchronization};
    RevocationRegistry foreignRevocations{synchronization};
    RootCapabilityIssuer issuer{objects};
    CapabilitySpace space{objects, localRevocations, clock, 101U, 103U};
    TestKernelObject objectInstance;
    objectInstance.Configure(ObjectType::Endpoint, ResourceDomainId{7U});

    const auto objectResult = issuer.CreateObject(objectInstance);
    const auto localRootResult = localRevocations.CreateRoot();
    const auto foreignRootResult = foreignRevocations.CreateRoot();
    ASTERIA_EXPECT(runner, "registry provenance fixture is created", objectResult.HasValue(),
                   localRootResult.HasValue(), foreignRootResult.HasValue());
    if (!objectResult.HasValue() || !localRootResult.HasValue() || !foreignRootResult.HasValue())
    {
        return;
    }

    ASTERIA_EXPECT(runner, "independent registries issue distinct domain identities",
                   localRootResult.GetValue().GetValue() != foreignRootResult.GetValue().GetValue());
    ASTERIA_EXPECT(runner, "a registry rejects another registry's domain identity",
                   !localRevocations.GetCurrentEpoch(foreignRootResult.GetValue()).HasValue());

    const auto foreignMint = issuer.Mint(space, objectResult.GetValue(), MakeRights(), foreignRootResult.GetValue(),
                                         MonotonicTime::Infinite(), 0U);
    ASTERIA_EXPECT(runner, "root minting rejects a foreign revocation lineage", !foreignMint.HasValue(),
                   foreignMint.GetError() == CapabilityError::InvalidArgument);
}

void TestHandleIsolationAndObjectLifetime(TestContext& runner) noexcept
{
    CapabilityFixture firstFixture{71U, 73U};
    CapabilityFixture secondFixture{71U, 73U};
    ASTERIA_EXPECT(
        runner, "capability-space identities are allocated uniquely for the boot",
        firstFixture.GetSpace().GetIdentifier().IsValid(), secondFixture.GetSpace().GetIdentifier().IsValid(),
        firstFixture.GetSpace().GetIdentifier().GetValue() != secondFixture.GetSpace().GetIdentifier().GetValue());
    const ObjectId firstObject = CreateEndpoint(firstFixture, runner);
    const ObjectId secondObject = CreateEndpoint(secondFixture, runner);
    const RevocationDomainId firstDomain = CreateRootDomain(firstFixture, runner);
    const RevocationDomainId secondDomain = CreateRootDomain(secondFixture, runner);
    if (!firstObject.IsValid() || !secondObject.IsValid() || !firstDomain.IsValid() || !secondDomain.IsValid())
    {
        return;
    }

    const CapabilityHandle firstHandle = MintEndpoint(firstFixture, runner, firstObject, firstDomain);
    static_cast<void>(MintEndpoint(secondFixture, runner, secondObject, secondDomain));

    const auto crossSpaceLookup =
        secondFixture.GetSpace().Lookup(firstHandle, ObjectType::Endpoint, CapabilityOperation::Inspect);
    ASTERIA_EXPECT(runner, "space identity prevents cross-space handle reuse with equal keys",
                   !crossSpaceLookup.HasValue(), crossSpaceLookup.GetError() == CapabilityError::InvalidHandle);

    const CapabilityHandle forged{firstHandle.GetSelector(), firstHandle.GetAuthenticator() ^ (UInt64{1U} << 63U)};
    const auto forgedLookup =
        firstFixture.GetSpace().Lookup(forged, ObjectType::Endpoint, CapabilityOperation::Inspect);
    ASTERIA_EXPECT(runner, "modified 64-bit validation token is rejected", !forgedLookup.HasValue(),
                   forgedLookup.GetError() == CapabilityError::InvalidHandle);

    ASTERIA_EXPECT(runner, "capability closes successfully",
                   firstFixture.GetSpace().Close(firstHandle) == CapabilityError::Ok);
    ASTERIA_EXPECT(runner, "closing the same capability twice is rejected",
                   firstFixture.GetSpace().Close(firstHandle) == CapabilityError::InvalidHandle);
    const CapabilityHandle replacement = MintEndpoint(firstFixture, runner, firstObject, firstDomain);
    ASTERIA_EXPECT(runner, "closed slot is reused with a new generation", replacement.IsValid());
    ASTERIA_EXPECT(runner, "closed capability reuses the same available slot index",
                   (replacement.GetSelector() & CapabilityGenerationPolicy::SelectorIndexMask) ==
                       (firstHandle.GetSelector() & CapabilityGenerationPolicy::SelectorIndexMask));
    ASTERIA_EXPECT(runner, "slot reuse advances the explicit handle generation",
                   replacement.GetSelector() != firstHandle.GetSelector());
    const auto staleLookup =
        firstFixture.GetSpace().Lookup(firstHandle, ObjectType::Endpoint, CapabilityOperation::Inspect);
    ASTERIA_EXPECT(runner, "old generation receives the uniform invalid-handle error", !staleLookup.HasValue(),
                   staleLookup.GetError() == CapabilityError::InvalidHandle);

    ASTERIA_EXPECT(runner, "registered object destruction succeeds",
                   firstFixture.GetIssuer().DestroyObject(firstObject) == ObjectRegistryError::Ok);
    const auto destroyedLookup =
        firstFixture.GetSpace().Lookup(replacement, ObjectType::Endpoint, CapabilityOperation::Inspect);
    ASTERIA_EXPECT(runner, "object generation invalidates surviving capabilities", !destroyedLookup.HasValue(),
                   destroyedLookup.GetError() == CapabilityError::ObjectNotFound);
}

void TestObjectLeaseLifetime(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    const ObjectId object = CreateEndpoint(fixture, runner);
    const RevocationDomainId domain = CreateRootDomain(fixture, runner);
    if (!object.IsValid() || !domain.IsValid())
    {
        return;
    }

    const CapabilityHandle handle = MintEndpoint(fixture, runner, object, domain);
    IKernelObject* pinnedObject{nullptr};
    {
        const auto liveLookup = fixture.GetSpace().Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Inspect);
        ASTERIA_EXPECT(runner, "lookup acquires an in-flight object lease", liveLookup.HasValue());
        if (!liveLookup.HasValue())
        {
            return;
        }
        pinnedObject = &liveLookup.GetValue().GetObject();

        ASTERIA_EXPECT(runner, "object destruction enters quiescing while a lease is active",
                       fixture.GetIssuer().DestroyObject(object) == ObjectRegistryError::Ok);
        const auto rejectedLookup =
            fixture.GetSpace().Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Inspect);
        ASTERIA_EXPECT(runner, "quiescing object rejects new lookup leases", !rejectedLookup.HasValue(),
                       rejectedLookup.GetError() == CapabilityError::ObjectNotFound);
        const auto prematureCollection = fixture.GetIssuer().CollectDestroyedObject();
        ASTERIA_EXPECT(runner, "destruction cannot complete while an object lease remains active",
                       !prematureCollection.HasValue(),
                       prematureCollection.GetError() == ObjectRegistryError::NoDestructionReady);
        ASTERIA_EXPECT(runner, "existing lease retains the actual object and identity during quiescing",
                       liveLookup.GetValue().GetObject().GetObjectType() == ObjectType::Endpoint,
                       liveLookup.GetValue().GetDescriptor().Object.GetValue() == object.GetValue());
    }

    const auto collectedObject = fixture.GetIssuer().CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "final lease release makes the pinned object available to its owner",
                   collectedObject.HasValue(), collectedObject.GetValue().Object.GetValue() == object.GetValue(),
                   collectedObject.GetValue().Instance == pinnedObject);
    const auto replacementResult = fixture.CreateObject(ObjectType::Endpoint, ResourceDomainId{7U});
    ASTERIA_EXPECT(runner, "released final lease permits slot reuse with a new object generation",
                   replacementResult.HasValue(), replacementResult.GetValue().GetValue() != object.GetValue());
    const auto staleObjectLookup =
        fixture.GetSpace().Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Inspect);
    ASTERIA_EXPECT(runner, "old object generation remains invalid after registry slot reuse",
                   !staleObjectLookup.HasValue(), staleObjectLookup.GetError() == CapabilityError::ObjectNotFound);
}

void TestLeaseAuthorizationSnapshot(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    const ObjectId object = CreateEndpoint(fixture, runner);
    const RevocationDomainId stableDomain = CreateRootDomain(fixture, runner);
    const RevocationDomainId revokedDomain = CreateRootDomain(fixture, runner);
    if (!object.IsValid() || !stableDomain.IsValid() || !revokedDomain.IsValid())
    {
        return;
    }

    const CapabilityHandle closedHandle = MintEndpoint(fixture, runner, object, stableDomain);
    const CapabilityHandle revokedHandle = MintEndpoint(fixture, runner, object, revokedDomain);
    const CapabilityHandle expiredHandle =
        MintEndpoint(fixture, runner, object, stableDomain, MakeRights(), MonotonicTime{20U});

    {
        const auto closedLease =
            fixture.GetSpace().Lookup(closedHandle, ObjectType::Endpoint, CapabilityOperation::Inspect);
        const auto revokedLease =
            fixture.GetSpace().Lookup(revokedHandle, ObjectType::Endpoint, CapabilityOperation::Inspect);
        const auto expiredLease =
            fixture.GetSpace().Lookup(expiredHandle, ObjectType::Endpoint, CapabilityOperation::Inspect);
        ASTERIA_EXPECT(runner, "dispatch-scoped leases are acquired before authorization changes",
                       closedLease.HasValue(), revokedLease.HasValue(), expiredLease.HasValue());
        if (!closedLease.HasValue() || !revokedLease.HasValue() || !expiredLease.HasValue())
        {
            return;
        }

        ASTERIA_EXPECT(runner, "closing a handle prevents subsequent authorization",
                       fixture.GetSpace().Close(closedHandle) == CapabilityError::Ok);
        ASTERIA_EXPECT(runner, "revoking a domain prevents subsequent authorization",
                       fixture.GetRevocations().Revoke(revokedDomain) == RevocationError::Ok);
        fixture.GetClock().Set(20U);

        const auto closedLookup =
            fixture.GetSpace().Lookup(closedHandle, ObjectType::Endpoint, CapabilityOperation::Inspect);
        const auto revokedLookup =
            fixture.GetSpace().Lookup(revokedHandle, ObjectType::Endpoint, CapabilityOperation::Inspect);
        const auto expiredLookup =
            fixture.GetSpace().Lookup(expiredHandle, ObjectType::Endpoint, CapabilityOperation::Inspect);
        ASTERIA_EXPECT(runner, "closed handle cannot authorize a new dispatch", !closedLookup.HasValue(),
                       closedLookup.GetError() == CapabilityError::InvalidHandle);
        ASTERIA_EXPECT(runner, "revoked handle cannot authorize a new dispatch", !revokedLookup.HasValue(),
                       revokedLookup.GetError() == CapabilityError::Revoked);
        ASTERIA_EXPECT(runner, "expired handle cannot authorize a new dispatch", !expiredLookup.HasValue(),
                       expiredLookup.GetError() == CapabilityError::Expired);
        ASTERIA_EXPECT(runner, "already-authorized dispatch leases retain the object snapshot",
                       &closedLease.GetValue().GetObject() == &fixture.GetKernelObject(0U),
                       &revokedLease.GetValue().GetObject() == &fixture.GetKernelObject(0U),
                       &expiredLease.GetValue().GetObject() == &fixture.GetKernelObject(0U));

        ASTERIA_EXPECT(runner, "snapshot test begins object quiescing",
                       fixture.GetIssuer().DestroyObject(object) == ObjectRegistryError::Ok);
        const auto prematureCollection = fixture.GetIssuer().CollectDestroyedObject();
        ASTERIA_EXPECT(runner, "all dispatch leases pin the object through authorization changes",
                       !prematureCollection.HasValue(),
                       prematureCollection.GetError() == ObjectRegistryError::NoDestructionReady);
    }

    const auto collectedObject = fixture.GetIssuer().CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "object becomes collectable after every dispatch lease ends", collectedObject.HasValue(),
                   collectedObject.GetValue().Instance == &fixture.GetKernelObject(0U));
}

void TestInvalidInputsAndClock(TestContext& runner) noexcept
{
    static CapabilityFixture fixture;
    const ObjectId object = CreateEndpoint(fixture, runner);
    const RevocationDomainId domain = CreateRootDomain(fixture, runner);
    if (!object.IsValid() || !domain.IsValid())
    {
        return;
    }

    const auto invalidRightsResult =
        fixture.GetIssuer().Mint(fixture.GetSpace(), object, CapabilityRights{static_cast<CapabilityRight>(0x8000U)},
                                 domain, MonotonicTime::Infinite(), 0U);
    ASTERIA_EXPECT(runner, "unknown capability-right bits are rejected", !invalidRightsResult.HasValue(),
                   invalidRightsResult.GetError() == CapabilityError::InvalidArgument);

    const auto invalidTypeResult = fixture.CreateObject(static_cast<ObjectType>(0xFFU), ResourceDomainId{7U});
    ASTERIA_EXPECT(runner, "unknown object type is rejected", !invalidTypeResult.HasValue(),
                   invalidTypeResult.GetError() == ObjectRegistryError::InvalidArgument);

    const auto expiredMint =
        fixture.GetIssuer().Mint(fixture.GetSpace(), object, MakeRights(), domain, MonotonicTime{10U}, 0U);
    ASTERIA_EXPECT(runner, "mint rejects a deadline equal to the current time", !expiredMint.HasValue(),
                   expiredMint.GetError() == CapabilityError::Expired);

    const CapabilityHandle handle = MintEndpoint(fixture, runner, object, domain);
    const auto invalidOperation =
        fixture.GetSpace().Lookup(handle, ObjectType::Endpoint, static_cast<CapabilityOperation>(0xFFU));
    ASTERIA_EXPECT(runner, "unknown capability operation is rejected", !invalidOperation.HasValue(),
                   invalidOperation.GetError() == CapabilityError::InvalidArgument);
    fixture.GetClock().Set(20U);
    const auto forwardLookup = fixture.GetSpace().Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Inspect);
    ASTERIA_EXPECT(runner, "monotonic clock advance is accepted", forwardLookup.HasValue());

    fixture.GetClock().Set(19U);
    const auto rollbackLookup = fixture.GetSpace().Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Inspect);
    ASTERIA_EXPECT(runner, "clock rollback fails closed", !rollbackLookup.HasValue(),
                   rollbackLookup.GetError() == CapabilityError::InvalidConfiguration);

    static CapabilityFixture infiniteClockFixture;
    const ObjectId infiniteClockObject = CreateEndpoint(infiniteClockFixture, runner);
    const RevocationDomainId infiniteClockDomain = CreateRootDomain(infiniteClockFixture, runner);
    const CapabilityHandle infiniteClockHandle =
        MintEndpoint(infiniteClockFixture, runner, infiniteClockObject, infiniteClockDomain);
    infiniteClockFixture.GetClock().Set(~UInt64{0U});
    const auto infiniteClockLookup =
        infiniteClockFixture.GetSpace().Lookup(infiniteClockHandle, ObjectType::Endpoint, CapabilityOperation::Inspect);
    ASTERIA_EXPECT(runner, "reserved all-ones clock value fails closed", !infiniteClockLookup.HasValue(),
                   infiniteClockLookup.GetError() == CapabilityError::InvalidConfiguration);

    CapabilityFixture zeroKeyFixture{0U, 0U};
    ASTERIA_EXPECT(runner, "an all-zero validation key is rejected", !zeroKeyFixture.GetSpace().IsReady());

    SpinLock firstSynchronization;
    SpinLock secondSynchronization;
    ManualClock mismatchedClock{0U};
    Asteria::TestSupport::TestObjectRegistry mismatchedObjects{firstSynchronization};
    RevocationRegistry mismatchedRevocations{secondSynchronization};
    CapabilitySpace mismatchedSpace{mismatchedObjects, mismatchedRevocations, mismatchedClock, 1U, 2U};
    ASTERIA_EXPECT(runner, "registries with different synchronization objects are rejected",
                   !mismatchedSpace.IsReady());

    CapabilityFixture foreignFixture;
    const RevocationDomainId foreignDomain = CreateRootDomain(foreignFixture, runner);
    const auto foreignMint = fixture.GetIssuer().Mint(foreignFixture.GetSpace(), object, MakeRights(), foreignDomain,
                                                      MonotonicTime::Infinite(), 0U);
    ASTERIA_EXPECT(runner, "issuer cannot mint into a space bound to another object registry", !foreignMint.HasValue(),
                   foreignMint.GetError() == CapabilityError::InvalidConfiguration);
}

void TestRegistryBounds(TestContext& runner) noexcept
{
    CapabilityFixture objectFixture;
    ObjectId objects[Asteria::TestSupport::TestObjectRegistry::EntryCapacity]{};
    for (Foundation::Size index{0U}; index < Asteria::TestSupport::TestObjectRegistry::EntryCapacity; ++index)
    {
        objects[index] = CreateEndpoint(objectFixture, runner);
    }

    const auto objectExhaustion = objectFixture.CreateObject(ObjectType::Endpoint, ResourceDomainId{7U});
    ASTERIA_EXPECT(runner, "fixed object registry reports exhaustion", !objectExhaustion.HasValue(),
                   objectExhaustion.GetError() == ObjectRegistryError::RegistryExhausted);
    ASTERIA_EXPECT(runner, "object active count remains bounded by registry capacity",
                   objectFixture.GetObjects().GetActiveCount() ==
                       Asteria::TestSupport::TestObjectRegistry::EntryCapacity);

    ASTERIA_EXPECT(runner, "object registry destroys an occupied slot",
                   objectFixture.GetIssuer().DestroyObject(objects[0U]) == ObjectRegistryError::Ok);
    const auto collectedObject = objectFixture.GetIssuer().CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "unreferenced destroyed object is immediately collectable", collectedObject.HasValue(),
                   collectedObject.GetValue().Object.GetValue() == objects[0U].GetValue());
    const auto replacementObject = objectFixture.CreateObject(ObjectType::Endpoint, ResourceDomainId{7U});
    ASTERIA_EXPECT(runner, "object slot reuse advances its generation", replacementObject.HasValue(),
                   replacementObject.GetValue().GetValue() != objects[0U].GetValue());
    ASTERIA_EXPECT(runner, "stale object identity cannot destroy its replacement",
                   objectFixture.GetIssuer().DestroyObject(objects[0U]) == ObjectRegistryError::InvalidObject);

    CapabilityFixture depthFixture;
    RevocationDomainId deepest = CreateRootDomain(depthFixture, runner);
    for (Foundation::UInt8 depth{0U}; depth < RevocationRegistry::MaximumDepth; ++depth)
    {
        const auto child = depthFixture.GetRevocations().CreateChild(deepest);
        ASTERIA_EXPECT(runner, "revocation lineage accepts an in-range depth", child.HasValue());
        if (!child.HasValue())
        {
            return;
        }
        deepest = child.GetValue();
    }
    const auto depthExhaustion = depthFixture.GetRevocations().CreateChild(deepest);
    ASTERIA_EXPECT(runner, "revocation lineage rejects depth beyond its fixed bound", !depthExhaustion.HasValue(),
                   depthExhaustion.GetError() == RevocationError::DepthExceeded);

    CapabilityFixture domainFixture;
    for (Foundation::Size index{0U}; index < RevocationRegistry::MaximumDomains; ++index)
    {
        const auto domain = domainFixture.GetRevocations().CreateRoot();
        ASTERIA_EXPECT(runner, "revocation registry accepts an in-range domain", domain.HasValue());
    }
    const auto domainExhaustion = domainFixture.GetRevocations().CreateRoot();
    ASTERIA_EXPECT(runner, "fixed revocation registry reports exhaustion", !domainExhaustion.HasValue(),
                   domainExhaustion.GetError() == RevocationError::DomainExhausted);
}

void TestFixedCapacity(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    const ObjectId object = CreateEndpoint(fixture, runner);
    const RevocationDomainId domain = CreateRootDomain(fixture, runner);
    if (!object.IsValid() || !domain.IsValid())
    {
        return;
    }

    for (std::size_t index{0U}; index < CapabilitySpace::MaximumSlots; ++index)
    {
        const auto result = fixture.GetIssuer().Mint(fixture.GetSpace(), object, MakeRights(), domain,
                                                     MonotonicTime::Infinite(), static_cast<UInt64>(index));
        ASTERIA_EXPECT(runner, "fixed slot accepts an in-range mint", result.HasValue());
    }

    const auto exhaustedResult =
        fixture.GetIssuer().Mint(fixture.GetSpace(), object, MakeRights(), domain, MonotonicTime::Infinite(), 0U);
    ASTERIA_EXPECT(runner, "fixed slot limit reports explicit exhaustion", !exhaustedResult.HasValue(),
                   exhaustedResult.GetError() == CapabilityError::SpaceExhausted);
    ASTERIA_EXPECT(runner, "active count remains bounded by slot capacity",
                   fixture.GetSpace().GetActiveCount() == CapabilitySpace::MaximumSlots);
}

void TestAtomicBatchTransfer(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    CapabilitySpace destination{fixture.GetObjects(), fixture.GetRevocations(), fixture.GetClock(),
                                0x1111'2222'3333'4444ULL, 0x5555'6666'7777'8888ULL};
    const ObjectId object = CreateEndpoint(fixture, runner);
    const RevocationDomainId domain = CreateRootDomain(fixture, runner);
    if (!object.IsValid() || !domain.IsValid() || !destination.IsReady())
    {
        return;
    }

    const CapabilityHandle moved = MintTransferCapability(fixture, fixture.GetSpace(), object, domain,
                                                          MakeTransferRights(), MonotonicTime{100U}, 0xAAAAU);
    const CapabilityHandle copied = MintTransferCapability(fixture, fixture.GetSpace(), object, domain,
                                                           MakeTransferRights(), MonotonicTime::Infinite(), 0xBBBBU);
    ASTERIA_EXPECT(runner, "batch-transfer fixtures mint two source capabilities", moved.IsValid(), copied.IsValid());
    if (!moved.IsValid() || !copied.IsValid())
    {
        return;
    }

    const CapabilityTransferRequest requests[]{
        MakeTransferRequest(moved, CapabilityTransferDisposition::Move, CapabilityRights{CapabilityRight::Invoke},
                            MonotonicTime{80U}),
        MakeTransferRequest(copied, CapabilityTransferDisposition::Copy, CapabilityRights{CapabilityRight::Read},
                            MonotonicTime{90U}),
    };
    const auto result =
        fixture.GetSpace().TransferBatchTo(destination, CapabilityTransferRequestView{.Data = requests, .Count = 2U});
    bool canonicalResult{result.HasValue()};
    if (result.HasValue())
    {
        const CapabilityTransferBatch& batch = result.GetValue();
        canonicalResult = (batch.Count == 2U) && batch.DestinationHandles[0].IsValid() &&
                          batch.DestinationHandles[1].IsValid() && !batch.DestinationHandles[2].IsValid() &&
                          !batch.DestinationHandles[3].IsValid();
        for (const Foundation::UInt8 reserved : batch.Reserved)
        {
            canonicalResult = canonicalResult && (reserved == 0U);
        }
    }
    ASTERIA_EXPECT(runner, "mixed move/copy batch returns ordered handles and canonical unused storage",
                   canonicalResult);
    if (!result.HasValue())
    {
        return;
    }

    ASTERIA_EXPECT(runner, "move invalidates only its source while copy preserves its source",
                   fixture.GetSpace().GetActiveCount() == 1U, destination.GetActiveCount() == 2U,
                   !fixture.GetSpace().Lookup(moved, ObjectType::Endpoint, CapabilityOperation::Inspect).HasValue(),
                   fixture.GetSpace().Lookup(copied, ObjectType::Endpoint, CapabilityOperation::Inspect).HasValue());

    const auto movedLookup =
        destination.Lookup(result.GetValue().DestinationHandles[0], ObjectType::Endpoint, CapabilityOperation::Invoke);
    const auto copiedLookup =
        destination.Lookup(result.GetValue().DestinationHandles[1], ObjectType::Endpoint, CapabilityOperation::Read);
    ASTERIA_EXPECT(runner, "destination capabilities preserve identity metadata and exact attenuation",
                   movedLookup.HasValue(), copiedLookup.HasValue(),
                   movedLookup.GetValue().GetDescriptor().Rights.IsEqualTo(CapabilityRights{CapabilityRight::Invoke}),
                   copiedLookup.GetValue().GetDescriptor().Rights.IsEqualTo(CapabilityRights{CapabilityRight::Read}),
                   movedLookup.GetValue().GetDescriptor().LeaseDeadline.GetTicks() == 80U,
                   copiedLookup.GetValue().GetDescriptor().LeaseDeadline.GetTicks() == 90U,
                   movedLookup.GetValue().GetDescriptor().SealedObjectSelector == 0xAAAAU,
                   copiedLookup.GetValue().GetDescriptor().SealedObjectSelector == 0xBBBBU);

    bool sourceCapacityRecovered{true};
    for (Foundation::Size index{fixture.GetSpace().GetActiveCount()}; index < CapabilitySpace::MaximumSlots; ++index)
    {
        sourceCapacityRecovered =
            sourceCapacityRecovered && MintTransferCapability(fixture, fixture.GetSpace(), object, domain).IsValid();
    }
    ASTERIA_EXPECT(runner, "cross-space move returns its source slot to the reusable capacity", sourceCapacityRecovered,
                   fixture.GetSpace().GetActiveCount() == CapabilitySpace::MaximumSlots);
}

void TestMaximumBatchTransfer(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    CapabilitySpace destination{fixture.GetObjects(), fixture.GetRevocations(), fixture.GetClock(), 0xABCDU, 0xEF01U};
    const ObjectId object = CreateEndpoint(fixture, runner);
    const RevocationDomainId domain = CreateRootDomain(fixture, runner);
    if (!object.IsValid() || !domain.IsValid() || !destination.IsReady())
    {
        return;
    }

    CapabilityHandle sources[CapabilityTransferBatch::MaximumTransfers]{};
    CapabilityTransferRequest requests[CapabilityTransferBatch::MaximumTransfers]{};
    bool fixturesValid{true};
    for (Foundation::Size index{0U}; index < CapabilityTransferBatch::MaximumTransfers; ++index)
    {
        sources[index] = MintTransferCapability(fixture, fixture.GetSpace(), object, domain, MakeTransferRights(),
                                                MonotonicTime{100U}, 0x1000U + static_cast<UInt64>(index));
        requests[index] = MakeTransferRequest(
            sources[index],
            (index % 2U) == 0U ? CapabilityTransferDisposition::Move : CapabilityTransferDisposition::Copy,
            CapabilityRights{CapabilityRight::Inspect}, MonotonicTime{50U + static_cast<UInt64>(index)});
        fixturesValid = fixturesValid && sources[index].IsValid();
    }
    ASTERIA_EXPECT(runner, "maximum transfer batch mints four sources", fixturesValid);
    if (!fixturesValid)
    {
        return;
    }

    const auto result =
        fixture.GetSpace().TransferBatchTo(destination, CapabilityTransferRequestView{
                                                            .Data = requests,
                                                            .Count = CapabilityTransferBatch::MaximumTransfers,
                                                        });
    bool exactBatch = result.HasValue() && (result.GetValue().Count == CapabilityTransferBatch::MaximumTransfers) &&
                      (fixture.GetSpace().GetActiveCount() == 2U) &&
                      (destination.GetActiveCount() == CapabilityTransferBatch::MaximumTransfers);
    if (result.HasValue())
    {
        for (Foundation::Size index{0U}; index < CapabilityTransferBatch::MaximumTransfers; ++index)
        {
            const CapabilityHandle destinationHandle = result.GetValue().DestinationHandles[index];
            const auto destinationLookup =
                destination.Lookup(destinationHandle, ObjectType::Endpoint, CapabilityOperation::Inspect);
            const auto sourceLookup =
                fixture.GetSpace().Lookup(sources[index], ObjectType::Endpoint, CapabilityOperation::Inspect);
            exactBatch = exactBatch && destinationHandle.IsValid() && destinationLookup.HasValue() &&
                         destinationLookup.GetValue().GetDescriptor().Rights.IsEqualTo(
                             CapabilityRights{CapabilityRight::Inspect}) &&
                         (destinationLookup.GetValue().GetDescriptor().LeaseDeadline.GetTicks() ==
                          (50U + static_cast<UInt64>(index))) &&
                         (destinationLookup.GetValue().GetDescriptor().SealedObjectSelector ==
                          (0x1000U + static_cast<UInt64>(index))) &&
                         ((index % 2U) == 0U ? (!sourceLookup.HasValue() &&
                                                (sourceLookup.GetError() == CapabilityError::InvalidHandle))
                                             : sourceLookup.HasValue());
        }
    }
    ASTERIA_EXPECT(runner, "maximum mixed batch preserves order and commits all four attachments", exactBatch);
}

void TestBatchTransferFailureAtomicity(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    CapabilitySpace destination{fixture.GetObjects(), fixture.GetRevocations(), fixture.GetClock(), 0x1111U, 0x2222U};
    const ObjectId object = CreateEndpoint(fixture, runner);
    const RevocationDomainId domain = CreateRootDomain(fixture, runner);
    if (!object.IsValid() || !domain.IsValid() || !destination.IsReady())
    {
        return;
    }

    const CapabilityHandle first = MintTransferCapability(fixture, fixture.GetSpace(), object, domain);
    const CapabilityHandle second = MintTransferCapability(fixture, fixture.GetSpace(), object, domain);
    if (!first.IsValid() || !second.IsValid())
    {
        ASTERIA_EXPECT(runner, "failure-atomicity fixtures mint source capabilities", false);
        return;
    }

    const CapabilityTransferRequest amplification[]{
        MakeTransferRequest(first, CapabilityTransferDisposition::Move),
        MakeTransferRequest(second, CapabilityTransferDisposition::Copy, CapabilityRights{CapabilityRight::Manage}),
    };
    const auto amplificationResult = fixture.GetSpace().TransferBatchTo(
        destination, CapabilityTransferRequestView{.Data = amplification, .Count = 2U});
    ASTERIA_EXPECT(runner, "late batch validation failure preserves every source and destination slot",
                   !amplificationResult.HasValue(),
                   amplificationResult.GetError() == CapabilityError::RightsAmplification,
                   fixture.GetSpace().GetActiveCount() == 2U, destination.GetActiveCount() == 0U,
                   fixture.GetSpace().Lookup(first, ObjectType::Endpoint, CapabilityOperation::Inspect).HasValue(),
                   fixture.GetSpace().Lookup(second, ObjectType::Endpoint, CapabilityOperation::Inspect).HasValue());

    const CapabilityTransferRequest duplicate[]{
        MakeTransferRequest(first, CapabilityTransferDisposition::Move),
        MakeTransferRequest(first, CapabilityTransferDisposition::Copy),
    };
    const auto duplicateResult =
        fixture.GetSpace().TransferBatchTo(destination, CapabilityTransferRequestView{.Data = duplicate, .Count = 2U});
    ASTERIA_EXPECT(runner, "one source selector cannot appear twice in an atomic batch", !duplicateResult.HasValue(),
                   duplicateResult.GetError() == CapabilityError::InvalidArgument,
                   fixture.GetSpace().GetActiveCount() == 2U, destination.GetActiveCount() == 0U);

    CapabilityTransferRequest invalid = MakeTransferRequest(first, CapabilityTransferDisposition::Move);
    invalid.Reserved = 1U;
    const auto reservedResult =
        fixture.GetSpace().TransferBatchTo(destination, CapabilityTransferRequestView{.Data = &invalid, .Count = 1U});
    const auto nullResult =
        fixture.GetSpace().TransferBatchTo(destination, CapabilityTransferRequestView{.Data = nullptr, .Count = 1U});
    const auto oversizedResult =
        fixture.GetSpace().TransferBatchTo(destination, CapabilityTransferRequestView{
                                                            .Data = &invalid,
                                                            .Count = CapabilityTransferBatch::MaximumTransfers + 1U,
                                                        });
    ASTERIA_EXPECT(runner, "reserved, null, and oversized request views fail before mutation",
                   !reservedResult.HasValue(), reservedResult.GetError() == CapabilityError::InvalidArgument,
                   !nullResult.HasValue(), nullResult.GetError() == CapabilityError::InvalidArgument,
                   !oversizedResult.HasValue(), oversizedResult.GetError() == CapabilityError::InvalidArgument,
                   fixture.GetSpace().GetActiveCount() == 2U, destination.GetActiveCount() == 0U);

    const auto emptyResult =
        fixture.GetSpace().TransferBatchTo(destination, CapabilityTransferRequestView{.Data = nullptr, .Count = 0U});
    bool emptyCanonical = emptyResult.HasValue() && (emptyResult.GetValue().Count == 0U);
    if (emptyResult.HasValue())
    {
        for (const CapabilityHandle handle : emptyResult.GetValue().DestinationHandles)
        {
            emptyCanonical = emptyCanonical && !handle.IsValid();
        }
        for (const Foundation::UInt8 reserved : emptyResult.GetValue().Reserved)
        {
            emptyCanonical = emptyCanonical && (reserved == 0U);
        }
    }
    ASTERIA_EXPECT(runner, "zero-count transfer is one canonical no-op", emptyCanonical);

    CapabilityFixture foreignFixture;
    const CapabilityTransferRequest valid = MakeTransferRequest(first, CapabilityTransferDisposition::Move);
    const auto foreignResult = fixture.GetSpace().TransferBatchTo(
        foreignFixture.GetSpace(), CapabilityTransferRequestView{.Data = &valid, .Count = 1U});
    ASTERIA_EXPECT(runner, "spaces outside one registry and lock domain cannot transact", !foreignResult.HasValue(),
                   foreignResult.GetError() == CapabilityError::InvalidConfiguration,
                   fixture.GetSpace().Lookup(first, ObjectType::Endpoint, CapabilityOperation::Inspect).HasValue());
}

void TestBatchTransferAuthorizationFailures(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    CapabilitySpace destination{fixture.GetObjects(), fixture.GetRevocations(), fixture.GetClock(), 0x7777U, 0x8888U};
    CapabilitySpace foreignSource{fixture.GetObjects(), fixture.GetRevocations(), fixture.GetClock(), 0x9999U, 0xAAAAU};
    const ObjectId object = CreateEndpoint(fixture, runner);
    const RevocationDomainId liveDomain = CreateRootDomain(fixture, runner);
    const RevocationDomainId revokedDomain = CreateRootDomain(fixture, runner);
    if (!object.IsValid() || !liveDomain.IsValid() || !revokedDomain.IsValid() || !destination.IsReady() ||
        !foreignSource.IsReady())
    {
        return;
    }

    const CapabilityHandle noTransfer =
        MintTransferCapability(fixture, fixture.GetSpace(), object, liveDomain,
                               CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Duplicate));
    const CapabilityHandle noDuplicate =
        MintTransferCapability(fixture, fixture.GetSpace(), object, liveDomain,
                               CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Transfer));
    const CapabilityHandle finite = MintTransferCapability(fixture, fixture.GetSpace(), object, liveDomain,
                                                           MakeTransferRights(), MonotonicTime{50U});
    const CapabilityHandle expired = MintTransferCapability(fixture, fixture.GetSpace(), object, liveDomain,
                                                            MakeTransferRights(), MonotonicTime{20U});
    const CapabilityHandle revoked = MintTransferCapability(fixture, fixture.GetSpace(), object, revokedDomain);
    const CapabilityHandle stale = MintTransferCapability(fixture, fixture.GetSpace(), object, liveDomain);
    const CapabilityHandle foreign = MintTransferCapability(fixture, foreignSource, object, liveDomain);
    const bool minted = noTransfer.IsValid() && noDuplicate.IsValid() && finite.IsValid() && expired.IsValid() &&
                        revoked.IsValid() && stale.IsValid() && foreign.IsValid();
    ASTERIA_EXPECT(runner, "transfer authorization fixtures mint every capability", minted);
    if (!minted)
    {
        return;
    }

    const Foundation::Size sourceCountBeforeClose = fixture.GetSpace().GetActiveCount();
    ASTERIA_EXPECT(runner, "transfer stale-handle fixture closes successfully",
                   fixture.GetSpace().Close(stale) == CapabilityError::Ok);
    ASTERIA_EXPECT(runner, "transfer revoked-handle fixture revokes successfully",
                   fixture.GetRevocations().Revoke(revokedDomain) == RevocationError::Ok);
    const Foundation::Size expectedSourceCount = sourceCountBeforeClose - 1U;

    const CapabilityTransferRequest deniedMove = MakeTransferRequest(noTransfer, CapabilityTransferDisposition::Move,
                                                                     CapabilityRights{CapabilityRight::Inspect});
    const CapabilityTransferRequest deniedCopy = MakeTransferRequest(noDuplicate, CapabilityTransferDisposition::Copy,
                                                                     CapabilityRights{CapabilityRight::Inspect});
    const CapabilityTransferRequest wrongType =
        MakeTransferRequest(finite, CapabilityTransferDisposition::Move, CapabilityRights{CapabilityRight::Inspect},
                            MonotonicTime{40U}, ObjectType::Frame);
    const CapabilityTransferRequest expiredRequest = MakeTransferRequest(
        finite, CapabilityTransferDisposition::Move, CapabilityRights{CapabilityRight::Inspect}, MonotonicTime{10U});
    const CapabilityTransferRequest extendedLease = MakeTransferRequest(
        finite, CapabilityTransferDisposition::Move, CapabilityRights{CapabilityRight::Inspect}, MonotonicTime{51U});
    const CapabilityTransferRequest revokedRequest = MakeTransferRequest(revoked, CapabilityTransferDisposition::Move);
    const CapabilityTransferRequest staleRequest = MakeTransferRequest(stale, CapabilityTransferDisposition::Move);
    const CapabilityTransferRequest foreignRequest = MakeTransferRequest(foreign, CapabilityTransferDisposition::Move);

    const auto deniedMoveResult = fixture.GetSpace().TransferBatchTo(
        destination, CapabilityTransferRequestView{.Data = &deniedMove, .Count = 1U});
    const auto deniedCopyResult = fixture.GetSpace().TransferBatchTo(
        destination, CapabilityTransferRequestView{.Data = &deniedCopy, .Count = 1U});
    const auto wrongTypeResult =
        fixture.GetSpace().TransferBatchTo(destination, CapabilityTransferRequestView{.Data = &wrongType, .Count = 1U});
    const auto expiredRequestResult = fixture.GetSpace().TransferBatchTo(
        destination, CapabilityTransferRequestView{.Data = &expiredRequest, .Count = 1U});
    const auto extendedLeaseResult = fixture.GetSpace().TransferBatchTo(
        destination, CapabilityTransferRequestView{.Data = &extendedLease, .Count = 1U});
    const auto revokedResult = fixture.GetSpace().TransferBatchTo(
        destination, CapabilityTransferRequestView{.Data = &revokedRequest, .Count = 1U});
    const auto staleResult = fixture.GetSpace().TransferBatchTo(
        destination, CapabilityTransferRequestView{.Data = &staleRequest, .Count = 1U});
    const auto foreignResult = fixture.GetSpace().TransferBatchTo(
        destination, CapabilityTransferRequestView{.Data = &foreignRequest, .Count = 1U});
    ASTERIA_EXPECT(runner, "transfer authorization failures report their exact typed errors",
                   !deniedMoveResult.HasValue(), deniedMoveResult.GetError() == CapabilityError::RightsDenied,
                   !deniedCopyResult.HasValue(), deniedCopyResult.GetError() == CapabilityError::RightsDenied,
                   !wrongTypeResult.HasValue(), wrongTypeResult.GetError() == CapabilityError::TypeMismatch,
                   !expiredRequestResult.HasValue(), expiredRequestResult.GetError() == CapabilityError::Expired,
                   !extendedLeaseResult.HasValue(), extendedLeaseResult.GetError() == CapabilityError::LeaseExtension,
                   !revokedResult.HasValue(), revokedResult.GetError() == CapabilityError::Revoked,
                   !staleResult.HasValue(), staleResult.GetError() == CapabilityError::InvalidHandle,
                   !foreignResult.HasValue(), foreignResult.GetError() == CapabilityError::InvalidHandle);

    CapabilityTransferRequest zeroRights = MakeTransferRequest(finite, CapabilityTransferDisposition::Move);
    zeroRights.RequestedRights = CapabilityRights{};
    CapabilityTransferRequest invalidType = MakeTransferRequest(finite, CapabilityTransferDisposition::Move);
    invalidType.ExpectedType = static_cast<ObjectType>(0xFFU);
    CapabilityTransferRequest invalidDisposition = MakeTransferRequest(finite, CapabilityTransferDisposition::Move);
    invalidDisposition.Disposition = static_cast<CapabilityTransferDisposition>(0xFFU);
    const auto zeroRightsResult = fixture.GetSpace().TransferBatchTo(
        destination, CapabilityTransferRequestView{.Data = &zeroRights, .Count = 1U});
    const auto invalidTypeResult = fixture.GetSpace().TransferBatchTo(
        destination, CapabilityTransferRequestView{.Data = &invalidType, .Count = 1U});
    const auto invalidDispositionResult =
        fixture.GetSpace().TransferBatchTo(destination, CapabilityTransferRequestView{
                                                            .Data = &invalidDisposition,
                                                            .Count = 1U,
                                                        });
    ASTERIA_EXPECT(runner, "all rejected transfer requests preserve both spaces", !zeroRightsResult.HasValue(),
                   zeroRightsResult.GetError() == CapabilityError::InvalidArgument, !invalidTypeResult.HasValue(),
                   invalidTypeResult.GetError() == CapabilityError::InvalidArgument,
                   !invalidDispositionResult.HasValue(),
                   invalidDispositionResult.GetError() == CapabilityError::InvalidArgument,
                   fixture.GetSpace().GetActiveCount() == expectedSourceCount, destination.GetActiveCount() == 0U,
                   fixture.GetSpace().Lookup(finite, ObjectType::Endpoint, CapabilityOperation::Inspect).HasValue());

    fixture.GetClock().Set(20U);
    const CapabilityTransferRequest expiredSource = MakeTransferRequest(
        expired, CapabilityTransferDisposition::Move, CapabilityRights{CapabilityRight::Inspect}, MonotonicTime{21U});
    const auto expiredSourceResult = fixture.GetSpace().TransferBatchTo(
        destination, CapabilityTransferRequestView{.Data = &expiredSource, .Count = 1U});
    ASTERIA_EXPECT(runner, "an expired source lease cannot be transferred or partially consumed",
                   !expiredSourceResult.HasValue(), expiredSourceResult.GetError() == CapabilityError::Expired,
                   fixture.GetSpace().GetActiveCount() == expectedSourceCount, destination.GetActiveCount() == 0U);

    const CapabilityHandle destinationWatermark = MintTransferCapability(fixture, destination, object, liveDomain);
    fixture.GetClock().Set(21U);
    const auto watermarkLookup =
        destination.Lookup(destinationWatermark, ObjectType::Endpoint, CapabilityOperation::Inspect);
    ASTERIA_EXPECT(runner, "destination clock watermark advances independently of the source",
                   destinationWatermark.IsValid(), watermarkLookup.HasValue());

    fixture.GetClock().Set(20U);
    const CapabilityTransferRequest rollbackRequest = MakeTransferRequest(
        finite, CapabilityTransferDisposition::Move, CapabilityRights{CapabilityRight::Inspect}, MonotonicTime{40U});
    const auto rollbackResult = fixture.GetSpace().TransferBatchTo(
        destination, CapabilityTransferRequestView{.Data = &rollbackRequest, .Count = 1U});
    fixture.GetClock().Set(21U);
    ASTERIA_EXPECT(
        runner, "destination-only clock rollback rejects transfer without slot mutation", !rollbackResult.HasValue(),
        rollbackResult.GetError() == CapabilityError::InvalidConfiguration,
        fixture.GetSpace().GetActiveCount() == expectedSourceCount, destination.GetActiveCount() == 1U,
        fixture.GetSpace().Lookup(finite, ObjectType::Endpoint, CapabilityOperation::Inspect).HasValue(),
        destination.Lookup(destinationWatermark, ObjectType::Endpoint, CapabilityOperation::Inspect).HasValue());

    fixture.GetClock().Set(~UInt64{0U});
    const auto infiniteClockResult = fixture.GetSpace().TransferBatchTo(
        destination, CapabilityTransferRequestView{.Data = &rollbackRequest, .Count = 1U});
    const auto infiniteClockNoOp =
        fixture.GetSpace().TransferBatchTo(destination, CapabilityTransferRequestView{.Data = nullptr, .Count = 0U});
    ASTERIA_EXPECT(runner, "reserved infinite clock fails nonempty transfer while zero-count stays a no-op",
                   !infiniteClockResult.HasValue(),
                   infiniteClockResult.GetError() == CapabilityError::InvalidConfiguration,
                   infiniteClockNoOp.HasValue(), infiniteClockNoOp.GetValue().Count == 0U,
                   fixture.GetSpace().GetActiveCount() == expectedSourceCount, destination.GetActiveCount() == 1U);
}

void TestBatchTransferClockWatermarks(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    CapabilitySpace destination{fixture.GetObjects(), fixture.GetRevocations(), fixture.GetClock(), 0xBBBBU, 0xCCCCU};
    const ObjectId object = CreateEndpoint(fixture, runner);
    const RevocationDomainId domain = CreateRootDomain(fixture, runner);
    if (!object.IsValid() || !domain.IsValid() || !destination.IsReady())
    {
        return;
    }

    const CapabilityHandle source = MintTransferCapability(fixture, fixture.GetSpace(), object, domain);
    if (!source.IsValid())
    {
        ASTERIA_EXPECT(runner, "clock-watermark fixture mints a source capability", false);
        return;
    }

    fixture.GetClock().Set(21U);
    {
        const auto sourceWatermark =
            fixture.GetSpace().Lookup(source, ObjectType::Endpoint, CapabilityOperation::Inspect);
        ASTERIA_EXPECT(runner, "source clock watermark advances independently of the destination",
                       sourceWatermark.HasValue());
    }

    fixture.GetClock().Set(20U);
    const CapabilityTransferRequest request =
        MakeTransferRequest(source, CapabilityTransferDisposition::Copy, CapabilityRights{CapabilityRight::Inspect});
    const auto sourceRollback =
        fixture.GetSpace().TransferBatchTo(destination, CapabilityTransferRequestView{.Data = &request, .Count = 1U});
    fixture.GetClock().Set(21U);
    ASTERIA_EXPECT(runner, "source-only clock rollback rejects transfer without slot mutation",
                   !sourceRollback.HasValue(), sourceRollback.GetError() == CapabilityError::InvalidConfiguration,
                   fixture.GetSpace().GetActiveCount() == 1U, destination.GetActiveCount() == 0U,
                   fixture.GetSpace().Lookup(source, ObjectType::Endpoint, CapabilityOperation::Inspect).HasValue());

    fixture.GetClock().Set(30U);
    const auto success =
        fixture.GetSpace().TransferBatchTo(destination, CapabilityTransferRequestView{.Data = &request, .Count = 1U});
    ASTERIA_EXPECT(runner, "successful transfer establishes both space clock watermarks", success.HasValue(),
                   fixture.GetSpace().GetActiveCount() == 1U, destination.GetActiveCount() == 1U);
    if (!success.HasValue())
    {
        return;
    }

    fixture.GetClock().Set(29U);
    const auto sourceAfterRollback =
        fixture.GetSpace().Lookup(source, ObjectType::Endpoint, CapabilityOperation::Inspect);
    const auto destinationAfterRollback = destination.Lookup(success.GetValue().DestinationHandles[0],
                                                             ObjectType::Endpoint, CapabilityOperation::Inspect);
    fixture.GetClock().Set(30U);
    ASTERIA_EXPECT(
        runner, "successful transfer advances both watermarks without changing either slot",
        !sourceAfterRollback.HasValue(), sourceAfterRollback.GetError() == CapabilityError::InvalidConfiguration,
        !destinationAfterRollback.HasValue(),
        destinationAfterRollback.GetError() == CapabilityError::InvalidConfiguration,
        fixture.GetSpace().Lookup(source, ObjectType::Endpoint, CapabilityOperation::Inspect).HasValue(),
        destination.Lookup(success.GetValue().DestinationHandles[0], ObjectType::Endpoint, CapabilityOperation::Inspect)
            .HasValue());
}

void TestTransferCapacityAndSameSpace(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    CapabilitySpace destination{fixture.GetObjects(), fixture.GetRevocations(), fixture.GetClock(), 0x3333U, 0x4444U};
    const ObjectId object = CreateEndpoint(fixture, runner);
    const RevocationDomainId domain = CreateRootDomain(fixture, runner);
    if (!object.IsValid() || !domain.IsValid() || !destination.IsReady())
    {
        return;
    }

    const CapabilityHandle source = MintTransferCapability(fixture, fixture.GetSpace(), object, domain);
    CapabilityHandle destinationHandles[CapabilitySpace::MaximumSlots]{};
    bool filled{source.IsValid()};
    for (Foundation::Size index{0U}; index < CapabilitySpace::MaximumSlots; ++index)
    {
        destinationHandles[index] = MintTransferCapability(fixture, destination, object, domain, MakeTransferRights(),
                                                           MonotonicTime::Infinite(), 1U + static_cast<UInt64>(index));
        filled = filled && destinationHandles[index].IsValid();
    }
    ASTERIA_EXPECT(runner, "destination fixture fills every bounded capability slot", filled,
                   destination.GetActiveCount() == CapabilitySpace::MaximumSlots);
    if (!filled)
    {
        return;
    }

    const CapabilityTransferRequest copy = MakeTransferRequest(source, CapabilityTransferDisposition::Copy);
    const auto fullResult =
        fixture.GetSpace().TransferBatchTo(destination, CapabilityTransferRequestView{.Data = &copy, .Count = 1U});
    ASTERIA_EXPECT(runner, "full destination rejects a copy without consuming the source", !fullResult.HasValue(),
                   fullResult.GetError() == CapabilityError::SpaceExhausted,
                   destination.GetActiveCount() == CapabilitySpace::MaximumSlots,
                   fixture.GetSpace().Lookup(source, ObjectType::Endpoint, CapabilityOperation::Inspect).HasValue());

    const CapabilityTransferRequest sameSpaceMove =
        MakeTransferRequest(destinationHandles[0], CapabilityTransferDisposition::Move, MakeTransferRights());
    const auto moveResult =
        destination.TransferBatchTo(destination, CapabilityTransferRequestView{.Data = &sameSpaceMove, .Count = 1U});
    ASTERIA_EXPECT(
        runner, "same-space move reuses its invalidated slot even at full capacity", moveResult.HasValue(),
        moveResult.GetValue().Count == 1U, destination.GetActiveCount() == CapabilitySpace::MaximumSlots,
        !destination.Lookup(destinationHandles[0], ObjectType::Endpoint, CapabilityOperation::Inspect).HasValue(),
        destination
            .Lookup(moveResult.GetValue().DestinationHandles[0], ObjectType::Endpoint, CapabilityOperation::Invoke)
            .HasValue());

    if (moveResult.HasValue())
    {
        const CapabilityTransferRequest sameSpaceCopy =
            MakeTransferRequest(moveResult.GetValue().DestinationHandles[0], CapabilityTransferDisposition::Copy);
        const auto copyResult = destination.TransferBatchTo(
            destination, CapabilityTransferRequestView{.Data = &sameSpaceCopy, .Count = 1U});
        ASTERIA_EXPECT(
            runner, "same-space copy still requires a distinct free destination slot", !copyResult.HasValue(),
            copyResult.GetError() == CapabilityError::SpaceExhausted,
            destination
                .Lookup(moveResult.GetValue().DestinationHandles[0], ObjectType::Endpoint, CapabilityOperation::Inspect)
                .HasValue());

        const CapabilityTransferRequest moves[]{
            MakeTransferRequest(destinationHandles[1], CapabilityTransferDisposition::Move, MakeTransferRights()),
            MakeTransferRequest(moveResult.GetValue().DestinationHandles[0], CapabilityTransferDisposition::Move,
                                MakeTransferRights()),
        };
        const auto movesResult =
            destination.TransferBatchTo(destination, CapabilityTransferRequestView{.Data = moves, .Count = 2U});
        bool orderedReuse =
            movesResult.HasValue() && (movesResult.GetValue().Count == 2U) &&
            (destination.GetActiveCount() == CapabilitySpace::MaximumSlots) &&
            !destination.Lookup(destinationHandles[1], ObjectType::Endpoint, CapabilityOperation::Inspect).HasValue() &&
            !destination
                 .Lookup(moveResult.GetValue().DestinationHandles[0], ObjectType::Endpoint,
                         CapabilityOperation::Inspect)
                 .HasValue();
        if (movesResult.HasValue())
        {
            const auto firstLookup = destination.Lookup(movesResult.GetValue().DestinationHandles[0],
                                                        ObjectType::Endpoint, CapabilityOperation::Inspect);
            const auto secondLookup = destination.Lookup(movesResult.GetValue().DestinationHandles[1],
                                                         ObjectType::Endpoint, CapabilityOperation::Inspect);
            orderedReuse = orderedReuse && firstLookup.HasValue() && secondLookup.HasValue() &&
                           (firstLookup.GetValue().GetDescriptor().SealedObjectSelector == 2U) &&
                           (secondLookup.GetValue().GetDescriptor().SealedObjectSelector == 1U);
        }
        ASTERIA_EXPECT(runner, "full same-space multi-move reuses slots in request order", orderedReuse);
    }
}

void TestConcurrentBatchCopies(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    CapabilitySpace destination{fixture.GetObjects(), fixture.GetRevocations(), fixture.GetClock(), 0x5555U, 0x6666U};
    const ObjectId object = CreateEndpoint(fixture, runner);
    const RevocationDomainId domain = CreateRootDomain(fixture, runner);
    if (!object.IsValid() || !domain.IsValid() || !destination.IsReady())
    {
        return;
    }
    const CapabilityHandle source = MintTransferCapability(fixture, fixture.GetSpace(), object, domain);
    if (!source.IsValid())
    {
        ASTERIA_EXPECT(runner, "concurrent transfer fixture mints a source capability", false);
        return;
    }

    constexpr Foundation::Size WorkerCount{4U};
    constexpr Foundation::Size CopiesPerWorker{32U};
    CapabilityHandle received[WorkerCount][CopiesPerWorker]{};
    std::atomic<std::size_t> unexpectedResults{0U};
    const CapabilityTransferRequest request = MakeTransferRequest(source, CapabilityTransferDisposition::Copy);
    const auto worker = [&](const Foundation::Size workerIndex) noexcept
    {
        for (Foundation::Size copyIndex{0U}; copyIndex < CopiesPerWorker; ++copyIndex)
        {
            const auto result = fixture.GetSpace().TransferBatchTo(
                destination, CapabilityTransferRequestView{.Data = &request, .Count = 1U});
            if (!result.HasValue())
            {
                ++unexpectedResults;
                continue;
            }
            received[workerIndex][copyIndex] = result.GetValue().DestinationHandles[0];
        }
    };

    std::thread first{worker, 0U};
    std::thread second{worker, 1U};
    std::thread third{worker, 2U};
    std::thread fourth{worker, 3U};
    first.join();
    second.join();
    third.join();
    fourth.join();

    bool everyHandleResolves{unexpectedResults.load() == 0U};
    for (Foundation::Size workerIndex{0U}; workerIndex < WorkerCount; ++workerIndex)
    {
        for (Foundation::Size copyIndex{0U}; copyIndex < CopiesPerWorker; ++copyIndex)
        {
            everyHandleResolves =
                everyHandleResolves &&
                destination.Lookup(received[workerIndex][copyIndex], ObjectType::Endpoint, CapabilityOperation::Invoke)
                    .HasValue();
        }
    }
    ASTERIA_EXPECT(runner, "concurrent copies serialize into unique valid destination handles", everyHandleResolves,
                   destination.GetActiveCount() == WorkerCount * CopiesPerWorker,
                   fixture.GetSpace().GetActiveCount() == 1U);
}

void TestConcurrentLookupAndRevoke(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    const ObjectId object = CreateEndpoint(fixture, runner);
    const RevocationDomainId domain = CreateRootDomain(fixture, runner);
    if (!object.IsValid() || !domain.IsValid())
    {
        return;
    }

    const CapabilityHandle handle = MintEndpoint(fixture, runner, object, domain);
    std::atomic<std::size_t> readyWorkers{0U};
    std::atomic<bool> begin{false};
    std::atomic<std::size_t> firstLookupsComplete{0U};
    std::atomic<std::size_t> unexpectedResults{0U};

    const auto lookupWorker = [&]() noexcept
    {
        ++readyWorkers;
        while (!begin.load())
        {
            std::this_thread::yield();
        }

        for (std::size_t iteration{0U}; iteration < 2'000U; ++iteration)
        {
            const auto result = fixture.GetSpace().Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Inspect);
            if (!result.HasValue() && (result.GetError() != CapabilityError::Revoked))
            {
                ++unexpectedResults;
            }
            if (iteration == 0U)
            {
                ++firstLookupsComplete;
            }
        }
    };

    std::thread firstWorker{lookupWorker};
    std::thread secondWorker{lookupWorker};
    std::thread thirdWorker{lookupWorker};
    while (readyWorkers.load() != 3U)
    {
        std::this_thread::yield();
    }
    begin.store(true);
    while (firstLookupsComplete.load() != 3U)
    {
        std::this_thread::yield();
    }
    const RevocationError revokeResult = fixture.GetRevocations().Revoke(domain);
    firstWorker.join();
    secondWorker.join();
    thirdWorker.join();

    ASTERIA_EXPECT(runner, "concurrent revocation succeeds", revokeResult == RevocationError::Ok);
    ASTERIA_EXPECT(runner, "concurrent lookup observes only valid or revoked states", unexpectedResults.load() == 0U);
}

void TestConcurrentLookupAndDestroy(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    const ObjectId object = CreateEndpoint(fixture, runner);
    const RevocationDomainId domain = CreateRootDomain(fixture, runner);
    if (!object.IsValid() || !domain.IsValid())
    {
        return;
    }

    const CapabilityHandle handle = MintEndpoint(fixture, runner, object, domain);
    std::atomic<std::size_t> readyWorkers{0U};
    std::atomic<bool> begin{false};
    std::atomic<std::size_t> unexpectedResults{0U};

    const auto lookupWorker = [&]() noexcept
    {
        ++readyWorkers;
        while (!begin.load())
        {
            std::this_thread::yield();
        }

        for (std::size_t iteration{0U}; iteration < 2'000U; ++iteration)
        {
            const auto result = fixture.GetSpace().Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Inspect);
            if (!result.HasValue() && (result.GetError() != CapabilityError::ObjectNotFound))
            {
                ++unexpectedResults;
            }
        }
    };

    std::thread firstWorker{lookupWorker};
    std::thread secondWorker{lookupWorker};
    std::thread thirdWorker{lookupWorker};
    while (readyWorkers.load() != 3U)
    {
        std::this_thread::yield();
    }
    begin.store(true);
    const ObjectRegistryError destroyResult = fixture.GetIssuer().DestroyObject(object);
    firstWorker.join();
    secondWorker.join();
    thirdWorker.join();

    ASTERIA_EXPECT(runner, "object destruction linearizes against concurrent lookups",
                   destroyResult == ObjectRegistryError::Ok);
    ASTERIA_EXPECT(runner, "concurrent lookup observes only valid or quiescing-object states",
                   unexpectedResults.load() == 0U);
    const auto collectedObject = fixture.GetIssuer().CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "concurrent lookup releases every pin before object collection", collectedObject.HasValue(),
                   collectedObject.GetValue().Object.GetValue() == object.GetValue(),
                   collectedObject.GetValue().Instance == &fixture.GetKernelObject(0U));
}
} // namespace Asteria::Test

namespace Asteria::Test
{
// A mint under authority re-authorizes the authorizing capability and inherits its revocation
// domain and lease deadline.
void TestMintUnderAuthority(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    const auto domainObject = fixture.CreateObject(ObjectType::ResourceDomain, ResourceDomainId{7U});
    const ObjectId endpoint = CreateEndpoint(fixture, runner);
    const ObjectId second = CreateEndpoint(fixture, runner);
    const RevocationDomainId domain = CreateRootDomain(fixture, runner);
    const ObjectId resource = domainObject.HasValue() ? domainObject.GetValue() : ObjectId{};
    const IKernelObject& resourceInstance = fixture.GetKernelObject(0U);
    const CapabilityRights manage = CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Inspect);
    const CapabilityHandle authority =
        MintTransferCapability(fixture, fixture.GetSpace(), resource, domain, manage, MonotonicTime{100U});
    const CapabilityHandle inspectOnly = MintTransferCapability(fixture, fixture.GetSpace(), resource, domain,
                                                                CapabilityRights{CapabilityRight::Inspect});
    const CapabilityHandle revoker = MintTransferCapability(fixture, fixture.GetSpace(), resource, domain,
                                                            CapabilityRights{CapabilityRight::Revoke});
    const CapabilityRights created = CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Invoke);
    RootCapabilityIssuer& issuer = fixture.GetIssuer();
    CapabilitySpace& space = fixture.GetSpace();

    const auto minted = issuer.MintUnderAuthority(space, endpoint, created, authority, ObjectType::ResourceDomain,
                                                  CapabilityOperation::Manage, resourceInstance);
    const auto queried = minted.HasValue()
                             ? space.Query(minted.GetValue())
                             : Foundation::Result<Kernel::Object::CapabilityDescriptor, CapabilityError>::Failure(
                                   CapabilityError::InvalidHandle);
    ASTERIA_EXPECT(runner, "a mint under authority takes the authority's domain and deadline", resource.IsValid(),
                   authority.IsValid(), inspectOnly.IsValid(), revoker.IsValid(), minted.HasValue(), queried.HasValue(),
                   queried.HasValue() && (queried.GetValue().Object.GetValue() == endpoint.GetValue()),
                   queried.HasValue() && (queried.GetValue().Type == ObjectType::Endpoint),
                   queried.HasValue() && queried.GetValue().Rights.IsEqualTo(created),
                   queried.HasValue() && (queried.GetValue().RevocationDomain.GetValue() == domain.GetValue()),
                   queried.HasValue() && (queried.GetValue().LeaseDeadline.GetTicks() == 100U),
                   queried.HasValue() && (queried.GetValue().SealedObjectSelector == 0U),
                   queried.HasValue() && (queried.GetValue().ChargeOwner.GetValue() == 7U));

    const auto unauthorized = issuer.MintUnderAuthority(space, second, created, inspectOnly, ObjectType::ResourceDomain,
                                                        CapabilityOperation::Manage, resourceInstance);
    const auto wrongType =
        issuer.MintUnderAuthority(space, second, created, minted.GetValue(), ObjectType::ResourceDomain,
                                  CapabilityOperation::Inspect, resourceInstance);
    const auto wrongObject = issuer.MintUnderAuthority(space, second, created, authority, ObjectType::ResourceDomain,
                                                       CapabilityOperation::Manage, fixture.GetKernelObject(1U));
    const auto invalidRights =
        issuer.MintUnderAuthority(space, second, CapabilityRights::All().With(static_cast<CapabilityRight>(0x8000U)),
                                  authority, ObjectType::ResourceDomain, CapabilityOperation::Manage, resourceInstance);
    const auto unknown =
        issuer.MintUnderAuthority(space, ObjectId{0x7FFF'0001U}, created, authority, ObjectType::ResourceDomain,
                                  CapabilityOperation::Manage, resourceInstance);
    ASTERIA_EXPECT(runner, "a mint under authority checks the authority like a lookup and the object",
                   !unauthorized.HasValue() && (unauthorized.GetError() == CapabilityError::RightsDenied),
                   !wrongType.HasValue() && (wrongType.GetError() == CapabilityError::TypeMismatch),
                   !wrongObject.HasValue() && (wrongObject.GetError() == CapabilityError::InvalidArgument),
                   !invalidRights.HasValue() && (invalidRights.GetError() == CapabilityError::InvalidArgument),
                   !unknown.HasValue() && (unknown.GetError() == CapabilityError::ObjectNotFound));

    fixture.GetClock().Set(100U);
    const auto expired = issuer.MintUnderAuthority(space, second, created, authority, ObjectType::ResourceDomain,
                                                   CapabilityOperation::Manage, resourceInstance);
    const auto expiredLookup = space.Lookup(minted.GetValue(), ObjectType::Endpoint, CapabilityOperation::Invoke);
    ASTERIA_EXPECT(runner, "an expired authority mints nothing and its creations expire with it",
                   !expired.HasValue() && (expired.GetError() == CapabilityError::Expired),
                   !expiredLookup.HasValue() && (expiredLookup.GetError() == CapabilityError::Expired));

    CapabilityFixture revoked;
    const auto revokedResource = revoked.CreateObject(ObjectType::ResourceDomain, ResourceDomainId{7U});
    const ObjectId revokedEndpoint = CreateEndpoint(revoked, runner);
    const ObjectId revokedSecond = CreateEndpoint(revoked, runner);
    const RevocationDomainId revokedDomain = CreateRootDomain(revoked, runner);
    const ObjectId revokedResourceId = revokedResource.HasValue() ? revokedResource.GetValue() : ObjectId{};
    const CapabilityHandle grant =
        MintTransferCapability(revoked, revoked.GetSpace(), revokedResourceId, revokedDomain, manage);
    const CapabilityHandle grantRevoker = MintTransferCapability(
        revoked, revoked.GetSpace(), revokedResourceId, revokedDomain, CapabilityRights{CapabilityRight::Revoke});
    const auto before = revoked.GetIssuer().MintUnderAuthority(revoked.GetSpace(), revokedEndpoint, created, grant,
                                                               ObjectType::ResourceDomain, CapabilityOperation::Manage,
                                                               revoked.GetKernelObject(0U));
    const bool revokedGrant = revoked.GetSpace().Revoke(grantRevoker) == CapabilityError::Ok;
    const auto after = revoked.GetIssuer().MintUnderAuthority(revoked.GetSpace(), revokedSecond, created, grant,
                                                              ObjectType::ResourceDomain, CapabilityOperation::Manage,
                                                              revoked.GetKernelObject(0U));
    const auto creationLookup =
        before.HasValue()
            ? revoked.GetSpace().Lookup(before.GetValue(), ObjectType::Endpoint, CapabilityOperation::Invoke)
            : revoked.GetSpace().Lookup(CapabilityHandle{}, ObjectType::Endpoint, CapabilityOperation::Invoke);
    const bool closed = revoked.GetSpace().Close(grant) == CapabilityError::Ok;
    const auto afterClose = revoked.GetIssuer().MintUnderAuthority(
        revoked.GetSpace(), revokedSecond, created, grant, ObjectType::ResourceDomain, CapabilityOperation::Manage,
        revoked.GetKernelObject(0U));
    ASTERIA_EXPECT(runner, "revoking the authority's domain ends its creations and later mints", before.HasValue(),
                   revokedGrant, !after.HasValue() && (after.GetError() == CapabilityError::Revoked),
                   !creationLookup.HasValue() && (creationLookup.GetError() == CapabilityError::Revoked), closed,
                   !afterClose.HasValue() && (afterClose.GetError() == CapabilityError::InvalidHandle));
}
void TestProcessorAuthorityAndClocks(TestContext& runner) noexcept
{
    CapabilityFixture fixture;
    const auto root = CreateRootDomain(fixture, runner);
    const auto pci = fixture.CreateObject(ObjectType::PciFunction, ResourceDomainId{7U});
    ASTERIA_EXPECT(runner, "placement source registered", pci.HasValue());
    if (!pci.HasValue())
    {
        return;
    }
    const auto inspect = MintTransferCapability(fixture, fixture.GetSpace(), pci.GetValue(), root,
                                                CapabilityRights{CapabilityRight::Inspect});
    ASTERIA_EXPECT(runner, "PCI inspection alone is not interrupt authority",
                   !fixture.GetSpace().HasProcessorDeviceAuthority());
    const auto invoke = MintTransferCapability(fixture, fixture.GetSpace(), pci.GetValue(), root,
                                               CapabilityRights{CapabilityRight::Invoke}, MonotonicTime{200U});
    ASTERIA_EXPECT(runner, "PCI interrupt creation authority pins placement to BSP",
                   fixture.GetSpace().HasProcessorDeviceAuthority());
    ASTERIA_EXPECT(runner, "closed source authority no longer pins placement",
                   fixture.GetSpace().Close(invoke) == CapabilityError::Ok,
                   !fixture.GetSpace().HasProcessorDeviceAuthority());
    Platform::Synchronization::SetHostedProcessor(Kernel::Scheduler::CpuId{0U});
    fixture.GetClock().Set(100U);
    ASTERIA_EXPECT(runner, "CPU 0 records its local watermark", fixture.GetSpace().Query(inspect).HasValue());
    Platform::Synchronization::SetHostedProcessor(Kernel::Scheduler::CpuId{1U});
    fixture.GetClock().Set(90U);
    ASTERIA_EXPECT(runner, "epsilon skew between CPUs is not local clock reversal",
                   fixture.GetSpace().Query(inspect).HasValue());
    fixture.GetClock().Set(89U);
    const auto reversed = fixture.GetSpace().Query(inspect);
    ASTERIA_EXPECT(runner, "same CPU regression is still rejected",
                   !reversed.HasValue() && reversed.GetError() == CapabilityError::InvalidConfiguration);
    Platform::Synchronization::SetHostedProcessor(Kernel::Scheduler::CpuId{0U});
    fixture.GetClock().Set(101U);
    ASTERIA_EXPECT(runner, "first CPU's watermark is independent", fixture.GetSpace().Query(inspect).HasValue());
    Platform::Synchronization::SetHostedProcessor(Kernel::Scheduler::CpuId{});
}
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"CapabilitySpaceTests"};
    Asteria::Test::TestMintAndLookup(runner);
    Asteria::Test::TestTypeAndRightsChecks(runner);
    Asteria::Test::TestAttenuationAndDuplication(runner);
    Asteria::Test::TestHierarchicalRevocation(runner);
    Asteria::Test::TestRevocationRegistryContract(runner);
    Asteria::Test::TestRevocationRegistryProvenance(runner);
    Asteria::Test::TestRevocationDomainSlotRetirement(runner);
    Asteria::Test::TestHandleIsolationAndObjectLifetime(runner);
    Asteria::Test::TestObjectLeaseLifetime(runner);
    Asteria::Test::TestLeaseAuthorizationSnapshot(runner);
    Asteria::Test::TestInvalidInputsAndClock(runner);
    Asteria::Test::TestRegistryBounds(runner);
    Asteria::Test::TestFixedCapacity(runner);
    Asteria::Test::TestAtomicBatchTransfer(runner);
    Asteria::Test::TestMaximumBatchTransfer(runner);
    Asteria::Test::TestBatchTransferFailureAtomicity(runner);
    Asteria::Test::TestBatchTransferAuthorizationFailures(runner);
    Asteria::Test::TestBatchTransferClockWatermarks(runner);
    Asteria::Test::TestTransferCapacityAndSameSpace(runner);
    Asteria::Test::TestConcurrentBatchCopies(runner);
    Asteria::Test::TestConcurrentLookupAndRevoke(runner);
    Asteria::Test::TestConcurrentLookupAndDestroy(runner);
    Asteria::Test::TestMintUnderAuthority(runner);
    Asteria::Test::TestProcessorAuthorityAndClocks(runner);
    return runner.Finish();
}
