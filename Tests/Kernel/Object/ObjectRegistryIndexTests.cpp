// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <memory>
#include <vector>

#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Kernel::Object::IKernelObject;
using Kernel::Object::ObjectExclusiveRange;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Platform::Synchronization::SpinLock;

// The kernel's registry: MaximumObjects entries.
using FullRegistry = TestSupport::SizedObjectRegistry<ObjectRegistry::MaximumObjects>;

class TestKernelObject final : public IKernelObject
{
public:
    constexpr TestKernelObject() noexcept = default;

    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return Type;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return ResourceDomainId{1U};
    }

    ObjectType Type{ObjectType::Frame};
};

[[nodiscard]] constexpr UInt64 SlotOf(const ObjectId object) noexcept
{
    return object.GetValue() & 0xFFFFU;
}

[[nodiscard]] constexpr UInt64 GenerationOf(const ObjectId object) noexcept
{
    return object.GetValue() >> 16U;
}

void TestAttach(TestContext& runner) noexcept
{
    SpinLock synchronization;
    ObjectRegistry bare{synchronization};
    RootCapabilityIssuer issuer{bare};
    TestKernelObject object;
    alignas(ObjectRegistry::EntryAlignment) static Foundation::UInt8 storage[4U * ObjectRegistry::EntryBytes + 8U]{};
    const auto unattached = issuer.CreateObject(object);
    const bool tooMany = !bare.AttachEntries(storage, ObjectRegistry::MaximumObjects + 1U);
    const bool none = !bare.AttachEntries(storage, 0U);
    const bool misaligned = !bare.AttachEntries(storage + 4U, 4U);
    const bool attached = bare.AttachEntries(storage, 4U);
    const bool again = !bare.AttachEntries(storage, 4U);
    ASTERIA_EXPECT(runner, "a registry without entries registers nothing, and attaches once, aligned, within bounds",
                   !unattached.HasValue() && (unattached.GetError() == ObjectRegistryError::RegistryExhausted), tooMany,
                   none, misaligned, attached, again, bare.GetCapacity() == 4U, issuer.CreateObject(object).HasValue());
}

// Every one of the 16,384 entries is found through the free bitmap, lowest
// first as the old scan chose; the collector visits only quiescing entries.
void TestFullCapacity(TestContext& runner) noexcept
{
    static SpinLock synchronization;
    static FullRegistry registry{synchronization};
    static RootCapabilityIssuer issuer{registry};
    static TestKernelObject objects[ObjectRegistry::MaximumObjects + 1U];
    static ObjectId identities[ObjectRegistry::MaximumObjects];
    bool ordered{true};
    for (Size index{0U}; index < ObjectRegistry::MaximumObjects; ++index)
    {
        const auto created = issuer.CreateObject(objects[index]);
        identities[index] = created.HasValue() ? created.GetValue() : ObjectId{};
        ordered = ordered && created.HasValue() && (SlotOf(identities[index]) == index + 1U) &&
                  (GenerationOf(identities[index]) == 1U);
    }
    const auto exhausted = issuer.CreateObject(objects[ObjectRegistry::MaximumObjects]);
    ASTERIA_EXPECT(runner, "16,384 objects take the entries lowest first, and the next is refused", ordered,
                   registry.GetCapacity() == ObjectRegistry::MaximumObjects,
                   registry.GetActiveCount() == ObjectRegistry::MaximumObjects,
                   !exhausted.HasValue() && (exhausted.GetError() == ObjectRegistryError::RegistryExhausted));

    // Destroy every third object; the collector returns exactly those, in
    // entry order, and nothing else.
    bool destroyed{true};
    Size expected{0U};
    for (Size index{0U}; index < ObjectRegistry::MaximumObjects; index += 3U)
    {
        destroyed = destroyed && (issuer.DestroyObject(identities[index]) == ObjectRegistryError::Ok);
        ++expected;
    }
    bool inOrder{true};
    Size collected{0U};
    for (;;)
    {
        const auto record = issuer.CollectDestroyedObject();
        if (!record.HasValue())
        {
            inOrder = inOrder && (record.GetError() == ObjectRegistryError::NoDestructionReady);
            break;
        }
        inOrder = inOrder && (record.GetValue().Object.GetValue() == identities[collected * 3U].GetValue()) &&
                  (record.GetValue().Instance == &objects[collected * 3U]);
        ++collected;
    }
    // New objects reuse the freed entries lowest first, one generation on.
    bool reused{true};
    for (Size index{0U}; index < 4U; ++index)
    {
        const auto created = issuer.CreateObject(objects[index * 3U]);
        reused = reused && created.HasValue() && (SlotOf(created.GetValue()) == (index * 3U) + 1U) &&
                 (GenerationOf(created.GetValue()) == 2U);
    }
    ASTERIA_EXPECT(runner, "the collector returns only the destroyed entries, and creation reuses them lowest first",
                   destroyed, inOrder, collected == expected, reused,
                   registry.GetActiveCount() == (ObjectRegistry::MaximumObjects - expected + 4U));
}

