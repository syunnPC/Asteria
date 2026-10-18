// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <new>
#include <thread>

#include "Kernel/Device/IommuDomain.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Hal::Device::IIommuDevice;
using Hal::Device::IIommuDomainBackend;
using Hal::Device::IommuBackendError;
using Hal::Device::IommuBackendMapRequest;
using Hal::Device::IommuBackendOwnershipError;
using Hal::Device::IommuDeviceError;
using Kernel::Device::DmaAddress;
using Kernel::Device::DmaPermission;
using Kernel::Device::DmaPermissions;
using Kernel::Device::IommuAttachmentReceipt;
using Kernel::Device::IommuDomain;
using Kernel::Device::IommuDomainConfiguration;
using Kernel::Device::IommuDomainError;
using Kernel::Device::IommuDomainState;
using Kernel::Device::IommuMappingReceipt;
using Kernel::Device::IommuMappingRequest;
using Kernel::Memory::BootFrame;
using Kernel::Memory::Frame;
using Kernel::Memory::FrameDescriptor;
using Kernel::Memory::FrameError;
using Kernel::Memory::FrameId;
using Kernel::Memory::FrameKind;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PhysicalAddress;
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
using Platform::Synchronization::SpinLockGuard;

[[nodiscard]] constexpr bool RequestsEqual(const IommuBackendMapRequest& left,
                                           const IommuBackendMapRequest& right) noexcept
{
    return (left.DmaStart.GetValue() == right.DmaStart.GetValue()) &&
           (left.PhysicalStart.GetValue() == right.PhysicalStart.GetValue()) &&
           (left.Pages.GetValue() == right.Pages.GetValue()) &&
           (left.Permissions.GetBits() == right.Permissions.GetBits());
}

class FakeIommuDevice final : public IIommuDevice
{
public:
    [[nodiscard]] bool HasTrustedDma() const noexcept override
    {
        return false;
    }
    [[nodiscard]] Foundation::UInt32 GetRequesterId() const noexcept override
    {
        return RequesterId;
    }

    [[nodiscard]] IommuDeviceError Claim(const IommuDomain& owner) noexcept override
    {
        SpinLockGuard guard{m_synchronization};
        ++ClaimCalls;
        if (!m_ready)
        {
            return IommuDeviceError::NotReady;
        }
        if (m_owner != nullptr)
        {
            return IommuDeviceError::AlreadyClaimed;
        }
        m_owner = &owner;
        return IommuDeviceError::None;
    }

    [[nodiscard]] IommuDeviceError Release(const IommuDomain& owner) noexcept override
    {
        SpinLockGuard guard{m_synchronization};
        ++ReleaseCalls;
        if (NextRelease != IommuDeviceError::None)
        {
            const IommuDeviceError error = NextRelease;
            NextRelease = IommuDeviceError::None;
            return error;
        }
        if (m_owner != &owner)
        {
            return IommuDeviceError::InvalidOwner;
        }
        m_owner = nullptr;
        return IommuDeviceError::None;
    }

    [[nodiscard]] bool IsClaimedBy(const IommuDomain& owner) const noexcept
    {
        SpinLockGuard guard{m_synchronization};
        return m_owner == &owner;
    }

    void SetReady(const bool ready) noexcept
    {
        SpinLockGuard guard{m_synchronization};
        m_ready = ready;
    }

    IommuDeviceError NextRelease{IommuDeviceError::None};
    UInt64 ClaimCalls{0U};
    UInt64 ReleaseCalls{0U};
    Foundation::UInt32 RequesterId{0x0010U};

private:
    mutable SpinLock m_synchronization;
    const IommuDomain* m_owner{nullptr};
    bool m_ready{true};
};

class FakeIommuBackend final : public IIommuDomainBackend
{
public:
    [[nodiscard]] Hal::Device::IommuBackendFaults GetFaults() noexcept override
    {
        return Hal::Device::IommuBackendFaults{
            .Count = Faults, .LastSource = 0x0010U, .LastReason = 5U, .Reserved = {}};
    }

    UInt64 Faults{0U};

    [[nodiscard]] bool IsReady() const noexcept override
    {
        SpinLockGuard guard{m_synchronization};
        return m_ready;
    }

    [[nodiscard]] IommuBackendOwnershipError Claim(const IommuDomain& owner,
                                                   const Foundation::UInt8 inputAddressBits) noexcept override
    {
        SpinLockGuard guard{m_synchronization};
        ++ClaimCalls;
        if (NextClaim != IommuBackendOwnershipError::None)
        {
            const IommuBackendOwnershipError error = NextClaim;
            NextClaim = IommuBackendOwnershipError::None;
            return error;
        }
        if (!m_ready)
        {
            return IommuBackendOwnershipError::NotReady;
        }
        if (m_owner != nullptr)
        {
            return IommuBackendOwnershipError::AlreadyOwned;
        }
        if ((inputAddressBits < 32U) || (inputAddressBits > 64U))
        {
            return IommuBackendOwnershipError::UnsupportedConfiguration;
        }
        m_owner = &owner;
        m_inputAddressBits = inputAddressBits;
        return IommuBackendOwnershipError::None;
    }

    [[nodiscard]] IommuBackendOwnershipError Release(const IommuDomain& owner) noexcept override
    {
        SpinLockGuard guard{m_synchronization};
        ++ReleaseCalls;
        if (NextOwnershipRelease != IommuBackendOwnershipError::None)
        {
            const IommuBackendOwnershipError error = NextOwnershipRelease;
            NextOwnershipRelease = IommuBackendOwnershipError::None;
            return error;
        }
        if (m_owner != &owner)
        {
            return IommuBackendOwnershipError::InvalidOwner;
        }
        m_owner = nullptr;
        m_inputAddressBits = 0U;
        for (NativeMapping& mapping : m_mappings)
        {
            mapping = NativeMapping{};
        }
        for (IIommuDevice*& device : m_devices)
        {
            device = nullptr;
        }
        m_mappingCount = 0U;
        m_deviceCount = 0U;
        return IommuBackendOwnershipError::None;
    }

    [[nodiscard]] Kernel::Device::DmaAddress
    ChooseDeviceAddress(Kernel::Device::DmaAddress requested, Kernel::Memory::PhysicalAddress) const noexcept override
    {
        return requested;
    }

    [[nodiscard]] IommuBackendError Map(const IommuBackendMapRequest& request) noexcept override
    {
        SpinLockGuard guard{m_synchronization};
        ++MapCalls;
        const IommuBackendError error = NextMap;
        NextMap = IommuBackendError::None;
        if ((error != IommuBackendError::None) && (error != IommuBackendError::CommittedStateUnknown))
        {
            return error;
        }
        if (m_owner == nullptr)
        {
            return IommuBackendError::NotReady;
        }
        for (NativeMapping& mapping : m_mappings)
        {
            if (!mapping.Occupied)
            {
                mapping.Request = request;
                mapping.Occupied = true;
                ++m_mappingCount;
                return error;
            }
        }
        return IommuBackendError::CapacityExceeded;
    }

    [[nodiscard]] IommuBackendError Unmap(const IommuBackendMapRequest& request) noexcept override
    {
        SpinLockGuard guard{m_synchronization};
        ++UnmapCalls;
        const IommuBackendError error = NextUnmap;
        NextUnmap = IommuBackendError::None;
        if (error != IommuBackendError::None)
        {
            return error;
        }
        for (NativeMapping& mapping : m_mappings)
        {
            if (mapping.Occupied && RequestsEqual(mapping.Request, request))
            {
                mapping = NativeMapping{};
                --m_mappingCount;
                return IommuBackendError::None;
            }
        }
        return IommuBackendError::InvalidRequest;
    }

    [[nodiscard]] IommuBackendError Attach(IIommuDevice& device) noexcept override
    {
        SpinLockGuard guard{m_synchronization};
        ++AttachCalls;
        const IommuBackendError error = NextAttach;
        NextAttach = IommuBackendError::None;
        if ((error != IommuBackendError::None) && (error != IommuBackendError::CommittedStateUnknown))
        {
            return error;
        }
        for (IIommuDevice*& entry : m_devices)
        {
            if (entry == nullptr)
            {
                entry = &device;
                ++m_deviceCount;
                return error;
            }
        }
        return IommuBackendError::CapacityExceeded;
    }

    [[nodiscard]] IommuBackendError Detach(IIommuDevice& device) noexcept override
    {
        SpinLockGuard guard{m_synchronization};
        ++DetachCalls;
        const IommuBackendError error = NextDetach;
        NextDetach = IommuBackendError::None;
        if (error != IommuBackendError::None)
        {
            return error;
        }
        for (IIommuDevice*& entry : m_devices)
        {
            if (entry == &device)
            {
                entry = nullptr;
                --m_deviceCount;
                return IommuBackendError::None;
            }
        }
        return IommuBackendError::InvalidRequest;
    }

    [[nodiscard]] Size GetMappingCount() const noexcept
    {
        SpinLockGuard guard{m_synchronization};
        return m_mappingCount;
    }

    [[nodiscard]] Size GetDeviceCount() const noexcept
    {
        SpinLockGuard guard{m_synchronization};
        return m_deviceCount;
    }

