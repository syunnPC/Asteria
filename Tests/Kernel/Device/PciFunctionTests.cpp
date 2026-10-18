// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Kernel/Device/IommuDomain.hpp"
#include "Kernel/Device/PciFunction.hpp"
#include "Kernel/Device/PciFunctionControl.hpp"
#include "Kernel/Interrupt/InterruptPool.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Hal::Device::IIommuDevice;
using Hal::Device::IIommuDomainBackend;
using Hal::Device::IommuBackendError;
using Hal::Device::IommuBackendMapRequest;
using Hal::Device::IommuBackendOwnershipError;
using Hal::Device::IommuDeviceError;
using Kernel::Device::IommuDomain;
using Kernel::Device::IommuAttachmentToken;
using Kernel::Device::IommuDomainConfiguration;
using Kernel::Device::IommuDomainFailure;
using Kernel::Device::IommuDomainError;
using Kernel::Device::IommuDomainState;
using Kernel::Device::PciFunction;
using Kernel::Device::PciFunctionAssignmentReceipt;
using Kernel::Device::PciFunctionDescriptor;
using Kernel::Device::PciFunctionError;
using Kernel::Device::PciFunctionState;
using Kernel::Device::PciFunctionAssignmentSequencePolicy;
using Kernel::Device::PciFunctionAddress;
using Kernel::Memory::AddressSpaceId;
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
using Kernel::Protection::ProtectionDomain;
using Kernel::Protection::ProtectionDomainError;
using Kernel::Protection::ProtectionDomainId;
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

class NullIommuBackend final : public IIommuDomainBackend
{
public:
    [[nodiscard]] bool IsReady() const noexcept override
    {
        return true;
    }

    [[nodiscard]] Hal::Device::IommuBackendFaults GetFaults() noexcept override
    {
        return {};
    }

    [[nodiscard]] IommuBackendOwnershipError Claim(const IommuDomain&, const Foundation::UInt8) noexcept override
    {
        return IommuBackendOwnershipError::None;
    }

    [[nodiscard]] IommuBackendOwnershipError Release(const IommuDomain&) noexcept override
    {
        ++ReleaseCalls;
        return ReleaseFails ? IommuBackendOwnershipError::HardwareFailure : IommuBackendOwnershipError::None;
    }

    bool ReleaseFails{false};
    bool AttachFails{false};
    Size ReleaseCalls{0U};

    [[nodiscard]] Kernel::Device::DmaAddress
    ChooseDeviceAddress(Kernel::Device::DmaAddress requested, Kernel::Memory::PhysicalAddress) const noexcept override
    {
        return requested;
    }

    [[nodiscard]] IommuBackendError Map(const IommuBackendMapRequest&) noexcept override
    {
        return IommuBackendError::None;
    }

    [[nodiscard]] IommuBackendError Unmap(const IommuBackendMapRequest&) noexcept override
    {
        return IommuBackendError::None;
    }

    [[nodiscard]] IommuBackendError Attach(IIommuDevice&) noexcept override
    {
        return AttachFails ? IommuBackendError::CommittedStateUnknown : IommuBackendError::None;
    }

    [[nodiscard]] IommuBackendError Detach(IIommuDevice&) noexcept override
    {
        return IommuBackendError::None;
    }
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

[[nodiscard]] constexpr ResourceDomainLimits MakeLimits(const Foundation::UInt64 metadataBytes = 1U << 20U) noexcept
{
    return ResourceDomainLimits{
        .CommittedMemoryPages = 0U,
        .ResidentMemoryPages = 0U,
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
        if (Domain.InitializeRoot(ResourceDomainId{71U}, limits).Error != ResourceDomainError::None)
        {
            return;
        }
        const auto registered = m_registry.Issuer.CreateObject(Domain);
        Object = registered.HasValue() ? registered.GetValue() : ObjectId{};
        Identity = Object.IsValid() ? ResourceDomainObjectId{m_registry.Registry, Object} : ResourceDomainObjectId{};
    }

    ~DomainFixture() noexcept
    {
        static_cast<void>(Finish());
    }

    DomainFixture(const DomainFixture&) = delete;
    DomainFixture& operator=(const DomainFixture&) = delete;

    [[nodiscard]] bool Finish() noexcept
    {
        if (!Object.IsValid())
        {
            return true;
        }
        if ((Domain.Quiesce() != ResourceDomainError::None) ||
            (m_registry.Issuer.DestroyObject(Object) != ObjectRegistryError::Ok))
        {
            return false;
        }
        const auto collected = m_registry.Issuer.CollectDestroyedObject();
        if (!collected.HasValue() || (collected.GetValue().Instance != &Domain))
        {
            return false;
        }
        Object = ObjectId{};
        Identity = ResourceDomainObjectId{};
        return true;
    }

    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    ObjectId Object{};
    ResourceDomainObjectId Identity{};
};

[[nodiscard]] bool DestroyAndCollect(RegistryFixture& fixture, const ObjectId object, IKernelObject& instance) noexcept;

class DriverFixture final
{
public:
    explicit constexpr DriverFixture(const ResourceDomainId owner) noexcept
        : AddressSpace{ObjectType::AddressSpace, owner}
    {
    }

    [[nodiscard]] bool Initialize(RegistryFixture& registry, DomainFixture& resource) noexcept
    {
        m_registry = &registry;
        const auto address = registry.Issuer.CreateObject(AddressSpace);
        if (!address.HasValue())
        {
            return false;
        }
        AddressObject = address.GetValue();
        if (Domain
                .Initialize(registry.Registry, resource.Identity, resource.Domain,
                            AddressSpaceId{registry.Registry, AddressObject}, registry.Capabilities)
                .Error != ProtectionDomainError::None)
        {
            return false;
        }
        const auto domain = registry.Issuer.CreateObject(Domain);
        if (!domain.HasValue())
        {
            return false;
        }
        DomainObject = domain.GetValue();
        Identity = ProtectionDomainId{registry.Registry, DomainObject};
        return Domain.BindIdentity(registry.Registry, Identity).Error == ProtectionDomainError::None;
    }

    [[nodiscard]] bool BeginDestroy() noexcept
    {
        if ((m_registry == nullptr) || !DomainObject.IsValid())
        {
            return false;
        }
        if (m_destroyStarted)
        {
            return true;
        }
        m_destroyStarted = m_registry->Issuer.DestroyObject(DomainObject) == ObjectRegistryError::Ok;
        return m_destroyStarted;
    }

    [[nodiscard]] bool CollectDomain() noexcept
    {
        if ((m_registry == nullptr) || !m_destroyStarted || !DomainObject.IsValid())
        {
            return false;
        }
        const auto collected = m_registry->Issuer.CollectDestroyedObject();
        if (!collected.HasValue() || (collected.GetValue().Instance != &Domain))
        {
            return false;
        }
        DomainObject = ObjectId{};
        Identity = ProtectionDomainId{};
        return true;
    }

    [[nodiscard]] bool Finish() noexcept
    {
        if (m_registry == nullptr)
        {
            return false;
        }
        if ((Domain.GetSnapshot().State != ProtectionDomainState::Quiesced) &&
            (Domain.Quiesce() != ProtectionDomainError::None))
        {
            return false;
        }
        if (DomainObject.IsValid() && (!BeginDestroy() || !CollectDomain()))
        {
            return false;
        }
        if (AddressObject.IsValid() && !DestroyAndCollect(*m_registry, AddressObject, AddressSpace))
        {
            return false;
        }
        AddressObject = ObjectId{};
        return true;
    }