struct ModelClaim
{
    bool Registered;
    ObjectId Identity;
    bool Claimed;
    UInt64 Start;
    UInt64 End;
};

// The claim tree against a reference that scans every claim: a claim
// conflicts exactly when it overlaps a claim of the same type, release and
// collection end a claim, and the tree stays consistent through thousands of
// insertions and removals in any order.
bool ClaimTreeMatches(const std::uint64_t seed) noexcept
{
    constexpr Size Objects{1500U};
    static SpinLock synchronization;
    static const auto registry = std::make_unique<TestSupport::SizedObjectRegistry<4096U>>(synchronization);
    static const auto issuer = std::make_unique<RootCapabilityIssuer>(*registry);
    static TestKernelObject objects[Objects];
    std::vector<ModelClaim> model(Objects, ModelClaim{false, ObjectId{}, false, 0U, 0U});
    TestSupport::Random random{seed};
    for (Size index{0U}; index < Objects; ++index)
    {
        objects[index].Type = (index % 7U == 0U) ? ObjectType::DeviceRegion : ObjectType::Frame;
        const auto created = issuer->CreateObject(objects[index]);
        model[index] =
            ModelClaim{created.HasValue(), created.HasValue() ? created.GetValue() : ObjectId{}, false, 0U, 0U};
    }
    bool matches{true};
    for (Size step{0U}; matches && (step < 12000U); ++step)
    {
        const Size index = static_cast<Size>(random.Below(Objects));
        ModelClaim& claim = model[index];
        const UInt64 choice = random.Below(10U);
        if (choice < 6U)
        {
            const UInt64 start = random.Below(1U << 20U) << 12U;
            const UInt64 end = start + ((1U + random.Below(64U)) << 12U);
            bool conflict{false};
            for (Size other{0U}; other < Objects; ++other)
            {
                conflict = conflict ||
                           ((other != index) && model[other].Claimed && (objects[other].Type == objects[index].Type) &&
                            (start < model[other].End) && (model[other].Start < end));
            }
            const ObjectRegistryError result =
                registry->ClaimExclusiveRange(claim.Identity, objects[index].Type, {start, end});
            const ObjectRegistryError expected =
                claim.Claimed ? ObjectRegistryError::InvalidArgument
                              : (conflict ? ObjectRegistryError::ExclusiveRangeConflict : ObjectRegistryError::Ok);
            matches = result == expected;
            if (result == ObjectRegistryError::Ok)
            {
                claim.Claimed = true;
                claim.Start = start;
                claim.End = end;
            }
        }
        else if (choice < 9U)
        {
            const ObjectRegistryError result = registry->ReleaseExclusiveRange(claim.Identity);
            matches = result == (claim.Claimed ? ObjectRegistryError::Ok : ObjectRegistryError::InvalidArgument);
            claim.Claimed = false;
        }
        else
        {
            // Collection ends the claim; the entry returns with a new identity.
            const auto collected = (issuer->DestroyObject(claim.Identity) == ObjectRegistryError::Ok)
                                       ? issuer->CollectDestroyedObject(claim.Identity)
                                       : decltype(issuer->CollectDestroyedObject(claim.Identity))::Failure(
                                             ObjectRegistryError::InvalidObject);
            const auto created = issuer->CreateObject(objects[index]);
            matches = collected.HasValue() && created.HasValue();
            claim = ModelClaim{created.HasValue(), created.HasValue() ? created.GetValue() : ObjectId{}, false, 0U, 0U};
        }
    }
    // Leave the registry empty of claims for the next seed.
    for (Size index{0U}; index < Objects; ++index)
    {
        if (model[index].Claimed)
        {
            matches = matches && (registry->ReleaseExclusiveRange(model[index].Identity) == ObjectRegistryError::Ok);
        }
        if (model[index].Registered)
        {
            matches = matches && (issuer->DestroyObject(model[index].Identity) == ObjectRegistryError::Ok) &&
                      issuer->CollectDestroyedObject(model[index].Identity).HasValue();
        }
    }
    return matches && (registry->GetActiveCount() == 0U);
}

