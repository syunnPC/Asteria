// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::UInt64;
using Hal::Memory::AddressSpacePageTableActivationError;
using Hal::Memory::AddressSpacePageTableError;
using Hal::Memory::AddressSpacePageTableMapResult;
using Hal::Memory::AddressSpacePageTableOwnershipError;
using Hal::Memory::IAddressSpacePageTable;
using Kernel::Memory::AddressSpace;
using Kernel::Memory::AddressSpaceError;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::InitialMapping;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::InitialMappingPlan;
using Kernel::Memory::InitialMappingPlanError;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MappingPermissions;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::VirtualAddress;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::IKernelObject;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationRegistry;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Protection::ProtectionDomainBinding;
using Kernel::Protection::ProtectionDomain;
using Kernel::Protection::ProtectionDomainError;
using Kernel::Protection::ProtectionDomainId;
using Kernel::Protection::ProtectionDomainKind;
using Kernel::Protection::ProtectionDomainSnapshot;
using Kernel::Protection::ProtectionDomainState;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
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

class TestAddressSpacePageTable final : public IAddressSpacePageTable
{
public:
    [[nodiscard]] bool IsReady() const noexcept override
    {
        return true;
    }

    [[nodiscard]] AddressSpacePageTableOwnershipError Claim(const AddressSpace& owner,
                                                            const InitialMappingPlan&) noexcept override
    {
        if (m_owner != nullptr)
        {
            return AddressSpacePageTableOwnershipError::AlreadyOwned;
        }
        m_owner = &owner;
        return AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] AddressSpacePageTableOwnershipError Release(const AddressSpace& owner) noexcept override
    {
        if (m_owner != &owner)
        {
            return AddressSpacePageTableOwnershipError::InvalidOwner;
        }
        m_owner = nullptr;
        return AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] AddressSpacePageTableActivationError Activate(const AddressSpace& owner,
                                                                Kernel::Scheduler::CpuId processor) noexcept override
    {
        if ((m_owner != &owner) || !processor.IsValid())
        {
            return AddressSpacePageTableActivationError::InvalidOwner;
        }
        return AddressSpacePageTableActivationError::UnsupportedConfiguration;
    }

    [[nodiscard]] AddressSpacePageTableError Unmap(const AddressSpaceMappingRecord&,
                                                   AddressSpacePageTableMapResult&) noexcept override
    {
        return AddressSpacePageTableError::UnsupportedConfiguration;
    }

    [[nodiscard]] AddressSpacePageTableError Map(const AddressSpaceMappingRecord&,
                                                 AddressSpacePageTableMapResult&) noexcept override
    {
        return AddressSpacePageTableError::InternalFailure;
    }

private:
    const AddressSpace* m_owner{nullptr};
};

[[nodiscard]] constexpr MappingPermissions ReadWrite() noexcept
{
    return MappingPermissions{MappingPermission::Read}.With(MappingPermission::Write);
}

[[nodiscard]] constexpr MappingPermissions ReadExecute() noexcept
{
    return MappingPermissions{MappingPermission::Read}.With(MappingPermission::Execute);
}

