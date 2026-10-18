// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <type_traits>

#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Kernel::Object::IKernelObject;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPin;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Platform::Synchronization::SpinLock;

static_assert(std::is_move_constructible_v<ObjectPin>);
static_assert(!std::is_move_assignable_v<ObjectPin>);
static_assert(!std::is_copy_constructible_v<ObjectPin>);
static_assert(!std::is_copy_assignable_v<ObjectPin>);

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

class ObjectFixture final
{
public:
    constexpr ObjectFixture() noexcept : m_registry{m_synchronization}, m_issuer{m_registry}
    {
    }

    [[nodiscard]] ObjectRegistry& GetRegistry() noexcept
    {
        return m_registry;
    }

    [[nodiscard]] RootCapabilityIssuer& GetIssuer() noexcept
    {
        return m_issuer;
    }

private:
    SpinLock m_synchronization;
    Asteria::TestSupport::TestObjectRegistry m_registry;
    RootCapabilityIssuer m_issuer;
};

[[nodiscard]] ObjectId Register(ObjectFixture& fixture, IKernelObject& object, TestContext& runner,
                                const char* const description) noexcept
{
    const auto result = fixture.GetIssuer().CreateObject(object);
    ASTERIA_EXPECT(runner, description, result.HasValue());
    if (!result.HasValue())
    {
        return ObjectId{};
    }
    return result.GetValue();
}

void TestValidationAndFailureAtomicity(TestContext& runner) noexcept
{
    ObjectFixture fixture;
    TestKernelObject endpoint{ObjectType::Endpoint, ResourceDomainId{7U}};
    TestKernelObject thread{ObjectType::Thread, ResourceDomainId{8U}};
    const ObjectId endpointId = Register(fixture, endpoint, runner, "validation fixture registers an endpoint");
    const ObjectId threadId = Register(fixture, thread, runner, "validation fixture registers a thread");
    if (!endpointId.IsValid() || !threadId.IsValid())
    {
        return;
    }

    ObjectPin output;
    ASTERIA_EXPECT(runner, "zero object identity is rejected without publishing a pin",
                   fixture.GetRegistry().PinObject(ObjectId{}, ObjectType::Endpoint, output) ==
                       ObjectPinError::InvalidArgument,
                   !output.IsValid());
    ASTERIA_EXPECT(runner, "nonzero identity with slot zero is rejected without indexing the registry",
                   fixture.GetRegistry().PinObject(ObjectId{Foundation::UInt64{1U} << 16U}, ObjectType::Endpoint,
                                                   output) == ObjectPinError::ObjectNotFound,
                   !output.IsValid());
    ASTERIA_EXPECT(runner, "nonzero identity with generation zero is rejected without indexing the registry",
                   fixture.GetRegistry().PinObject(ObjectId{1U}, ObjectType::Endpoint, output) ==
                       ObjectPinError::ObjectNotFound,
                   !output.IsValid());
    ASTERIA_EXPECT(runner, "identity beyond the fixed slot range is rejected without indexing the registry",
                   fixture.GetRegistry().PinObject(ObjectId{(Foundation::UInt64{1U} << 16U) | 257U},
                                                   ObjectType::Endpoint, output) == ObjectPinError::ObjectNotFound,
                   !output.IsValid());
    ASTERIA_EXPECT(runner, "invalid expected type is rejected without publishing a pin",
                   fixture.GetRegistry().PinObject(endpointId, ObjectType::Invalid, output) ==
                       ObjectPinError::InvalidArgument,
                   !output.IsValid());
    ASTERIA_EXPECT(runner, "unknown expected type is rejected without publishing a pin",
                   fixture.GetRegistry().PinObject(endpointId, static_cast<ObjectType>(0xFFU), output) ==
                       ObjectPinError::InvalidArgument,
                   !output.IsValid());
    ASTERIA_EXPECT(runner, "type mismatch is explicit and failure-atomic",
                   fixture.GetRegistry().PinObject(endpointId, ObjectType::Thread, output) ==
                       ObjectPinError::TypeMismatch,
                   !output.IsValid());

    ASTERIA_EXPECT(runner, "matching object and type acquire a persistent pin",
                   fixture.GetRegistry().PinObject(endpointId, ObjectType::Endpoint, output) == ObjectPinError::Ok);
    ASTERIA_EXPECT(runner, "pin retains the exact generation, type, and object instance", output.IsValid(),
                   output.GetObjectId().GetValue() == endpointId.GetValue(),
                   output.GetObjectType() == ObjectType::Endpoint, &output.GetObject() == &endpoint);

    ASTERIA_EXPECT(runner, "occupied output rejects replacement instead of releasing its reference",
                   fixture.GetRegistry().PinObject(threadId, ObjectType::Thread, output) ==
                       ObjectPinError::OutputOccupied);
    ASTERIA_EXPECT(runner, "occupied-output failure preserves the original pin", output.IsValid(),
                   output.GetObjectId().GetValue() == endpointId.GetValue(), &output.GetObject() == &endpoint);

    output.Reset();
    ASTERIA_EXPECT(runner, "reset returns a pin to its empty state", !output.IsValid());
    ASTERIA_EXPECT(runner, "validation fixture quiesces both registered objects",
                   fixture.GetIssuer().DestroyObject(endpointId) == ObjectRegistryError::Ok,
                   fixture.GetIssuer().DestroyObject(threadId) == ObjectRegistryError::Ok);
    const auto firstCollection = fixture.GetIssuer().CollectDestroyedObject();
    const auto secondCollection = fixture.GetIssuer().CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "validation fixture collects both unpinned objects", firstCollection.HasValue(),
                   secondCollection.HasValue());
}