void TestClaimTree(TestContext& runner) noexcept
{
    const TestSupport::SeedRange seeds = TestSupport::GetSeedRange(0xC1A1'3000U, 4U);
    for (std::uint64_t seed{seeds.First}; seed < seeds.First + seeds.Count; ++seed)
    {
        ASTERIA_EXPECT(runner, "claims conflict exactly on a same-type overlap, through any sequence",
                       ClaimTreeMatches(seed));
    }

    // Disjoint claims of one type in ascending, descending and interleaved
    // order fill the tree; each overlap is still found.
    static SpinLock synchronization;
    static FullRegistry registry{synchronization};
    static RootCapabilityIssuer issuer{registry};
    static TestKernelObject objects[8192U];
    static ObjectId identities[8192U];
    bool claimed{true};
    for (Size index{0U}; index < 8192U; ++index)
    {
        const auto created = issuer.CreateObject(objects[index]);
        identities[index] = created.HasValue() ? created.GetValue() : ObjectId{};
        // Ascending, then descending, so both rotations run deep.
        const UInt64 page = (index < 4096U) ? index : (12287U - index);
        claimed = claimed && created.HasValue() &&
                  (registry.ClaimExclusiveRange(identities[index], ObjectType::Frame,
                                                {page << 13U, (page << 13U) + 4096U}) == ObjectRegistryError::Ok);
    }
    bool conflicts{true};
    for (Size index{0U}; index < 8192U; index += 97U)
    {
        // A second object overlapping each claim's last byte, and one
        // Bordering it, which is no overlap.
        conflicts = conflicts && (registry.ReleaseExclusiveRange(identities[index]) == ObjectRegistryError::Ok) &&
                    (registry.ClaimExclusiveRange(identities[index], ObjectType::Frame,
                                                  {(((index < 4096U) ? index : (12287U - index)) << 13U) + 4096U,
                                                   (((index < 4096U) ? index : (12287U - index)) << 13U) + 8192U}) ==
                     ObjectRegistryError::Ok) &&
                    (registry.ClaimExclusiveRange(identities[(index + 1U) % 8192U], ObjectType::Frame, {0U, 1U}) ==
                     ObjectRegistryError::InvalidArgument);
    }
    static TestKernelObject probe;
    const auto probeIdentity = issuer.CreateObject(probe);
    const bool overlap =
        probeIdentity.HasValue() &&
        (registry.ClaimExclusiveRange(probeIdentity.GetValue(), ObjectType::Frame,
                                      {(100U << 13U) + 4095U, (100U << 13U) + 4097U}) ==
         ObjectRegistryError::ExclusiveRangeConflict) &&
        (registry.ClaimExclusiveRange(probeIdentity.GetValue(), ObjectType::Frame,
                                      {(8191U << 13U) + 4096U, (8191U << 13U) + 8192U}) == ObjectRegistryError::Ok);
    ASTERIA_EXPECT(runner, "8,192 disjoint claims in ascending and descending order, then overlaps and borders",
                   claimed, conflicts, overlap);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"ObjectRegistryIndexTests"};
    Asteria::Test::TestAttach(runner);
    Asteria::Test::TestFullCapacity(runner);
    Asteria::Test::TestClaimTree(runner);
    return runner.Finish();
}