    TestKernelObject AddressSpace;
    ProtectionDomain Domain;
    ObjectId AddressObject{};
    ObjectId DomainObject{};
    ProtectionDomainId Identity{};

private:
    RegistryFixture* m_registry{nullptr};
    bool m_destroyStarted{false};
};

[[nodiscard]] constexpr PciFunctionDescriptor MakeDescriptor(const Foundation::UInt16 segment = 0U,
                                                             const Foundation::UInt8 bus = 0U,
                                                             const Foundation::UInt8 device = 1U,
                                                             const Foundation::UInt8 function = 0U) noexcept
{
    return PciFunctionDescriptor{
        .Address = PciFunctionAddress{segment, bus, device, function},
        .VendorId = 0x8086U,
        .DeviceId = 0x2922U,
        .RevisionId = 2U,
        .ProgrammingInterface = 1U,
        .Subclass = 6U,
        .ClassCode = 1U,
        .HeaderLayout = 0U,
        .Multifunction = false,
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

void TestInitializationAndDescriptor(TestContext& runner) noexcept
{
    RegistryFixture local;
    RegistryFixture foreign;
    DomainFixture domain{local};
    PciFunction pci;
    ASTERIA_EXPECT(runner, "default PCI function has no authority, charge, or publishable identity",
                   !pci.IsInitialized(), pci.GetObjectType() == ObjectType::PciFunction,
                   pci.GetChargeOwner().GetValue() == 0U, pci.GetSnapshot().State == PciFunctionState::Uninitialized,
                   pci.Quiesce().Error == PciFunctionError::NotReady, !local.Issuer.CreateObject(pci).HasValue());

    PciFunctionDescriptor invalid = MakeDescriptor();
    invalid.Address = PciFunctionAddress{};
    const auto invalidAddress = pci.Initialize(local.Registry, domain.Identity, domain.Domain, invalid);
    invalid = MakeDescriptor();
    invalid.VendorId = 0xFFFFU;
    const auto missingVendor = pci.Initialize(local.Registry, domain.Identity, domain.Domain, invalid);
    invalid = MakeDescriptor();
    invalid.HeaderLayout = 0x80U;
    const auto invalidHeader = pci.Initialize(local.Registry, domain.Identity, domain.Domain, invalid);
    ASTERIA_EXPECT(runner, "invalid or absent PCI descriptors are rejected without a charge",
                   invalidAddress.Error == PciFunctionError::InvalidDescriptor,
                   missingVendor.Error == PciFunctionError::InvalidDescriptor,
                   invalidHeader.Error == PciFunctionError::InvalidDescriptor,
                   domain.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U);

    const auto invalidResource =
        pci.Initialize(local.Registry, ResourceDomainObjectId{}, domain.Domain, MakeDescriptor());
    const auto foreignResource = pci.Initialize(local.Registry, ResourceDomainObjectId{foreign.Registry, domain.Object},
                                                domain.Domain, MakeDescriptor());
    ASTERIA_EXPECT(runner, "ResourceDomain identity must be valid and registry-qualified",
                   invalidResource.Error == PciFunctionError::InvalidConfiguration,
                   foreignResource.Error == PciFunctionError::ResourceDomainRegistryMismatch);

    TestKernelObject wrongType{ObjectType::Thread, ResourceDomainId{9U}};
    const auto wrongObject = local.Issuer.CreateObject(wrongType);
    if (wrongObject.HasValue())
    {
        const auto wrong =
            pci.Initialize(local.Registry, ResourceDomainObjectId{local.Registry, wrongObject.GetValue()},
                           domain.Domain, MakeDescriptor());
        ASTERIA_EXPECT(runner, "typed ResourceDomain pin rejects a foreign object type",
                       wrong.Error == PciFunctionError::ResourceDomainPinRejected,
                       wrong.Pin == ObjectPinError::TypeMismatch);
    }

    ResourceDomainLedger otherLedger;
    ResourceDomain otherDomain{otherLedger};
    static_cast<void>(otherDomain.InitializeRoot(ResourceDomainId{72U}, MakeLimits()));
    ASTERIA_EXPECT(runner, "ResourceDomain identity names the exact supplied instance",
                   pci.Initialize(local.Registry, domain.Identity, otherDomain, MakeDescriptor()).Error ==
                       PciFunctionError::ResourceDomainInstanceMismatch);
    static_cast<void>(otherDomain.Quiesce());

    const PciFunctionDescriptor descriptor = MakeDescriptor(4U, 5U, 6U, 7U);
    const auto initialized = pci.Initialize(local.Registry, domain.Identity, domain.Domain, descriptor);
    const auto snapshot = pci.GetSnapshot();
    const auto object = local.Issuer.CreateObject(pci);
    ASTERIA_EXPECT(runner, "valid descriptor publishes one charged PCI function object",
                   initialized.Error == PciFunctionError::None, pci.IsInitialized(),
                   pci.GetChargeOwner().GetValue() == 71U, snapshot.State == PciFunctionState::Available,
                   snapshot.Descriptor.Address.IsEqualTo(descriptor.Address),
                   snapshot.Descriptor.DeviceId == descriptor.DeviceId,
                   domain.Domain.GetSnapshot().Usage.KernelMetadataBytes == sizeof(PciFunction), object.HasValue(),
                   pci.Initialize(local.Registry, domain.Identity, domain.Domain, descriptor).Error ==
                       PciFunctionError::AlreadyInitialized);
    const bool quiesced = pci.Quiesce().Error == PciFunctionError::None;
    const bool collected = quiesced && DestroyAndCollect(local, object.GetValue(), pci);
    ProtectionDomain terminalDriver;
    const auto terminalAssign = pci.Assign(ProtectionDomainId{}, terminalDriver);
    ASTERIA_EXPECT(
        runner, "quiescence removes the charge and prevents terminal republication", quiesced, !pci.IsInitialized(),
        pci.GetChargeOwner().GetValue() == 0U, domain.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U, collected,
        pci.Quiesce().Error == PciFunctionError::Quiesced, !terminalAssign.HasValue(),
        terminalAssign.GetError().Error == PciFunctionError::Quiesced, !local.Issuer.CreateObject(pci).HasValue());
    if (wrongObject.HasValue())
    {
        ASTERIA_EXPECT(runner, "failed initialization retains no wrong-type pin",
                       DestroyAndCollect(local, wrongObject.GetValue(), wrongType));
    }
    ASTERIA_EXPECT(runner, "quiescence releases the exact ResourceDomain generation pin", domain.Finish());
}

void TestResourceFailure(TestContext& runner) noexcept
{
    RegistryFixture fixture;
    DomainFixture domain{fixture, MakeLimits(sizeof(PciFunction) - 1U)};
    PciFunction pci;
    const auto failure = pci.Initialize(fixture.Registry, domain.Identity, domain.Domain, MakeDescriptor());
    ASTERIA_EXPECT(runner, "metadata quota failure leaves no charge or ResourceDomain pin",
                   failure.Error == PciFunctionError::ResourceDomainRejected,
                   failure.Resource == ResourceDomainError::LimitExceeded,
                   pci.GetSnapshot().State == PciFunctionState::Uninitialized,
                   domain.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U, domain.Finish());
}

void TestAssignmentProvenanceAndLifetime(TestContext& runner) noexcept
{
    RegistryFixture fixture;
    RegistryFixture foreign;
    DomainFixture resource{fixture};
    DomainFixture foreignResource{foreign};
    DriverFixture driver{ResourceDomainId{81U}};
    DriverFixture secondDriver{ResourceDomainId{82U}};
    DriverFixture quiescedDriver{ResourceDomainId{85U}};
    DriverFixture foreignDriver{ResourceDomainId{84U}};
    PciFunction pci;
    TestKernelObject wrongType{ObjectType::Thread, ResourceDomainId{83U}};
    const auto wrongObject = fixture.Issuer.CreateObject(wrongType);
    const bool driversReady = driver.Initialize(fixture, resource) && secondDriver.Initialize(fixture, resource) &&
                              quiescedDriver.Initialize(fixture, resource) &&
                              foreignDriver.Initialize(foreign, foreignResource) && wrongObject.HasValue();
    ASTERIA_EXPECT(runner, "assignment fixture initializes real driver domains", driversReady,
                   pci.Initialize(fixture.Registry, resource.Identity, resource.Domain, MakeDescriptor()).Error ==
                       PciFunctionError::None,
                   quiescedDriver.Domain.Quiesce() == ProtectionDomainError::None);

    const auto invalid = pci.Assign(ProtectionDomainId{}, driver.Domain);
    const auto foreignResult = pci.Assign(foreignDriver.Identity, foreignDriver.Domain);
    const auto wrong = pci.Assign(ProtectionDomainId{fixture.Registry, wrongObject.GetValue()}, driver.Domain);
    const auto mismatchedInstance = pci.Assign(secondDriver.Identity, driver.Domain);
    const auto terminal = pci.Assign(quiescedDriver.Identity, quiescedDriver.Domain);
    ASTERIA_EXPECT(runner, "assignment rejects invalid provenance, instance, type, and state", !invalid.HasValue(),
                   invalid.GetError().Error == PciFunctionError::InvalidDriver, !foreignResult.HasValue(),
                   foreignResult.GetError().Error == PciFunctionError::DriverRegistryMismatch, !wrong.HasValue(),
                   wrong.GetError().Error == PciFunctionError::DriverPinRejected,
                   wrong.GetError().Pin == ObjectPinError::TypeMismatch, !mismatchedInstance.HasValue(),
                   mismatchedInstance.GetError().Error == PciFunctionError::DriverInstanceMismatch,
                   !terminal.HasValue(), terminal.GetError().Error == PciFunctionError::DriverStateRejected,
                   terminal.GetError().Driver == ProtectionDomainError::Quiesced);

    const auto assigned = pci.Assign(driver.Identity, driver.Domain);
    const auto duplicate = pci.Assign(secondDriver.Identity, secondDriver.Domain);
    const auto assignedSnapshot = pci.GetSnapshot();
    ASTERIA_EXPECT(
        runner, "one exact live driver generation owns the assignment and cannot quiesce", assigned.HasValue(),
        assigned.GetValue().Assignment.IsFromFunction(pci), assigned.GetValue().Driver.IsEqualTo(driver.Identity),
        !duplicate.HasValue(), duplicate.GetError().Error == PciFunctionError::AlreadyAssigned,
        assignedSnapshot.State == PciFunctionState::Assigned, assignedSnapshot.Driver.IsEqualTo(driver.Identity),
        assignedSnapshot.LastAssignmentSequence == 1U, driver.Domain.Quiesce() == ProtectionDomainError::InUse);

    const bool destroyStarted = driver.BeginDestroy();
    const auto prematureCollection = fixture.Issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "assignment pin retains a quiescing driver generation", destroyStarted,
                   !prematureCollection.HasValue());

    PciFunction other;
    const auto foreignToken = other.ReleaseAssignment(assigned.GetValue().Assignment);
    const auto released = pci.ReleaseAssignment(assigned.GetValue().Assignment);
    const auto stale = pci.ReleaseAssignment(assigned.GetValue().Assignment);
    const auto releasedSnapshot = pci.GetSnapshot();
    const bool collected = driver.CollectDomain();
    ASTERIA_EXPECT(runner, "exact release drops the driver use and pin before collection",
                   foreignToken.Error == PciFunctionError::InvalidAssignment, released.Error == PciFunctionError::None,
                   stale.Error == PciFunctionError::StaleAssignment,
                   releasedSnapshot.State == PciFunctionState::Available, !releasedSnapshot.Driver.IsValid(), collected,
                   driver.Domain.Quiesce() == ProtectionDomainError::None);

    const auto reassigned = pci.Assign(secondDriver.Identity, secondDriver.Domain);
    const auto priorGeneration = pci.ReleaseAssignment(assigned.GetValue().Assignment);
    const auto reassignedSnapshot = pci.GetSnapshot();
    ASTERIA_EXPECT(runner, "a later driver receives a fresh sequence immune to stale release", reassigned.HasValue(),
                   reassigned.GetValue().Assignment.GetSequence() == 2U,
                   priorGeneration.Error == PciFunctionError::StaleAssignment,
                   reassignedSnapshot.State == PciFunctionState::Assigned,
                   reassignedSnapshot.Driver.IsEqualTo(secondDriver.Identity),
                   pci.ReleaseAssignment(reassigned.GetValue().Assignment).Error == PciFunctionError::None);

    ASTERIA_EXPECT(runner, "assignment teardown leaves every driver and unrelated object collectable",
                   pci.Quiesce().Error == PciFunctionError::None, driver.Finish(), secondDriver.Finish(),
                   quiescedDriver.Finish(), foreignDriver.Finish(),
                   DestroyAndCollect(fixture, wrongObject.GetValue(), wrongType));
}

void TestIommuOrdering(TestContext& runner) noexcept
{
    RegistryFixture fixture;
    DomainFixture resource{fixture};
    DriverFixture driver{ResourceDomainId{91U}};
    PciFunction pci;
    NullIommuBackend firstBackend;
    NullIommuBackend secondBackend;
    IommuDomain firstDomain{fixture.Registry, firstBackend};
    IommuDomain secondDomain{fixture.Registry, secondBackend};
    const IommuDomainConfiguration configuration{
        .InputAddressBits = 48U,
        .Reserved = {},
    };
    ASTERIA_EXPECT(
        runner, "initialized IOMMU domains cannot claim before driver assignment", driver.Initialize(fixture, resource),
        pci.Initialize(fixture.Registry, resource.Identity, resource.Domain, MakeDescriptor()).Error ==
            PciFunctionError::None,
        firstDomain.Initialize(resource.Identity, resource.Domain, configuration).Error == IommuDomainError::None,
        secondDomain.Initialize(resource.Identity, resource.Domain, configuration).Error == IommuDomainError::None,
        pci.Claim(firstDomain) == IommuDeviceError::NotReady);

    const auto assigned = pci.Assign(driver.Identity, driver.Domain);
    std::atomic<bool> start{false};
    bool attached[2]{};
    IommuAttachmentToken attachments[2]{};
    IommuDomainFailure failures[2]{};
    const auto attach = [&](IommuDomain& domain, const Size index) noexcept
    {
        while (!start.load(std::memory_order_acquire))
        {
            start.wait(false, std::memory_order_acquire);
        }
        const auto result = domain.Attach(pci);
        attached[index] = result.HasValue();
        if (result.HasValue())
        {
            attachments[index] = result.GetValue().Attachment;
        }
        else
        {
            failures[index] = result.GetError();
        }
    };
    std::thread firstWorker{[&]() noexcept
                            {
                                attach(firstDomain, 0U);
                            }};
    std::thread secondWorker{[&]() noexcept
                             {
                                 attach(secondDomain, 1U);
                             }};
    start.store(true, std::memory_order_release);
    start.notify_all();
    firstWorker.join();
    secondWorker.join();

    const Size successfulAttachments = static_cast<Size>(attached[0U]) + static_cast<Size>(attached[1U]);
    IommuDomain& owner = attached[0U] ? firstDomain : secondDomain;
    IommuDomain& loser = attached[0U] ? secondDomain : firstDomain;
    const Size ownerIndex = attached[0U] ? 0U : 1U;
    const Size loserIndex = attached[0U] ? 1U : 0U;
    ASTERIA_EXPECT(runner, "IOMMU claim is unique and blocks premature driver teardown", assigned.HasValue(),
                   successfulAttachments == 1U, failures[loserIndex].Error == IommuDomainError::DeviceRejected,
                   failures[loserIndex].Device == IommuDeviceError::AlreadyClaimed,
                   pci.Release(loser) == IommuDeviceError::InvalidOwner,
                   pci.ReleaseAssignment(assigned.GetValue().Assignment).Error == PciFunctionError::IommuStillClaimed,
                   pci.Quiesce().Error == PciFunctionError::AssignmentBusy, pci.GetSnapshot().IommuClaimed);
    ASTERIA_EXPECT(runner, "IOMMU detach precedes assignment release and terminal quiescence",
                   owner.Detach(attachments[ownerIndex]).Error == IommuDomainError::None,
                   owner.Detach(attachments[ownerIndex]).Error == IommuDomainError::StaleAttachment,
                   firstDomain.Quiesce().Error == IommuDomainError::None,
                   secondDomain.Quiesce().Error == IommuDomainError::None,
                   pci.ReleaseAssignment(assigned.GetValue().Assignment).Error == PciFunctionError::None,
                   !pci.GetSnapshot().IommuClaimed, pci.Quiesce().Error == PciFunctionError::None, driver.Finish());
}

void TestConcurrentAssignment(TestContext& runner) noexcept
{
    RegistryFixture fixture;
    DomainFixture domain{fixture};
    PciFunction pci;
    constexpr Size WorkerCount{8U};
    DriverFixture drivers[WorkerCount]{
        DriverFixture{ResourceDomainId{101U}}, DriverFixture{ResourceDomainId{102U}},
        DriverFixture{ResourceDomainId{103U}}, DriverFixture{ResourceDomainId{104U}},
        DriverFixture{ResourceDomainId{105U}}, DriverFixture{ResourceDomainId{106U}},
        DriverFixture{ResourceDomainId{107U}}, DriverFixture{ResourceDomainId{108U}},
    };
    bool fixtureReady = pci.Initialize(fixture.Registry, domain.Identity, domain.Domain, MakeDescriptor()).Error ==
                        PciFunctionError::None;
    for (Size index{0U}; index < WorkerCount; ++index)
    {
        fixtureReady = drivers[index].Initialize(fixture, domain) && fixtureReady;
    }

    std::atomic<bool> start{false};
    std::atomic<Size> successes{0U};
    PciFunctionAssignmentReceipt receipts[WorkerCount]{};
    std::thread workers[WorkerCount];
    for (Size index{0U}; index < WorkerCount; ++index)
    {
        workers[index] = std::thread{[&, index]() noexcept
                                     {
                                         while (!start.load(std::memory_order_acquire))
                                         {
                                             start.wait(false, std::memory_order_acquire);
                                         }
                                         const auto result = pci.Assign(drivers[index].Identity, drivers[index].Domain);
                                         if (result.HasValue())
                                         {
                                             receipts[index] = result.GetValue();
                                             static_cast<void>(successes.fetch_add(1U, std::memory_order_acq_rel));
                                         }
                                     }};
    }
    start.store(true, std::memory_order_release);
    start.notify_all();
    for (std::thread& worker : workers)
    {
        worker.join();
    }

    bool releasedWinner{false};
    for (const PciFunctionAssignmentReceipt& receipt : receipts)
    {
        if (receipt.Assignment.IsValid())
        {
            releasedWinner = pci.ReleaseAssignment(receipt.Assignment).Error == PciFunctionError::None;
        }
    }

    constexpr Size SnapshotIterations{64U};
    std::atomic<bool> snapshotStart{false};
    std::atomic<bool> writerValid{true};
    std::atomic<bool> snapshotsValid{true};
    std::thread writer{[&]() noexcept
                       {
                           while (!snapshotStart.load(std::memory_order_acquire))
                           {
                               snapshotStart.wait(false, std::memory_order_acquire);
                           }
                           for (Size index{0U}; index < SnapshotIterations; ++index)
                           {
                               const auto result = pci.Assign(drivers[0U].Identity, drivers[0U].Domain);
                               if (!result.HasValue() || (pci.ReleaseAssignment(result.GetValue().Assignment).Error !=
                                                          PciFunctionError::None))
                               {
                                   writerValid.store(false, std::memory_order_release);
                                   return;
                               }
                           }
                       }};
    std::thread observer{[&]() noexcept
                         {
                             while (!snapshotStart.load(std::memory_order_acquire))
                             {
                                 snapshotStart.wait(false, std::memory_order_acquire);
                             }
                             for (Size index{0U}; index < SnapshotIterations; ++index)
                             {
                                 const auto snapshot = pci.GetSnapshot();
                                 const bool available = (snapshot.State == PciFunctionState::Available) &&
                                                        !snapshot.Driver.IsValid() && !snapshot.IommuClaimed;
                                 const bool assigned = (snapshot.State == PciFunctionState::Assigned) &&
                                                       snapshot.Driver.IsEqualTo(drivers[0U].Identity) &&
                                                       !snapshot.IommuClaimed;
                                 if (!available && !assigned)
                                 {
                                     snapshotsValid.store(false, std::memory_order_release);
                                     return;
                                 }
                             }
                         }};
    snapshotStart.store(true, std::memory_order_release);
    snapshotStart.notify_all();
    writer.join();
    observer.join();

    bool allCollected{true};
    for (Size index{0U}; index < WorkerCount; ++index)
    {
        allCollected = drivers[index].Finish() && allCollected;
    }
    ASTERIA_EXPECT(runner, "concurrent assignment publishes exactly one retained driver", fixtureReady,
                   successes.load(std::memory_order_acquire) == 1U, releasedWinner,
                   writerValid.load(std::memory_order_acquire), snapshotsValid.load(std::memory_order_acquire),
                   pci.GetSnapshot().State == PciFunctionState::Available, allCollected,
                   pci.Quiesce().Error == PciFunctionError::None);
}
} // Namespace

// A function's configuration space: its Command register with the status
// half set, as firmware leaves it, recording what the kernel writes.
class FakeConfiguration final : public Hal::Device::IPciConfigurationSpace, public Hal::Device::IPciConfigurationWriter
{
public:
    [[nodiscard]] Hal::Device::PciConfigurationReadError Read32(PciFunctionAddress, const Foundation::UInt16 offset,
                                                                Foundation::UInt32& output) noexcept override
    {
        if (offset == RefuseReadAt)
        {
            return Hal::Device::PciConfigurationReadError::HardwareFailure;
        }
        output = (offset == 0x04U) ? Command : Space[(offset / 4U) % 64U];
        return Hal::Device::PciConfigurationReadError::None;
    }

