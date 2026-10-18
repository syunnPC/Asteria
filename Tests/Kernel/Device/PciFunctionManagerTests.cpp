// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Hal/Device/IPciConfigurationSpace.hpp"
#include "Kernel/Device/DeviceManager.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
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
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Hal::Device::IPciConfigurationSpace;
using Hal::Device::PciConfigurationReadError;
using Kernel::Device::DeviceManager;
using Kernel::Device::PciBusRange;
using Kernel::Device::PciBusRangeView;
using Kernel::Device::PciFunctionAddress;
using Kernel::Device::PciFunction;
using Kernel::Device::PciFunctionAssignmentReceipt;
using Kernel::Device::PciFunctionCatalog;
using Kernel::Device::PciFunctionCatalogError;
using Kernel::Device::PciFunctionCreateRequest;
using Kernel::Device::PciFunctionCreation;
using Kernel::Device::PciFunctionError;
using Kernel::Device::PciFunctionManagerError;
using Kernel::Device::PciFunctionState;
using Kernel::Memory::AddressSpaceId;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityOperation;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::IKernelObject;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectDestructionRecord;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPin;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationDomainId;
using Kernel::Object::RevocationRegistry;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Protection::ProtectionDomain;
using Kernel::Protection::ProtectionDomainError;
using Kernel::Protection::ProtectionDomainId;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
using Platform::Synchronization::SpinLock;
using Platform::Synchronization::SpinLockGuard;
using TestSupport::ManualClock;

class TestKernelObject final : public IKernelObject
{
public:
    constexpr TestKernelObject() noexcept : m_type{ObjectType::Thread}, m_owner{ResourceDomainId{91U}}
    {
    }

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

class FakePciConfigurationSpace final : public IPciConfigurationSpace
{
public:
    static constexpr Size MaximumEntries{DeviceManager::MaximumPciFunctions + 1U};

    [[nodiscard]] bool AddFunction(const PciFunctionAddress address, const UInt16 vendor, const UInt16 device,
                                   const bool multifunction) noexcept
    {
        SpinLockGuard guard{m_synchronization};
        if (!address.IsValid() || (vendor == 0xFFFFU) || (m_entryCount >= MaximumEntries))
        {
            return false;
        }
        for (Size index{0U}; index < m_entryCount; ++index)
        {
            if (m_entries[index].Address.IsEqualTo(address))
            {
                return false;
            }
        }
        m_entries[m_entryCount] = Entry{
            .Address = address,
            .Identifier = static_cast<UInt32>(vendor) | (static_cast<UInt32>(device) << 16U),
            .Class = 2U | (1U << 8U) | (6U << 16U) | (1U << 24U),
            .Header = static_cast<UInt32>(multifunction ? 0x80U : 0U) << 16U,
        };
        ++m_entryCount;
        return true;
    }

    [[nodiscard]] PciConfigurationReadError Read32(const PciFunctionAddress address, const UInt16 registerOffset,
                                                   UInt32& output) noexcept override
    {
        SpinLockGuard guard{m_synchronization};
        if (!address.IsValid() || ((registerOffset & 3U) != 0U) || (registerOffset > 0x0FFCU))
        {
            return PciConfigurationReadError::InvalidAddress;
        }
        const Entry* entry{nullptr};
        for (Size index{0U}; index < m_entryCount; ++index)
        {
            if (m_entries[index].Address.IsEqualTo(address))
            {
                entry = &m_entries[index];
                break;
            }
        }
        if (entry == nullptr)
        {
            output = 0xFFFF'FFFFU;
            return PciConfigurationReadError::None;
        }
        switch (registerOffset)
        {
        case 0x00U:
            output = entry->Identifier;
            break;
        case 0x08U:
            output = entry->Class;
            break;
        case 0x0CU:
            output = entry->Header;
            break;
        default:
            output = 0U;
            break;
        }
        return PciConfigurationReadError::None;
    }

private:
    struct Entry
    {
        PciFunctionAddress Address{};
        UInt32 Identifier{0U};
        UInt32 Class{0U};
        UInt32 Header{0U};
    };