    [[nodiscard]] IommuBackendMapRequest GetMapping(const Size index) const noexcept
    {
        SpinLockGuard guard{m_synchronization};
        Size occupiedIndex{0U};
        for (const NativeMapping& mapping : m_mappings)
        {
            if (!mapping.Occupied)
            {
                continue;
            }
            if (occupiedIndex == index)
            {
                return mapping.Request;
            }
            ++occupiedIndex;
        }
        return IommuBackendMapRequest{};
    }

    void SetReady(const bool ready) noexcept
    {
        SpinLockGuard guard{m_synchronization};
        m_ready = ready;
    }

    IommuBackendOwnershipError NextClaim{IommuBackendOwnershipError::None};
    IommuBackendOwnershipError NextOwnershipRelease{IommuBackendOwnershipError::None};
    IommuBackendError NextMap{IommuBackendError::None};
    IommuBackendError NextUnmap{IommuBackendError::None};
    IommuBackendError NextAttach{IommuBackendError::None};
    IommuBackendError NextDetach{IommuBackendError::None};
    UInt64 ClaimCalls{0U};
    UInt64 ReleaseCalls{0U};
    UInt64 MapCalls{0U};
    UInt64 UnmapCalls{0U};
    UInt64 AttachCalls{0U};
    UInt64 DetachCalls{0U};

private:
    struct NativeMapping
    {
        IommuBackendMapRequest Request{};
        bool Occupied{false};
    };

    mutable SpinLock m_synchronization;
    const IommuDomain* m_owner{nullptr};
    NativeMapping m_mappings[IommuDomain::MaximumMappings]{};
    IIommuDevice* m_devices[IommuDomain::MaximumAttachments]{};
    Size m_mappingCount{0U};
    Size m_deviceCount{0U};
    Foundation::UInt8 m_inputAddressBits{0U};
    bool m_ready{true};
};

[[nodiscard]] constexpr ResourceDomainLimits MakeLimits() noexcept
{
    return ResourceDomainLimits{
        .CommittedMemoryPages = 0U,
        .ResidentMemoryPages = 0U,
        .KernelMetadataBytes = 1U << 20U,
        .CapabilityHandles = 64U,
        .IpcQueueBytes = 0U,
        .Threads = 0U,
        .ChildDomains = 0U,
    };
}