void TestDestructionAndGeneration(TestContext& runner) noexcept
{
    ObjectFixture fixture;
    TestKernelObject first{ObjectType::Thread, ResourceDomainId{11U}};
    TestKernelObject replacement{ObjectType::Thread, ResourceDomainId{12U}};
    const ObjectId firstId = Register(fixture, first, runner, "lifetime fixture registers its first object");
    if (!firstId.IsValid())
    {
        return;
    }

    ObjectPin pin;
    ASTERIA_EXPECT(runner, "live object can be pinned before destruction",
                   fixture.GetRegistry().PinObject(firstId, ObjectType::Thread, pin) == ObjectPinError::Ok);
    ASTERIA_EXPECT(runner, "destruction moves a pinned object into quiescing",
                   fixture.GetIssuer().DestroyObject(firstId) == ObjectRegistryError::Ok);

    ObjectPin rejected;
    ASTERIA_EXPECT(runner, "quiescing generation rejects every new persistent pin",
                   fixture.GetRegistry().PinObject(firstId, ObjectType::Thread, rejected) ==
                       ObjectPinError::ObjectNotFound,
                   !rejected.IsValid());
    const auto prematureCollection = fixture.GetIssuer().CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "quiescing object remains unavailable while its pin is held",
                   !prematureCollection.HasValue(),
                   prematureCollection.GetError() == ObjectRegistryError::NoDestructionReady);
    ASTERIA_EXPECT(runner, "existing pin remains usable throughout quiescing", pin.IsValid(),
                   &pin.GetObject() == &first);

    ObjectPin moved{static_cast<ObjectPin&&>(pin)};
    ASTERIA_EXPECT(runner, "move construction transfers exactly one registry reference", !pin.IsValid(),
                   moved.IsValid(), moved.GetObjectId().GetValue() == firstId.GetValue(),
                   moved.GetObjectType() == ObjectType::Thread, &moved.GetObject() == &first);
    moved.Reset();
    const auto collected = fixture.GetIssuer().CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "last pin release makes the exact generation collectable", collected.HasValue(),
                   collected.GetValue().Object.GetValue() == firstId.GetValue(),
                   collected.GetValue().Instance == &first);

    const ObjectId replacementId =
        Register(fixture, replacement, runner, "collected registry slot accepts a replacement object");
    ASTERIA_EXPECT(runner, "slot reuse advances the object generation", replacementId.IsValid(),
                   replacementId.GetValue() != firstId.GetValue());
    ASTERIA_EXPECT(runner, "stale generation cannot pin the replacement object",
                   fixture.GetRegistry().PinObject(firstId, ObjectType::Thread, rejected) ==
                       ObjectPinError::ObjectNotFound,
                   !rejected.IsValid());
    ASTERIA_EXPECT(runner, "new generation pins only the replacement instance",
                   fixture.GetRegistry().PinObject(replacementId, ObjectType::Thread, rejected) == ObjectPinError::Ok,
                   &rejected.GetObject() == &replacement);
    ASTERIA_EXPECT(runner, "replacement object can enter quiescing while pinned",
                   fixture.GetIssuer().DestroyObject(replacementId) == ObjectRegistryError::Ok);
    rejected.Reset();
    const auto replacementCollection = fixture.GetIssuer().CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "replacement cleanup observes the new generation after pin release",
                   replacementCollection.HasValue(),
                   replacementCollection.GetValue().Object.GetValue() == replacementId.GetValue(),
                   replacementCollection.GetValue().Instance == &replacement);
}