    SpinLock m_synchronization;
    Entry m_entries[MaximumEntries]{};
    Size m_entryCount{0U};
};

[[nodiscard]] constexpr ResourceDomainLimits MakeLimits() noexcept
{
    return ResourceDomainLimits{
        .CommittedMemoryPages = 0U,
        .ResidentMemoryPages = 0U,
        .KernelMetadataBytes = 4U << 20U,
        .CapabilityHandles = 512U,
        .IpcQueueBytes = 0U,
        .Threads = 0U,
        .ChildDomains = 0U,
    };
}

[[nodiscard]] constexpr PciFunctionAddress MakeAddress(const Size index) noexcept
{
    // Index 0 and 1 are functions 0 and 1 of device 0; then function 0 of
    // devices 1-31 on bus 0, and after them of devices 0-31 on bus 1, so the
    // manager's whole capacity has distinct, valid addresses.
    return (index == 0U) ? PciFunctionAddress{0U, 0U, 0U, 0U}
                         : ((index == 1U) ? PciFunctionAddress{0U, 0U, 0U, 1U}
                                          : PciFunctionAddress{0U, static_cast<UInt8>((index - 1U) >> 5U),
                                                               static_cast<UInt8>((index - 1U) & 0x1FU), 0U});
}

class ManagerFixture final
{
public:
    ManagerFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          Space{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL}, Domain{Ledger},
          Catalog{Configuration}, Manager{Registry, Issuer}
    {
    }

    [[nodiscard]] bool Initialize(const Size functionCount, const bool bindCatalog = true) noexcept
    {
        if ((functionCount == 0U) || (functionCount > FakePciConfigurationSpace::MaximumEntries) ||
            (Domain.InitializeRoot(ResourceDomainId{71U}, MakeLimits()).Error != ResourceDomainError::None))
        {
            return false;
        }
        const auto registered = Issuer.CreateObject(Domain);
        const auto revocation = Revocations.CreateRoot();
        if (!registered.HasValue() || !revocation.HasValue())
        {
            return false;
        }
        DomainObject = registered.GetValue();
        DomainIdentity = ResourceDomainObjectId{Registry, DomainObject};
        Revocation = revocation.GetValue();

        for (Size index{0U}; index < functionCount; ++index)
        {
            const bool multifunction = (index == 0U) && (functionCount > 1U);
            if (!Configuration.AddFunction(MakeAddress(index), 0x8086U, static_cast<UInt16>(0x1000U + index),
                                           multifunction))
            {
                return false;
            }
        }
        const PciBusRange range{
            .Segment = 0U,
            .FirstBus = 0U,
            .LastBus = 7U,
            .Reserved = {},
        };
        if (Catalog
                .Discover(PciBusRangeView{
                    .Data = &range,
                    .Count = 1U,
                })
                .Error != PciFunctionCatalogError::None)
        {
            return false;
        }
        return !bindCatalog || (Manager.BindPciFunctionCatalog(Catalog) == PciFunctionManagerError::None);
    }

    [[nodiscard]] PciFunctionCreateRequest MakeRequest(const PciFunctionAddress address) noexcept
    {
        return PciFunctionCreateRequest{
            .Destination = &Space,
            .ResourceDomainStorage = &Domain,
            .ResourceDomainIdentity = DomainIdentity,
            .RevocationDomain = Revocation,
            .LeaseDeadline = MonotonicTime::Infinite(),
            .Address = address,
            .Reserved = {},
        };
    }

    [[nodiscard]] bool Finish() noexcept
    {
        const auto pci = Manager.GetPciFunctionSnapshot();
        if (((pci.FreeSlots + pci.RetiredSlots) != DeviceManager::MaximumPciFunctions) || (pci.ActiveSlots != 0U) ||
            (pci.QuiescingSlots != 0U) || (pci.FaultedSlots != 0U) ||
            (Domain.GetSnapshot().Usage.KernelMetadataBytes != 0U) || (Domain.Quiesce() != ResourceDomainError::None) ||
            (Issuer.DestroyObject(DomainObject) != ObjectRegistryError::Ok))
        {
            return false;
        }
        const auto collected = Issuer.CollectDestroyedObject();
        return collected.HasValue() && (collected.GetValue().Object.GetValue() == DomainObject.GetValue()) &&
               (collected.GetValue().Instance == &Domain);
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace Space;
    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    FakePciConfigurationSpace Configuration;
    PciFunctionCatalog Catalog;
    DeviceManager Manager;
    ObjectId DomainObject{};
    ResourceDomainObjectId DomainIdentity{};
    RevocationDomainId Revocation{};
};

[[nodiscard]] bool DestroyAndCollect(ManagerFixture& fixture, const ObjectId object, IKernelObject& instance) noexcept
{
    if (fixture.Issuer.DestroyObject(object) != ObjectRegistryError::Ok)
    {
        return false;
    }
    const auto collected = fixture.Issuer.CollectDestroyedObject();
    return collected.HasValue() && (collected.GetValue().Object.GetValue() == object.GetValue()) &&
           (collected.GetValue().Instance == &instance);
}

class DriverFixture final
{
public:
    constexpr DriverFixture() noexcept : AddressSpace{ObjectType::AddressSpace, ResourceDomainId{81U}}
    {
    }

