// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Object::IKernelObject;
using Kernel::Object::ObjectExclusiveRange;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Platform::Synchronization::SpinLock;

class TestKernelObject final : public IKernelObject
{
public:
    explicit constexpr TestKernelObject(const ObjectType type) noexcept : m_type{type}
    {
    }

    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return m_type;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return ResourceDomainId{1U};
    }

private:
    ObjectType m_type;
};

[[nodiscard]] ObjectId Register(RootCapabilityIssuer& issuer, IKernelObject& object) noexcept
{
    const auto registered = issuer.CreateObject(object);
    return registered.HasValue() ? registered.GetValue() : ObjectId{};
}

// Overlapping, nested, and bordering Frame claims, including claims of
// quiescing Frames, are exercised through the AddressSpace model. These are
// the claim rules no mapper reaches: argument, identity, and type validation,
// a second claim, release without a claim, and claims of other types.
void TestClaimValidation(TestContext& runner) noexcept
{
    SpinLock synchronization;
    Asteria::TestSupport::TestObjectRegistry registry{synchronization};
    RootCapabilityIssuer issuer{registry};
    TestKernelObject first{ObjectType::Frame};
    TestKernelObject second{ObjectType::Frame};
    TestKernelObject other{ObjectType::DeviceRegion};
    const ObjectId firstId = Register(issuer, first);
    const ObjectId secondId = Register(issuer, second);
    const ObjectId otherId = Register(issuer, other);
    const ObjectExclusiveRange range{0x1000U, 0x3000U};
    ASTERIA_EXPECT(runner, "claims validate their range, type, and exact live identity", firstId.IsValid(),
                   secondId.IsValid(), otherId.IsValid(),
                   registry.ClaimExclusiveRange(firstId, ObjectType::Frame, {0x2000U, 0x2000U}) ==
                       ObjectRegistryError::InvalidArgument,
                   registry.ClaimExclusiveRange(firstId, static_cast<ObjectType>(0xFFU), range) ==
                       ObjectRegistryError::InvalidArgument,
                   registry.ClaimExclusiveRange(firstId, ObjectType::DeviceRegion, range) ==
                       ObjectRegistryError::InvalidObject,
                   registry.ClaimExclusiveRange(ObjectId{firstId.GetValue() + (Foundation::UInt64{1U} << 16U)},
                                                ObjectType::Frame, range) == ObjectRegistryError::InvalidObject,
                   registry.ClaimExclusiveRange(ObjectId{Foundation::UInt64{1U} << 16U}, ObjectType::Frame, range) ==
                       ObjectRegistryError::InvalidObject,
                   registry.ReleaseExclusiveRange(firstId) == ObjectRegistryError::InvalidArgument);
    ASTERIA_EXPECT(runner, "an object holds one claim, and only its own type is excluded",
                   registry.ClaimExclusiveRange(firstId, ObjectType::Frame, range) == ObjectRegistryError::Ok,
                   registry.ClaimExclusiveRange(firstId, ObjectType::Frame, {0x8000U, 0x9000U}) ==
                       ObjectRegistryError::InvalidArgument,
                   registry.ClaimExclusiveRange(otherId, ObjectType::DeviceRegion, range) == ObjectRegistryError::Ok,
                   registry.ClaimExclusiveRange(secondId, ObjectType::Frame, {0x2000U, 0x2800U}) ==
                       ObjectRegistryError::ExclusiveRangeConflict);
    ASTERIA_EXPECT(
        runner, "a released claim frees its range", registry.ReleaseExclusiveRange(firstId) == ObjectRegistryError::Ok,
        registry.ClaimExclusiveRange(secondId, ObjectType::Frame, {0x2000U, 0x2800U}) == ObjectRegistryError::Ok,
        registry.ReleaseExclusiveRange(ObjectId{}) == ObjectRegistryError::InvalidObject);
    ASTERIA_EXPECT(runner, "collection ends a claim with its object",
                   issuer.DestroyObject(secondId) == ObjectRegistryError::Ok,
                   issuer.CollectDestroyedObject().HasValue(),
                   registry.ClaimExclusiveRange(firstId, ObjectType::Frame, range) == ObjectRegistryError::Ok,
                   issuer.DestroyObject(firstId) == ObjectRegistryError::Ok,
                   issuer.DestroyObject(otherId) == ObjectRegistryError::Ok, issuer.CollectDestroyedObject().HasValue(),
                   issuer.CollectDestroyedObject().HasValue());
}
// Each owner collects its own object: an exact collection skips other
// collectable objects, refuses a live, referenced, stale, or unknown
// identity, and collects each object once.
void TestExactCollection(TestContext& runner) noexcept
{
    SpinLock synchronization;
    Asteria::TestSupport::TestObjectRegistry registry{synchronization};
    RootCapabilityIssuer issuer{registry};
    TestKernelObject first{ObjectType::Endpoint};
    TestKernelObject second{ObjectType::FaultToken};
    TestKernelObject live{ObjectType::Notification};
    const ObjectId firstId = Register(issuer, first);
    const ObjectId secondId = Register(issuer, second);
    const ObjectId liveId = Register(issuer, live);
    const bool destroyed = (issuer.DestroyObject(firstId) == ObjectRegistryError::Ok) &&
                           (issuer.DestroyObject(secondId) == ObjectRegistryError::Ok);
    const auto secondRecord = issuer.CollectDestroyedObject(secondId);
    ASTERIA_EXPECT(runner, "an exact collection returns that object and leaves an earlier collectable one",
                   firstId.IsValid(), secondId.IsValid(), liveId.IsValid(), destroyed, secondRecord.HasValue(),
                   secondRecord.HasValue() && (secondRecord.GetValue().Instance == &second),
                   secondRecord.HasValue() && (secondRecord.GetValue().Object.GetValue() == secondId.GetValue()),
                   secondRecord.HasValue() && secondRecord.GetValue().IsFromRegistry(registry));
    const auto again = issuer.CollectDestroyedObject(secondId);
    const auto alive = issuer.CollectDestroyedObject(liveId);
    const auto unknown = issuer.CollectDestroyedObject(ObjectId{});
    const auto stale = issuer.CollectDestroyedObject(ObjectId{firstId.GetValue() + (Foundation::UInt64{1U} << 16U)});
    ASTERIA_EXPECT(runner, "a collected, live, unknown, or stale identity is not collectable",
                   !again.HasValue() && (again.GetError() == ObjectRegistryError::InvalidObject),
                   !alive.HasValue() && (alive.GetError() == ObjectRegistryError::InvalidObject),
                   !unknown.HasValue() && (unknown.GetError() == ObjectRegistryError::InvalidObject),
                   !stale.HasValue() && (stale.GetError() == ObjectRegistryError::InvalidObject));

    // A quiescing object stays referenced by a pin until the pin resets.
    Kernel::Object::ObjectPin pin;
    const bool pinned = registry.PinObject(liveId, ObjectType::Notification, pin) == Kernel::Object::ObjectPinError::Ok;
    const bool liveDestroyed = issuer.DestroyObject(liveId) == ObjectRegistryError::Ok;
    const auto referenced = issuer.CollectDestroyedObject(liveId);
    pin.Reset();
    const auto released = issuer.CollectDestroyedObject(liveId);
    const auto firstRecord = issuer.CollectDestroyedObject(firstId);
    ASTERIA_EXPECT(runner, "a referenced object waits for its last pin, then each owner collects its own", pinned,
                   liveDestroyed,
                   !referenced.HasValue() && (referenced.GetError() == ObjectRegistryError::NoDestructionReady),
                   released.HasValue() && (released.GetValue().Instance == &live),
                   firstRecord.HasValue() && (firstRecord.GetValue().Instance == &first),
                   !issuer.CollectDestroyedObject().HasValue());
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"ObjectRegistryTests"};
    Asteria::Test::TestClaimValidation(runner);
    Asteria::Test::TestExactCollection(runner);
    return runner.Finish();
}