    [[nodiscard]] Hal::Device::PciConfigurationWriteError Write32(PciFunctionAddress, const Foundation::UInt16 offset,
                                                                  const Foundation::UInt32 value) noexcept override
    {
        if (Refuse)
        {
            return Hal::Device::PciConfigurationWriteError::HardwareFailure;
        }
        if (offset == 0x04U)
        {
            Command = (Command & 0xFFFF'0000U) | value;
            LastWritten = value;
        }
        else if (offset == 0x50U)
        {
            Space[offset / 4U] = (Space[offset / 4U] & 0xFFFFU) | (value & 0xFFFF'0000U);
        }
        else
        {
            Space[(offset / 4U) % 64U] = value;
        }
        ++Writes;
        return Hal::Device::PciConfigurationWriteError::None;
    }

    // The status half lists capabilities: a 64-bit MSI at 0x50.
    Foundation::UInt32 Command{0x0010'0006U};
    Foundation::UInt32 Space[64U]{0U, 0U, 0U,    0U, 0U, 0U, 0U, 0U, 0U, 0U,          0U,
                                  0U, 0U, 0x50U, 0U, 0U, 0U, 0U, 0U, 0U, 0x0080'0005U};
    Foundation::UInt32 LastWritten{0U};
    Size Writes{0U};
    /// Refuses every write while set.
    bool Refuse{false};
    /// The one offset whose reads fail; none by default.
    Foundation::UInt16 RefuseReadAt{0xFFFFU};
};

// The subtree of one ResourceDomain.
class OneDomain final : public Kernel::Resource::IResourceDomainSubtree
{
public:
    explicit constexpr OneDomain(const Kernel::Resource::ResourceDomain& domain) noexcept : m_domain{domain}
    {
    }

    [[nodiscard]] bool Contains(const Kernel::Resource::ResourceDomain* const domain) const noexcept override
    {
        return domain == &m_domain;
    }

private:
    const Kernel::Resource::ResourceDomain& m_domain;
};

// An assignment starts with bus mastering off; only the assignee may enable it, and only behind an
// attached IommuDomain; the Device Manager's revocation stands; the drain releases the assignment
// of a driver charged to the dying subtree.
void TestFunctionControl(TestContext& runner) noexcept
{
    using Kernel::Device::PciFunctionControl;
    using Kernel::Device::PciFunctionControlError;
    RegistryFixture fixture;
    DomainFixture resource{fixture};
    DriverFixture driver{ResourceDomainId{93U}};
    DriverFixture other{ResourceDomainId{94U}};
    PciFunction pci;
    NullIommuBackend backend;
    IommuDomain iommu{fixture.Registry, backend};
    FakeConfiguration configuration;
    PciFunctionControl control;
    const PciFunctionControlError unconfigured = control.EnableBusMaster(pci, ProtectionDomainId{});
    const bool ready = driver.Initialize(fixture, resource) && other.Initialize(fixture, resource) &&
                       (pci.Initialize(fixture.Registry, resource.Identity, resource.Domain, MakeDescriptor()).Error ==
                        PciFunctionError::None) &&
                       (iommu
                            .Initialize(resource.Identity, resource.Domain,
                                        IommuDomainConfiguration{.InputAddressBits = 48U, .Reserved = {}})
                            .Error == IommuDomainError::None) &&
                       control.Configure(configuration, configuration);
    const PciFunctionControlError assigned = control.Assign(pci, driver.Identity, driver.Domain);
    const Foundation::UInt32 afterAssign = configuration.Command;
    const PciFunctionControlError unprotected = control.EnableBusMaster(pci, driver.Identity);
    const PciFunctionControlError stranger = control.EnableBusMaster(pci, other.Identity);
    const PciFunctionControlError strangerAttach = control.Attach(pci, other.Identity, iommu);
    configuration.Command |= 0x0004U;
    const PciFunctionControlError attached = control.Attach(pci, driver.Identity, iommu);
    const Foundation::UInt32 afterAttach = configuration.Command;
    const PciFunctionControlError twice = control.Attach(pci, driver.Identity, iommu);
    const PciFunctionControlError enabled = control.EnableBusMaster(pci, driver.Identity);
    const Foundation::UInt32 afterEnable = configuration.Command;
    ASTERIA_EXPECT(
        runner, "only the assignee, behind an attached IommuDomain, makes the function a bus master",
        unconfigured == PciFunctionControlError::NotConfigured, ready, assigned == PciFunctionControlError::None,
        // Assign turns bus mastering off and INTx Disable on.
        (afterAssign & 0xFFFFU) == 0x0402U, (configuration.LastWritten >> 16U) == 0U,
        unprotected == PciFunctionControlError::IommuRequired, stranger == PciFunctionControlError::NotAssignee,
        strangerAttach == PciFunctionControlError::NotAssignee, attached == PciFunctionControlError::None,
        (afterAttach & 0x0004U) == 0U, pci.GetSnapshot().IommuClaimed,
        twice == PciFunctionControlError::AttachmentConflict, enabled == PciFunctionControlError::None,
        (afterEnable & 0xFFFFU) == 0x0406U);

    const PciFunctionControlError revoked = control.DisableBusMaster(pci, ProtectionDomainId{}, true);
    const Foundation::UInt32 afterRevoke = configuration.Command;
    const PciFunctionControlError reenable = control.EnableBusMaster(pci, driver.Identity);
    const PciFunctionControlError foreignDisable = control.DisableBusMaster(pci, other.Identity, false);
    ASTERIA_EXPECT(runner, "the Device Manager's revocation stands until a reassignment",
                   revoked == PciFunctionControlError::None, (afterRevoke & 0xFFFFU) == 0x0402U,
                   reenable == PciFunctionControlError::Revoked, foreignDisable == PciFunctionControlError::NotAssignee,
                   control.GetSnapshot().BusMasterRefusals == 3U, control.GetSnapshot().BusMasterRevocations == 1U);

    // Detaching and attaching again; a revoked function cannot attach.
    const PciFunctionControlError foreignDetach = control.Detach(pci, other.Identity, iommu);
    const PciFunctionControlError detached = control.Detach(pci, driver.Identity, iommu);
    const bool unclaimed = !pci.GetSnapshot().IommuClaimed;
    const PciFunctionControlError revokedAttach = control.Attach(pci, driver.Identity, iommu);
    const PciFunctionControlError reassigned = control.Assign(pci, driver.Identity, driver.Domain);
    const PciFunctionControlError reattached = control.Attach(pci, driver.Identity, iommu);
    ASTERIA_EXPECT(runner, "the assignee detaches its function, and a revoked one cannot attach",
                   foreignDetach == PciFunctionControlError::NotAssignee, detached == PciFunctionControlError::None,
                   unclaimed, revokedAttach == PciFunctionControlError::Revoked,
                   reassigned == PciFunctionControlError::None, reattached == PciFunctionControlError::None,
                   control.EnableBusMaster(pci, driver.Identity) == PciFunctionControlError::None);

    // The drain: bus mastering off and revoked, then the release, which
    // detaches from a surviving domain first.
    Kernel::Resource::ResourceDomainLedger unrelatedLedger;
    const Kernel::Resource::ResourceDomain unrelatedDomain{unrelatedLedger};
    const PciFunctionControlError untouched = control.ReleaseCharged(OneDomain{unrelatedDomain});
    const Size stillAssigned = control.GetSnapshot().Assigned;
    const PciFunctionControlError disabled = control.DisableCharged(OneDomain{resource.Domain});
    const Foundation::UInt32 afterDisable = configuration.Command;
    const PciFunctionControlError afterDrainEnable = control.EnableBusMaster(pci, driver.Identity);
    const PciFunctionControlError drained = control.ReleaseCharged(OneDomain{resource.Domain});
    ASTERIA_EXPECT(runner, "the drain stops, revokes, detaches and releases the dying driver's function",
                   untouched == PciFunctionControlError::None, stillAssigned == 1U,
                   disabled == PciFunctionControlError::None, (afterDisable & 0x0004U) == 0U,
                   afterDrainEnable == PciFunctionControlError::Revoked, drained == PciFunctionControlError::None,
                   control.GetSnapshot().Assigned == 0U, control.GetSnapshot().Releases == 2U,
                   (configuration.Command & 0x0004U) == 0U, !pci.GetSnapshot().IommuClaimed,
                   pci.GetSnapshot().State == PciFunctionState::Available);

    // : the drained function is quarantined until its
    // Device Manager confirms it quiet for the drained assignment.
    const Foundation::UInt64 drainedSequence = pci.GetSnapshot().LastAssignmentSequence;
    const bool wasQuarantined = pci.GetSnapshot().Quarantined;
    const PciFunctionControlError quarantined = control.Assign(pci, driver.Identity, driver.Domain);
    const PciFunctionControlError stale = control.ConfirmQuiesced(pci, drainedSequence - 1U);
    const PciFunctionControlError confirmed = control.ConfirmQuiesced(pci, drainedSequence);
    const PciFunctionControlError repeated = control.ConfirmQuiesced(pci, drainedSequence);
    ASTERIA_EXPECT(runner, "a drained function is assigned again only after its quarantine is lifted", wasQuarantined,
                   quarantined == PciFunctionControlError::Quarantined,
                   stale == PciFunctionControlError::QuarantineMismatch, confirmed == PciFunctionControlError::None,
                   repeated == PciFunctionControlError::QuarantineMismatch, !pci.GetSnapshot().Quarantined,
                   control.GetSnapshot().Quarantines == 1U, control.GetSnapshot().Confirmations == 1U);

    // Quiescing a domain stops every function attached to it first.
    const PciFunctionControlError lastAssign = control.Assign(pci, driver.Identity, driver.Domain);
    const PciFunctionControlError lastAttach = control.Attach(pci, driver.Identity, iommu);
    const PciFunctionControlError lastEnable = control.EnableBusMaster(pci, driver.Identity);
    const bool quiesced = control.QuiesceDomain(iommu);
    const PciFunctionControlError orphanEnable = control.EnableBusMaster(pci, driver.Identity);
    ASTERIA_EXPECT(runner, "quiescing a domain stops its functions' bus mastering and forgets them",
                   lastAssign == PciFunctionControlError::None, lastAttach == PciFunctionControlError::None,
                   lastEnable == PciFunctionControlError::None, quiesced, (configuration.Command & 0x0004U) == 0U,
                   iommu.GetSnapshot().State == IommuDomainState::Quiesced, !pci.GetSnapshot().IommuClaimed,
                   orphanEnable == PciFunctionControlError::IommuRequired, control.QuiesceDomain(iommu),
                   control.Release(pci) == PciFunctionControlError::None);
}

// A drain parks the drained IommuDomain's Frame pins with the function it quarantines, since no one
// can prove the dead driver's DMA ended; only the Device Manager's confirmation of that drained
// assignment releases them, and a Frame its owner destroys meanwhile stays uncollected. A domain
// whose mappings do not fit the table is not quiesced. Records what a confirmation released.
class RecordingObserver final : public Kernel::Device::IParkedPinObserver
{
public:
    void Released(const Kernel::Object::ObjectId frame, const Kernel::Memory::PhysicalAddress physicalStart,
                  const Kernel::Memory::PageCount pages) noexcept override
    {
        ++Count;
        Frame = frame;
        PhysicalStart = physicalStart.GetValue();
        Pages = pages.GetValue();
    }

    Foundation::Size Count{0U};
    Kernel::Object::ObjectId Frame{};
    Foundation::UInt64 PhysicalStart{0U};
    Foundation::UInt64 Pages{0U};
};

void TestParkedStorage(TestContext& runner, const bool retry = false, const bool retired = false,
                       const bool normal = false, const bool ambiguous = false) noexcept
{
    using Kernel::Device::PciFunctionControl;
    using Kernel::Device::PciFunctionControlError;
    RegistryFixture fixture;
    DomainFixture resource{fixture};
    DriverFixture driver{ResourceDomainId{93U}};
    PciFunction pci;
    NullIommuBackend backend;
    IommuDomain iommu{fixture.Registry, backend};
    FakeConfiguration configuration;
    PciFunctionControl control;
    Kernel::Memory::BootFrame frame;
    auto descriptor = MakeDescriptor();
    if (!retired && !ambiguous)
    {
        descriptor.ClassCode = 0xFFU;
    }
    const bool ready =
        driver.Initialize(fixture, resource) &&
        (pci.Initialize(fixture.Registry, resource.Identity, resource.Domain, descriptor).Error ==
         PciFunctionError::None) &&
        (iommu
             .Initialize(resource.Identity, resource.Domain,
                         IommuDomainConfiguration{.InputAddressBits = 48U, .Reserved = {}})
             .Error == IommuDomainError::None) &&
        control.Configure(configuration, configuration) &&
        (frame.Initialize(ResourceDomainId{71U}, Kernel::Memory::FrameDescriptor{
                                                     .PhysicalStart = Kernel::Memory::PhysicalAddress{0x0010'0000U},
                                                     .Pages = Kernel::Memory::PageCount{64U},
                                                     .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                                                     .Kind = Kernel::Memory::FrameKind::Ram,
                                                     .Reserved = {}}) == Kernel::Memory::FrameError::None);
    const auto frameObject = fixture.Issuer.CreateObject(frame);
    const Kernel::Memory::FrameId frameId = frameObject.HasValue()
                                                ? Kernel::Memory::FrameId{fixture.Registry, frameObject.GetValue()}
                                                : Kernel::Memory::FrameId{};
    const auto request = [&](const Foundation::UInt64 dma, const Foundation::UInt64 offset)
    {
        return Kernel::Device::IommuMappingRequest{
            .FrameStorage = &frame,
            .FrameIdentity = frameId,
            .DmaStart = Kernel::Device::DmaAddress{dma},
            .FrameOffsetPages = Kernel::Memory::PageCount{offset},
            .Pages = Kernel::Memory::PageCount{1U},
            .Permissions = Kernel::Device::DmaPermissions{Kernel::Device::DmaPermission::Read}.With(
                Kernel::Device::DmaPermission::Write),
            .Reserved = {}};
    };
    const PciFunctionControlError assigned = control.Assign(pci, driver.Identity, driver.Domain);
    const Foundation::UInt64 sequence = pci.GetSnapshot().LastAssignmentSequence;
    // A domain holds at most MaximumMappings, and the table holds every domain's: a full domain's
    // drain fits.
    static_assert(PciFunctionControl::ParkedCapacity ==
                  Kernel::Device::IommuDomainPool::Capacity * IommuDomain::MaximumMappings);
    bool mappedAll{true};
    for (Foundation::UInt64 index{0U}; index < IommuDomain::MaximumMappings; ++index)
    {
        mappedAll = iommu.Map(request(0x0020'0000U + (index << 12U), index)).HasValue() && mappedAll;
    }
    const bool beyond = !iommu.Map(request(0x0020'0000U + (IommuDomain::MaximumMappings << 12U), 0U)).HasValue();
    backend.AttachFails = ambiguous;
    const PciFunctionControlError attached = control.Attach(pci, driver.Identity, iommu);
    const PciFunctionControlError disabled = control.DisableCharged(OneDomain{resource.Domain});
    const auto full = iommu.GetSnapshot();
    bool unmapped{true};
    for (Foundation::UInt64 index{1U}; !ambiguous && (index < IommuDomain::MaximumMappings); ++index)
    {
        unmapped = (iommu.UnmapAt(Kernel::Device::DmaAddress{0x0020'0000U + (index << 12U)}).Error ==
                    IommuDomainError::None) &&
                   unmapped;
    }
    ASTERIA_EXPECT(runner, "a domain holds at most MaximumMappings, all of which the parked table holds", ready,
                   frameObject.HasValue(), assigned == PciFunctionControlError::None,
                   attached == (ambiguous ? PciFunctionControlError::IommuRejected : PciFunctionControlError::None),
                   mappedAll, beyond, disabled == PciFunctionControlError::None,
                   full.State == (ambiguous ? IommuDomainState::Faulted : IommuDomainState::Ready),
                   full.MappingCount == IommuDomain::MaximumMappings, control.GetSnapshot().ParkedPins == 0U, unmapped);

    const Foundation::UInt64 retained = (retired || ambiguous) ? IommuDomain::MaximumMappings : 1U;
    if (ambiguous)
    {
        ASTERIA_EXPECT(runner, "ambiguously exposed storage still refuses a bare quiesce",
                       iommu.Quiesce().Error == IommuDomainError::ParkingRefused,
                       iommu.GetSnapshot().MappingCount == retained, backend.ReleaseCalls == 0U);
    }
    if (retired)
    {
        ASTERIA_EXPECT(runner, "normal AHCI Unmap removes translation but retains every DMA pin",
                       iommu.UnmapAt(Kernel::Device::DmaAddress{0x0020'0000U}).Error == IommuDomainError::None,
                       iommu.GetSnapshot().MappingCount == retained);
        bool reusable{true};
        for (Foundation::Size cycle{0U}; cycle < 100U; ++cycle)
        {
            const auto remapped = iommu.Map(request(0x0020'0000U, 0U));
            reusable = remapped.HasValue() && reusable;
            if (remapped.HasValue())
            {
                reusable = (iommu.Unmap(remapped.GetValue().Mapping).Error == IommuDomainError::None) && reusable;
                reusable =
                    (iommu.Unmap(remapped.GetValue().Mapping).Error == IommuDomainError::StaleMapping) && reusable;
            }
        }
        ASTERIA_EXPECT(runner, "retired storage can be remapped at capacity without releasing its pin or alias policy",
                       reusable, iommu.GetSnapshot().MappingCount == retained,
                       control.Detach(pci, driver.Identity, iommu) == PciFunctionControlError::DmaPending);
        Kernel::Memory::BootFrame alias;
        NullIommuBackend aliasBackend;
        IommuDomain aliasDomain{fixture.Registry, aliasBackend};
        ASTERIA_CHECK(runner, alias.Initialize(ResourceDomainId{71U}, frame.GetSnapshot().Descriptor) ==
                                  Kernel::Memory::FrameError::None);
        const auto aliasObject = fixture.Issuer.CreateObject(alias);
        ASTERIA_CHECK(runner, aliasObject.HasValue());
        ASTERIA_CHECK(
            runner,
            aliasDomain.Initialize(resource.Identity, resource.Domain, {.InputAddressBits = 48U, .Reserved = {}})
                    .Error == IommuDomainError::None);
        auto aliasRequest = request(0x0040'0000U, 0U);
        aliasRequest.FrameStorage = &alias;
        aliasRequest.FrameIdentity = Kernel::Memory::FrameId{fixture.Registry, aliasObject.GetValue()};
        const auto aliasMapped = aliasDomain.Map(aliasRequest);
        ASTERIA_EXPECT(runner, "retired DMA policy blocks a second Frame naming the same physical pages",
                       !aliasMapped.HasValue(), aliasMapped.GetError().Error == IommuDomainError::FramePolicyRejected);
        ASTERIA_CHECK(runner, aliasDomain.Quiesce().Error == IommuDomainError::None);
        ASTERIA_CHECK(runner, DestroyAndCollect(fixture, aliasObject.GetValue(), alias));
        ASTERIA_CHECK(runner, fixture.Issuer.DestroyObject(frameObject.GetValue()) == ObjectRegistryError::Ok);
        ASTERIA_EXPECT(runner, "a destroyed Frame remains uncollectable after normal Unmap",
                       !fixture.Issuer.CollectDestroyedObject(frameObject.GetValue()).HasValue());
    }
    if (retry)
    {
        const Size releases = backend.ReleaseCalls;
        configuration.Refuse = true;
        ASTERIA_EXPECT(
            runner, "failed bus-master-off prevents backend release and retains the retry association",
            !(normal ? control.QuiesceDomain(iommu) : control.QuiesceDrainedDomain(iommu, OneDomain{resource.Domain})),
            backend.ReleaseCalls == releases, iommu.GetSnapshot().MappingCount == retained);
        configuration.Refuse = false;
        backend.ReleaseFails = true;
        ASTERIA_EXPECT(
            runner, "failed backend Release keeps mappings and assignment for the retry",
            !(normal ? control.QuiesceDomain(iommu) : control.QuiesceDrainedDomain(iommu, OneDomain{resource.Domain})),
            iommu.GetSnapshot().MappingCount == retained, pci.GetSnapshot().IommuClaimed,
            control.GetSnapshot().ParkedPins == 0U);
        backend.ReleaseFails = false;
    }

    // One mapping left: the drain parks its pin and quarantines the function.
    const bool quiesced =
        normal ? control.QuiesceDomain(iommu) : control.QuiesceDrainedDomain(iommu, OneDomain{resource.Domain});
    const PciFunctionControlError released =
        normal ? control.Release(pci) : control.ReleaseCharged(OneDomain{resource.Domain});
    const auto parked = control.GetSnapshot();
    const bool quarantined = pci.GetSnapshot().Quarantined;
    const bool destroyed =
        retired ||
        (frameObject.HasValue() && (fixture.Issuer.DestroyObject(frameObject.GetValue()) == ObjectRegistryError::Ok));
    const auto early = fixture.Issuer.CollectDestroyedObject(frameObject.GetValue());
    RecordingObserver observer;
    control.SetParkedPinObserver(&observer);
    const PciFunctionControlError stale = control.ConfirmQuiesced(pci, sequence - 1U);
    const Foundation::Size stillParked = control.GetSnapshot().ParkedPins;
    const Foundation::Size observedEarly = observer.Count;
    const PciFunctionControlError confirmed = control.ConfirmQuiesced(pci, sequence);
    const auto lifted = control.GetSnapshot();
    const auto collected = fixture.Issuer.CollectDestroyedObject(frameObject.GetValue());
    ASTERIA_EXPECT(runner, "the drain parks the pin until the Device Manager confirms the drained assignment", quiesced,
                   iommu.GetSnapshot().State == IommuDomainState::Quiesced, released == PciFunctionControlError::None,
                   parked.ParkedPins == retained, parked.PinsParked == retained, parked.PinsReleased == 0U, quarantined,
                   destroyed, !early.HasValue(), stale == PciFunctionControlError::QuarantineMismatch,
                   stillParked == retained, confirmed == PciFunctionControlError::None, lifted.ParkedPins == 0U,
                   lifted.PinsReleased == retained, collected.HasValue() && (collected.GetValue().Instance == &frame));
    ASTERIA_EXPECT(runner, "the observer hears of the released pin's Frame and its mapped page only",
                   observedEarly == 0U, observer.Count == retained,
                   frameObject.HasValue() && (observer.Frame.GetValue() == frameObject.GetValue().GetValue()),
                   observer.PhysicalStart == ((retired || ambiguous) ? 0x0013'F000U : 0x0010'0000U),
                   observer.Pages == 1U);
}

// The assignee programs its function's MSI, one Interrupt's worth, INTx disabled; the function
// cannot be released or reassigned until the messages are off.
void TestFunctionMessages(TestContext& runner) noexcept
{
    using Kernel::Device::PciFunctionControl;
    using Kernel::Device::PciFunctionControlError;
    RegistryFixture fixture;
    DomainFixture resource{fixture};
    DriverFixture driver{ResourceDomainId{95U}};
    DriverFixture other{ResourceDomainId{96U}};
    PciFunction pci;
    FakeConfiguration configuration;
    PciFunctionControl control;
    const bool ready = driver.Initialize(fixture, resource) && other.Initialize(fixture, resource) &&
                       (pci.Initialize(fixture.Registry, resource.Identity, resource.Domain, MakeDescriptor()).Error ==
                        PciFunctionError::None) &&
                       control.Configure(configuration, configuration) &&
                       (control.Assign(pci, driver.Identity, driver.Domain) == PciFunctionControlError::None);
    const PciFunctionControlError stranger = control.EnableMessages(pci, other.Identity, 0xFEE0'0000U, 0x41U);
    const PciFunctionControlError enabled = control.EnableMessages(pci, driver.Identity, 0xFEE0'0000U, 0x41U);
    const PciFunctionControlError twice = control.EnableMessages(pci, driver.Identity, 0xFEE0'0000U, 0x42U);
    const PciFunctionControlError held = control.Release(pci);
    const PciFunctionControlError reassigned = control.Assign(pci, other.Identity, other.Domain);
    ASTERIA_EXPECT(runner, "the assignee's MSI is programmed once, INTx disabled, and holds the function", ready,
                   stranger == PciFunctionControlError::NotAssignee, enabled == PciFunctionControlError::None,
                   configuration.Space[0x54U / 4U] == 0xFEE0'0000U, configuration.Space[0x5CU / 4U] == 0x41U,
                   ((configuration.Space[0x50U / 4U] >> 16U) & 1U) == 1U, (configuration.Command & 0x0400U) != 0U,
                   twice == PciFunctionControlError::MessagesInUse, held == PciFunctionControlError::MessagesInUse,
                   reassigned == PciFunctionControlError::MessagesInUse);

    const PciFunctionControlError disabled = control.DisableMessages(pci);
    const PciFunctionControlError again = control.DisableMessages(pci);
    const PciFunctionControlError released = control.Release(pci);
    configuration.Space[0x50U / 4U] |= 0x0001'0000U;
    const PciFunctionControlError assignedClean = control.Assign(pci, other.Identity, other.Domain);
    ASTERIA_EXPECT(runner, "messages off, the function is released; a new assignment starts with MSI off",
                   disabled == PciFunctionControlError::None, ((configuration.Space[0x50U / 4U] >> 16U) & 1U) == 0U,
                   (configuration.Command & 0x0400U) != 0U, again == PciFunctionControlError::NotAssigned,
                   released == PciFunctionControlError::None, assignedClean == PciFunctionControlError::None,
                   ((configuration.Space[0x50U / 4U] >> 16U) & 1U) == 0U,
                   control.Release(pci) == PciFunctionControlError::None);
}

/// Hands out vectors 0x40 and 0x41, each raised by the message 0xFEE00000
/// with the vector as data.
class FakeVectors final : public Hal::Interrupt::IMessageVectors
{
public:
    [[nodiscard]] Hal::Interrupt::MessageTarget Bind(Kernel::Interrupt::Interrupt& interrupt) noexcept override
    {
        for (Size index{0U}; index < 2U; ++index)
        {
            if (Vectors[index] == nullptr)
            {
                Vectors[index] = &interrupt;
                Bound = (index == 0U) ? &interrupt : Bound;
                return Hal::Interrupt::MessageTarget{.Vector = 0x40U + index,
                                                     .Address = 0xFEE0'0000U,
                                                     .Data = static_cast<Foundation::UInt16>(0x40U + index),
                                                     .Reserved = {}};
            }
        }
        return {};
    }

    [[nodiscard]] bool Unbind(const Foundation::UInt64 vector,
                              const Kernel::Interrupt::Interrupt& interrupt) noexcept override
    {
        if ((vector < 0x40U) || (vector > 0x41U) || (Vectors[vector - 0x40U] != &interrupt))
        {
            return false;
        }
        Vectors[vector - 0x40U] = nullptr;
        Bound = (vector == 0x40U) ? nullptr : Bound;
        ++Unbinds;
        return true;
    }

    Kernel::Interrupt::Interrupt* Vectors[2U]{};
    Kernel::Interrupt::Interrupt* Bound{nullptr};
    Size Unbinds{0U};
};

// An Interrupt is created only for the function's assignee, programs its MSI and binds a vector; it
// is armed, takes a message, is acknowledged, and its destruction turns the messages off before
// Collect frees the vector and the slot.
void TestInterruptPool(TestContext& runner) noexcept
{
    using Kernel::Device::PciFunctionControl;
    using Kernel::Device::PciFunctionControlError;
    using Kernel::Interrupt::InterruptCreateRequest;
    using Kernel::Interrupt::InterruptPool;
    using Kernel::Object::CapabilityHandle;
    using Kernel::Object::CapabilityRight;
    using Kernel::Object::CapabilityRights;
    using Kernel::Object::MonotonicTime;
    using Kernel::Object::ObjectPoolError;
    static RegistryFixture fixture;
    static DomainFixture resource{fixture};
    static DriverFixture driver{ResourceDomainId{97U}};
    static DriverFixture other{ResourceDomainId{98U}};
    static PciFunction pci;
    static FakeConfiguration configuration;
    static PciFunctionControl control;
    static FakeVectors vectors;
    static InterruptPool pool{fixture.Registry, fixture.Issuer};
    const bool ready = driver.Initialize(fixture, resource) && other.Initialize(fixture, resource) &&
                       (pci.Initialize(fixture.Registry, resource.Identity, resource.Domain, MakeDescriptor()).Error ==
                        PciFunctionError::None) &&
                       control.Configure(configuration, configuration) &&
                       (control.Assign(pci, driver.Identity, driver.Domain) == PciFunctionControlError::None);
    const auto pciObject = fixture.Issuer.CreateObject(pci);
    const auto root = fixture.Revocations.CreateRoot();
    const auto mint = [&](const ObjectId object, const CapabilityRights rights) noexcept
    {
        const auto minted =
            fixture.Issuer.Mint(fixture.Capabilities, object, rights, root.GetValue(), MonotonicTime::Infinite(), 0U);
        return minted.HasValue() ? minted.GetValue() : CapabilityHandle{};
    };
    const CapabilityHandle authority = mint(resource.Object, CapabilityRights{CapabilityRight::Manage});
    const CapabilityHandle function = pciObject.HasValue()
                                          ? mint(pciObject.GetValue(), CapabilityRights{CapabilityRight::Invoke})
                                          : CapabilityHandle{};
    const auto request = [&](const ProtectionDomainId caller, const Foundation::UInt64 source = 0U) noexcept
    {
        return InterruptCreateRequest{.Space = &fixture.Capabilities,
                                      .Authority = authority,
                                      .Function = function,
                                      .Caller = caller,
                                      .Source = source};
    };
    const Foundation::UInt64 baseline = resource.Domain.GetSnapshot().Usage.KernelMetadataBytes;
    const auto unconfigured = pool.Create(request(driver.Identity));
    const bool configured = pool.Configure(vectors, control);
    const auto stranger = pool.Create(request(other.Identity));
    const auto otherSource = pool.Create(request(driver.Identity, 1U));
    const bool cleanAfterRefusal = (vectors.Bound == nullptr) && (pool.GetSnapshot().ActiveSlots == 0U) &&
                                   (resource.Domain.GetSnapshot().Usage.KernelMetadataBytes == baseline);
    const auto created = pool.Create(request(driver.Identity));
    const auto twice = pool.Create(request(driver.Identity));
    ASTERIA_EXPECT(runner, "only the assignee's Interrupt is created, programming the MSI and binding a vector", ready,
                   pciObject.HasValue(), root.HasValue(), authority.IsValid(), function.IsValid(),
                   !unconfigured.HasValue() && (unconfigured.GetError().Error == ObjectPoolError::InvalidConfiguration),
                   configured, !stranger.HasValue() && (stranger.GetError().Error == ObjectPoolError::NotOwned),
                   !otherSource.HasValue() && (otherSource.GetError().Error == ObjectPoolError::InvalidRequest),
                   cleanAfterRefusal, created.HasValue(), vectors.Bound != nullptr,
                   configuration.Space[0x54U / 4U] == 0xFEE0'0000U, configuration.Space[0x5CU / 4U] == 0x40U,
                   ((configuration.Space[0x50U / 4U] >> 16U) & 1U) == 1U,
                   !twice.HasValue() && (twice.GetError().Error == ObjectPoolError::Busy),
                   resource.Domain.GetSnapshot().Usage.KernelMetadataBytes ==
                       (baseline + InterruptPool::GetReservation().KernelMetadataBytes));
    if (!created.HasValue() || (vectors.Bound == nullptr))
    {
        return;
    }

    const CapabilityHandle interrupt = created.GetValue().Capability;
    const auto armed = pool.Control(fixture.Capabilities, interrupt, InterruptPool::Rearm);
    const auto delivered = vectors.Bound->HandleInterrupt();
    const auto readiness = pool.FindReadiness(fixture.Capabilities, interrupt);
    const bool signaled = readiness.HasValue() && readiness.GetValue().Notification->GetSnapshot().Pending;
    const auto acknowledged = pool.Control(fixture.Capabilities, interrupt, InterruptPool::KeepMasked);
    const auto badOperation = pool.Control(fixture.Capabilities, interrupt, 3U);
    ASTERIA_EXPECT(runner, "an armed Interrupt takes a message, signals its Notification and is acknowledged",
                   armed.HasValue() && (armed.GetValue() == 0U), delivered.HasValue() && delivered.GetValue().NewEvent,
                   readiness.HasValue(), signaled, acknowledged.HasValue() && (acknowledged.GetValue() == 1U),
                   !badOperation.HasValue());

    const PciFunctionControlError held = control.Release(pci);
    const auto destroyed = pool.Destroy(fixture.Capabilities, interrupt);
    const bool messagesOff = ((configuration.Space[0x50U / 4U] >> 16U) & 1U) == 0U;
    const bool stillBound = vectors.Bound != nullptr;
    const auto collected = pool.Collect();
    ASTERIA_EXPECT(runner, "destruction turns the messages off; Collect frees the vector, the slot and the charge",
                   held == PciFunctionControlError::MessagesInUse, destroyed.HasValue(), messagesOff, stillBound,
                   collected.HasValue() && (collected.GetValue() == 1U), vectors.Bound == nullptr,
                   vectors.Unbinds == 3U, resource.Domain.GetSnapshot().Usage.KernelMetadataBytes == baseline,
                   control.Release(pci) == PciFunctionControlError::None);

    // A drain whose message disable is refused leaves the Interrupt masked
    // and Busy, not destroyed; the retry, once the write goes, stops it.
    const bool reassigned = control.Assign(pci, driver.Identity, driver.Domain) == PciFunctionControlError::None;
    const auto again = pool.Create(request(driver.Identity));
    configuration.Refuse = true;
    const auto refused = pool.DestroyCharged(OneDomain{resource.Domain});
    const bool kept = (pool.GetSnapshot().ActiveSlots == 1U) && (pool.GetSnapshot().QuiescingSlots == 0U);
    configuration.Refuse = false;
    const auto retried = pool.DestroyCharged(OneDomain{resource.Domain});
    const bool messagesStopped = ((configuration.Space[0x50U / 4U] >> 16U) & 1U) == 0U;
    const auto reclaimed = pool.Collect();
    ASTERIA_EXPECT(runner, "a refused message disable makes the drain Busy and its retry finishes the stop", reassigned,
                   again.HasValue(), refused.Error == ObjectPoolError::Busy, kept,
                   retried.Error == ObjectPoolError::None, messagesStopped,
                   reclaimed.HasValue() && (reclaimed.GetValue() == 1U),
                   control.Release(pci) == PciFunctionControlError::None);
}

void TestFirmwareReservations(TestContext& runner) noexcept
{
    using Kernel::Device::PciFunctionControl;
    using Kernel::Device::PciFunctionControlError;
    RegistryFixture fixture;
    DomainFixture resource{fixture};
    DriverFixture driver{ResourceDomainId{81U}};
    PciFunction usb;
    PciFunction display;
    PciFunction bridge;
    auto descriptor = MakeDescriptor();
    descriptor.ClassCode = 3U;
    auto bridgeDescriptor = MakeDescriptor();
    bridgeDescriptor.HeaderLayout = 1U;
    FakeConfiguration configuration;
    PciFunctionControl control;
    const bool ready =
        driver.Initialize(fixture, resource) &&
        (usb.Initialize(fixture.Registry, resource.Identity, resource.Domain, MakeDescriptor()).Error ==
         PciFunctionError::None) &&
        (display.Initialize(fixture.Registry, resource.Identity, resource.Domain, descriptor).Error ==
         PciFunctionError::None) &&
        (bridge.Initialize(fixture.Registry, resource.Identity, resource.Domain, bridgeDescriptor).Error ==
         PciFunctionError::None) &&
        control.Configure(configuration, configuration);
    ASTERIA_EXPECT(runner, "firmware DMA and display assignments return distinct errors before any configuration write",
                   ready, usb.ReserveFirmwareDma(), usb.GetAssignmentRestriction() == PciFunctionError::ReservedMemory,
                   display.GetAssignmentRestriction() == PciFunctionError::DisplayReserved,
                   control.Assign(usb, driver.Identity, driver.Domain) == PciFunctionControlError::ReservedMemory,
                   control.Assign(display, driver.Identity, driver.Domain) == PciFunctionControlError::DisplayReserved,
                   bridge.GetAssignmentRestriction() == PciFunctionError::BridgeReserved,
                   control.Assign(bridge, driver.Identity, driver.Domain) == PciFunctionControlError::BridgeReserved,
                   configuration.Writes == 0U, usb.GetSnapshot().LastAssignmentSequence == 0U,
                   display.GetSnapshot().LastAssignmentSequence == 0U);
    const auto reserved = usb.Assign(driver.Identity, driver.Domain);
    const auto forwarding = bridge.Assign(driver.Identity, driver.Domain);
    const auto scanout = display.Assign(driver.Identity, driver.Domain);
    ASTERIA_EXPECT(runner, "direct assignment cannot bypass the firmware or display restriction",
                   !forwarding.HasValue(), forwarding.GetError().Error == PciFunctionError::BridgeReserved,
                   !reserved.HasValue(), reserved.GetError().Error == PciFunctionError::ReservedMemory,
                   !scanout.HasValue(), scanout.GetError().Error == PciFunctionError::DisplayReserved);
    // The trusted-driver profile never reaches a function nothing may assign.
    PciFunction ordinary;
    const bool ordinaryReady =
        ordinary.Initialize(fixture.Registry, resource.Identity, resource.Domain, MakeDescriptor()).Error ==
        PciFunctionError::None;
    const bool untrusted = !ordinary.HasTrustedDma();
    const bool granted = ordinary.GrantTrustedDma();
    const auto assigned = ordinary.Assign(driver.Identity, driver.Domain);
    const bool whileAssigned = ordinary.GrantTrustedDma();
    ASTERIA_EXPECT(runner, "trusted DMA is granted before assignment, never to a reserved, bridge or display function",
                   ordinaryReady, untrusted, granted, ordinary.HasTrustedDma(), !usb.GrantTrustedDma(),
                   !display.GrantTrustedDma(), !bridge.GrantTrustedDma(), !usb.HasTrustedDma(), !bridge.HasTrustedDma(),
                   assigned.HasValue(), !whileAssigned,
                   assigned.HasValue() &&
                       (ordinary.ReleaseAssignment(assigned.GetValue().Assignment).Error == PciFunctionError::None),
                   ordinary.Quiesce().Error == PciFunctionError::None);
    ASTERIA_EXPECT(runner, "reserved functions can be quiesced without ever becoming driver DMA endpoints",
                   usb.Quiesce().Error == PciFunctionError::None, display.Quiesce().Error == PciFunctionError::None,
                   driver.Domain.Quiesce() == ProtectionDomainError::None);
}

// The assignee of a USB 1.1/2.0 host controller reads the dwords that place its registers and its
// EHCI EECP list or UHCI LEGSUP, and writes only what relinquishes firmware's legacy support. The
// Capability list (status bit 20, pointer 0x34 with its reserved low bits set) holds power
// management, ID 1, at 0x50 and MSI at 0x40; the EHCI legacy capability, also ID 1, is at 0x68
// outside it, with the BIOS semaphore set.
void TestUsbLegacySupport(TestContext& runner) noexcept
{
    using Kernel::Device::PciFunctionControl;
    using Kernel::Device::PciFunctionControlError;
    const auto describe =
        [](const Foundation::UInt8 device, const Foundation::UInt8 subclass, const Foundation::UInt8 interface) noexcept
    {
        PciFunctionDescriptor descriptor = MakeDescriptor(0U, 0U, device, 0U);
        descriptor.ClassCode = 0x0CU;
        descriptor.Subclass = subclass;
        descriptor.ProgrammingInterface = interface;
        return descriptor;
    };
    RegistryFixture fixture;
    DomainFixture resource{fixture};
    DriverFixture driver{ResourceDomainId{95U}};
    DriverFixture other{ResourceDomainId{96U}};
    PciFunction ehci;
    PciFunction uhci;
    PciFunction ohci;
    PciFunction smbus;
    PciFunction bypass;
    FakeConfiguration configuration;
    configuration.Space[0x00U / 4U] = 0x293A'8086U;
    configuration.Space[0x08U / 4U] = 0x0C03'2003U;
    configuration.Space[0x10U / 4U] = 0xF7DF'A400U;
    configuration.Space[0x34U / 4U] = 0x53U;
    configuration.Space[0x3CU / 4U] = 0x0000'010BU;
    configuration.Space[0x40U / 4U] = 0x0000'0305U;
    configuration.Space[0x50U / 4U] = 0x0000'4301U;
    configuration.Space[0x68U / 4U] = 0x0001'0001U;
    configuration.Space[0x6CU / 4U] = 0xE000'E03FU;
    configuration.Space[0xC0U / 4U] = 0x1234'8F3FU;
    configuration.Space[0xF8U / 4U] = 0x0000'0001U;
    configuration.Space[0xFCU / 4U] = 0x5A5A'0FFCU;
    PciFunctionControl control;
    Foundation::UInt32 value{0U};
    const PciFunctionControlError unconfigured =
        control.AccessLegacySupport(ehci, driver.Identity, 0U, false, 0U, value);
    const bool ready =
        driver.Initialize(fixture, resource) && other.Initialize(fixture, resource) &&
        (ehci.Initialize(fixture.Registry, resource.Identity, resource.Domain, describe(1U, 3U, 0x20U)).Error ==
         PciFunctionError::None) &&
        (uhci.Initialize(fixture.Registry, resource.Identity, resource.Domain, describe(2U, 3U, 0x00U)).Error ==
         PciFunctionError::None) &&
        (ohci.Initialize(fixture.Registry, resource.Identity, resource.Domain, describe(3U, 3U, 0x10U)).Error ==
         PciFunctionError::None) &&
        (smbus.Initialize(fixture.Registry, resource.Identity, resource.Domain, describe(4U, 5U, 0x00U)).Error ==
         PciFunctionError::None) &&
        (bypass.Initialize(fixture.Registry, resource.Identity, resource.Domain, describe(5U, 3U, 0x20U)).Error ==
         PciFunctionError::None) &&
        control.Configure(configuration, configuration);
    const PciFunctionControlError unassigned = control.AccessLegacySupport(ehci, driver.Identity, 0U, false, 0U, value);
    // The Device Manager reads a function no driver holds (its quiet proof
    // after a drain), the dwords a driver reads and no others.
    PciFunctionControl unconfiguredManager;
    const PciFunctionControlError managerUnconfigured =
        unconfiguredManager.ReadReleasedConfiguration(ehci, 0x68U, value);
    Foundation::UInt32 managed{0U};
    const PciFunctionControlError managerLegacy = control.ReadReleasedConfiguration(ehci, 0x68U, managed);
    const Foundation::UInt32 managedLegacy = managed;
    const PciFunctionControlError managerLegsup = control.ReadReleasedConfiguration(uhci, 0xC0U, managed);
    const Foundation::UInt32 managedLegsup = managed;
    ASTERIA_EXPECT(runner, "the manager reads an unassigned controller's whitelisted dwords only",
                   managerUnconfigured == PciFunctionControlError::NotConfigured,
                   managerLegacy == PciFunctionControlError::None, managedLegacy == 0x0001'0001U,
                   managerLegsup == PciFunctionControlError::None, managedLegsup == 0x1234'8F3FU,
                   control.ReadReleasedConfiguration(ohci, 0x40U, managed) == PciFunctionControlError::AccessDenied,
                   control.ReadReleasedConfiguration(ehci, 0x04U, managed) == PciFunctionControlError::AccessDenied,
                   control.ReadReleasedConfiguration(ehci, 0x42U, managed) == PciFunctionControlError::AccessDenied,
                   control.ReadReleasedConfiguration(smbus, 0x00U, managed) == PciFunctionControlError::AccessDenied,
                   managed == 0U);
    const bool assigned = (control.Assign(ehci, driver.Identity, driver.Domain) == PciFunctionControlError::None) &&
                          (control.Assign(uhci, driver.Identity, driver.Domain) == PciFunctionControlError::None) &&
                          (control.Assign(ohci, driver.Identity, driver.Domain) == PciFunctionControlError::None) &&
                          (control.Assign(smbus, driver.Identity, driver.Domain) == PciFunctionControlError::None);
    // Assigned behind the control's back, so not through it.
    const auto direct = bypass.Assign(driver.Identity, driver.Domain);
    const auto read = [&](const PciFunction& function, const Foundation::UInt16 offset) noexcept
    {
        value = 0xDEAD'BEEFU;
        return control.AccessLegacySupport(function, driver.Identity, offset, false, 0U, value);
    };
    const auto write =
        [&](const PciFunction& function, const Foundation::UInt16 offset, const Foundation::UInt32 written) noexcept
    {
        return control.AccessLegacySupport(function, driver.Identity, offset, true, written, value);
    };
    constexpr PciFunctionControlError Denied{PciFunctionControlError::AccessDenied};
    ASTERIA_EXPECT(runner, "the manager cannot read a controller a driver holds",
                   control.ReadReleasedConfiguration(ehci, 0x68U, value) == PciFunctionControlError::NotAssignee,
                   control.ReadReleasedConfiguration(bypass, 0x00U, value) == PciFunctionControlError::NotAssignee,
                   value == 0U);
    ASTERIA_EXPECT(runner, "only the assignee of a USB host controller reaches its legacy-support dwords",
                   unconfigured == PciFunctionControlError::NotConfigured, ready,
                   unassigned == PciFunctionControlError::NotAssigned, assigned, direct.HasValue(),
                   read(bypass, 0x00U) == PciFunctionControlError::NotAssigned,
                   control.AccessLegacySupport(ehci, other.Identity, 0U, false, 0U, value) ==
                       PciFunctionControlError::NotAssignee,
                   read(smbus, 0x00U) == Denied, value == 0U, read(ehci, 0x41U) == Denied, read(ehci, 0x42U) == Denied,
                   read(ehci, 0x100U) == Denied, read(ehci, 0x04U) == Denied, read(ehci, 0x14U) == Denied,
                   read(ehci, 0x34U) == Denied, read(uhci, 0x40U) == Denied, read(uhci, 0xC4U) == Denied,
                   read(ohci, 0xC0U) == Denied, read(ohci, 0x40U) == Denied);
    ASTERIA_EXPECT(runner, "identity, class, BAR 0, interrupt, EHCI extended and UHCI LEGSUP dwords are readable",
                   read(ehci, 0x00U) == PciFunctionControlError::None, value == 0x293A'8086U,
                   read(uhci, 0x08U) == PciFunctionControlError::None, value == 0x0C03'2003U,
                   read(ohci, 0x10U) == PciFunctionControlError::None, value == 0xF7DF'A400U,
                   read(ohci, 0x3CU) == PciFunctionControlError::None, value == 0x0000'010BU,
                   read(ehci, 0x40U) == PciFunctionControlError::None, value == 0x0000'0305U,
                   read(ehci, 0xFCU) == PciFunctionControlError::None, value == 0x5A5A'0FFCU,
                   read(uhci, 0xC0U) == PciFunctionControlError::None, value == 0x1234'8F3FU);

    const Size writesBefore = configuration.Writes;
    const PciFunctionControlError claimed = write(ehci, 0x68U, 0x0100'0001U);
    const Foundation::UInt32 semaphores = configuration.Space[0x68U / 4U];
    const PciFunctionControlError acknowledged = write(ehci, 0x6CU, 0xE03F'0000U);
    const PciFunctionControlError last = write(ehci, 0xF8U, 0x0100'0001U);
    ASTERIA_EXPECT(runner, "an EHCI write sets only OS Owned and acknowledges only SMI status",
                   claimed == PciFunctionControlError::None, semaphores == 0x0101'0001U,
                   acknowledged == PciFunctionControlError::None, configuration.Space[0x6CU / 4U] == 0xE000'0000U,
                   last == PciFunctionControlError::None, configuration.Space[0xF8U / 4U] == 0x0100'0001U,
                   configuration.Writes == writesBefore + 3U);
    ASTERIA_EXPECT(runner, "an EHCI write that would keep firmware's SMIs or touch anything else is refused",
                   write(ehci, 0x68U, 0x0000'0001U) == Denied, write(ehci, 0x68U, 0x0100'4001U) == Denied,
                   write(ehci, 0x50U, 0x0100'4301U) == Denied, write(ehci, 0x54U, 0U) == Denied,
                   write(ehci, 0x6CU, 0xE000'0001U) == Denied, write(ehci, 0x6CU, 0x0000'2000U) == Denied,
                   write(ehci, 0x70U, 0U) == Denied, write(ehci, 0xC0U, 0xAF00U) == Denied,
                   write(uhci, 0x68U, 0x0100'0001U) == Denied, write(ohci, 0x6CU, 0xE000'0000U) == Denied,
                   configuration.Writes == writesBefore + 3U);
    // A list that loops, or links below 0x40 (the next pointer's reserved
    // bits set), counts as holding every offset; one unread does too.
    configuration.Space[0x50U / 4U] = 0x0000'5001U;
    const PciFunctionControlError looped = write(ehci, 0x68U, 0x0100'0001U);
    configuration.Space[0x50U / 4U] = 0x0000'4301U;
    configuration.Space[0x40U / 4U] = 0x0000'0705U;
    const PciFunctionControlError low = write(ehci, 0x68U, 0x0100'0001U);
    configuration.Space[0x40U / 4U] = 0x0000'0305U;
    configuration.RefuseReadAt = 0x34U;
    const PciFunctionControlError unread = write(ehci, 0x68U, 0x0100'0001U);
    configuration.RefuseReadAt = 0xFFFFU;
    // Without the MSI in the list, an ID 1 at 0x40 is a legacy capability;
    // without a list at all, so is the one at 0x50.
    configuration.Space[0x50U / 4U] = 0x0000'0301U;
    configuration.Space[0x40U / 4U] = 0x0000'0001U;
    const PciFunctionControlError first = write(ehci, 0x40U, 0x0100'0001U);
    const PciFunctionControlError firstStatus = write(ehci, 0x44U, 0xE000'0000U);
    configuration.Command &= ~(Foundation::UInt32{1U} << 20U);
    const PciFunctionControlError listless = write(ehci, 0x50U, 0x0100'0301U);
    ASTERIA_EXPECT(runner, "the standard capability list decides which ID 1 is the legacy capability", looped == Denied,
                   low == Denied, unread == Denied, first == PciFunctionControlError::None,
                   configuration.Space[0x40U / 4U] == 0x0100'0001U, firstStatus == PciFunctionControlError::None,
                   listless == PciFunctionControlError::None, configuration.Space[0x50U / 4U] == 0x0100'0301U);

    const PciFunctionControlError released = write(uhci, 0xC0U, 0xAF00U);
    ASTERIA_EXPECT(runner, "a UHCI write only acknowledges LEGSUP status and sets PIRQ, keeping the high half",
                   released == PciFunctionControlError::None, configuration.Space[0xC0U / 4U] == 0x1234'AF00U,
                   write(uhci, 0xC0U, 0x0001U) == Denied, write(uhci, 0xC0U, 0x0080U) == Denied,
                   write(uhci, 0xC0U, 0x0001'AF00U) == Denied);
    configuration.Refuse = true;
    const PciFunctionControlError rejected = write(ehci, 0x6CU, 0xE000'0000U);
    configuration.Refuse = false;
    configuration.RefuseReadAt = 0x3CU;
    const PciFunctionControlError unreadable = read(ohci, 0x3CU);
    configuration.RefuseReadAt = 0xC0U;
    const PciFunctionControlError unreadLegacy = write(uhci, 0xC0U, 0xAF00U);
    configuration.RefuseReadAt = 0xFFFFU;
    ASTERIA_EXPECT(
        runner, "a refused configuration read or write is reported",
        rejected == PciFunctionControlError::ConfigurationRejected,
        unreadable == PciFunctionControlError::ConfigurationRejected,
        unreadLegacy == PciFunctionControlError::ConfigurationRejected,
        control.Release(ehci) == PciFunctionControlError::None, control.Release(uhci) == PciFunctionControlError::None,
        control.Release(ohci) == PciFunctionControlError::None, control.Release(smbus) == PciFunctionControlError::None,
        direct.HasValue() && (bypass.ReleaseAssignment(direct.GetValue().Assignment).Error == PciFunctionError::None));
    Foundation::UInt32 releasedClass{0U};
    configuration.RefuseReadAt = 0x10U;
    const PciFunctionControlError releasedRefused = control.ReadReleasedConfiguration(ohci, 0x10U, releasedClass);
    configuration.RefuseReadAt = 0xFFFFU;
    ASTERIA_EXPECT(runner, "a released controller is the manager's to read again",
                   control.ReadReleasedConfiguration(uhci, 0x08U, releasedClass) == PciFunctionControlError::None,
                   releasedClass == 0x0C03'2003U, releasedRefused == PciFunctionControlError::ConfigurationRejected);
}

int RunPciFunctionTests() noexcept
{
    TestContext runner{"PciFunctionTests"};
    TestUsbLegacySupport(runner);
    TestFirmwareReservations(runner);
    TestInitializationAndDescriptor(runner);
    TestResourceFailure(runner);
    TestAssignmentProvenanceAndLifetime(runner);
    TestIommuOrdering(runner);
    TestConcurrentAssignment(runner);
    TestFunctionControl(runner);
    TestParkedStorage(runner);
    TestParkedStorage(runner, true);
    TestParkedStorage(runner, false, true);
    TestParkedStorage(runner, true, true, true);
    TestParkedStorage(runner, true, false, false, true);
    TestFunctionMessages(runner);
    TestInterruptPool(runner);

    Foundation::UInt64 sequence{0U};
    ASTERIA_EXPECT(
        runner, "assignment sequence never wraps", PciFunctionAssignmentSequencePolicy::TryAdvance(0U, sequence),
        sequence == 1U,
        !PciFunctionAssignmentSequencePolicy::TryAdvance(PciFunctionAssignmentSequencePolicy::Maximum, sequence));
    return runner.Finish();
}
} // Namespace Asteria::Test

int main()
{
    return Asteria::Test::RunPciFunctionTests();
}