    [[nodiscard]] bool Initialize(ManagerFixture& fixture) noexcept
    {
        m_fixture = &fixture;
        const auto address = fixture.Issuer.CreateObject(AddressSpace);
        if (!address.HasValue())
        {
            return false;
        }
        AddressObject = address.GetValue();
        if (Domain
                .Initialize(fixture.Registry, fixture.DomainIdentity, fixture.Domain,
                            AddressSpaceId{fixture.Registry, AddressObject}, fixture.Space)
                .Error != ProtectionDomainError::None)
        {
            return false;
        }
        const auto domain = fixture.Issuer.CreateObject(Domain);
        if (!domain.HasValue())
        {
            return false;
        }
        DomainObject = domain.GetValue();
        Identity = ProtectionDomainId{fixture.Registry, DomainObject};
        return Domain.BindIdentity(fixture.Registry, Identity).Error == ProtectionDomainError::None;
    }

    [[nodiscard]] bool Finish() noexcept
    {
        return (m_fixture != nullptr) && (Domain.Quiesce() == ProtectionDomainError::None) &&
               DestroyAndCollect(*m_fixture, DomainObject, Domain) &&
               DestroyAndCollect(*m_fixture, AddressObject, AddressSpace);
    }

    TestKernelObject AddressSpace;
    ProtectionDomain Domain;
    ObjectId AddressObject{};
    ObjectId DomainObject{};
    ProtectionDomainId Identity{};

private:
    ManagerFixture* m_fixture{nullptr};
};

[[nodiscard]] bool RetireAndReclaim(ManagerFixture& fixture, const PciFunctionCreation& creation) noexcept
{
    if ((fixture.Space.Close(creation.Capability) != CapabilityError::Ok) ||
        !fixture.Manager.RetirePciFunction(creation.Function).HasValue())
    {
        return false;
    }
    const auto collected = fixture.Issuer.CollectDestroyedObject();
    return collected.HasValue() && fixture.Manager.ReclaimPciFunction(collected.GetValue()).HasValue();
}

void TestAuthorityRetirementAndReuse(TestContext& runner) noexcept
{
    ManagerFixture fixture;
    DriverFixture driver;
    ASTERIA_EXPECT(runner, "PCI manager and real driver fixtures initialize", fixture.Initialize(1U),
                   driver.Initialize(fixture));

    const PciFunctionAddress address = MakeAddress(0U);
    const auto created = fixture.Manager.CreatePciFunction(fixture.MakeRequest(address));
    if (!created.HasValue())
    {
        ASTERIA_EXPECT(runner, "catalog function materializes", false);
        return;
    }
    const PciFunctionCreation first = created.GetValue();
    constexpr CapabilityRights expectedRights =
        CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Manage).With(CapabilityRight::Transfer);
    bool exactAuthority{false};
    {
        auto lookup = fixture.Space.Lookup(first.Capability, ObjectType::PciFunction, CapabilityOperation::Manage);
        exactAuthority = lookup.HasValue() && lookup.GetValue().GetDescriptor().Rights.IsEqualTo(expectedRights) &&
                         static_cast<PciFunction&>(lookup.GetValue().GetObject())
                             .GetSnapshot()
                             .Descriptor.Address.IsEqualTo(address);
    }
    const auto denied = fixture.Space.Lookup(first.Capability, ObjectType::PciFunction, CapabilityOperation::Write);
    ASTERIA_EXPECT(runner, "catalog record receives exact bounded management authority", exactAuthority,
                   !denied.HasValue(), denied.GetError() == CapabilityError::RightsDenied,
                   fixture.Manager.GetPciFunctionSnapshot().ActiveSlots == 1U,
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes ==
                       (sizeof(Kernel::Device::PciFunction) + sizeof(ProtectionDomain)));

