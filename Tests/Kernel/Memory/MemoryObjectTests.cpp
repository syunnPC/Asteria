// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Kernel/Memory/MemoryObject.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::MemoryCommitment;
using Kernel::Memory::MemoryObject;
using Kernel::Memory::MemoryObjectDescriptor;
using Kernel::Memory::MemoryObjectError;
using Kernel::Memory::MemoryObjectId;
using Kernel::Memory::MemoryObjectState;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Object::IKernelObject;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
using Platform::Synchronization::SpinLock;

class TestKernelObject final : public IKernelObject
{
public:
    constexpr TestKernelObject(const ObjectType type, const ResourceDomainId owner) noexcept
        : m_type{type}, m_owner{owner}
    {
    }

    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return m_type;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return m_owner;
    }

private:
    ObjectType m_type;
    ResourceDomainId m_owner;
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

[[nodiscard]] constexpr ResourceDomainLimits MakeLimits(const Foundation::UInt64 metadataBytes = 1U << 20U) noexcept
{
    return ResourceDomainLimits{
        .CommittedMemoryPages = 1024U,
        .ResidentMemoryPages = 1024U,
        .KernelMetadataBytes = metadataBytes,
        .CapabilityHandles = 64U,
        .IpcQueueBytes = 0U,
        .Threads = 0U,
        .ChildDomains = 0U,
    };
}

[[nodiscard]] constexpr PagePermissions ReadWriteUser() noexcept
{
    return PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User);
}

[[nodiscard]] constexpr MemoryObjectDescriptor
MakeDescriptor(const MemoryCommitment commitment = MemoryCommitment::Committed,
               const MemoryCachePolicy cachePolicy = MemoryCachePolicy::WriteBack) noexcept
{
    return MemoryObjectDescriptor{
        .Pages = PageCount{16U},
        .CachePolicy = cachePolicy,
        .MaximumPermissions = ReadWriteUser(),
        .Commitment = commitment,
        .Reserved = {},
    };
}

[[nodiscard]] bool DestroyAndCollect(RegistryFixture& fixture, const ObjectId object, IKernelObject& instance) noexcept
{
    if (!object.IsValid() || (fixture.Issuer.DestroyObject(object) != ObjectRegistryError::Ok))
    {
        return false;
    }
    const auto collected = fixture.Issuer.CollectDestroyedObject();
    return collected.HasValue() && (collected.GetValue().Object.GetValue() == object.GetValue()) &&
           (collected.GetValue().Instance == &instance);
}

class DomainFixture final
{
private:
    RegistryFixture& m_registry;

public:
    explicit DomainFixture(RegistryFixture& registry, const ResourceDomainLimits& limits = MakeLimits()) noexcept
        : m_registry{registry}, Domain{Ledger}
    {
        const auto initialized = Domain.InitializeRoot(ResourceDomainId{41U}, limits);
        if (initialized.Error != ResourceDomainError::None)
        {
            return;
        }
        const auto registered = m_registry.Issuer.CreateObject(Domain);
        Object = registered.HasValue() ? registered.GetValue() : ObjectId{};
        Identity = Object.IsValid() ? ResourceDomainObjectId{m_registry.Registry, Object} : ResourceDomainObjectId{};
    }

    ~DomainFixture() noexcept
    {
        if (!Object.IsValid())
        {
            return;
        }
        static_cast<void>(Domain.Quiesce());
        static_cast<void>(m_registry.Issuer.DestroyObject(Object));
        static_cast<void>(m_registry.Issuer.CollectDestroyedObject());
    }

    DomainFixture(const DomainFixture&) = delete;
    DomainFixture& operator=(const DomainFixture&) = delete;

    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    ObjectId Object{};
    ResourceDomainObjectId Identity{};
};