void TestMultipleReferences(TestContext& runner) noexcept
{
    ObjectFixture fixture;
    TestKernelObject first{ObjectType::Endpoint, ResourceDomainId{21U}};
    TestKernelObject second{ObjectType::Thread, ResourceDomainId{22U}};
    const ObjectId firstId = Register(fixture, first, runner, "multi-reference fixture registers its first object");
    const ObjectId secondId = Register(fixture, second, runner, "multi-reference fixture registers its second object");
    if (!firstId.IsValid() || !secondId.IsValid())
    {
        return;
    }

    ObjectPin firstPin;
    ObjectPin secondPin;
    ASTERIA_EXPECT(runner, "independent objects acquire independent pins",
                   fixture.GetRegistry().PinObject(firstId, ObjectType::Endpoint, firstPin) == ObjectPinError::Ok,
                   fixture.GetRegistry().PinObject(secondId, ObjectType::Thread, secondPin) == ObjectPinError::Ok);
    ASTERIA_EXPECT(runner, "both pinned objects can enter quiescing",
                   fixture.GetIssuer().DestroyObject(firstId) == ObjectRegistryError::Ok,
                   fixture.GetIssuer().DestroyObject(secondId) == ObjectRegistryError::Ok);

    firstPin.Reset();
    const auto firstCollection = fixture.GetIssuer().CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "reset makes only the released object collectable", firstCollection.HasValue(),
                   firstCollection.GetValue().Object.GetValue() == firstId.GetValue(),
                   firstCollection.GetValue().Instance == &first);
    const auto secondPrematureCollection = fixture.GetIssuer().CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "independent pin continues to delay its object's collection",
                   !secondPrematureCollection.HasValue(),
                   secondPrematureCollection.GetError() == ObjectRegistryError::NoDestructionReady);
    secondPin.Reset();
    const auto secondCollection = fixture.GetIssuer().CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "reset releases the second object's final reference", secondCollection.HasValue(),
                   secondCollection.GetValue().Object.GetValue() == secondId.GetValue(),
                   secondCollection.GetValue().Instance == &second);

    TestKernelObject shared{ObjectType::Notification, ResourceDomainId{23U}};
    const ObjectId sharedId = Register(fixture, shared, runner, "same-object fixture registers a shared object");
    ObjectPin sharedFirst;
    ObjectPin sharedSecond;
    ASTERIA_EXPECT(
        runner, "one object may hold multiple persistent references", sharedId.IsValid(),
        fixture.GetRegistry().PinObject(sharedId, ObjectType::Notification, sharedFirst) == ObjectPinError::Ok,
        fixture.GetRegistry().PinObject(sharedId, ObjectType::Notification, sharedSecond) == ObjectPinError::Ok);
    ASTERIA_EXPECT(runner, "multiply pinned object enters quiescing",
                   fixture.GetIssuer().DestroyObject(sharedId) == ObjectRegistryError::Ok);
    sharedFirst.Reset();
    const auto sharedPrematureCollection = fixture.GetIssuer().CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "releasing one of two pins does not permit collection",
                   !sharedPrematureCollection.HasValue(),
                   sharedPrematureCollection.GetError() == ObjectRegistryError::NoDestructionReady);
    sharedSecond.Reset();
    const auto sharedCollection = fixture.GetIssuer().CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "collection begins only after every persistent pin releases", sharedCollection.HasValue(),
                   sharedCollection.GetValue().Object.GetValue() == sharedId.GetValue(),
                   sharedCollection.GetValue().Instance == &shared);
}