    const auto duplicate = fixture.Manager.CreatePciFunction(fixture.MakeRequest(address));
    PciFunctionAssignmentReceipt firstAssignment{};
    bool assignmentCreated{false};
    {
        auto lookup = fixture.Space.Lookup(first.Capability, ObjectType::PciFunction, CapabilityOperation::Manage);
        if (lookup.HasValue())
        {
            auto& function = static_cast<PciFunction&>(lookup.GetValue().GetObject());
            const auto assigned = function.Assign(driver.Identity, driver.Domain);
            assignmentCreated = assigned.HasValue();
            if (assigned.HasValue())
            {
                firstAssignment = assigned.GetValue();
            }
        }
    }
    const auto busyRetire = fixture.Manager.RetirePciFunction(first.Function);
    bool assignmentReleased{false};
    {
        auto lookup = fixture.Space.Lookup(first.Capability, ObjectType::PciFunction, CapabilityOperation::Manage);
        if (lookup.HasValue() && assignmentCreated)
        {
            auto& function = static_cast<PciFunction&>(lookup.GetValue().GetObject());
            assignmentReleased = function.ReleaseAssignment(firstAssignment.Assignment).Error == PciFunctionError::None;
        }
    }
    ASTERIA_EXPECT(runner, "duplicate BDF and retirement with a live driver are rejected", !duplicate.HasValue(),
                   duplicate.GetError().Error == PciFunctionManagerError::FunctionAlreadyOwned, assignmentCreated,
                   !busyRetire.HasValue(), busyRetire.GetError().Error == PciFunctionManagerError::FunctionBusy,
                   assignmentReleased);

    ObjectPin retirementPin;
    bool leaseBlockedCollection{false};
    bool pinBlockedCollection{false};
    bool retirementSealed{false};
    {
        auto retained = fixture.Space.Lookup(first.Capability, ObjectType::PciFunction, CapabilityOperation::Manage);
        if (retained.HasValue() &&
            (fixture.Registry.PinObject(first.Function.GetObjectId(), ObjectType::PciFunction, retirementPin) ==
             ObjectPinError::Ok) &&
            (fixture.Space.Close(first.Capability) == CapabilityError::Ok) &&
            fixture.Manager.RetirePciFunction(first.Function).HasValue())
        {
            auto& function = static_cast<PciFunction&>(retained.GetValue().GetObject());
            const PciFunctionError publicQuiesce = function.Quiesce().Error;
            const auto rejected = function.Assign(driver.Identity, driver.Domain);
            retirementSealed = !rejected.HasValue() &&
                               (rejected.GetError().Error == PciFunctionError::RetirementPending) &&
                               (publicQuiesce == PciFunctionError::ManagerOwned);
            const auto premature = fixture.Issuer.CollectDestroyedObject();
            leaseBlockedCollection =
                !premature.HasValue() && (premature.GetError() == ObjectRegistryError::NoDestructionReady);
        }
    }
    const auto pinned = fixture.Issuer.CollectDestroyedObject();
    pinBlockedCollection = !pinned.HasValue() && (pinned.GetError() == ObjectRegistryError::NoDestructionReady);
    retirementPin.Reset();
    const auto collected = fixture.Issuer.CollectDestroyedObject();
    const ObjectDestructionRecord receipt = collected.HasValue() ? collected.GetValue() : ObjectDestructionRecord{};
    const auto whileQuiescing = fixture.Manager.CreatePciFunction(fixture.MakeRequest(address));
    const auto reclaimed = collected.HasValue() ? fixture.Manager.ReclaimPciFunction(collected.GetValue())
                                                : decltype(fixture.Manager.ReclaimPciFunction({}))::Failure({});
    ASTERIA_EXPECT(runner, "retirement seal waits for leases and pins before releasing the BDF", retirementSealed,
                   leaseBlockedCollection, pinBlockedCollection, !whileQuiescing.HasValue(),
                   whileQuiescing.GetError().Error == PciFunctionManagerError::FunctionAlreadyOwned,
                   reclaimed.HasValue(),
                   fixture.Manager.GetPciFunctionSnapshot().FreeSlots == DeviceManager::MaximumPciFunctions);