[[nodiscard]] constexpr InitialMapping MakeSeedMapping(const UInt64 virtualStart, const UInt64 physicalStart,
                                                       const UInt64 pages, const MappingPermissions permissions,
                                                       const InitialMappingOwner owner) noexcept
{
    return InitialMapping{
        .VirtualStart = VirtualAddress{virtualStart},
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Pages = PageCount{pages},
        .Permissions = permissions,
        .CachePolicy = MemoryCachePolicy::WriteBack,
        .Owner = owner,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool BuildSeedPlan(InitialMappingPlan& plan) noexcept
{
    constexpr UInt64 base{0xFFFF'8000'0000'0000ULL};
    return (plan.AddMapping(MakeSeedMapping(base + 0x1000U, 0x0010'0000U, 2U, ReadExecute(),
                                            InitialMappingOwner::NucleusCode)) == InitialMappingPlanError::None) &&
           (plan.AddMapping(MakeSeedMapping(base + 0x4000U, 0x0010'4000U, 4U, ReadWrite(),
                                            InitialMappingOwner::NucleusStack)) == InitialMappingPlanError::None) &&
           (plan.AddMapping(MakeSeedMapping(base + 0x8000U, 0x0010'8000U, 16U, ReadWrite(),
                                            InitialMappingOwner::PageTable)) == InitialMappingPlanError::None) &&
           (plan.Seal() == InitialMappingPlanError::None);
}

class RegisteredAddressSpace final
{
private:
    ObjectRegistry& m_registry;
    RootCapabilityIssuer& m_issuer;
    TestAddressSpacePageTable m_pageTable;
    InitialMappingPlan m_seedPlan;

public:
    RegisteredAddressSpace(ObjectRegistry& registry, RootCapabilityIssuer& issuer) noexcept
        : m_registry{registry}, m_issuer{issuer}
    {
        if (!BuildSeedPlan(m_seedPlan) ||
            (Storage.Initialize(ResourceDomainId{7U}, m_pageTable, m_seedPlan) != AddressSpaceError::None))
        {
            return;
        }
        const auto object = m_issuer.CreateObject(Storage);
        if (!object.HasValue())
        {
            return;
        }
        Object = object.GetValue();
        Identity = AddressSpaceId{m_registry, Object};
        Ready = Storage.BindIdentity(m_registry, Identity).Error == AddressSpaceError::None;
    }

    ~RegisteredAddressSpace() noexcept
    {
        const auto state = Storage.GetSnapshot().State;
        if ((state != Kernel::Memory::AddressSpaceState::Uninitialized) &&
            (state != Kernel::Memory::AddressSpaceState::Quiesced))
        {
            static_cast<void>(Storage.Quiesce());
        }
        if (Object.IsValid())
        {
            static_cast<void>(m_issuer.DestroyObject(Object));
            static_cast<void>(m_issuer.CollectDestroyedObject());
        }
    }

    RegisteredAddressSpace(const RegisteredAddressSpace&) = delete;
    RegisteredAddressSpace& operator=(const RegisteredAddressSpace&) = delete;

    AddressSpace Storage;
    ObjectId Object{};
    AddressSpaceId Identity{};
    bool Ready{false};
};

class RegistryFixture final
{
public:
    RegistryFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          Capabilities{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL}
    {
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace Capabilities;
};

[[nodiscard]] constexpr ResourceDomainLimits ResourceLimits() noexcept
{
    return ResourceDomainLimits{
        .CommittedMemoryPages = 4096U,
        .ResidentMemoryPages = 4096U,
        .KernelMetadataBytes = 64U * 1024U,
        .CapabilityHandles = 4096U,
        .IpcQueueBytes = 64U * 1024U,
        .Threads = 256U,
        .ChildDomains = 16U,
    };
}

[[nodiscard]] bool DestroyAndCollect(RootCapabilityIssuer& issuer, const ObjectId object,
                                     IKernelObject& expected) noexcept
{
    if (issuer.DestroyObject(object) != ObjectRegistryError::Ok)
    {
        return false;
    }
    const auto collected = issuer.CollectDestroyedObject();
    return collected.HasValue() && (collected.GetValue().Object.GetValue() == object.GetValue()) &&
           (collected.GetValue().Instance == &expected);
}

[[nodiscard]] bool SameBinding(const ProtectionDomainBinding& left, const ProtectionDomainBinding& right) noexcept
{
    return left.Identity.IsEqualTo(right.Identity) && left.AddressSpace.IsEqualTo(right.AddressSpace) &&
           (left.AddressSpaceStorage == right.AddressSpaceStorage) && (left.Capabilities == right.Capabilities) &&
           (left.CapabilitySpace.GetValue() == right.CapabilitySpace.GetValue()) &&
           left.ResourceDomainIdentity.IsEqualTo(right.ResourceDomainIdentity) &&
           (left.ResourceDomainStorage == right.ResourceDomainStorage) &&
           (left.ResourceDomain.GetValue() == right.ResourceDomain.GetValue());
}

[[nodiscard]] bool SameSnapshot(const ProtectionDomainSnapshot& left, const ProtectionDomainSnapshot& right) noexcept
{
    return left.Identity.IsEqualTo(right.Identity) && left.AddressSpace.IsEqualTo(right.AddressSpace) &&
           (left.CapabilitySpace.GetValue() == right.CapabilitySpace.GetValue()) &&
           left.ResourceDomainIdentity.IsEqualTo(right.ResourceDomainIdentity) &&
           (left.ResourceDomain.GetValue() == right.ResourceDomain.GetValue()) && (left.Kind == right.Kind) &&
           (left.State == right.State);
}

void TestBindingAndLifetime(TestContext& runner) noexcept
{
    RegistryFixture fixture;
    ResourceDomainLedger resourceLedger;
    ResourceDomain resourceDomain{resourceLedger};
    RegisteredAddressSpace addressSpace{fixture.Registry, fixture.Issuer};
    ProtectionDomain domain;
    const bool resourceInitialized =
        resourceDomain.InitializeRoot(ResourceDomainId{9U}, ResourceLimits()).Error == ResourceDomainError::None;
    const auto resource = fixture.Issuer.CreateObject(resourceDomain);
    const ResourceDomainObjectId resourceIdentity =
        resource.HasValue() ? ResourceDomainObjectId{fixture.Registry, resource.GetValue()} : ResourceDomainObjectId{};
    const auto initialized = domain.Initialize(fixture.Registry, resourceIdentity, resourceDomain,
                                               addressSpace.Identity, fixture.Capabilities);
    const auto registered = fixture.Issuer.CreateObject(domain);
    const ProtectionDomainId identity =
        registered.HasValue() ? ProtectionDomainId{fixture.Registry, registered.GetValue()} : ProtectionDomainId{};
    const auto bound = domain.BindIdentity(fixture.Registry, identity);
    const auto bindingBeforeRepeatedInitialization = domain.GetBinding();
    const ProtectionDomainSnapshot snapshotBeforeRepeatedInitialization = domain.GetSnapshot();
    const UInt64 reservationBeforeRepeatedInitialization = resourceDomain.GetSnapshot().Usage.KernelMetadataBytes;
    const auto repeated = domain.Initialize(fixture.Registry, resourceIdentity, resourceDomain, addressSpace.Identity,
                                            fixture.Capabilities);
    const auto crossKindRepeated =
        domain.InitializeKernelOnly(fixture.Registry, resourceIdentity, resourceDomain, addressSpace.Identity);
    const auto binding = domain.GetBinding();
    const auto snapshot = domain.GetSnapshot();
    const auto resourceSnapshot = resourceDomain.GetSnapshot();

    ASTERIA_EXPECT(runner, "repeated initialization preserves a Ready user-capable domain",
                   repeated.Error == ProtectionDomainError::AlreadyInitialized,
                   crossKindRepeated.Error == ProtectionDomainError::AlreadyInitialized,
                   bindingBeforeRepeatedInitialization.HasValue(), binding.HasValue(),
                   SameBinding(bindingBeforeRepeatedInitialization.GetValue(), binding.GetValue()),
                   SameSnapshot(snapshotBeforeRepeatedInitialization, snapshot),
                   reservationBeforeRepeatedInitialization == sizeof(ProtectionDomain),
                   resourceSnapshot.Usage.KernelMetadataBytes == reservationBeforeRepeatedInitialization);

    ASTERIA_EXPECT(
        runner, "a domain publishes one exact immutable execution binding", resourceInitialized, resource.HasValue(),
        addressSpace.Ready, initialized.Error == ProtectionDomainError::None, registered.HasValue(),
        bound.Error == ProtectionDomainError::None, binding.HasValue(), binding.GetValue().Identity.IsEqualTo(identity),
        binding.GetValue().AddressSpace.IsEqualTo(addressSpace.Identity),
        binding.GetValue().AddressSpaceStorage == &addressSpace.Storage,
        binding.GetValue().Capabilities == &fixture.Capabilities,
        binding.GetValue().CapabilitySpace.GetValue() == fixture.Capabilities.GetIdentifier().GetValue(),
        binding.GetValue().ResourceDomainIdentity.IsEqualTo(resourceIdentity),
        binding.GetValue().ResourceDomainStorage == &resourceDomain, binding.GetValue().ResourceDomain.GetValue() == 9U,
        snapshot.ResourceDomainIdentity.IsEqualTo(resourceIdentity), snapshot.Identity.IsEqualTo(identity),
        snapshot.Kind == ProtectionDomainKind::UserCapable, snapshot.State == ProtectionDomainState::Ready,
        resourceSnapshot.Usage.KernelMetadataBytes == sizeof(ProtectionDomain),
        domain.GetObjectType() == ObjectType::ProtectionDomain, domain.GetChargeOwner().GetValue() == 9U);

    const bool destroyAddress =
        addressSpace.Object.IsValid() && (fixture.Issuer.DestroyObject(addressSpace.Object) == ObjectRegistryError::Ok);
    const auto held = fixture.Issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "repeated initialization retains the exact AddressSpace pin", destroyAddress,
                   !held.HasValue(), held.GetError() == ObjectRegistryError::NoDestructionReady);

    const bool quiesced = domain.Quiesce() == ProtectionDomainError::None;
    const bool addressQuiesced = addressSpace.Storage.Quiesce() == AddressSpaceError::None;
    const auto addressRecord = fixture.Issuer.CollectDestroyedObject();
    const bool resourceReleased = (resourceDomain.GetSnapshot().Usage.KernelMetadataBytes == 0U) &&
                                  (resourceDomain.Quiesce() == ResourceDomainError::None);
    const bool domainCollected =
        registered.HasValue() && DestroyAndCollect(fixture.Issuer, registered.GetValue(), domain);
    const bool resourceCollected =
        resource.HasValue() && DestroyAndCollect(fixture.Issuer, resource.GetValue(), resourceDomain);
    ASTERIA_EXPECT(runner, "Quiesce releases pins, reservation, and publication identity", quiesced, addressQuiesced,
                   addressRecord.HasValue(), addressRecord.GetValue().Instance == &addressSpace.Storage,
                   domainCollected, domain.GetChargeOwner().GetValue() == 0U,
                   !fixture.Issuer.CreateObject(domain).HasValue(), resourceReleased, resourceCollected,
                   domain.GetSnapshot().Kind == ProtectionDomainKind::Uninitialized,
                   domain.GetSnapshot().State == ProtectionDomainState::Quiesced);
}

void TestKernelOnlyBinding(TestContext& runner) noexcept
{
    RegistryFixture fixture;
    ResourceDomainLedger resourceLedger;
    ResourceDomain resourceDomain{resourceLedger};
    RegisteredAddressSpace addressSpace{fixture.Registry, fixture.Issuer};
    ProtectionDomain domain;
    const bool resourceInitialized =
        resourceDomain.InitializeRoot(ResourceDomainId{9U}, ResourceLimits()).Error == ResourceDomainError::None;
    const auto resource = fixture.Issuer.CreateObject(resourceDomain);
    const ResourceDomainObjectId resourceIdentity =
        resource.HasValue() ? ResourceDomainObjectId{fixture.Registry, resource.GetValue()} : ResourceDomainObjectId{};
    const auto initialized =
        domain.InitializeKernelOnly(fixture.Registry, resourceIdentity, resourceDomain, addressSpace.Identity);
    const auto registered = fixture.Issuer.CreateObject(domain);
    const ProtectionDomainId identity =
        registered.HasValue() ? ProtectionDomainId{fixture.Registry, registered.GetValue()} : ProtectionDomainId{};
    const auto bound = domain.BindIdentity(fixture.Registry, identity);
    const auto bindingBeforeRepeatedInitialization = domain.GetBinding();
    const ProtectionDomainSnapshot snapshotBeforeRepeatedInitialization = domain.GetSnapshot();
    const UInt64 reservationBeforeRepeatedInitialization = resourceDomain.GetSnapshot().Usage.KernelMetadataBytes;
    const auto repeated =
        domain.InitializeKernelOnly(fixture.Registry, resourceIdentity, resourceDomain, addressSpace.Identity);
    const auto crossKindRepeated = domain.Initialize(fixture.Registry, resourceIdentity, resourceDomain,
                                                     addressSpace.Identity, fixture.Capabilities);
    const auto binding = domain.GetBinding();
    const ProtectionDomainSnapshot snapshot = domain.GetSnapshot();

    ASTERIA_EXPECT(runner, "repeated initialization preserves a Ready kernel-only domain",
                   repeated.Error == ProtectionDomainError::AlreadyInitialized,
                   crossKindRepeated.Error == ProtectionDomainError::AlreadyInitialized,
                   bindingBeforeRepeatedInitialization.HasValue(), binding.HasValue(),
                   SameBinding(bindingBeforeRepeatedInitialization.GetValue(), binding.GetValue()),
                   SameSnapshot(snapshotBeforeRepeatedInitialization, snapshot),
                   reservationBeforeRepeatedInitialization == sizeof(ProtectionDomain),
                   resourceDomain.GetSnapshot().Usage.KernelMetadataBytes == reservationBeforeRepeatedInitialization);

    ASTERIA_EXPECT(runner, "a kernel-only domain binds real resources without syscall authority", resourceInitialized,
                   resource.HasValue(), addressSpace.Ready, initialized.Error == ProtectionDomainError::None,
                   registered.HasValue(), bound.Error == ProtectionDomainError::None, binding.HasValue(),
                   binding.GetValue().Identity.IsEqualTo(identity),
                   binding.GetValue().AddressSpace.IsEqualTo(addressSpace.Identity),
                   binding.GetValue().AddressSpaceStorage == &addressSpace.Storage,
                   binding.GetValue().Capabilities == nullptr, !binding.GetValue().CapabilitySpace.IsValid(),
                   binding.GetValue().ResourceDomainIdentity.IsEqualTo(resourceIdentity),
                   binding.GetValue().ResourceDomainStorage == &resourceDomain,
                   binding.GetValue().ResourceDomain.GetValue() == 9U,
                   snapshot.Kind == ProtectionDomainKind::KernelOnly, snapshot.State == ProtectionDomainState::Ready,
                   domain.GetChargeOwner().GetValue() == 9U,
                   resourceDomain.GetSnapshot().Usage.KernelMetadataBytes == sizeof(ProtectionDomain));

    const bool destroyResource =
        resource.HasValue() && (fixture.Issuer.DestroyObject(resource.GetValue()) == ObjectRegistryError::Ok);
    const auto heldResource = fixture.Issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "repeated initialization retains the exact ResourceDomain pin", destroyResource,
                   !heldResource.HasValue(), heldResource.GetError() == ObjectRegistryError::NoDestructionReady);

    const bool quiesced = (domain.Quiesce() == ProtectionDomainError::None) &&
                          (addressSpace.Storage.Quiesce() == AddressSpaceError::None) &&
                          (resourceDomain.Quiesce() == ResourceDomainError::None);
    const auto resourceRecord = fixture.Issuer.CollectDestroyedObject();
    const bool cleaned = quiesced && resourceRecord.HasValue() && resource.HasValue() &&
                         (resourceRecord.GetValue().Object.GetValue() == resource.GetValue().GetValue()) &&
                         (resourceRecord.GetValue().Instance == &resourceDomain) && registered.HasValue() &&
                         DestroyAndCollect(fixture.Issuer, registered.GetValue(), domain) &&
                         addressSpace.Object.IsValid() &&
                         DestroyAndCollect(fixture.Issuer, addressSpace.Object, addressSpace.Storage);
    ASTERIA_EXPECT(runner, "the kernel-only binding releases its real pins", cleaned);
}

void TestInitializationRejections(TestContext& runner) noexcept
{
    RegistryFixture local;
    RegistryFixture foreign;
    ResourceDomainLedger resourceLedger;
    ResourceDomain resourceDomain{resourceLedger};
    RegisteredAddressSpace addressSpace{local.Registry, local.Issuer};
    TestKernelObject thread{ObjectType::Thread, ResourceDomainId{7U}};
    const auto wrong = local.Issuer.CreateObject(thread);
    const bool resourceInitialized =
        resourceDomain.InitializeRoot(ResourceDomainId{9U}, ResourceLimits()).Error == ResourceDomainError::None;
    const auto resource = local.Issuer.CreateObject(resourceDomain);
    const ResourceDomainObjectId resourceIdentity =
        resource.HasValue() ? ResourceDomainObjectId{local.Registry, resource.GetValue()} : ResourceDomainObjectId{};
    const AddressSpaceId wrongId =
        wrong.HasValue() ? AddressSpaceId{local.Registry, wrong.GetValue()} : AddressSpaceId{};

    ProtectionDomain invalid;
    const auto empty = invalid.Initialize(local.Registry, ResourceDomainObjectId{}, resourceDomain,
                                          addressSpace.Identity, local.Capabilities);
    const auto foreignCapability = invalid.Initialize(local.Registry, resourceIdentity, resourceDomain,
                                                      addressSpace.Identity, foreign.Capabilities);
    const auto wrongType =
        invalid.Initialize(local.Registry, resourceIdentity, resourceDomain, wrongId, local.Capabilities);
    const auto foreignAddress =
        invalid.Initialize(local.Registry, resourceIdentity, resourceDomain,
                           AddressSpaceId{foreign.Registry, addressSpace.Object}, local.Capabilities);
    const auto foreignResource =
        invalid.Initialize(local.Registry, ResourceDomainObjectId{foreign.Registry, resource.GetValue()},
                           resourceDomain, addressSpace.Identity, local.Capabilities);
    const auto wrongResourceType =
        invalid.Initialize(local.Registry, ResourceDomainObjectId{local.Registry, wrong.GetValue()}, resourceDomain,
                           addressSpace.Identity, local.Capabilities);

    ASTERIA_EXPECT(runner, "invalid dependencies fail without partial publication", resourceInitialized,
                   resource.HasValue(), addressSpace.Ready, empty.Error == ProtectionDomainError::InvalidConfiguration,
                   foreignCapability.Error == ProtectionDomainError::InvalidConfiguration,
                   wrongType.Error == ProtectionDomainError::AddressSpacePinRejected,
                   wrongType.Pin == ObjectPinError::TypeMismatch,
                   foreignAddress.Error == ProtectionDomainError::InvalidConfiguration,
                   foreignResource.Error == ProtectionDomainError::InvalidConfiguration,
                   wrongResourceType.Error == ProtectionDomainError::ResourceDomainPinRejected,
                   wrongResourceType.Pin == ObjectPinError::TypeMismatch,
                   invalid.GetSnapshot().State == ProtectionDomainState::Uninitialized,
                   invalid.GetChargeOwner().GetValue() == 0U,
                   resourceDomain.GetSnapshot().Usage.KernelMetadataBytes == 0U);

    const bool cleaned = addressSpace.Ready && (addressSpace.Storage.Quiesce() == AddressSpaceError::None) &&
                         DestroyAndCollect(local.Issuer, addressSpace.Object, addressSpace.Storage) &&
                         wrong.HasValue() && DestroyAndCollect(local.Issuer, wrong.GetValue(), thread) &&
                         (resourceDomain.Quiesce() == ResourceDomainError::None) && resource.HasValue() &&
                         DestroyAndCollect(local.Issuer, resource.GetValue(), resourceDomain);
    ASTERIA_EXPECT(runner, "rejected dependencies acquire no lasting pin", cleaned);
}

void TestIdentityBinding(TestContext& runner) noexcept
{
    RegistryFixture local;
    RegistryFixture foreign;
    ResourceDomainLedger resourceLedger;
    ResourceDomain resourceDomain{resourceLedger};
    RegisteredAddressSpace addressSpace{local.Registry, local.Issuer};
    TestKernelObject impostor{ObjectType::ProtectionDomain, ResourceDomainId{7U}};
    ProtectionDomain domain;
    const bool resourceInitialized =
        resourceDomain.InitializeRoot(ResourceDomainId{9U}, ResourceLimits()).Error == ResourceDomainError::None;
    const auto resource = local.Issuer.CreateObject(resourceDomain);
    const ResourceDomainObjectId resourceIdentity =
        resource.HasValue() ? ResourceDomainObjectId{local.Registry, resource.GetValue()} : ResourceDomainObjectId{};
    const auto impostorObject = local.Issuer.CreateObject(impostor);
    const auto initialized =
        domain.Initialize(local.Registry, resourceIdentity, resourceDomain, addressSpace.Identity, local.Capabilities);
    const auto registered = local.Issuer.CreateObject(domain);
    const auto invalid = domain.BindIdentity(local.Registry, {});
    const auto foreignIdentity =
        registered.HasValue()
            ? domain.BindIdentity(local.Registry, ProtectionDomainId{foreign.Registry, registered.GetValue()})
            : Kernel::Protection::ProtectionDomainFailure{};
    const auto wrongInstance =
        impostorObject.HasValue()
            ? domain.BindIdentity(local.Registry, ProtectionDomainId{local.Registry, impostorObject.GetValue()})
            : Kernel::Protection::ProtectionDomainFailure{};
    const auto bound =
        registered.HasValue()
            ? domain.BindIdentity(local.Registry, ProtectionDomainId{local.Registry, registered.GetValue()})
            : Kernel::Protection::ProtectionDomainFailure{};
    const auto repeated =
        registered.HasValue()
            ? domain.BindIdentity(local.Registry, ProtectionDomainId{local.Registry, registered.GetValue()})
            : Kernel::Protection::ProtectionDomainFailure{};

    ASTERIA_EXPECT(runner, "identity binding rejects provenance and instance substitution", resourceInitialized,
                   resource.HasValue(), addressSpace.Ready, initialized.Error == ProtectionDomainError::None,
                   registered.HasValue(), invalid.Error == ProtectionDomainError::InvalidIdentity,
                   foreignIdentity.Error == ProtectionDomainError::RegistryMismatch,
                   wrongInstance.Error == ProtectionDomainError::ObjectInstanceMismatch,
                   bound.Error == ProtectionDomainError::None, repeated.Error == ProtectionDomainError::AlreadyBound);

    const bool quiesced = domain.Quiesce() == ProtectionDomainError::None;
    const bool addressQuiesced = addressSpace.Storage.Quiesce() == AddressSpaceError::None;
    const bool resourceQuiesced = resourceDomain.Quiesce() == ResourceDomainError::None;
    const bool cleaned = quiesced && addressQuiesced && resourceQuiesced && registered.HasValue() &&
                         addressSpace.Object.IsValid() && impostorObject.HasValue() && resource.HasValue() &&
                         DestroyAndCollect(local.Issuer, registered.GetValue(), domain) &&
                         DestroyAndCollect(local.Issuer, addressSpace.Object, addressSpace.Storage) &&
                         DestroyAndCollect(local.Issuer, impostorObject.GetValue(), impostor) &&
                         DestroyAndCollect(local.Issuer, resource.GetValue(), resourceDomain);
    ASTERIA_EXPECT(runner, "identity failures leave the valid bind recoverable", cleaned);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"ProtectionDomainTests"};
    Asteria::Test::TestBindingAndLifetime(runner);
    Asteria::Test::TestKernelOnlyBinding(runner);
    Asteria::Test::TestInitializationRejections(runner);
    Asteria::Test::TestIdentityBinding(runner);
    return runner.Finish();
}
