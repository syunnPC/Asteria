// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Kernel/Device/DeviceRegion.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Kernel::Device::DeviceAccessOperation;
using Kernel::Device::DeviceAccessWidth;
using Kernel::Device::DeviceAccessWidths;
using Kernel::Device::DeviceRegion;
using Kernel::Device::DeviceRegionDescriptor;
using Kernel::Device::DeviceRegionError;
using Kernel::Device::DeviceRegionId;
using Kernel::Device::DeviceRegionKind;
using Kernel::Device::DeviceRegionPermission;
using Kernel::Device::DeviceRegionPermissions;
using Kernel::Device::DeviceRegionState;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Memory::PhysicalAddress;
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

class DomainFixture final
{
private:
    RegistryFixture& m_registry;

public:
    explicit DomainFixture(RegistryFixture& registry, const ResourceDomainLimits& limits = MakeLimits()) noexcept
        : m_registry{registry}, Domain{Ledger}
    {
        const auto initialized = Domain.InitializeRoot(ResourceDomainId{51U}, limits);
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

[[nodiscard]] constexpr DeviceRegionPermissions ReadWrite() noexcept
{
    return DeviceRegionPermissions{DeviceRegionPermission::Read}.With(DeviceRegionPermission::Write);
}

[[nodiscard]] constexpr DeviceAccessWidths AllWidths() noexcept
{
    return DeviceAccessWidths{DeviceAccessWidth::Byte}
        .With(DeviceAccessWidth::Word)
        .With(DeviceAccessWidth::DoubleWord)
        .With(DeviceAccessWidth::QuadWord);
}

[[nodiscard]] constexpr DeviceRegionDescriptor MakeMmioDescriptor() noexcept
{
    return DeviceRegionDescriptor{
        .BaseAddress = 0x0000'0000'FEC0'0000ULL,
        .LengthBytes = 0x2000U,
        .Kind = DeviceRegionKind::MemoryMappedIo,
        .Permissions = ReadWrite(),
        .Widths = AllWidths(),
        .CachePolicy = MemoryCachePolicy::Uncached,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr DeviceRegionDescriptor MakePortDescriptor() noexcept
{
    return DeviceRegionDescriptor{
        .BaseAddress = 0x0CF8U,
        .LengthBytes = 8U,
        .Kind = DeviceRegionKind::PortIo,
        .Permissions = ReadWrite(),
        .Widths = DeviceAccessWidths{DeviceAccessWidth::Byte}
                      .With(DeviceAccessWidth::Word)
                      .With(DeviceAccessWidth::DoubleWord),
        .CachePolicy = MemoryCachePolicy::Uncached,
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

void TestInitializationAndIdentity(TestContext& runner) noexcept
{
    RegistryFixture local;
    RegistryFixture foreign;
    DomainFixture domain{local};
    DeviceRegion region;
    TestKernelObject wrongType{ObjectType::Thread, ResourceDomainId{9U}};
    const auto wrongObject = local.Issuer.CreateObject(wrongType);

    ASTERIA_EXPECT(runner, "default DeviceRegion owns no hardware range or resource charge", !region.IsInitialized(),
                   region.GetObjectType() == ObjectType::DeviceRegion, region.GetChargeOwner().GetValue() == 0U,
                   region.GetSnapshot().State == DeviceRegionState::Uninitialized,
                   region.Quiesce().Error == DeviceRegionError::NotReady);
    ASTERIA_EXPECT(runner, "uninitialized DeviceRegion cannot be published",
                   !local.Issuer.CreateObject(region).HasValue());

    const DeviceRegionDescriptor descriptor = MakeMmioDescriptor();
    ASTERIA_EXPECT(runner, "invalid ResourceDomain identity is rejected",
                   region.Initialize(local.Registry, ResourceDomainObjectId{}, domain.Domain, descriptor).Error ==
                       DeviceRegionError::InvalidConfiguration);
    ASTERIA_EXPECT(runner, "ResourceDomain identity carries registry provenance",
                   region.Initialize(local.Registry, ResourceDomainObjectId{foreign.Registry, domain.Object},
                                     domain.Domain, descriptor)
                           .Error == DeviceRegionError::InvalidConfiguration);
    if (wrongObject.HasValue())
    {
        const auto failure = region.Initialize(
            local.Registry, ResourceDomainObjectId{local.Registry, wrongObject.GetValue()}, domain.Domain, descriptor);
        ASTERIA_EXPECT(runner, "typed ResourceDomain pin rejects a different object type",
                       failure.Error == DeviceRegionError::ResourceDomainPinRejected,
                       failure.Pin == Kernel::Object::ObjectPinError::TypeMismatch);
    }

    ResourceDomainLedger otherLedger;
    ResourceDomain otherDomain{otherLedger};
    static_cast<void>(otherDomain.InitializeRoot(ResourceDomainId{52U}, MakeLimits()));
    ASTERIA_EXPECT(runner, "ResourceDomain identity names the exact supplied instance",
                   region.Initialize(local.Registry, domain.Identity, otherDomain, descriptor).Error ==
                       DeviceRegionError::ResourceDomainInstanceMismatch);
    static_cast<void>(otherDomain.Quiesce());

    ASTERIA_EXPECT(runner, "valid hardware range publishes immutable identity and metadata charge",
                   region.Initialize(local.Registry, domain.Identity, domain.Domain, descriptor).Error ==
                       DeviceRegionError::None,
                   region.IsInitialized(), region.GetChargeOwner().GetValue() == 51U,
                   region.GetSnapshot().Descriptor.BaseAddress == descriptor.BaseAddress,
                   domain.Domain.GetSnapshot().Usage.KernelMetadataBytes == sizeof(DeviceRegion),
                   region.Initialize(local.Registry, domain.Identity, domain.Domain, descriptor).Error ==
                       DeviceRegionError::AlreadyInitialized);

    const auto object = local.Issuer.CreateObject(region);
    ASTERIA_EXPECT(runner, "DeviceRegion identity includes exact registry provenance", object.HasValue(),
                   (DeviceRegionId{local.Registry, object.GetValue()}.IsValid()),
                   (!DeviceRegionId{local.Registry, object.GetValue()}.IsEqualTo(
                       DeviceRegionId{foreign.Registry, object.GetValue()})));
    ASTERIA_EXPECT(runner, "quiescence releases metadata and permits registry collection",
                   region.Quiesce().Error == DeviceRegionError::None,
                   domain.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U,
                   DestroyAndCollect(local, object.GetValue(), region));
    if (wrongObject.HasValue())
    {
        ASTERIA_EXPECT(runner, "validation failures retain no provisional pin",
                       DestroyAndCollect(local, wrongObject.GetValue(), wrongType));
    }
}

void TestDescriptorValidation(TestContext& runner) noexcept
{
    RegistryFixture fixture;
    DomainFixture domain{fixture};
    DeviceRegion region;
    const DeviceRegionDescriptor valid = MakeMmioDescriptor();

    struct InvalidCase
    {
        DeviceRegionDescriptor Descriptor;
        const char* Description;
    };
    DeviceRegionDescriptor empty = valid;
    empty.LengthBytes = 0U;
    DeviceRegionDescriptor overflow = valid;
    overflow.BaseAddress = ~Foundation::UInt64{0U} - 3U;
    overflow.LengthBytes = 8U;
    DeviceRegionDescriptor unknownKind = valid;
    unknownKind.Kind = static_cast<DeviceRegionKind>(0xFFU);
    DeviceRegionDescriptor noPermission = valid;
    noPermission.Permissions = DeviceRegionPermissions{};
    DeviceRegionDescriptor unknownPermission = valid;
    unknownPermission.Permissions = DeviceRegionPermissions{static_cast<DeviceRegionPermission>(0x80U)};
    DeviceRegionDescriptor noWidth = valid;
    noWidth.Widths = DeviceAccessWidths{};
    DeviceRegionDescriptor unknownWidth = valid;
    unknownWidth.Widths = DeviceAccessWidths{static_cast<DeviceAccessWidth>(0x80U)};
    DeviceRegionDescriptor cachedMmio = valid;
    cachedMmio.CachePolicy = MemoryCachePolicy::WriteBack;
    DeviceRegionDescriptor portOverflow = MakePortDescriptor();
    portOverflow.BaseAddress = 0xFFFFU;
    portOverflow.LengthBytes = 2U;
    DeviceRegionDescriptor cachedPort = MakePortDescriptor();
    cachedPort.CachePolicy = MemoryCachePolicy::WriteCombining;
    DeviceRegionDescriptor widePort = MakePortDescriptor();
    widePort.Widths = widePort.Widths.With(DeviceAccessWidth::QuadWord);
    DeviceRegionDescriptor reserved = valid;
    reserved.Reserved[3] = 1U;

    const InvalidCase cases[]{
        {empty, "empty hardware extent is rejected"},
        {overflow, "hardware extent overflow is rejected"},
        {unknownKind, "unknown region kind is rejected"},
        {noPermission, "empty access policy is rejected"},
        {unknownPermission, "unknown access bit is rejected"},
        {noWidth, "empty width policy is rejected"},
        {unknownWidth, "unknown width bit is rejected"},
        {cachedMmio, "cacheable MMIO is rejected"},
        {portOverflow, "port range cannot exceed 16-bit I/O space"},
        {cachedPort, "port I/O has no cacheable policy"},
        {widePort, "port I/O cannot authorize quad-word access"},
        {reserved, "descriptor reserved bytes remain canonical"},
    };

    for (const InvalidCase& value : cases)
    {
        ASTERIA_EXPECT(runner, value.Description,
                       region.Initialize(fixture.Registry, domain.Identity, domain.Domain, value.Descriptor).Error ==
                           DeviceRegionError::InvalidDescriptor,
                       region.GetSnapshot().State == DeviceRegionState::Uninitialized,
                       domain.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U);
    }
}

void TestRegisterAccessAndMapping(TestContext& runner) noexcept
{
    RegistryFixture fixture;
    DomainFixture domain{fixture};
    DeviceRegion region;
    ASTERIA_EXPECT(runner, "MMIO access fixture initializes",
                   region.Initialize(fixture.Registry, domain.Identity, domain.Domain, MakeMmioDescriptor()).Error ==
                       DeviceRegionError::None);

    ASTERIA_EXPECT(runner, "retained register operations keep the region live until release",
                   region.RetainRegisterAccess(0U, DeviceAccessWidth::DoubleWord, DeviceAccessOperation::Read, 0U) ==
                       DeviceRegionError::None,
                   region.RetainRegisterAccess(8U, DeviceAccessWidth::QuadWord, DeviceAccessOperation::Write, 0U) ==
                       DeviceRegionError::None,
                   region.GetSnapshot().ActiveRegisterAccesses == 2U,
                   region.Quiesce().Error == DeviceRegionError::InUse,
                   region.ReleaseRegisterAccess() == DeviceRegionError::None,
                   region.ReleaseRegisterAccess() == DeviceRegionError::None);
    ASTERIA_EXPECT(runner, "invalid register operations are failure-atomic",
                   region.RetainRegisterAccess(2U, DeviceAccessWidth::DoubleWord, DeviceAccessOperation::Read, 0U) ==
                       DeviceRegionError::MisalignedAccess,
                   region.RetainRegisterAccess(0x1FFFU, DeviceAccessWidth::Word, DeviceAccessOperation::Read, 0U) ==
                       DeviceRegionError::RangeExceeded,
                   region.RetainRegisterAccess(0U, static_cast<DeviceAccessWidth>(0x40U), DeviceAccessOperation::Read,
                                               0U) == DeviceRegionError::UnsupportedWidth,
                   region.RetainRegisterAccess(0U, DeviceAccessWidth::Byte, static_cast<DeviceAccessOperation>(0xFFU),
                                               0U) == DeviceRegionError::InvalidOperation,
                   region.ReleaseRegisterAccess() == DeviceRegionError::AccessReferenceUnderflow,
                   region.GetSnapshot().ActiveRegisterAccesses == 0U);

    const PagePermissions readWriteUser =
        PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User);
    ASTERIA_EXPECT(runner, "page-aligned MMIO mapping retains the region policy",
                   region.RetainMapping(PhysicalAddress{0x0000'0000'FEC0'0000ULL}, PageCount{1U}, readWriteUser,
                                        MemoryCachePolicy::Uncached) == DeviceRegionError::None,
                   region.GetSnapshot().ActiveMappings == 1U, region.Quiesce().Error == DeviceRegionError::InUse,
                   region.ReleaseMapping() == DeviceRegionError::None);
    ASTERIA_EXPECT(runner, "invalid MMIO mappings leave all references unchanged",
                   region.RetainMapping(PhysicalAddress{0x0000'0000'FEBF'F000ULL}, PageCount{1U}, readWriteUser,
                                        MemoryCachePolicy::Uncached) == DeviceRegionError::RangeExceeded,
                   region.RetainMapping(PhysicalAddress{0x0000'0000'FEC0'0000ULL}, PageCount{1U},
                                        readWriteUser.With(PagePermission::Execute),
                                        MemoryCachePolicy::Uncached) == DeviceRegionError::MappingAccessDenied,
                   region.RetainMapping(PhysicalAddress{0x0000'0000'FEC0'0000ULL}, PageCount{1U}, readWriteUser,
                                        MemoryCachePolicy::WriteCombining) == DeviceRegionError::MappingCacheMismatch,
                   region.ReleaseMapping() == DeviceRegionError::MappingReferenceUnderflow,
                   region.GetSnapshot().ActiveMappings == 0U);
    static_cast<void>(region.Quiesce());
    DeviceRegion combining;
    auto combiningDescriptor = MakeMmioDescriptor();
    combiningDescriptor.CachePolicy = MemoryCachePolicy::WriteCombining;
    ASTERIA_EXPECT(runner, "WC publication permits UC fallback, excludes mixed live aliases, retains the final policy",
                   combining.Initialize(fixture.Registry, domain.Identity, domain.Domain, combiningDescriptor).Error ==
                       DeviceRegionError::None,
                   combining.GetMappingCachePolicy() == MemoryCachePolicy::WriteCombining,
                   combining.RetainMapping(PhysicalAddress{0xFEC0'0000U}, PageCount{1U}, readWriteUser,
                                           MemoryCachePolicy::WriteCombining) == DeviceRegionError::None,
                   combining.RetainMapping(PhysicalAddress{0xFEC0'1000U}, PageCount{1U}, readWriteUser,
                                           MemoryCachePolicy::Uncached) == DeviceRegionError::MappingCacheMismatch,
                   combining.ReleaseMapping() == DeviceRegionError::None,
                   combining.RetainMapping(PhysicalAddress{0xFEC0'0000U}, PageCount{1U}, readWriteUser,
                                           MemoryCachePolicy::Uncached) == DeviceRegionError::None,
                   combining.RetainMapping(PhysicalAddress{0xFEC0'1000U}, PageCount{1U}, readWriteUser,
                                           MemoryCachePolicy::WriteCombining) ==
                       DeviceRegionError::MappingCacheMismatch,
                   combining.ReleaseMapping() == DeviceRegionError::None,
                   combining.GetMappingCachePolicy() == MemoryCachePolicy::Uncached,
                   combining.ValidateMapping(PhysicalAddress{0xFEC0'0000U}, PageCount{1U}, readWriteUser,
                                             MemoryCachePolicy::WriteBack) == DeviceRegionError::MappingCacheMismatch,
                   combining.ValidateMapping(PhysicalAddress{0xFEC0'0000U}, PageCount{1U}, readWriteUser,
                                             MemoryCachePolicy::WriteCombining) == DeviceRegionError::None);
    MemoryCachePolicy frozen{MemoryCachePolicy::WriteBack};
    ASTERIA_EXPECT(
        runner, "terminal latch preserves the last type even after unmap and forbids racing fallback",
        combining.FreezeMappingCachePolicy(frozen), frozen == MemoryCachePolicy::Uncached,
        combining.GetMappingCachePolicy() == MemoryCachePolicy::Uncached,
        combining.RetainMapping(PhysicalAddress{0xFEC0'0000U}, PageCount{1U}, readWriteUser,
                                MemoryCachePolicy::WriteCombining) == DeviceRegionError::MappingCacheMismatch,
        combining.RetainMapping(PhysicalAddress{0xFEC0'0000U}, PageCount{1U}, readWriteUser,
                                MemoryCachePolicy::Uncached) == DeviceRegionError::None,
        combining.ReleaseMapping() == DeviceRegionError::None, combining.Quiesce().Error == DeviceRegionError::None);

    DeviceRegion port;
    ASTERIA_EXPECT(runner, "port-I/O fixture initializes",
                   port.Initialize(fixture.Registry, domain.Identity, domain.Domain, MakePortDescriptor()).Error ==
                       DeviceRegionError::None);
    ASTERIA_EXPECT(runner, "port-I/O supports mediated access but never page mapping",
                   port.RetainRegisterAccess(0U, DeviceAccessWidth::DoubleWord, DeviceAccessOperation::Write, 0U) ==
                       DeviceRegionError::None,
                   port.ReleaseRegisterAccess() == DeviceRegionError::None,
                   port.RetainRegisterAccess(0U, DeviceAccessWidth::QuadWord, DeviceAccessOperation::Read, 0U) ==
                       DeviceRegionError::UnsupportedWidth,
                   port.RetainMapping(PhysicalAddress{0x1000U}, PageCount{1U}, PagePermissions{PagePermission::Read},
                                      MemoryCachePolicy::Uncached) == DeviceRegionError::MappingNotSupported);
    static_cast<void>(port.Quiesce());

    DeviceRegion readOnly;
    DeviceRegionDescriptor readOnlyDescriptor = MakeMmioDescriptor();
    readOnlyDescriptor.Permissions = DeviceRegionPermissions{DeviceRegionPermission::Read};
    static_cast<void>(readOnly.Initialize(fixture.Registry, domain.Identity, domain.Domain, readOnlyDescriptor));
    ASTERIA_EXPECT(runner, "read-only region rejects writable register and mapping requests",
                   readOnly.RetainRegisterAccess(0U, DeviceAccessWidth::Byte, DeviceAccessOperation::Write, 0U) ==
                       DeviceRegionError::UnsupportedAccess,
                   readOnly.RetainMapping(PhysicalAddress{0x0000'0000'FEC0'0000ULL}, PageCount{1U}, readWriteUser,
                                          MemoryCachePolicy::Uncached) == DeviceRegionError::MappingAccessDenied);
    static_cast<void>(readOnly.Quiesce());
}

// Whichever operation wins, a terminal policy and a retained mapping must
// agree; a lost single-CAS latch preserves its output and skips drawing.
void TestTerminalPolicyRace(TestContext& context) noexcept
{
    RegistryFixture fixture;
    DomainFixture domain{fixture};
    for (Foundation::Size iteration{0U}; iteration < 32U; ++iteration)
    {
        DeviceRegion region;
        auto descriptor = MakeMmioDescriptor();
        descriptor.CachePolicy = MemoryCachePolicy::WriteCombining;
        ASTERIA_EXPECT(context, "race fixture publishes WC",
                       region.Initialize(fixture.Registry, domain.Identity, domain.Domain, descriptor).Error ==
                           DeviceRegionError::None);
        std::atomic<bool> start{false};
        bool latched{false};
        MemoryCachePolicy frozen{MemoryCachePolicy::WriteBack};
        DeviceRegionError mapped{DeviceRegionError::NotReady};
        const PagePermissions permissions = PagePermissions{PagePermission::Read}.With(PagePermission::Write);
        std::thread stopper{[&]() noexcept
                            {
                                while (!start.load())
                                {
                                    std::this_thread::yield();
                                }
                                latched = region.FreezeMappingCachePolicy(frozen);
                            }};
        std::thread mapper{[&]() noexcept
                           {
                               while (!start.load())
                               {
                                   std::this_thread::yield();
                               }
                               mapped = region.RetainMapping(PhysicalAddress{0xFEC0'0000U}, PageCount{1U}, permissions,
                                                             MemoryCachePolicy::Uncached);
                           }};
        start.store(true);
        stopper.join();
        mapper.join();
        const bool retained = mapped == DeviceRegionError::None;
        ASTERIA_EXPECT(context, "terminal policy selection cannot disagree with the racing mapping",
                       mapped == DeviceRegionError::None || mapped == DeviceRegionError::MappingCacheMismatch,
                       latched ? ((frozen == MemoryCachePolicy::Uncached) == retained)
                               : (retained && frozen == MemoryCachePolicy::WriteBack),
                       region.GetMappingCachePolicy() ==
                           (retained ? MemoryCachePolicy::Uncached : MemoryCachePolicy::WriteCombining));
        if (retained)
        {
            ASTERIA_EXPECT(context, "race fixture releases its retained mapping",
                           region.ReleaseMapping() == DeviceRegionError::None);
        }
        ASTERIA_EXPECT(context, "race fixture quiesces without retaining charge",
                       region.Quiesce().Error == DeviceRegionError::None);
    }
}

void TestQuotaFailure(TestContext& runner) noexcept
{
    RegistryFixture fixture;
    DomainFixture domain{fixture, MakeLimits(sizeof(DeviceRegion) - 1U)};
    DeviceRegion region;
    const auto failure = region.Initialize(fixture.Registry, domain.Identity, domain.Domain, MakeMmioDescriptor());
    ASTERIA_EXPECT(runner, "quota rejection leaves no metadata charge or retained domain pin",
                   failure.Error == DeviceRegionError::ResourceDomainRejected,
                   failure.Resource == ResourceDomainError::LimitExceeded,
                   region.GetSnapshot().State == DeviceRegionState::Uninitialized,
                   domain.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U);
}
void TestWritePolicyValidation(TestContext& context) noexcept
{
    RegistryFixture fixture;
    DomainFixture domain{fixture};
    DeviceRegion region;
    Foundation::UInt32 value{256U};
    Kernel::Device::DevicePortWriteList list{
        .Offset = 0U, .Width = DeviceAccessWidth::Byte, .Values = &value, .Count = 1U};
    const auto descriptor = MakePortDescriptor();
    ASTERIA_EXPECT(
        context, "oversized byte policy values are rejected at publication",
        region.Initialize(fixture.Registry, domain.Identity, domain.Domain, descriptor, {.Lists = &list, .Count = 1U})
                .Error == DeviceRegionError::InvalidDescriptor);
    value = 0xADU;
    list.Offset = descriptor.LengthBytes;
    ASTERIA_EXPECT(
        context, "policy offsets must fit the port region",
        region.Initialize(fixture.Registry, domain.Identity, domain.Domain, descriptor, {.Lists = &list, .Count = 1U})
                .Error == DeviceRegionError::InvalidDescriptor);
    ASTERIA_EXPECT(
        context, "policy counts and null pointers are bounded",
        region.Initialize(fixture.Registry, domain.Identity, domain.Domain, descriptor, {.Lists = &list, .Count = 33U})
                .Error == DeviceRegionError::InvalidDescriptor,
        region.Initialize(fixture.Registry, domain.Identity, domain.Domain, descriptor, {.Lists = nullptr, .Count = 1U})
                .Error == DeviceRegionError::InvalidDescriptor);
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"DeviceRegionTests"};
    Asteria::Test::TestInitializationAndIdentity(runner);
    Asteria::Test::TestDescriptorValidation(runner);
    Asteria::Test::TestRegisterAccessAndMapping(runner);
    Asteria::Test::TestTerminalPolicyRace(runner);
    Asteria::Test::TestQuotaFailure(runner);
    Asteria::Test::TestWritePolicyValidation(runner);
    return runner.Finish();
}