    const auto replacementResult = fixture.Manager.CreatePciFunction(fixture.MakeRequest(address));
    bool staleTokenRejected{false};
    bool replacementReleased{false};
    if (replacementResult.HasValue())
    {
        const PciFunctionCreation replacement = replacementResult.GetValue();
        auto lookup =
            fixture.Space.Lookup(replacement.Capability, ObjectType::PciFunction, CapabilityOperation::Manage);
        if (lookup.HasValue())
        {
            auto& function = static_cast<PciFunction&>(lookup.GetValue().GetObject());
            const auto next = function.Assign(driver.Identity, driver.Domain);
            if (next.HasValue())
            {
                staleTokenRejected =
                    function.ReleaseAssignment(firstAssignment.Assignment).Error == PciFunctionError::StaleAssignment;
                replacementReleased =
                    next.GetValue().Assignment.GetSequence() == 2U &&
                    (function.ReleaseAssignment(next.GetValue().Assignment).Error == PciFunctionError::None);
            }
        }
    }
    const auto staleRetire = fixture.Manager.RetirePciFunction(first.Function);
    ASTERIA_EXPECT(runner, "slot reuse preserves assignment sequence and registry generation",
                   replacementResult.HasValue(), !replacementResult.GetValue().Function.IsEqualTo(first.Function),
                   staleTokenRejected, replacementReleased, !staleRetire.HasValue(),
                   staleRetire.GetError().Error == PciFunctionManagerError::FunctionNotOwned);