void TestValidationAndIdentity(TestContext& runner) noexcept
{
    RegistryFixture local;
    RegistryFixture foreign;
    DomainFixture domain{local};
    MemoryObject object;
    MemoryObject other;
    TestKernelObject wrongType{ObjectType::Thread, ResourceDomainId{9U}};
    const auto wrongObject = local.Issuer.CreateObject(wrongType);
    const auto initial = object.GetSnapshot();

    ASTERIA_EXPECT(runner, "default MemoryObject has no range, charge, or teardown work", domain.Object.IsValid(),
                   !object.IsInitialized(), object.GetObjectType() == ObjectType::MemoryObject,
                   object.GetChargeOwner().GetValue() == 0U, initial.State == MemoryObjectState::Uninitialized,
                   object.Quiesce().Error == MemoryObjectError::NotReady);

    const auto premature = local.Issuer.CreateObject(object);
    ASTERIA_EXPECT(runner, "uninitialized MemoryObject cannot be published", !premature.HasValue(),
                   premature.GetError() == ObjectRegistryError::InvalidArgument);

    const MemoryObjectDescriptor valid = MakeDescriptor();
    auto failure = object.Initialize(local.Registry, ResourceDomainObjectId{}, domain.Domain, valid);
    ASTERIA_EXPECT(runner, "invalid resource identity is failure-atomic",
                   failure.Error == MemoryObjectError::InvalidConfiguration,
                   object.GetSnapshot().State == MemoryObjectState::Uninitialized);

    failure = object.Initialize(local.Registry, ResourceDomainObjectId{foreign.Registry, domain.Object}, domain.Domain,
                                valid);
    ASTERIA_EXPECT(runner, "resource identity carries exact registry provenance",
                   failure.Error == MemoryObjectError::InvalidConfiguration);

    if (wrongObject.HasValue())
    {
        failure = object.Initialize(local.Registry, ResourceDomainObjectId{local.Registry, wrongObject.GetValue()},
                                    domain.Domain, valid);
        ASTERIA_EXPECT(runner, "typed resource pin rejects a non-domain object",
                       failure.Error == MemoryObjectError::ResourceDomainPinRejected,
                       failure.Pin == Kernel::Object::ObjectPinError::TypeMismatch);
    }

    ResourceDomainLedger otherLedger;
    ResourceDomain otherDomain{otherLedger};
    static_cast<void>(otherDomain.InitializeRoot(ResourceDomainId{42U}, MakeLimits()));
    failure = object.Initialize(local.Registry, domain.Identity, otherDomain, valid);
    ASTERIA_EXPECT(runner, "resource identity must name the exact supplied domain instance",
                   failure.Error == MemoryObjectError::ResourceDomainInstanceMismatch);
    static_cast<void>(otherDomain.Quiesce());

    MemoryObjectDescriptor invalid = valid;
    invalid.Pages = PageCount{};
    ASTERIA_EXPECT(runner, "empty logical memory is rejected",
                   object.Initialize(local.Registry, domain.Identity, domain.Domain, invalid).Error ==
                       MemoryObjectError::InvalidDescriptor);
    invalid = valid;
    invalid.Pages = PageCount{(~Foundation::UInt64{0U} / 4096U) + 1U};
    ASTERIA_EXPECT(runner, "logical byte-size overflow is rejected",
                   object.Initialize(local.Registry, domain.Identity, domain.Domain, invalid).Error ==
                       MemoryObjectError::InvalidDescriptor);
    invalid = valid;
    invalid.MaximumPermissions = PagePermissions{PagePermission::Write};
    ASTERIA_EXPECT(runner, "logical policy always requires readable mappings",
                   object.Initialize(local.Registry, domain.Identity, domain.Domain, invalid).Error ==
                       MemoryObjectError::InvalidDescriptor);
    invalid = valid;
    invalid.Commitment = MemoryCommitment::DeviceDma;
    invalid.MaximumPermissions = ReadWriteUser().With(PagePermission::Execute);
    ASTERIA_EXPECT(runner, "device DMA memory cannot authorize execution",
                   object.Initialize(local.Registry, domain.Identity, domain.Domain, invalid).Error ==
                       MemoryObjectError::InvalidDescriptor);
    invalid = valid;
    invalid.Reserved[0] = 1U;
    ASTERIA_EXPECT(runner, "descriptor reserved bytes remain canonical",
                   object.Initialize(local.Registry, domain.Identity, domain.Domain, invalid).Error ==
                       MemoryObjectError::InvalidDescriptor);

    ASTERIA_EXPECT(runner, "valid logical policy is immutable after one publication",
                   object.Initialize(local.Registry, domain.Identity, domain.Domain, valid).Error ==
                       MemoryObjectError::None,
                   object.IsInitialized(), object.GetChargeOwner().GetValue() == 41U,
                   object.GetSnapshot().Descriptor.Pages.GetValue() == 16U,
                   object.Initialize(local.Registry, domain.Identity, domain.Domain, valid).Error ==
                       MemoryObjectError::AlreadyInitialized);

    const auto objectResult = local.Issuer.CreateObject(object);
    ASTERIA_EXPECT(runner, "MemoryObject identity includes exact registry provenance", objectResult.HasValue(),
                   (MemoryObjectId{local.Registry, objectResult.GetValue()}.IsValid()),
                   (!MemoryObjectId{local.Registry, objectResult.GetValue()}.IsEqualTo(
                       MemoryObjectId{foreign.Registry, objectResult.GetValue()})));
    ASTERIA_EXPECT(runner, "quiesced MemoryObject can be retired and collected",
                   object.Quiesce().Error == MemoryObjectError::None,
                   DestroyAndCollect(local, objectResult.GetValue(), object));
    if (wrongObject.HasValue())
    {
        ASTERIA_EXPECT(runner, "validation fixture releases every provisional pin",
                       DestroyAndCollect(local, wrongObject.GetValue(), wrongType));
    }
}