void TestDestructorRelease(TestContext& runner) noexcept
{
    ObjectFixture fixture;
    TestKernelObject object{ObjectType::Timer, ResourceDomainId{31U}};
    const ObjectId objectId = Register(fixture, object, runner, "destructor fixture registers its object");
    if (!objectId.IsValid())
    {
        return;
    }

    {
        ObjectPin scoped;
        ASTERIA_EXPECT(runner, "scoped pin acquires one persistent reference",
                       fixture.GetRegistry().PinObject(objectId, ObjectType::Timer, scoped) == ObjectPinError::Ok);
        ASTERIA_EXPECT(runner, "scoped pin keeps a quiescing object alive",
                       fixture.GetIssuer().DestroyObject(objectId) == ObjectRegistryError::Ok);
        const auto prematureCollection = fixture.GetIssuer().CollectDestroyedObject();
        ASTERIA_EXPECT(runner, "scoped pin prevents collection before its destructor runs",
                       !prematureCollection.HasValue(),
                       prematureCollection.GetError() == ObjectRegistryError::NoDestructionReady);
    }

    const auto collected = fixture.GetIssuer().CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "ObjectPin destructor releases its final reference", collected.HasValue(),
                   collected.GetValue().Object.GetValue() == objectId.GetValue(),
                   collected.GetValue().Instance == &object);
}

void TestConfiguredReferenceCapacity(TestContext& runner) noexcept
{
    SpinLock synchronization;
    Asteria::TestSupport::TestObjectRegistry registry{synchronization, 2U};
    RootCapabilityIssuer issuer{registry};
    TestKernelObject object{ObjectType::Endpoint, ResourceDomainId{41U}};
    const auto registered = issuer.CreateObject(object);
    if (!registered.HasValue())
    {
        ASTERIA_EXPECT(runner, "bounded-reference fixture registers its object", false);
        return;
    }

    ObjectPin first;
    ObjectPin second;
    ObjectPin rejected;
    const ObjectPinError firstError = registry.PinObject(registered.GetValue(), ObjectType::Endpoint, first);
    const ObjectPinError secondError = registry.PinObject(registered.GetValue(), ObjectType::Endpoint, second);
    const ObjectPinError exhausted = registry.PinObject(registered.GetValue(), ObjectType::Endpoint, rejected);
    ASTERIA_EXPECT(runner, "configured reference capacity rejects acquisition atomically",
                   firstError == ObjectPinError::Ok, secondError == ObjectPinError::Ok,
                   exhausted == ObjectPinError::ReferenceExhausted, !rejected.IsValid());

    first.Reset();
    const ObjectPinError retried = registry.PinObject(registered.GetValue(), ObjectType::Endpoint, rejected);
    ASTERIA_EXPECT(runner, "releasing capacity permits a later reference acquisition", retried == ObjectPinError::Ok,
                   rejected.IsValid(), &rejected.GetObject() == &object);

    second.Reset();
    rejected.Reset();
    const ObjectRegistryError destroyed = issuer.DestroyObject(registered.GetValue());
    const auto collected = issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "bounded-reference object retires after every acquired pin releases",
                   destroyed == ObjectRegistryError::Ok, collected.HasValue(),
                   collected.GetValue().Instance == &object);
}

} // namespace Asteria::Test

int main()
{
    TestContext runner{"ObjectPinTests"};
    Asteria::Test::TestValidationAndFailureAtomicity(runner);
    Asteria::Test::TestDestructionAndGeneration(runner);
    Asteria::Test::TestMultipleReferences(runner);
    Asteria::Test::TestDestructorRelease(runner);
    Asteria::Test::TestConfiguredReferenceCapacity(runner);
    return runner.Finish();
}