    bool replacementQuiescing{false};
    if (replacementResult.HasValue())
    {
        const PciFunctionCreation replacement = replacementResult.GetValue();
        if ((fixture.Space.Close(replacement.Capability) == CapabilityError::Ok) &&
            fixture.Manager.RetirePciFunction(replacement.Function).HasValue())
        {
            replacementQuiescing = true;
        }
    }
    const auto replacementCollected = fixture.Issuer.CollectDestroyedObject();
    const auto staleReceipt = fixture.Manager.ReclaimPciFunction(receipt);
    const auto exactReplacement = replacementQuiescing && replacementCollected.HasValue()
                                      ? fixture.Manager.ReclaimPciFunction(replacementCollected.GetValue())
                                      : decltype(fixture.Manager.ReclaimPciFunction({}))::Failure({});
    ASTERIA_EXPECT(runner, "old receipt cannot reclaim a quiescing replacement generation", !staleReceipt.HasValue(),
                   staleReceipt.GetError().Error == PciFunctionManagerError::InvalidDestructionRecord,
                   replacementQuiescing, exactReplacement.HasValue(), driver.Finish(), fixture.Finish());
}

void TestFailureAtomicity(TestContext& runner) noexcept
{
    static ManagerFixture local;
    static ManagerFixture foreign;
    static ManagerFixture unbound;
    FakePciConfigurationSpace emptyConfiguration;
    PciFunctionCatalog emptyCatalog{emptyConfiguration};
    ASTERIA_EXPECT(runner, "PCI manager rollback fixtures initialize", local.Initialize(1U), foreign.Initialize(1U),
                   unbound.Initialize(1U, false));

    PciFunctionCreateRequest invalid = local.MakeRequest(MakeAddress(0U));
    invalid.Reserved[0U] = 1U;
    const auto invalidResult = local.Manager.CreatePciFunction(invalid);
    PciFunctionCreateRequest invalidTail = local.MakeRequest(MakeAddress(0U));
    invalidTail.Reserved[1U] = 1U;
    const auto invalidTailResult = local.Manager.CreatePciFunction(invalidTail);
    const PciFunctionManagerError emptyRejected = unbound.Manager.BindPciFunctionCatalog(emptyCatalog);
    const auto unavailable = unbound.Manager.CreatePciFunction(unbound.MakeRequest(MakeAddress(0U)));
    const PciFunctionManagerError bound = unbound.Manager.BindPciFunctionCatalog(unbound.Catalog);
    const PciFunctionManagerError rebound = unbound.Manager.BindPciFunctionCatalog(unbound.Catalog);
    const auto missing = local.Manager.CreatePciFunction(local.MakeRequest(PciFunctionAddress{0U, 0U, 3U, 0U}));
    ASTERIA_EXPECT(
        runner, "invalid requests and absent catalog records leave no slot state", !invalidResult.HasValue(),
        invalidResult.GetError().Error == PciFunctionManagerError::InvalidConfiguration, !invalidTailResult.HasValue(),
        invalidTailResult.GetError().Error == PciFunctionManagerError::InvalidConfiguration,
        emptyRejected == PciFunctionManagerError::CatalogRejected, !unavailable.HasValue(),
        unavailable.GetError().Error == PciFunctionManagerError::CatalogNotBound,
        bound == PciFunctionManagerError::None, rebound == PciFunctionManagerError::CatalogAlreadyBound,
        !missing.HasValue(), missing.GetError().CatalogFailure.Error == PciFunctionCatalogError::FunctionNotFound,
        local.Manager.GetPciFunctionSnapshot().FreeSlots == DeviceManager::MaximumPciFunctions);

    ResourceDomainLedger otherLedger;
    ResourceDomain otherDomain{otherLedger};
    static_cast<void>(otherDomain.InitializeRoot(ResourceDomainId{72U}, MakeLimits()));
    PciFunctionCreateRequest wrongInstance = local.MakeRequest(MakeAddress(0U));
    wrongInstance.ResourceDomainStorage = &otherDomain;
    const auto wrong = local.Manager.CreatePciFunction(wrongInstance);
    ASTERIA_EXPECT(runner, "exact ResourceDomain storage is validated before publication", !wrong.HasValue(),
                   wrong.GetError().Error == PciFunctionManagerError::FunctionRejected,
                   wrong.GetError().FunctionFailure.Error == PciFunctionError::ResourceDomainInstanceMismatch,
                   local.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U);
    static_cast<void>(otherDomain.Quiesce());

    PciFunctionCreateRequest foreignDestination = local.MakeRequest(MakeAddress(0U));
    foreignDestination.Destination = &foreign.Space;
    foreignDestination.RevocationDomain = foreign.Revocation;
    const auto mintFailure = local.Manager.CreatePciFunction(foreignDestination);
    const auto pending = local.Manager.GetPciFunctionSnapshot();
    const auto record = local.Issuer.CollectDestroyedObject();
    const auto forged = local.Manager.ReclaimPciFunction(ObjectDestructionRecord{});
    const bool exactReclaimed = record.HasValue() && local.Manager.ReclaimPciFunction(record.GetValue()).HasValue();
    ASTERIA_EXPECT(runner, "mint rollback keeps ownership until an authentic receipt is reclaimed",
                   !mintFailure.HasValue(), mintFailure.GetError().Error == PciFunctionManagerError::CapabilityRejected,
                   mintFailure.GetError().CapabilityError == CapabilityError::InvalidConfiguration,
                   pending.QuiescingSlots == 1U, !forged.HasValue(),
                   forged.GetError().Error == PciFunctionManagerError::InvalidDestructionRecord, exactReclaimed,
                   local.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U, foreign.Space.GetActiveCount() == 0U);

    const auto valid = local.Manager.CreatePciFunction(local.MakeRequest(MakeAddress(0U)));
    const auto foreignFirst = foreign.Manager.CreatePciFunction(foreign.MakeRequest(MakeAddress(0U)));
    const bool foreignGenerationAdvanced =
        foreignFirst.HasValue() && RetireAndReclaim(foreign, foreignFirst.GetValue());
    const auto foreignValid = foreign.Manager.CreatePciFunction(foreign.MakeRequest(MakeAddress(0U)));
    const auto foreignRetire = (valid.HasValue() && foreignValid.HasValue())
                                   ? local.Manager.RetirePciFunction(foreignValid.GetValue().Function)
                                   : decltype(local.Manager.RetirePciFunction({}))::Failure({});
    ASTERIA_EXPECT(runner, "registry provenance rejects a numerically colliding foreign ID", valid.HasValue(),
                   foreignGenerationAdvanced, foreignValid.HasValue(),
                   valid.GetValue().Function.GetValue() == foreignValid.GetValue().Function.GetValue(),
                   !foreignRetire.HasValue(),
                   foreignRetire.GetError().Error == PciFunctionManagerError::InvalidFunction,
                   local.Manager.GetPciFunctionSnapshot().ActiveSlots == 1U);
    ASTERIA_EXPECT(runner, "manager remains usable after complete capability rollback", valid.HasValue(),
                   foreignValid.HasValue(), RetireAndReclaim(local, valid.GetValue()),
                   RetireAndReclaim(foreign, foreignValid.GetValue()), local.Finish(), foreign.Finish(),
                   unbound.Finish());
}

void TestRegistrationFailure(TestContext& runner) noexcept
{
    ManagerFixture fixture;
    ASTERIA_EXPECT(runner, "registry-capacity fixture initializes", fixture.Initialize(1U));

    constexpr Size FillerCount{Asteria::TestSupport::TestObjectRegistry::EntryCapacity - 1U};
    TestKernelObject fillers[FillerCount]{TestKernelObject{ObjectType::Thread, ResourceDomainId{91U}}};
    ObjectId identities[FillerCount]{};
    bool filled{true};
    for (Size index{0U}; index < FillerCount; ++index)
    {
        const auto registered = fixture.Issuer.CreateObject(fillers[index]);
        if (!registered.HasValue())
        {
            filled = false;
            break;
        }
        identities[index] = registered.GetValue();
    }
    const auto rejected = fixture.Manager.CreatePciFunction(fixture.MakeRequest(MakeAddress(0U)));
    ASTERIA_EXPECT(runner, "registration failure resets unpublished storage and metadata charge", filled,
                   !rejected.HasValue(), rejected.GetError().Error == PciFunctionManagerError::RegistrationRejected,
                   rejected.GetError().RegistryError == ObjectRegistryError::RegistryExhausted,
                   fixture.Manager.GetPciFunctionSnapshot().FreeSlots == DeviceManager::MaximumPciFunctions,
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U);

    bool cleared{true};
    for (Size index{0U}; index < FillerCount; ++index)
    {
        cleared = DestroyAndCollect(fixture, identities[index], fillers[index]) && cleared;
    }
    ASTERIA_EXPECT(runner, "registry-capacity fixture leaves no object or slot behind", cleared, fixture.Finish());
}

void TestConcurrentRetirementSeal(TestContext& runner) noexcept
{
    ManagerFixture fixture;
    DriverFixture driver;
    if (!fixture.Initialize(1U) || !driver.Initialize(fixture))
    {
        ASTERIA_EXPECT(runner, "retirement-race fixtures initialize", false);
        return;
    }
    const auto created = fixture.Manager.CreatePciFunction(fixture.MakeRequest(MakeAddress(0U)));
    if (!created.HasValue())
    {
        ASTERIA_EXPECT(runner, "retirement-race function materializes", false);
        return;
    }

    const PciFunctionCreation creation = created.GetValue();
    std::atomic<bool> start{false};
    bool assignmentSucceeded{false};
    bool retirementSucceeded{false};
    PciFunctionAssignmentReceipt assignment{};
    PciFunctionError assignmentError{PciFunctionError::None};
    CapabilityError lookupError{CapabilityError::Ok};
    PciFunctionManagerError retirementError{PciFunctionManagerError::None};

    std::thread assigner{[&]() noexcept
                         {
                             while (!start.load(std::memory_order_acquire))
                             {
                                 start.wait(false, std::memory_order_acquire);
                             }
                             auto lookup = fixture.Space.Lookup(creation.Capability, ObjectType::PciFunction,
                                                                CapabilityOperation::Manage);
                             if (!lookup.HasValue())
                             {
                                 lookupError = lookup.GetError();
                                 return;
                             }
                             auto& function = static_cast<PciFunction&>(lookup.GetValue().GetObject());
                             const auto assigned = function.Assign(driver.Identity, driver.Domain);
                             assignmentSucceeded = assigned.HasValue();
                             if (assigned.HasValue())
                             {
                                 assignment = assigned.GetValue();
                             }
                             else
                             {
                                 assignmentError = assigned.GetError().Error;
                             }
                         }};
    std::thread retirer{[&]() noexcept
                        {
                            while (!start.load(std::memory_order_acquire))
                            {
                                start.wait(false, std::memory_order_acquire);
                            }
                            const auto retired = fixture.Manager.RetirePciFunction(creation.Function);
                            retirementSucceeded = retired.HasValue();
                            if (!retired.HasValue())
                            {
                                retirementError = retired.GetError().Error;
                            }
                        }};
    start.store(true, std::memory_order_release);
    start.notify_all();
    assigner.join();
    retirer.join();

    const bool assignmentWon =
        assignmentSucceeded && !retirementSucceeded && (retirementError == PciFunctionManagerError::FunctionBusy);
    const bool retirementWon =
        retirementSucceeded && !assignmentSucceeded &&
        ((assignmentError == PciFunctionError::RetirementPending) || (lookupError == CapabilityError::ObjectNotFound));
    bool assignmentReleased{!assignmentWon};
    if (assignmentWon)
    {
        auto lookup = fixture.Space.Lookup(creation.Capability, ObjectType::PciFunction, CapabilityOperation::Manage);
        assignmentReleased =
            lookup.HasValue() &&
            (static_cast<PciFunction&>(lookup.GetValue().GetObject()).ReleaseAssignment(assignment.Assignment).Error ==
             PciFunctionError::None);
    }
    const bool retiredForCleanup =
        retirementWon || (assignmentReleased && fixture.Manager.RetirePciFunction(creation.Function).HasValue());
    const bool capabilityClosed = fixture.Space.Close(creation.Capability) == CapabilityError::Ok;
    const auto collected = fixture.Issuer.CollectDestroyedObject();
    const bool reclaimed = retiredForCleanup && capabilityClosed && collected.HasValue() &&
                           fixture.Manager.ReclaimPciFunction(collected.GetValue()).HasValue();
    ASTERIA_EXPECT(runner, "Assign and Retire linearize on exactly one side of the seal",
                   assignmentWon || retirementWon, !(assignmentWon && retirementWon), assignmentReleased, reclaimed,
                   driver.Finish(), fixture.Finish());
}

void TestConcurrentUniquenessAndCapacity(TestContext& runner) noexcept
{
    ManagerFixture fixture;
    ASTERIA_EXPECT(runner, "PCI manager capacity fixture fills the complete bounded catalog",
                   fixture.Initialize(DeviceManager::MaximumPciFunctions));

    std::atomic<bool> start{false};
    bool succeeded[2U]{};
    PciFunctionCreation concurrent[2U]{};
    PciFunctionManagerError errors[2U]{PciFunctionManagerError::None, PciFunctionManagerError::None};
    const auto worker = [&](const Size index) noexcept
    {
        while (!start.load(std::memory_order_acquire))
        {
            start.wait(false, std::memory_order_acquire);
        }
        const auto result = fixture.Manager.CreatePciFunction(fixture.MakeRequest(MakeAddress(0U)));
        succeeded[index] = result.HasValue();
        if (result.HasValue())
        {
            concurrent[index] = result.GetValue();
        }
        else
        {
            errors[index] = result.GetError().Error;
        }
    };
    std::thread first{[&]() noexcept
                      {
                          worker(0U);
                      }};
    std::thread second{[&]() noexcept
                       {
                           worker(1U);
                       }};
    start.store(true, std::memory_order_release);
    start.notify_all();
    first.join();
    second.join();

    const Size winner = succeeded[0U] ? 0U : 1U;
    const Size loser = succeeded[0U] ? 1U : 0U;
    PciFunctionCreation creations[DeviceManager::MaximumPciFunctions]{};
    creations[0U] = concurrent[winner];
    bool capacityFilled =
        succeeded[winner] && !succeeded[loser] && (errors[loser] == PciFunctionManagerError::FunctionAlreadyOwned);
    for (Size index{1U}; index < DeviceManager::MaximumPciFunctions; ++index)
    {
        const auto created = fixture.Manager.CreatePciFunction(fixture.MakeRequest(MakeAddress(index)));
        if (!created.HasValue())
        {
            capacityFilled = false;
            break;
        }
        creations[index] = created.GetValue();
    }
    const auto excess =
        fixture.Manager.CreatePciFunction(fixture.MakeRequest(MakeAddress(DeviceManager::MaximumPciFunctions)));
    ASTERIA_EXPECT(runner, "concurrent duplicate creation is unique and capacity is exact", capacityFilled,
                   !excess.HasValue(), excess.GetError().Error == PciFunctionManagerError::CatalogRejected,
                   fixture.Manager.GetPciFunctionSnapshot().ActiveSlots == DeviceManager::MaximumPciFunctions);

    bool reclaimed{true};
    for (const PciFunctionCreation& creation : creations)
    {
        reclaimed = RetireAndReclaim(fixture, creation) && reclaimed;
    }
    ASTERIA_EXPECT(runner, "all concurrently populated PCI slots return after exact reclamation", reclaimed,
                   fixture.Finish());
}
} // namespace

int RunPciFunctionManagerTests() noexcept
{
    TestContext runner{"PciFunctionManagerTests"};
    TestAuthorityRetirementAndReuse(runner);
    TestFailureAtomicity(runner);
    TestRegistrationFailure(runner);
    TestConcurrentRetirementSeal(runner);
    TestConcurrentUniquenessAndCapacity(runner);
    return runner.Finish();
}
} // namespace Asteria::Test

int main()
{
    return Asteria::Test::RunPciFunctionManagerTests();
}