void TestCommitmentAccountingAndMappingLifetime(TestContext& runner) noexcept
{
    struct CommitmentCase
    {
        MemoryCommitment Commitment;
        Foundation::UInt64 Committed;
        Foundation::UInt64 Resident;
    };
    constexpr CommitmentCase cases[]{
        {MemoryCommitment::Committed, 16U, 0U},      {MemoryCommitment::Discardable, 0U, 0U},
        {MemoryCommitment::Reconstructible, 0U, 0U}, {MemoryCommitment::BestEffort, 0U, 0U},
        {MemoryCommitment::Pinned, 16U, 16U},        {MemoryCommitment::DeviceDma, 16U, 16U},
    };

    for (const CommitmentCase& value : cases)
    {
        RegistryFixture fixture;
        DomainFixture domain{fixture};
        MemoryObject object;
        MemoryObjectDescriptor descriptor = MakeDescriptor(value.Commitment);
        if (value.Commitment == MemoryCommitment::DeviceDma)
        {
            descriptor.CachePolicy = MemoryCachePolicy::Uncached;
        }
        const auto initialized = object.Initialize(fixture.Registry, domain.Identity, domain.Domain, descriptor);
        const auto usage = domain.Domain.GetSnapshot().Usage;
        ASTERIA_EXPECT(runner, "commitment kind reserves its exact domain guarantee",
                       initialized.Error == MemoryObjectError::None, usage.CommittedMemoryPages == value.Committed,
                       usage.ResidentMemoryPages == value.Resident, usage.KernelMetadataBytes == sizeof(MemoryObject));

        const MemoryObjectError retained =
            object.RetainMapping(2U, PageCount{2U}, ReadWriteUser(), descriptor.CachePolicy, descriptor.Commitment);
        ASTERIA_EXPECT(runner, "live mappings retain policy and Quiesce releases every charge",
                       retained == MemoryObjectError::None, object.GetSnapshot().ActiveMappings == 1U,
                       object.Quiesce().Error == MemoryObjectError::InUse,
                       object.ReleaseMapping() == MemoryObjectError::None,
                       object.Quiesce().Error == MemoryObjectError::None,
                       domain.Domain.GetSnapshot().Usage.CommittedMemoryPages == 0U,
                       domain.Domain.GetSnapshot().Usage.ResidentMemoryPages == 0U,
                       domain.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U);
    }
}

void TestMappingAndQuotaFailures(TestContext& runner) noexcept
{
    RegistryFixture fixture;
    DomainFixture domain{fixture};
    MemoryObject object;
    const auto descriptor = MakeDescriptor();
    ASTERIA_EXPECT(runner, "mapping validation fixture initializes",
                   object.Initialize(fixture.Registry, domain.Identity, domain.Domain, descriptor).Error ==
                       MemoryObjectError::None);

    ASTERIA_EXPECT(runner, "mapping policy failures are side-effect-free",
                   object.RetainMapping(15U, PageCount{2U}, ReadWriteUser(), MemoryCachePolicy::WriteBack,
                                        MemoryCommitment::Committed) == MemoryObjectError::MappingRangeExceeded,
                   object.RetainMapping(0U, PageCount{1U}, ReadWriteUser().With(PagePermission::Execute),
                                        MemoryCachePolicy::WriteBack,
                                        MemoryCommitment::Committed) == MemoryObjectError::MappingAccessDenied,
                   object.RetainMapping(0U, PageCount{1U}, ReadWriteUser(), MemoryCachePolicy::Uncached,
                                        MemoryCommitment::Committed) == MemoryObjectError::MappingCacheMismatch,
                   object.RetainMapping(0U, PageCount{1U}, ReadWriteUser(), MemoryCachePolicy::WriteBack,
                                        MemoryCommitment::Pinned) == MemoryObjectError::MappingCommitmentMismatch,
                   object.ReleaseMapping() == MemoryObjectError::MappingReferenceUnderflow,
                   object.GetSnapshot().ActiveMappings == 0U);
    static_cast<void>(object.Quiesce());

    RegistryFixture limitedRegistry;
    ResourceDomainLimits limits = MakeLimits(sizeof(MemoryObject) - 1U);
    DomainFixture limited{limitedRegistry, limits};
    MemoryObject rejected;
    const auto failure = rejected.Initialize(limitedRegistry.Registry, limited.Identity, limited.Domain, descriptor);
    ASTERIA_EXPECT(runner, "quota rejection leaves no charge or domain pin",
                   failure.Error == MemoryObjectError::ResourceDomainRejected,
                   failure.Resource == ResourceDomainError::LimitExceeded,
                   rejected.GetSnapshot().State == MemoryObjectState::Uninitialized,
                   limited.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U);
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"MemoryObjectTests"};
    Asteria::Test::TestValidationAndIdentity(runner);
    Asteria::Test::TestCommitmentAccountingAndMappingLifetime(runner);
    Asteria::Test::TestMappingAndQuotaFailures(runner);
    return runner.Finish();
}