[[nodiscard]] constexpr FrameDescriptor MakeFrameDescriptor(const UInt64 physicalStart = 0x0010'0000U,
                                                            const UInt64 pages = 128U,
                                                            const FrameKind kind = FrameKind::Ram) noexcept
{
    return FrameDescriptor{
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Pages = PageCount{pages},
        .CachePolicy = kind == FrameKind::Ram ? MemoryCachePolicy::WriteBack : MemoryCachePolicy::Uncached,
        .Kind = kind,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr DmaPermissions ReadWriteDma() noexcept
{
    return DmaPermissions{DmaPermission::Read}.With(DmaPermission::Write);
}

class DomainFixture final
{
public:
    explicit DomainFixture(const Kernel::Device::IDmaReservations* const reservations = nullptr) noexcept
        : Registry{RegistrySynchronization}, Issuer{Registry}, Resource{Ledger}, Domain{Registry, Backend, reservations}
    {
    }

    [[nodiscard]] bool Initialize(const UInt64 physicalStart = 0x0010'0000U, const UInt64 pages = 128U,
                                  const Foundation::UInt8 inputAddressBits = 48U) noexcept
    {
        if (Resource.InitializeRoot(ResourceDomainId{91U}, MakeLimits()).Error != ResourceDomainError::None)
        {
            return false;
        }
        const auto resourceObject = Issuer.CreateObject(Resource);
        if (!resourceObject.HasValue())
        {
            return false;
        }
        ResourceObject = resourceObject.GetValue();
        ResourceIdentity = ResourceDomainObjectId{Registry, ResourceObject};

        if (FrameStorage.Initialize(ResourceDomainId{91U}, MakeFrameDescriptor(physicalStart, pages)) !=
            FrameError::None)
        {
            return false;
        }
        const auto frameObject = Issuer.CreateObject(FrameStorage);
        if (!frameObject.HasValue())
        {
            return false;
        }
        FrameObject = frameObject.GetValue();
        FrameIdentity = FrameId{Registry, FrameObject};

        const auto initialized = Domain.Initialize(ResourceIdentity, Resource,
                                                   IommuDomainConfiguration{
                                                       .InputAddressBits = inputAddressBits,
                                                       .Reserved = {},
                                                   });
        if (initialized.Error != IommuDomainError::None)
        {
            return false;
        }
        const auto domainObject = Issuer.CreateObject(Domain);
        if (!domainObject.HasValue())
        {
            return false;
        }
        DomainObject = domainObject.GetValue();
        return true;
    }

    [[nodiscard]] IommuMappingRequest MakeRequest(const UInt64 dmaStart, const UInt64 offsetPages, const UInt64 pages,
                                                  const DmaPermissions permissions = DmaPermissions{
                                                      DmaPermission::Read}) noexcept
    {
        return IommuMappingRequest{
            .FrameStorage = &FrameStorage,
            .FrameIdentity = FrameIdentity,
            .DmaStart = DmaAddress{dmaStart},
            .FrameOffsetPages = PageCount{offsetPages},
            .Pages = PageCount{pages},
            .Permissions = permissions,
            .Reserved = {},
        };
    }

    [[nodiscard]] bool Finish() noexcept
    {
        const auto snapshot = Domain.GetSnapshot();
        if (snapshot.State != IommuDomainState::Quiesced)
        {
            if (Domain.Quiesce().Error != IommuDomainError::None)
            {
                return false;
            }
        }
        if (DomainObject.IsValid())
        {
            if (Issuer.DestroyObject(DomainObject) != ObjectRegistryError::Ok)
            {
                return false;
            }
            const auto collected = Issuer.CollectDestroyedObject();
            if (!collected.HasValue() || (collected.GetValue().Instance != &Domain))
            {
                return false;
            }
            DomainObject = ObjectId{};
        }
        if (FrameObject.IsValid())
        {
            if (Issuer.DestroyObject(FrameObject) != ObjectRegistryError::Ok)
            {
                return false;
            }
            const auto collected = Issuer.CollectDestroyedObject();
            if (!collected.HasValue() || (collected.GetValue().Instance != &FrameStorage))
            {
                return false;
            }
            FrameObject = ObjectId{};
        }
        if ((Resource.Quiesce() != ResourceDomainError::None) ||
            (Issuer.DestroyObject(ResourceObject) != ObjectRegistryError::Ok))
        {
            return false;
        }
        const auto collected = Issuer.CollectDestroyedObject();
        return collected.HasValue() && (collected.GetValue().Instance == &Resource);
    }

    SpinLock RegistrySynchronization;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RootCapabilityIssuer Issuer;
    ResourceDomainLedger Ledger;
    ResourceDomain Resource;
    FakeIommuBackend Backend;
    IommuDomain Domain;
    BootFrame FrameStorage;
    ObjectId ResourceObject{};
    ResourceDomainObjectId ResourceIdentity{};
    ObjectId FrameObject{};
    FrameId FrameIdentity{};
    ObjectId DomainObject{};
};

void TestInitializationFailures(TestContext& runner) noexcept
{
    SpinLock synchronization;
    Asteria::TestSupport::TestObjectRegistry registry{synchronization};
    RootCapabilityIssuer issuer{registry};
    ResourceDomainLedger ledger;
    ResourceDomain resource{ledger};
    const auto initialized = resource.InitializeRoot(ResourceDomainId{101U}, MakeLimits());
    const auto registered = issuer.CreateObject(resource);
    const ObjectId resourceObject = registered.HasValue() ? registered.GetValue() : ObjectId{};
    const ResourceDomainObjectId identity{registry, resourceObject};
    FakeIommuBackend backend;
    IommuDomain domain{registry, backend};
    ASTERIA_EXPECT(runner, "initialization-failure fixture publishes its ResourceDomain",
                   initialized.Error == ResourceDomainError::None, registered.HasValue());

    IommuDomainConfiguration configuration{
        .InputAddressBits = 31U,
        .Reserved = {},
    };
    const auto tooNarrow = domain.Initialize(identity, resource, configuration);
    configuration.InputAddressBits = 48U;
    configuration.Reserved[3] = 1U;
    const auto reserved = domain.Initialize(identity, resource, configuration);
    ASTERIA_EXPECT(runner, "invalid address width and reserved bytes are completely inert",
                   tooNarrow.Error == IommuDomainError::InvalidConfiguration,
                   reserved.Error == IommuDomainError::InvalidConfiguration, backend.ClaimCalls == 0U,
                   domain.GetSnapshot().State == IommuDomainState::Uninitialized,
                   resource.GetSnapshot().Usage.KernelMetadataBytes == 0U);

    SpinLock foreignSynchronization;
    Asteria::TestSupport::TestObjectRegistry foreignRegistry{foreignSynchronization};
    configuration.Reserved[3] = 0U;
    const auto foreign =
        domain.Initialize(ResourceDomainObjectId{foreignRegistry, resourceObject}, resource, configuration);
    ASTERIA_EXPECT(runner, "ResourceDomain identity carries exact registry provenance",
                   foreign.Error == IommuDomainError::ResourceDomainRegistryMismatch, backend.ClaimCalls == 0U,
                   resource.GetSnapshot().Usage.KernelMetadataBytes == 0U);

    backend.NextClaim = IommuBackendOwnershipError::UnsupportedConfiguration;
    const auto backendFailure = domain.Initialize(identity, resource, configuration);
    ASTERIA_EXPECT(runner, "failed backend claim rolls back metadata charge and ResourceDomain pin",
                   backendFailure.Error == IommuDomainError::BackendRejected,
                   backendFailure.Ownership == IommuBackendOwnershipError::UnsupportedConfiguration,
                   domain.GetSnapshot().State == IommuDomainState::Uninitialized,
                   resource.GetSnapshot().Usage.KernelMetadataBytes == 0U);

    const auto retry = domain.Initialize(identity, resource, configuration);
    ASTERIA_EXPECT(
        runner, "initialization retry charges and releases the exact fixed object size",
        retry.Error == IommuDomainError::None, resource.GetSnapshot().Usage.KernelMetadataBytes == sizeof(IommuDomain),
        domain.Quiesce().Error == IommuDomainError::None, resource.GetSnapshot().Usage.KernelMetadataBytes == 0U);
    const bool cleanup = (resource.Quiesce() == ResourceDomainError::None) &&
                         (issuer.DestroyObject(resourceObject) == ObjectRegistryError::Ok) &&
                         issuer.CollectDestroyedObject().HasValue();
    ASTERIA_EXPECT(runner, "initialization-failure fixture cleans up exactly", cleanup);
}

void TestMappingLifecycle(TestContext& runner) noexcept
{
    DomainFixture fixture;
    ASTERIA_EXPECT(runner, "IOMMU fixture initializes", fixture.Initialize());
    const auto initial = fixture.Domain.GetSnapshot();
    ASTERIA_EXPECT(runner, "initialization publishes exact owner, backend, and configuration",
                   initial.State == IommuDomainState::Ready, initial.BackendClaimed, initial.MappingCount == 0U,
                   initial.AttachmentCount == 0U, initial.Configuration.InputAddressBits == 48U,
                   fixture.Domain.GetObjectType() == ObjectType::IommuDomain,
                   fixture.Domain.GetChargeOwner().GetValue() == 91U);

    const auto mapped = fixture.Domain.Map(fixture.MakeRequest(0x0020'0000U, 2U, 4U));
    const auto native = fixture.Backend.GetMapping(0U);
    ASTERIA_EXPECT(runner, "Map publishes the exact Frame subrange to the backend", mapped.HasValue(),
                   mapped.GetValue().Mapping.IsValid(), mapped.GetValue().PhysicalStart.GetValue() == 0x0010'2000U,
                   mapped.GetValue().Pages.GetValue() == 4U, fixture.Backend.GetMappingCount() == 1U,
                   native.DmaStart.GetValue() == 0x0020'0000U, native.PhysicalStart.GetValue() == 0x0010'2000U);

    const auto beforeOverlap = fixture.Domain.GetSnapshot();
    const auto overlap = fixture.Domain.Map(fixture.MakeRequest(0x0020'1000U, 8U, 1U));
    const auto afterOverlap = fixture.Domain.GetSnapshot();
    ASTERIA_EXPECT(runner, "overlapping device ranges are rejected before backend mutation", !overlap.HasValue(),
                   overlap.GetError().Error == IommuDomainError::DmaRangeOverlap,
                   beforeOverlap.MappingCount == afterOverlap.MappingCount,
                   beforeOverlap.LastMappingSequence == afterOverlap.LastMappingSequence,
                   fixture.Backend.MapCalls == 1U);

    const auto mapping = mapped.GetValue().Mapping;
    ASTERIA_EXPECT(runner, "Unmap consumes one exact mapping generation",
                   fixture.Domain.Unmap(mapping).Error == IommuDomainError::None,
                   fixture.Backend.GetMappingCount() == 0U, fixture.Domain.GetSnapshot().MappingCount == 0U,
                   fixture.Domain.Unmap(mapping).Error == IommuDomainError::StaleMapping);

    // By device address (the IommuControl system call), and the backend's
    // Fault record passed through.
    const auto remapped = fixture.Domain.Map(fixture.MakeRequest(0x0040'0000U, 0U, 1U));
    fixture.Backend.Faults = 2U;
    const auto faults = fixture.Domain.GetFaults();
    ASTERIA_EXPECT(runner, "UnmapAt removes the mapping that starts at a device address", remapped.HasValue(),
                   fixture.Domain.UnmapAt(DmaAddress{0x0040'1000U}).Error == IommuDomainError::InvalidMapping,
                   fixture.Domain.UnmapAt(DmaAddress{0x0040'0000U}).Error == IommuDomainError::None,
                   fixture.Backend.GetMappingCount() == 0U,
                   fixture.Domain.UnmapAt(DmaAddress{0x0040'0000U}).Error == IommuDomainError::InvalidMapping,
                   faults.Count == 2U, faults.LastSource == 0x0010U, faults.LastReason == 5U);
    ASTERIA_EXPECT(runner, "empty healthy domain quiesces and releases its charge",
                   fixture.Domain.Quiesce().Error == IommuDomainError::None,
                   fixture.Domain.GetSnapshot().State == IommuDomainState::Quiesced, !fixture.Domain.IsInitialized());
    ASTERIA_EXPECT(runner, "mapping fixture cleans up exactly", fixture.Finish());
}

/// Takes up to two parked mappings, as PciFunctionControl does for a drain.
class RecordingParker final : public Kernel::Device::IIommuStorageParker
{
public:
    explicit RecordingParker(const Size room) noexcept : Room{room}
    {
    }

    [[nodiscard]] bool CanPark(const Size count) noexcept override
    {
        ++Asked;
        return count <= Room;
    }

    void Park(const Kernel::Device::IommuParkedMapping& mapping) noexcept override
    {
        if (Count >= 2U)
        {
            return;
        }
        Pins[Count].~ObjectPin();
        ::new (static_cast<void*>(&Pins[Count]))
            Kernel::Object::ObjectPin{static_cast<Kernel::Object::ObjectPin&&>(*mapping.Pin)};
        Mappings[Count] = mapping;
        ++Count;
    }

    // Releases everything parked, as a confirmation does.
    void ReleaseAll() noexcept
    {
        for (Size index{0U}; index < Count; ++index)
        {
            const Kernel::Device::IommuParkedMapping& mapping = Mappings[index];
            IommuDomain::ReleaseParkedMapping(Pins[index], *mapping.FrameStorage, *mapping.Registry,
                                              mapping.FrameObject, mapping.PhysicalStart, mapping.Pages,
                                              mapping.PolicyPermissions);
        }
        Count = 0U;
    }

    Size Room;
    Size Asked{0U};
    Size Count{0U};
    Kernel::Object::ObjectPin Pins[2U];
    Kernel::Device::IommuParkedMapping Mappings[2U]{};
};

// A drained domain whose DMA no one can prove ended hands its mappings' pins and Frame policies to
// a parker instead of releasing them; one without room refuses the whole quiesce, nothing changed.
// A Frame destroyed meanwhile cannot be collected until the parked pins go.
void TestParkedMappings(TestContext& runner) noexcept
{
    DomainFixture fixture;
    const bool ready = fixture.Initialize();
    const auto read = fixture.Domain.Map(fixture.MakeRequest(0x0020'0000U, 0U, 1U));
    const auto written = fixture.Domain.Map(fixture.MakeRequest(0x0030'0000U, 4U, 2U, ReadWriteDma()));
    RecordingParker cramped{1U};
    const IommuDomainError refused = fixture.Domain.Quiesce(&cramped).Error;
    const auto untouched = fixture.Domain.GetSnapshot();
    const Size backendMappings = fixture.Backend.GetMappingCount();
    RecordingParker parker{2U};
    const IommuDomainError parked = fixture.Domain.Quiesce(&parker).Error;
    const auto quiesced = fixture.Domain.GetSnapshot();
    ASTERIA_EXPECT(runner, "a parker without room refuses the quiesce; one with room takes every mapping", ready,
                   read.HasValue(), written.HasValue(), refused == IommuDomainError::ParkingRefused,
                   untouched.State == IommuDomainState::Ready, untouched.MappingCount == 2U, untouched.BackendClaimed,
                   backendMappings == 2U, cramped.Count == 0U, parked == IommuDomainError::None,
                   quiesced.State == IommuDomainState::Quiesced, quiesced.MappingCount == 0U, !quiesced.BackendClaimed,
                   parker.Count == 2U, parker.Mappings[0U].FrameObject.GetValue() == fixture.FrameObject.GetValue(),
                   parker.Mappings[0U].PhysicalStart.GetValue() == 0x0010'0000U,
                   parker.Mappings[1U].Pages.GetValue() == 2U);

    // The Frame's owner destroys it: the parked pins keep it.
    const bool destroyed = fixture.Issuer.DestroyObject(fixture.FrameObject) == ObjectRegistryError::Ok;
    const auto early = fixture.Issuer.CollectDestroyedObject(fixture.FrameObject);
    parker.ReleaseAll();
    const auto collected = fixture.Issuer.CollectDestroyedObject(fixture.FrameObject);
    const bool frameCollected = collected.HasValue() && (collected.GetValue().Instance == &fixture.FrameStorage);
    fixture.FrameObject = ObjectId{};
    ASTERIA_EXPECT(runner, "a destroyed Frame stays uncollected until its parked pins are released", destroyed,
                   !early.HasValue(), early.HasValue() || (early.GetError() == ObjectRegistryError::NoDestructionReady),
                   frameCollected);
    ASTERIA_EXPECT(runner, "parking fixture cleans up exactly", fixture.Finish());
}

void TestMappingValidationAndRollback(TestContext& runner) noexcept
{
    DomainFixture fixture;
    ASTERIA_EXPECT(runner, "validation fixture initializes", fixture.Initialize());

    auto invalid = fixture.MakeRequest(0x1003U, 0U, 1U);
    const auto misaligned = fixture.Domain.Map(invalid);
    invalid = fixture.MakeRequest(0x0001'0000'0000'0000ULL, 0U, 1U);
    const auto outsideInput = fixture.Domain.Map(invalid);
    invalid = fixture.MakeRequest(0x0030'0000U, 127U, 2U);
    const auto outsideFrame = fixture.Domain.Map(invalid);
    invalid = fixture.MakeRequest(0x0030'0000U, 0U, 1U);
    invalid.Reserved[6] = 1U;
    const auto reserved = fixture.Domain.Map(invalid);
    invalid = fixture.MakeRequest(0x0030'0000U, 0U, 1U);
    invalid.Permissions = DmaPermissions{};
    const auto emptyPermissions = fixture.Domain.Map(invalid);
    invalid.Permissions = std::bit_cast<DmaPermissions>(Foundation::UInt8{0x80U});
    const auto unknownPermissions = fixture.Domain.Map(invalid);
    ASTERIA_EXPECT(
        runner, "range, permission, and reserved-field failures are inert and pre-HAL", !misaligned.HasValue(),
        misaligned.GetError().Error == IommuDomainError::DmaRangeExceeded, !outsideInput.HasValue(),
        outsideInput.GetError().Error == IommuDomainError::DmaRangeExceeded, !outsideFrame.HasValue(),
        outsideFrame.GetError().Error == IommuDomainError::FrameRangeExceeded, !reserved.HasValue(),
        reserved.GetError().Error == IommuDomainError::InvalidMappingRequest, !emptyPermissions.HasValue(),
        emptyPermissions.GetError().Error == IommuDomainError::InvalidMappingRequest, !unknownPermissions.HasValue(),
        unknownPermissions.GetError().Error == IommuDomainError::InvalidMappingRequest, fixture.Backend.MapCalls == 0U,
        fixture.Domain.GetSnapshot().MappingCount == 0U);

    BootFrame unrelated;
    ASTERIA_EXPECT(runner, "unrelated Frame initializes for instance validation",
                   unrelated.Initialize(ResourceDomainId{91U}, MakeFrameDescriptor(0x0040'0000U, 4U)) ==
                       FrameError::None);
    invalid = fixture.MakeRequest(0x0030'0000U, 0U, 1U);
    invalid.FrameStorage = &unrelated;
    const auto mismatch = fixture.Domain.Map(invalid);
    ASTERIA_EXPECT(runner, "Frame identity is bound to the exact pinned instance", !mismatch.HasValue(),
                   mismatch.GetError().Error == IommuDomainError::FrameInstanceMismatch,
                   fixture.Domain.GetSnapshot().MappingCount == 0U);

    BootFrame deviceFrame;
    ASTERIA_EXPECT(
        runner, "device Frame initializes for RAM-only DMA validation",
        deviceFrame.Initialize(ResourceDomainId{91U}, MakeFrameDescriptor(0x0050'0000U, 4U, FrameKind::Device)) ==
            FrameError::None);
    const auto deviceFrameObject = fixture.Issuer.CreateObject(deviceFrame);
    invalid = fixture.MakeRequest(0x0030'0000U, 0U, 1U);
    invalid.FrameStorage = &deviceFrame;
    invalid.FrameIdentity =
        deviceFrameObject.HasValue() ? FrameId{fixture.Registry, deviceFrameObject.GetValue()} : FrameId{};
    const auto deviceFrameMapping = fixture.Domain.Map(invalid);
    ASTERIA_EXPECT(runner, "IOMMU mapping rejects MMIO/device Frames before the backend", deviceFrameObject.HasValue(),
                   !deviceFrameMapping.HasValue(),
                   deviceFrameMapping.GetError().Error == IommuDomainError::RamFrameRequired,
                   fixture.Backend.MapCalls == 0U);
    bool deviceFrameCollected{false};
    if (deviceFrameObject.HasValue())
    {
        const ObjectRegistryError destroyed = fixture.Issuer.DestroyObject(deviceFrameObject.GetValue());
        const auto collected = fixture.Issuer.CollectDestroyedObject();
        deviceFrameCollected = (destroyed == ObjectRegistryError::Ok) && collected.HasValue() &&
                               (collected.GetValue().Instance == &deviceFrame);
    }
    ASTERIA_EXPECT(runner, "rejected device Frame retains no IOMMU pin", deviceFrameCollected);

    SpinLock foreignSynchronization;
    Asteria::TestSupport::TestObjectRegistry foreignRegistry{foreignSynchronization};
    invalid = fixture.MakeRequest(0x0030'0000U, 0U, 1U);
    invalid.FrameIdentity = FrameId{foreignRegistry, fixture.FrameObject};
    const auto foreignFrame = fixture.Domain.Map(invalid);
    ASTERIA_EXPECT(runner, "Frame identity retains exact ObjectRegistry provenance", !foreignFrame.HasValue(),
                   foreignFrame.GetError().Error == IommuDomainError::FrameRegistryMismatch,
                   fixture.Backend.MapCalls == 0U, fixture.Domain.GetSnapshot().MappingCount == 0U);

    fixture.Backend.NextMap = IommuBackendError::CapacityExceeded;
    const auto ordinary = fixture.Domain.Map(fixture.MakeRequest(0x0030'0000U, 0U, 1U, ReadWriteDma()));
    ASTERIA_EXPECT(runner, "ordinary backend rejection rolls back pin and Frame policy", !ordinary.HasValue(),
                   ordinary.GetError().Backend == IommuBackendError::CapacityExceeded,
                   fixture.Domain.GetSnapshot().State == IommuDomainState::Ready,
                   fixture.Domain.GetSnapshot().MappingCount == 0U, fixture.Backend.GetMappingCount() == 0U);

    const auto retry = fixture.Domain.Map(fixture.MakeRequest(0x0030'0000U, 0U, 1U, ReadWriteDma()));
    ASTERIA_EXPECT(runner, "ordinary failure does not consume sequence and can be retried", retry.HasValue(),
                   retry.GetValue().Mapping.GetSequence() == 1U,
                   fixture.Domain.Unmap(retry.GetValue().Mapping).Error == IommuDomainError::None);
    ASTERIA_EXPECT(runner, "validation fixture cleans up exactly",
                   fixture.Domain.Quiesce().Error == IommuDomainError::None, fixture.Finish());
}

void TestAmbiguousMappingRecovery(TestContext& runner) noexcept
{
    DomainFixture fixture;
    ASTERIA_EXPECT(runner, "ambiguous-map fixture initializes", fixture.Initialize());
    fixture.Backend.NextMap = IommuBackendError::CommittedStateUnknown;
    const auto mapped = fixture.Domain.Map(fixture.MakeRequest(0x0040'0000U, 0U, 2U, ReadWriteDma()));
    const auto faulted = fixture.Domain.GetSnapshot();
    ASTERIA_EXPECT(runner, "ambiguous Map retains the possible translation and faults the domain", !mapped.HasValue(),
                   mapped.GetError().Backend == IommuBackendError::CommittedStateUnknown,
                   faulted.State == IommuDomainState::Faulted, faulted.MappingCount == 1U,
                   faulted.LastMappingSequence == 1U, fixture.Backend.GetMappingCount() == 1U);

    ASTERIA_EXPECT(runner, "faulted mapping pin prevents Frame collection",
                   fixture.Issuer.DestroyObject(fixture.FrameObject) == ObjectRegistryError::Ok,
                   !fixture.Issuer.CollectDestroyedObject().HasValue());
    ASTERIA_EXPECT(runner, "fault recovery releases native state before retained Frame authority",
                   fixture.Domain.Quiesce().Error == IommuDomainError::None, fixture.Backend.GetMappingCount() == 0U,
                   fixture.Domain.GetSnapshot().MappingCount == 0U);
    const auto collected = fixture.Issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "Frame becomes collectable only after faulted-domain recovery", collected.HasValue(),
                   collected.GetValue().Instance == &fixture.FrameStorage);
    fixture.FrameObject = ObjectId{};
    ASTERIA_EXPECT(runner, "ambiguous-map fixture cleans up exactly", fixture.Finish());
}

void TestMaximumDmaBoundary(TestContext& runner) noexcept
{
    DomainFixture fixture;
    ASTERIA_EXPECT(runner, "64-bit DMA boundary fixture initializes", fixture.Initialize(0x30'142F'0000ULL, 4U, 64U));
    const auto oversized = fixture.Domain.Map(fixture.MakeRequest(0U, 0U, 0x0010'0000'0000'0000ULL));
    ASTERIA_EXPECT(runner, "DMA byte-size multiplication overflow is rejected before HAL", !oversized.HasValue(),
                   oversized.GetError().Error == IommuDomainError::DmaRangeExceeded, fixture.Backend.MapCalls == 0U,
                   fixture.Domain.GetSnapshot().MappingCount == 0U);
    const auto wrapping = fixture.Domain.Map(fixture.MakeRequest(0xFFFF'FFFF'FFFF'F000ULL, 0U, 2U));
    ASTERIA_EXPECT(runner, "a DMA range crossing the 64-bit ceiling is rejected before HAL", !wrapping.HasValue(),
                   wrapping.GetError().Error == IommuDomainError::DmaRangeExceeded, fixture.Backend.MapCalls == 0U,
                   fixture.Domain.GetSnapshot().MappingCount == 0U);
    const auto mapped = fixture.Domain.Map(fixture.MakeRequest(0xFFFF'FFFF'FFFF'F000ULL, 0U, 1U));
    ASTERIA_EXPECT(runner, "the final aligned 64-bit DMA page is representable without wrap", mapped.HasValue(),
                   mapped.GetValue().DmaStart.GetValue() == 0xFFFF'FFFF'FFFF'F000ULL,
                   fixture.Backend.GetMapping(0U).DmaStart.GetValue() == 0xFFFF'FFFF'FFFF'F000ULL,
                   fixture.Backend.GetMapping(0U).PhysicalStart.GetValue() == 0x30'142F'0000ULL);
    ASTERIA_EXPECT(runner, "maximum DMA boundary mapping cleans up exactly", mapped.HasValue(),
                   fixture.Domain.Unmap(mapped.GetValue().Mapping).Error == IommuDomainError::None,
                   fixture.Domain.Quiesce().Error == IommuDomainError::None, fixture.Finish());
}

void TestUnmapFailureRecovery(TestContext& runner) noexcept
{
    DomainFixture fixture;
    ASTERIA_EXPECT(runner, "unmap-failure fixture initializes", fixture.Initialize(0x0700'0000U, 8U));
    const auto mapped = fixture.Domain.Map(fixture.MakeRequest(0x3000'0000U, 0U, 2U, ReadWriteDma()));
    fixture.Backend.NextUnmap = IommuBackendError::HardwareFailure;
    const auto failed =
        mapped.HasValue() ? fixture.Domain.Unmap(mapped.GetValue().Mapping) : Kernel::Device::IommuDomainFailure{};
    ASTERIA_EXPECT(runner, "failed Unmap keeps the mapping policy and token conservatively live", mapped.HasValue(),
                   failed.Error == IommuDomainError::BackendRejected,
                   failed.Backend == IommuBackendError::HardwareFailure,
                   fixture.Domain.GetSnapshot().State == IommuDomainState::Faulted,
                   fixture.Domain.GetSnapshot().MappingCount == 1U, fixture.Backend.GetMappingCount() == 1U);

    fixture.Backend.NextOwnershipRelease = IommuBackendOwnershipError::HardwareFailure;
    const auto firstRecovery = fixture.Domain.Quiesce();
    ASTERIA_EXPECT(runner, "failed backend release retains every ambiguous mapping authority",
                   firstRecovery.Error == IommuDomainError::BackendRejected,
                   firstRecovery.Ownership == IommuBackendOwnershipError::HardwareFailure,
                   fixture.Domain.GetSnapshot().MappingCount == 1U, fixture.Backend.GetMappingCount() == 1U);
    ASTERIA_EXPECT(runner, "retrying terminal recovery releases backend before mapping authority",
                   fixture.Domain.Quiesce().Error == IommuDomainError::None,
                   fixture.Domain.GetSnapshot().MappingCount == 0U, fixture.Backend.GetMappingCount() == 0U,
                   fixture.Finish());
}

void TestUnexposedStorageRelease(TestContext& runner, const bool refusedAttach) noexcept
{
    DomainFixture fixture;
    ASTERIA_CHECK(runner, fixture.Initialize(0x0800'0000U, 8U));
    fixture.Domain.RequireDmaQuiescence();
    const auto mapped = fixture.Domain.Map(fixture.MakeRequest(0x0020'0000U, 0U, 1U));
    ASTERIA_CHECK(runner, mapped.HasValue());
    FakeIommuDevice device;
    if (refusedAttach)
    {
        fixture.Backend.NextAttach = IommuBackendError::UnsupportedConfiguration;
        const auto attached = fixture.Domain.Attach(device);
        ASTERIA_EXPECT(runner, "ordinary Attach refusal leaves the storage unexposed", !attached.HasValue(),
                       attached.GetError().Backend == IommuBackendError::UnsupportedConfiguration,
                       fixture.Backend.GetDeviceCount() == 0U, !device.IsClaimedBy(fixture.Domain));
    }
    ASTERIA_EXPECT(runner, "a quiet-required domain with no requester retains its retired mapping until release",
                   fixture.Domain.Unmap(mapped.GetValue().Mapping).Error == IommuDomainError::None,
                   fixture.Domain.GetSnapshot().MappingCount == 1U, fixture.Domain.GetSnapshot().AttachmentCount == 0U);
    ASTERIA_EXPECT(runner, "never-exposed storage can quiesce without a device-quiet parker",
                   fixture.Domain.Quiesce().Error == IommuDomainError::None,
                   fixture.Domain.GetSnapshot().MappingCount == 0U, fixture.Finish());
}

void TestAmbiguousAttachmentRecovery(TestContext& runner) noexcept
{
    DomainFixture fixture;
    ASTERIA_EXPECT(runner, "ambiguous-attachment fixture initializes", fixture.Initialize(0x0800'0000U, 8U));
    FakeIommuDevice device;
    fixture.Backend.NextAttach = IommuBackendError::CommittedStateUnknown;
    const auto attached = fixture.Domain.Attach(device);
    ASTERIA_EXPECT(runner, "ambiguous Attach retains the endpoint claim and faults the domain", !attached.HasValue(),
                   attached.GetError().Backend == IommuBackendError::CommittedStateUnknown,
                   fixture.Domain.GetSnapshot().State == IommuDomainState::Faulted,
                   fixture.Domain.GetSnapshot().AttachmentCount == 1U, fixture.Backend.GetDeviceCount() == 1U,
                   device.IsClaimedBy(fixture.Domain));
    ASTERIA_EXPECT(runner, "ambiguous attachment recovery releases native binding before claim",
                   fixture.Domain.Quiesce().Error == IommuDomainError::None, fixture.Backend.GetDeviceCount() == 0U,
                   !device.IsClaimedBy(fixture.Domain), fixture.Finish());
}

void TestAttachmentRollbackFailureRecovery(TestContext& runner) noexcept
{
    DomainFixture fixture;
    ASTERIA_EXPECT(runner, "attachment-rollback failure fixture initializes", fixture.Initialize(0x0C00'0000U, 8U));
    FakeIommuDevice device;
    fixture.Backend.NextAttach = IommuBackendError::CapacityExceeded;
    device.NextRelease = IommuDeviceError::CorruptState;
    const auto attached = fixture.Domain.Attach(device);
    ASTERIA_EXPECT(runner, "failed ordinary Attach rollback faults and retains the device claim", !attached.HasValue(),
                   attached.GetError().Error == IommuDomainError::RollbackRejected,
                   attached.GetError().Backend == IommuBackendError::CapacityExceeded,
                   attached.GetError().Device == IommuDeviceError::CorruptState,
                   fixture.Domain.GetSnapshot().State == IommuDomainState::Faulted,
                   fixture.Domain.GetSnapshot().AttachmentCount == 1U, fixture.Backend.GetDeviceCount() == 0U,
                   device.IsClaimedBy(fixture.Domain));
    ASTERIA_EXPECT(runner, "terminal recovery retries a failed provisional device release",
                   fixture.Domain.Quiesce().Error == IommuDomainError::None, !device.IsClaimedBy(fixture.Domain),
                   fixture.Domain.GetSnapshot().AttachmentCount == 0U, fixture.Finish());
}

void TestAttachmentLifecycle(TestContext& runner) noexcept
{
    DomainFixture fixture;
    ASTERIA_EXPECT(runner, "attachment fixture initializes", fixture.Initialize());

    FakeIommuDevice unavailableDevice;
    unavailableDevice.SetReady(false);
    const auto unavailable = fixture.Domain.Attach(unavailableDevice);
    ASTERIA_EXPECT(runner, "Claim is the single readiness check and a rejection is inert", !unavailable.HasValue(),
                   unavailable.GetError().Error == IommuDomainError::DeviceRejected,
                   unavailable.GetError().Device == IommuDeviceError::NotReady, unavailableDevice.ClaimCalls == 1U,
                   fixture.Backend.AttachCalls == 0U, fixture.Domain.GetSnapshot().State == IommuDomainState::Ready);

    FakeIommuDevice device;
    const auto attached = fixture.Domain.Attach(device);
    ASTERIA_EXPECT(runner, "Attach claims the exact device before backend publication", attached.HasValue(),
                   attached.GetValue().Attachment.IsValid(), device.IsClaimedBy(fixture.Domain),
                   fixture.Backend.GetDeviceCount() == 1U, fixture.Domain.GetSnapshot().AttachmentCount == 1U);

    const auto duplicate = fixture.Domain.Attach(device);
    ASTERIA_EXPECT(runner, "duplicate attachment is rejected without a second device claim", !duplicate.HasValue(),
                   duplicate.GetError().Error == IommuDomainError::DeviceAlreadyAttached, device.ClaimCalls == 1U);

    const auto attachment = attached.GetValue().Attachment;
    ASTERIA_EXPECT(runner, "Detach consumes the exact attachment generation",
                   fixture.Domain.Detach(attachment).Error == IommuDomainError::None,
                   !device.IsClaimedBy(fixture.Domain), fixture.Backend.GetDeviceCount() == 0U,
                   fixture.Domain.Detach(attachment).Error == IommuDomainError::StaleAttachment);

    FakeIommuDevice rejectedDevice;
    fixture.Backend.NextAttach = IommuBackendError::CapacityExceeded;
    const auto rejected = fixture.Domain.Attach(rejectedDevice);
    ASTERIA_EXPECT(runner, "ordinary Attach failure releases the provisional device claim", !rejected.HasValue(),
                   rejected.GetError().Backend == IommuBackendError::CapacityExceeded,
                   !rejectedDevice.IsClaimedBy(fixture.Domain),
                   fixture.Domain.GetSnapshot().State == IommuDomainState::Ready,
                   fixture.Domain.GetSnapshot().AttachmentCount == 0U);

    const auto retry = fixture.Domain.Attach(rejectedDevice);
    fixture.Backend.NextDetach = IommuBackendError::HardwareFailure;
    const auto detachFailure =
        retry.HasValue() ? fixture.Domain.Detach(retry.GetValue().Attachment) : Kernel::Device::IommuDomainFailure{};
    ASTERIA_EXPECT(
        runner, "Detach hardware failure retains device authority and faults the domain", retry.HasValue(),
        retry.GetValue().Attachment.GetSequence() == 2U, detachFailure.Error == IommuDomainError::BackendRejected,
        fixture.Domain.GetSnapshot().State == IommuDomainState::Faulted, rejectedDevice.IsClaimedBy(fixture.Domain));
    ASTERIA_EXPECT(runner, "faulted attachment recovery drains backend before endpoint release",
                   fixture.Domain.Quiesce().Error == IommuDomainError::None,
                   !rejectedDevice.IsClaimedBy(fixture.Domain), fixture.Backend.GetDeviceCount() == 0U,
                   fixture.Finish());
}

void TestDeviceReleaseFailureRecovery(TestContext& runner) noexcept
{
    DomainFixture fixture;
    ASTERIA_EXPECT(runner, "device-release failure fixture initializes", fixture.Initialize(0x0900'0000U, 8U));
    FakeIommuDevice device;
    const auto attached = fixture.Domain.Attach(device);
    device.NextRelease = IommuDeviceError::CorruptState;
    const auto failed = attached.HasValue() ? fixture.Domain.Detach(attached.GetValue().Attachment)
                                            : Kernel::Device::IommuDomainFailure{};
    ASTERIA_EXPECT(runner, "failed endpoint Release faults and retains the exact device claim", attached.HasValue(),
                   failed.Error == IommuDomainError::DeviceRejected, failed.Device == IommuDeviceError::CorruptState,
                   fixture.Domain.GetSnapshot().State == IommuDomainState::Faulted,
                   fixture.Domain.GetSnapshot().AttachmentCount == 1U, fixture.Backend.GetDeviceCount() == 0U,
                   device.IsClaimedBy(fixture.Domain));
    ASTERIA_EXPECT(runner, "terminal retry releases an endpoint claim after Detach failure",
                   fixture.Domain.Quiesce().Error == IommuDomainError::None, !device.IsClaimedBy(fixture.Domain),
                   fixture.Domain.GetSnapshot().AttachmentCount == 0U, fixture.Finish());
}

void TestQuiesceDeviceReleaseFailureRecovery(TestContext& runner) noexcept
{
    DomainFixture fixture;
    ASTERIA_EXPECT(runner, "quiesce device-release failure fixture initializes", fixture.Initialize(0x0D00'0000U, 8U));
    FakeIommuDevice device;
    const auto attached = fixture.Domain.Attach(device);
    device.NextRelease = IommuDeviceError::CorruptState;
    const auto failed = fixture.Domain.Quiesce();
    const auto faulted = fixture.Domain.GetSnapshot();
    ASTERIA_EXPECT(runner, "Quiesce endpoint failure remains Faulted with the exact claim retained",
                   attached.HasValue(), failed.Error == IommuDomainError::DeviceRejected,
                   failed.Device == IommuDeviceError::CorruptState, faulted.State == IommuDomainState::Faulted,
                   !faulted.BackendClaimed, faulted.AttachmentCount == 1U, fixture.Backend.GetDeviceCount() == 0U,
                   device.IsClaimedBy(fixture.Domain));
    ASTERIA_EXPECT(runner, "Quiesce retry releases only the retained endpoint and resource charge",
                   fixture.Domain.Quiesce().Error == IommuDomainError::None, fixture.Backend.ReleaseCalls == 1U,
                   !device.IsClaimedBy(fixture.Domain),
                   fixture.Domain.GetSnapshot().State == IommuDomainState::Quiesced, fixture.Finish());
}

void TestTerminalRevocation(TestContext& runner) noexcept
{
    DomainFixture fixture;
    ASTERIA_EXPECT(runner, "terminal-revocation fixture initializes", fixture.Initialize(0x0500'0000U, 16U));
    FakeIommuDevice device;
    const auto mapping = fixture.Domain.Map(fixture.MakeRequest(0x2000'0000U, 0U, 4U, ReadWriteDma()));
    const auto attachment = fixture.Domain.Attach(device);
    ASTERIA_EXPECT(runner, "live DMA authorities exist before terminal revocation", mapping.HasValue(),
                   attachment.HasValue(), fixture.Domain.GetSnapshot().MappingCount == 1U,
                   fixture.Domain.GetSnapshot().AttachmentCount == 1U);
    ASTERIA_EXPECT(runner, "live healthy mapping pins its Frame generation",
                   fixture.Issuer.DestroyObject(fixture.FrameObject) == ObjectRegistryError::Ok,
                   !fixture.Issuer.CollectDestroyedObject().HasValue());
    ASTERIA_EXPECT(runner, "Quiesce revokes a crashed driver's mappings and attachments in order",
                   fixture.Domain.Quiesce().Error == IommuDomainError::None, fixture.Backend.GetMappingCount() == 0U,
                   fixture.Backend.GetDeviceCount() == 0U, !device.IsClaimedBy(fixture.Domain),
                   fixture.Domain.GetSnapshot().MappingCount == 0U, fixture.Domain.GetSnapshot().AttachmentCount == 0U);
    const auto collected = fixture.Issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "terminal revocation releases the final Frame pin", collected.HasValue(),
                   collected.GetValue().Instance == &fixture.FrameStorage);
    fixture.FrameObject = ObjectId{};
    ASTERIA_EXPECT(runner, "terminal-revocation fixture cleans up exactly", fixture.Finish());
}

void TestCrossDomainDeviceExclusion(TestContext& runner) noexcept
{
    DomainFixture first;
    DomainFixture second;
    ASTERIA_EXPECT(runner, "two independent IOMMU domains initialize", first.Initialize(0x0100'0000U),
                   second.Initialize(0x0200'0000U));
    FakeIommuDevice device;
    std::atomic<UInt64> ready{0U};
    std::atomic<bool> start{false};
    bool success[2U]{false, false};
    IommuAttachmentReceipt receipts[2U]{};
    IommuDomain* domains[2U]{&first.Domain, &second.Domain};

    std::thread workers[2U];
    for (Size index{0U}; index < 2U; ++index)
    {
        workers[index] = std::thread{[&, index]() noexcept
                                     {
                                         ready.fetch_add(1U, std::memory_order_acq_rel);
                                         while (!start.load(std::memory_order_acquire))
                                         {
                                             std::this_thread::yield();
                                         }
                                         const auto result = domains[index]->Attach(device);
                                         success[index] = result.HasValue();
                                         if (result.HasValue())
                                         {
                                             receipts[index] = result.GetValue();
                                         }
                                     }};
    }
    while (ready.load(std::memory_order_acquire) != 2U)
    {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (std::thread& worker : workers)
    {
        worker.join();
    }

    const Size winner = success[0U] ? 0U : 1U;
    ASTERIA_EXPECT(runner, "one hardware requester can belong to exactly one concurrent domain",
                   success[0U] != success[1U], device.IsClaimedBy(*domains[winner]),
                   domains[winner]->Detach(receipts[winner].Attachment).Error == IommuDomainError::None);
    ASTERIA_EXPECT(runner, "cross-domain attachment fixture cleans up exactly",
                   first.Domain.Quiesce().Error == IommuDomainError::None,
                   second.Domain.Quiesce().Error == IommuDomainError::None, first.Finish(), second.Finish());
}

void TestCrossDomainMappingTokenExclusion(TestContext& runner) noexcept
{
    DomainFixture first;
    DomainFixture second;
    ASTERIA_EXPECT(runner, "mapping-token provenance fixtures initialize", first.Initialize(0x0A00'0000U, 8U),
                   second.Initialize(0x0B00'0000U, 8U));
    const auto firstMapping = first.Domain.Map(first.MakeRequest(0x4000'0000U, 0U, 1U));
    const auto secondMapping = second.Domain.Map(second.MakeRequest(0x5000'0000U, 0U, 1U));
    const auto rejected = (firstMapping.HasValue() && secondMapping.HasValue())
                              ? second.Domain.Unmap(firstMapping.GetValue().Mapping)
                              : Kernel::Device::IommuDomainFailure{};
    ASTERIA_EXPECT(runner, "equal sequences from different domains cannot authorize Unmap", firstMapping.HasValue(),
                   secondMapping.HasValue(),
                   firstMapping.GetValue().Mapping.GetSequence() == secondMapping.GetValue().Mapping.GetSequence(),
                   rejected.Error == IommuDomainError::InvalidMapping, first.Domain.GetSnapshot().MappingCount == 1U,
                   second.Domain.GetSnapshot().MappingCount == 1U, first.Backend.GetMappingCount() == 1U,
                   second.Backend.GetMappingCount() == 1U);
    ASTERIA_EXPECT(runner, "domain-bound mapping tokens retain independent cleanup authority", firstMapping.HasValue(),
                   secondMapping.HasValue(),
                   first.Domain.Unmap(firstMapping.GetValue().Mapping).Error == IommuDomainError::None,
                   second.Domain.Unmap(secondMapping.GetValue().Mapping).Error == IommuDomainError::None,
                   first.Domain.Quiesce().Error == IommuDomainError::None,
                   second.Domain.Quiesce().Error == IommuDomainError::None, first.Finish(), second.Finish());
}

void TestCrossDomainAttachmentTokenExclusion(TestContext& runner) noexcept
{
    DomainFixture first;
    DomainFixture second;
    ASTERIA_EXPECT(runner, "attachment-token provenance fixtures initialize", first.Initialize(0x0E00'0000U, 8U),
                   second.Initialize(0x0F00'0000U, 8U));
    FakeIommuDevice firstDevice;
    FakeIommuDevice secondDevice;
    const auto firstAttachment = first.Domain.Attach(firstDevice);
    const auto secondAttachment = second.Domain.Attach(secondDevice);
    const auto rejected = (firstAttachment.HasValue() && secondAttachment.HasValue())
                              ? second.Domain.Detach(firstAttachment.GetValue().Attachment)
                              : Kernel::Device::IommuDomainFailure{};
    ASTERIA_EXPECT(runner, "equal sequences from different domains cannot authorize Detach", firstAttachment.HasValue(),
                   secondAttachment.HasValue(),
                   firstAttachment.GetValue().Attachment.GetSequence() ==
                       secondAttachment.GetValue().Attachment.GetSequence(),
                   rejected.Error == IommuDomainError::InvalidAttachment,
                   first.Domain.GetSnapshot().AttachmentCount == 1U, second.Domain.GetSnapshot().AttachmentCount == 1U,
                   firstDevice.IsClaimedBy(first.Domain), secondDevice.IsClaimedBy(second.Domain),
                   first.Backend.GetDeviceCount() == 1U, second.Backend.GetDeviceCount() == 1U);
    ASTERIA_EXPECT(runner, "domain-bound attachment tokens retain independent cleanup authority",
                   firstAttachment.HasValue(), secondAttachment.HasValue(),
                   first.Domain.Detach(firstAttachment.GetValue().Attachment).Error == IommuDomainError::None,
                   second.Domain.Detach(secondAttachment.GetValue().Attachment).Error == IommuDomainError::None,
                   first.Domain.Quiesce().Error == IommuDomainError::None,
                   second.Domain.Quiesce().Error == IommuDomainError::None, first.Finish(), second.Finish());
}

void TestConcurrentMappings(TestContext& runner) noexcept
{
    DomainFixture fixture;
    ASTERIA_EXPECT(runner, "concurrent mapping fixture initializes", fixture.Initialize(0x0300'0000U, 64U));
    constexpr Size WorkerCount{4U};
    std::atomic<UInt64> ready{0U};
    std::atomic<bool> start{false};
    bool success[WorkerCount]{};
    IommuMappingReceipt receipts[WorkerCount]{};
    std::thread workers[WorkerCount];
    for (Size index{0U}; index < WorkerCount; ++index)
    {
        workers[index] = std::thread{[&, index]() noexcept
                                     {
                                         ready.fetch_add(1U, std::memory_order_acq_rel);
                                         while (!start.load(std::memory_order_acquire))
                                         {
                                             std::this_thread::yield();
                                         }
                                         const auto result = fixture.Domain.Map(fixture.MakeRequest(
                                             0x1000'0000U + (index * 0x0010'0000U), index * 4U, 4U, ReadWriteDma()));
                                         success[index] = result.HasValue();
                                         if (result.HasValue())
                                         {
                                             receipts[index] = result.GetValue();
                                         }
                                     }};
    }
    while (ready.load(std::memory_order_acquire) != WorkerCount)
    {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (std::thread& worker : workers)
    {
        worker.join();
    }

    bool allMapped = fixture.Domain.GetSnapshot().MappingCount == WorkerCount;
    for (Size index{0U}; index < WorkerCount; ++index)
    {
        allMapped = allMapped && success[index];
    }
    ASTERIA_EXPECT(runner, "concurrent disjoint mappings serialize without loss", allMapped,
                   fixture.Backend.GetMappingCount() == WorkerCount);

    bool allUnmapped{true};
    for (const IommuMappingReceipt& receipt : receipts)
    {
        allUnmapped = allUnmapped && (fixture.Domain.Unmap(receipt.Mapping).Error == IommuDomainError::None);
    }
    ASTERIA_EXPECT(runner, "concurrent mappings retain exact independent cleanup tokens", allUnmapped,
                   fixture.Domain.GetSnapshot().MappingCount == 0U, fixture.Backend.GetMappingCount() == 0U,
                   fixture.Domain.Quiesce().Error == IommuDomainError::None, fixture.Finish());
}

void TestBoundedCapacities(TestContext& runner) noexcept
{
    DomainFixture fixture;
    ASTERIA_EXPECT(runner, "bounded-capacity fixture initializes", fixture.Initialize(0x1000'0000U, 128U));

    IommuMappingReceipt mappings[IommuDomain::MaximumMappings]{};
    bool allMappingsCreated{true};
    for (Size index{0U}; index < IommuDomain::MaximumMappings; ++index)
    {
        const auto mapped = fixture.Domain.Map(fixture.MakeRequest(0x6000'0000U + (index * 0x1000U), index, 1U));
        allMappingsCreated = allMappingsCreated && mapped.HasValue();
        if (mapped.HasValue())
        {
            mappings[index] = mapped.GetValue();
        }
    }
    const auto excessMapping = fixture.Domain.Map(fixture.MakeRequest(0x7000'0000U, IommuDomain::MaximumMappings, 1U));
    ASTERIA_EXPECT(runner, "mapping capacity rejects one excess request before the backend", allMappingsCreated,
                   !excessMapping.HasValue(),
                   excessMapping.GetError().Error == IommuDomainError::MappingCapacityExceeded,
                   fixture.Domain.GetSnapshot().MappingCount == IommuDomain::MaximumMappings,
                   fixture.Backend.MapCalls == IommuDomain::MaximumMappings);
    bool allMappingsRemoved{true};
    for (const IommuMappingReceipt& mapping : mappings)
    {
        allMappingsRemoved =
            allMappingsRemoved && (fixture.Domain.Unmap(mapping.Mapping).Error == IommuDomainError::None);
    }

    FakeIommuDevice devices[IommuDomain::MaximumAttachments + 1U]{};
    IommuAttachmentReceipt attachments[IommuDomain::MaximumAttachments]{};
    bool allAttachmentsCreated{true};
    for (Size index{0U}; index < IommuDomain::MaximumAttachments; ++index)
    {
        const auto attached = fixture.Domain.Attach(devices[index]);
        allAttachmentsCreated = allAttachmentsCreated && attached.HasValue();
        if (attached.HasValue())
        {
            attachments[index] = attached.GetValue();
        }
    }
    const auto excessAttachment = fixture.Domain.Attach(devices[IommuDomain::MaximumAttachments]);
    ASTERIA_EXPECT(runner, "attachment capacity rejects one excess endpoint before Claim or HAL", allMappingsRemoved,
                   allAttachmentsCreated, !excessAttachment.HasValue(),
                   excessAttachment.GetError().Error == IommuDomainError::AttachmentCapacityExceeded,
                   devices[IommuDomain::MaximumAttachments].ClaimCalls == 0U,
                   fixture.Domain.GetSnapshot().AttachmentCount == IommuDomain::MaximumAttachments,
                   fixture.Backend.AttachCalls == IommuDomain::MaximumAttachments);
    bool allAttachmentsRemoved{true};
    for (const IommuAttachmentReceipt& attachment : attachments)
    {
        allAttachmentsRemoved =
            allAttachmentsRemoved && (fixture.Domain.Detach(attachment.Attachment).Error == IommuDomainError::None);
    }
    ASTERIA_EXPECT(runner, "all bounded mapping and attachment slots release exactly", allAttachmentsRemoved,
                   fixture.Domain.Quiesce().Error == IommuDomainError::None, fixture.Finish());
}

void TestFaultedOperationGuards(TestContext& runner) noexcept
{
    DomainFixture fixture;
    ASTERIA_EXPECT(runner, "faulted-operation guard fixture initializes", fixture.Initialize(0x1100'0000U, 16U));
    FakeIommuDevice attachedDevice;
    FakeIommuDevice newDevice;
    const auto attachment = fixture.Domain.Attach(attachedDevice);
    const auto mapping = fixture.Domain.Map(fixture.MakeRequest(0x7100'0000U, 0U, 1U));
    fixture.Backend.NextMap = IommuBackendError::CommittedStateUnknown;
    const auto ambiguous = fixture.Domain.Map(fixture.MakeRequest(0x7200'0000U, 1U, 1U));
    ASTERIA_EXPECT(runner, "ambiguous native Map enters Faulted with live authorities", attachment.HasValue(),
                   mapping.HasValue(), !ambiguous.HasValue(),
                   ambiguous.GetError().Backend == IommuBackendError::CommittedStateUnknown,
                   fixture.Domain.GetSnapshot().State == IommuDomainState::Faulted);

    const UInt64 mapCalls = fixture.Backend.MapCalls;
    const UInt64 unmapCalls = fixture.Backend.UnmapCalls;
    const UInt64 attachCalls = fixture.Backend.AttachCalls;
    const UInt64 detachCalls = fixture.Backend.DetachCalls;
    const auto rejectedMap = fixture.Domain.Map(fixture.MakeRequest(0x7300'0000U, 2U, 1U));
    const auto rejectedUnmap =
        mapping.HasValue() ? fixture.Domain.Unmap(mapping.GetValue().Mapping) : Kernel::Device::IommuDomainFailure{};
    const auto rejectedAttach = fixture.Domain.Attach(newDevice);
    const auto rejectedDetach = attachment.HasValue() ? fixture.Domain.Detach(attachment.GetValue().Attachment)
                                                      : Kernel::Device::IommuDomainFailure{};
    const auto snapshot = fixture.Domain.GetSnapshot();
    ASTERIA_EXPECT(runner, "Faulted rejects Map, Unmap, Attach, and Detach without side effects",
                   !rejectedMap.HasValue(), rejectedMap.GetError().Error == IommuDomainError::Faulted,
                   rejectedUnmap.Error == IommuDomainError::Faulted, !rejectedAttach.HasValue(),
                   rejectedAttach.GetError().Error == IommuDomainError::Faulted,
                   rejectedDetach.Error == IommuDomainError::Faulted, fixture.Backend.MapCalls == mapCalls,
                   fixture.Backend.UnmapCalls == unmapCalls, fixture.Backend.AttachCalls == attachCalls,
                   fixture.Backend.DetachCalls == detachCalls, newDevice.ClaimCalls == 0U, snapshot.MappingCount == 2U,
                   snapshot.AttachmentCount == 1U);
    ASTERIA_EXPECT(runner, "terminal recovery is the only operation admitted after a fault",
                   fixture.Domain.Quiesce().Error == IommuDomainError::None,
                   !attachedDevice.IsClaimedBy(fixture.Domain), fixture.Backend.GetMappingCount() == 0U,
                   fixture.Backend.GetDeviceCount() == 0U, fixture.Finish());
}
// A synthetic firmware reservation applies to one requester only.
class Reservations final : public Kernel::Device::IDmaReservations
{
public:
    static constexpr UInt64 Base{0x2000'0000U};
    static constexpr UInt64 Limit{0x2000'7FFFU};
    static constexpr Foundation::UInt32 Reserved{0x0219U};

    [[nodiscard]] bool Overlaps(const Foundation::UInt32 requester, const UInt64 first,
                                const UInt64 last) const noexcept override
    {
        return (requester == Reserved) && (first <= Limit) && (Base <= last);
    }
};

// A domain never maps a device address overlapping a firmware reservation of
// a requester it serves: Map refuses such a range while the requester is
// attached, and Attach refuses the requester while such a mapping is
// present. A refusal changes nothing; adjacent ranges and requesters without
// the reservation are unaffected.
void TestFirmwareReservations(TestContext& runner) noexcept
{
    static const Reservations reservations{};
    DomainFixture fixture{&reservations};
    ASTERIA_EXPECT(runner, "reservation fixture initializes", fixture.Initialize());
    FakeIommuDevice reserved;
    reserved.RequesterId = Reservations::Reserved;
    FakeIommuDevice other;
    other.RequesterId = 0x042AU;
    const auto otherAttached = fixture.Domain.Attach(other);
    const auto inside = fixture.Domain.Map(fixture.MakeRequest(Reservations::Base + 0x2000U, 0U, 1U));
    const UInt64 mapsBefore = fixture.Backend.MapCalls;
    const UInt64 attachesBefore = fixture.Backend.AttachCalls;
    const auto refusedAttach = fixture.Domain.Attach(reserved);
    ASTERIA_EXPECT(runner,
                   "a requester without the reservation maps inside it, and the reserved requester then "
                   "cannot attach",
                   otherAttached.HasValue(), inside.HasValue(), !refusedAttach.HasValue(),
                   refusedAttach.HasValue() || (refusedAttach.GetError().Error == IommuDomainError::ReservedDmaRange),
                   reserved.ClaimCalls == 0U, fixture.Backend.AttachCalls == attachesBefore,
                   fixture.Domain.GetSnapshot().AttachmentCount == 1U, fixture.Backend.MapCalls == mapsBefore);

    const bool unmapped =
        inside.HasValue() && (fixture.Domain.Unmap(inside.GetValue().Mapping).Error == IommuDomainError::None);
    const auto attached = fixture.Domain.Attach(reserved);
    const auto refuse = [&](const UInt64 start, const UInt64 pages) noexcept
    {
        const auto mapped = fixture.Domain.Map(fixture.MakeRequest(start, 0U, pages));
        return !mapped.HasValue() && (mapped.GetError().Error == IommuDomainError::ReservedDmaRange);
    };
    const UInt64 maps = fixture.Backend.MapCalls;
    const bool atStart = refuse(Reservations::Base - 0x1000U, 2U);
    const bool atEnd = refuse(Reservations::Limit - 0xFFFU, 2U);
    const bool within = refuse(Reservations::Base + 0x4000U, 1U);
    const bool around = refuse(Reservations::Base - 0x10000U, 64U);
    ASTERIA_EXPECT(runner, "with the reserved requester attached, every overlapping range is refused untouched",
                   unmapped, attached.HasValue(), atStart, atEnd, within, around, fixture.Backend.MapCalls == maps,
                   fixture.Domain.GetSnapshot().MappingCount == 0U,
                   fixture.Domain.GetSnapshot().State == IommuDomainState::Ready);

    const auto below = fixture.Domain.Map(fixture.MakeRequest(Reservations::Base - 0x2000U, 0U, 2U));
    const auto above = fixture.Domain.Map(fixture.MakeRequest(Reservations::Limit + 1U, 2U, 2U));
    ASTERIA_EXPECT(runner, "ranges that end before or start after the reservation map", below.HasValue(),
                   above.HasValue(), fixture.Domain.GetSnapshot().MappingCount == 2U,
                   fixture.Domain.Quiesce().Error == IommuDomainError::None, fixture.Finish());
}
} // namespace

int RunIommuDomainTests() noexcept
{
    TestContext runner{"IommuDomainTests"};
    TestInitializationFailures(runner);
    TestMappingLifecycle(runner);
    TestParkedMappings(runner);
    TestMappingValidationAndRollback(runner);
    TestAmbiguousMappingRecovery(runner);
    TestMaximumDmaBoundary(runner);
    TestUnmapFailureRecovery(runner);
    TestAttachmentLifecycle(runner);
    TestAttachmentRollbackFailureRecovery(runner);
    TestDeviceReleaseFailureRecovery(runner);
    TestQuiesceDeviceReleaseFailureRecovery(runner);
    TestAmbiguousAttachmentRecovery(runner);
    TestUnexposedStorageRelease(runner, false);
    TestUnexposedStorageRelease(runner, true);
    TestTerminalRevocation(runner);
    TestCrossDomainDeviceExclusion(runner);
    TestCrossDomainMappingTokenExclusion(runner);
    TestCrossDomainAttachmentTokenExclusion(runner);
    TestConcurrentMappings(runner);
    TestBoundedCapacities(runner);
    TestFaultedOperationGuards(runner);
    TestFirmwareReservations(runner);
    return runner.Finish();
}
} // Namespace Asteria::Test

int main()
{
    return Asteria::Test::RunIommuDomainTests();
}
