// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_IOMMU_DOMAIN_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_IOMMU_DOMAIN_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Hal/Device/IIommuDevice.hpp"
#include "Hal/Device/IIommuDomainBackend.hpp"
#include "Kernel/Device/DmaReservations.hpp"
#include "Kernel/Device/DmaTypes.hpp"
#include "Kernel/Memory/Frame.hpp"
#include "Kernel/Object/IKernelObject.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Object
{
class ObjectRegistry;
}

namespace Asteria::Kernel::Device
{
class IommuDomain;

/// Identifies one IommuDomain generation within one ObjectRegistry.
/// The originating registry outlives every copy and use of this value.
class IommuDomainId final
{
public:
    constexpr IommuDomainId() noexcept = default;

    constexpr IommuDomainId(const Object::ObjectRegistry& registry, const Object::ObjectId object) noexcept
        : m_registry{&registry}, m_object{object}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetValue() const noexcept
    {
        return m_object.GetValue();
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_registry != nullptr) && m_object.IsValid();
    }

    [[nodiscard]] constexpr Object::ObjectId GetObjectId() const noexcept
    {
        return m_object;
    }

    [[nodiscard]] constexpr bool IsFromRegistry(const Object::ObjectRegistry& registry) const noexcept
    {
        return m_registry == &registry;
    }

    [[nodiscard]] constexpr bool IsEqualTo(const IommuDomainId other) const noexcept
    {
        return (m_registry == other.m_registry) && (m_object.GetValue() == other.m_object.GetValue());
    }

private:
    const Object::ObjectRegistry* m_registry{nullptr};
    Object::ObjectId m_object{};
};

/// Names one exact mapping generation. The originating IommuDomain instance
/// and its storage outlive every copy and use of this kernel-only token.
class IommuMappingToken final
{
public:
    constexpr IommuMappingToken() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetSequence() const noexcept
    {
        return m_sequence;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_domain != nullptr) && (m_sequence != 0U);
    }

    [[nodiscard]] constexpr bool IsFromDomain(const IommuDomain& domain) const noexcept
    {
        return m_domain == &domain;
    }

private:
    friend class IommuDomain;

    constexpr IommuMappingToken(const IommuDomain& domain, const Foundation::UInt64 sequence) noexcept
        : m_domain{&domain}, m_sequence{sequence}
    {
    }

    const IommuDomain* m_domain{nullptr};
    Foundation::UInt64 m_sequence{0U};
};

/// Names one exact device attachment generation. The originating IommuDomain
/// instance and its storage outlive every copy and use of this kernel-only token.
class IommuAttachmentToken final
{
public:
    constexpr IommuAttachmentToken() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetSequence() const noexcept
    {
        return m_sequence;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_domain != nullptr) && (m_sequence != 0U);
    }

    [[nodiscard]] constexpr bool IsFromDomain(const IommuDomain& domain) const noexcept
    {
        return m_domain == &domain;
    }

private:
    friend class IommuDomain;

    constexpr IommuAttachmentToken(const IommuDomain& domain, const Foundation::UInt64 sequence) noexcept
        : m_domain{&domain}, m_sequence{sequence}
    {
    }

    const IommuDomain* m_domain{nullptr};
    Foundation::UInt64 m_sequence{0U};
};

enum class IommuDomainState : Foundation::UInt8
{
    Uninitialized,
    Ready,
    Faulted,
    Quiesced,
};

enum class IommuDomainError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyInitialized,
    ResourceDomainRegistryMismatch,
    ResourceDomainPinRejected,
    ResourceDomainInstanceMismatch,
    ResourceDomainRejected,
    BackendRejected,
    NotReady,
    Faulted,
    AlreadyQuiesced,
    InvalidMappingRequest,
    FrameRegistryMismatch,
    FramePinRejected,
    FrameInstanceMismatch,
    FrameNotReady,
    RamFrameRequired,
    FrameRangeExceeded,
    DmaRangeExceeded,
    DmaRangeOverlap,
    MappingCapacityExceeded,
    MappingSequenceExhausted,
    FramePolicyRejected,
    InvalidMapping,
    StaleMapping,
    DeviceAlreadyAttached,
    DeviceRejected,
    AttachmentCapacityExceeded,
    AttachmentSequenceExhausted,
    InvalidAttachment,
    StaleAttachment,
    RollbackRejected,
    CorruptState,
    /// The parker cannot take every mapping; nothing changed.
    ParkingRefused,
    /// The device range overlaps a firmware DMA reservation (an RMRR) of a requester the domain
    /// serves, or would once the requester is attached; nothing changed.
    ReservedDmaRange,
};

class IommuSequencePolicy final
{
public:
    static constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U}};

    IommuSequencePolicy() = delete;

    [[nodiscard]] static constexpr bool TryAdvance(const Foundation::UInt64 current, Foundation::UInt64& next) noexcept
    {
        if (current == Maximum)
        {
            return false;
        }
        next = current + 1U;
        return true;
    }
};

struct IommuDomainConfiguration
{
    Foundation::UInt8 InputAddressBits;
    Foundation::UInt8 Reserved[7];
};

struct IommuMappingRequest
{
    Memory::Frame* FrameStorage;
    Memory::FrameId FrameIdentity;
    DmaAddress DmaStart;
    Memory::PageCount FrameOffsetPages;
    Memory::PageCount Pages;
    DmaPermissions Permissions;
    Foundation::UInt8 Reserved[7];
};

struct IommuMappingReceipt
{
    IommuMappingToken Mapping;
    Memory::FrameId Frame;
    DmaAddress DmaStart;
    Memory::PhysicalAddress PhysicalStart;
    Memory::PageCount Pages;
    DmaPermissions Permissions;
    Foundation::UInt8 Reserved[7];
};

struct IommuAttachmentReceipt
{
    IommuAttachmentToken Attachment;
};

struct IommuDomainFailure
{
    Hal::Device::IommuBackendOwnershipError Ownership;
    Hal::Device::IommuBackendError Backend;
    Hal::Device::IommuDeviceError Device;
    Object::ObjectPinError Pin;
    Memory::FrameMappingPolicyError FramePolicy;
    Resource::ResourceDomainError Resource;
    IommuDomainError Error;
    Foundation::UInt8 Reserved;
};

/// A quiescing domain's mapping handed on instead of released: Pin, which the parker moves from,
/// holds the Frame, and the Frame's DMA mapping-policy reservation stays held; whoever parks it
/// releases both with IommuDomain::ReleaseParkedMapping.
struct IommuParkedMapping
{
    Object::ObjectPin* Pin;
    Memory::Frame* FrameStorage;
    Object::ObjectRegistry* Registry;
    Object::ObjectId FrameObject;
    Memory::PhysicalAddress PhysicalStart;
    Memory::PageCount Pages;
    Memory::PagePermissions PolicyPermissions;
};

/// Where a drained domain's mappings go when no one can prove its DMA ended yet. Called with the
/// domain's lock held: it may take no lock ranking at or below the domain's.
class IIommuStorageParker
{
public:
    /// Whether count mappings can be parked, asked before anything changes.
    [[nodiscard]] virtual bool CanPark(Foundation::Size count) noexcept = 0;
    /// Takes one mapping: moves *mapping.Pin and keeps its reservation.
    virtual void Park(const IommuParkedMapping& mapping) noexcept = 0;

protected:
    constexpr IIommuStorageParker() noexcept = default;
    ~IIommuStorageParker() noexcept = default;
};

struct IommuDomainSnapshot
{
    Resource::ResourceDomainObjectId ResourceDomainIdentity;
    Object::ResourceDomainId ResourceDomain;
    Foundation::UInt64 MappingCount;
    Foundation::UInt64 AttachmentCount;
    Foundation::UInt64 LastMappingSequence;
    Foundation::UInt64 LastAttachmentSequence;
    IommuDomainConfiguration Configuration;
    IommuDomainState State;
    bool BackendClaimed;
    Foundation::UInt8 Reserved[6];
};

/// Owns one bounded device-visible address space and its native IOMMU backend. Map pins the exact
/// Frame generation and makes device-write DMA participate in the same Frame-wide
/// write-versus-execute policy as CPU mappings. Attach first claims a unique platform device
/// endpoint, preventing one requester from belonging to two domains. Ordinary capacity/unsupported
/// failures are failure-atomic. Any ambiguous native failure makes the domain Faulted and retains
/// every Frame policy, pin, and device claim until Quiesce proves the backend no longer exposes
/// prior translations. Operations may spin on the domain, registry, ResourceDomain ledger, Frame,
/// endpoint, and backend locks and are forbidden from ISRs and same-CPU reentry. Callee locks are
/// always nested below the domain lock.
class IommuDomain final : public Object::IKernelObject
{
public:
    static constexpr Foundation::Size MaximumMappings{64U};
    static constexpr Foundation::Size MaximumAttachments{16U};

    /// Reservations, when given, are the firmware DMA ranges of the platform's requesters: Map
    /// refuses a device range that overlaps one reserved for an attached requester, and Attach
    /// refuses a requester for which a present mapping overlaps one (ReservedDmaRange). It outlives
    /// the domain.
    constexpr IommuDomain(Object::ObjectRegistry& registry, Hal::Device::IIommuDomainBackend& backend,
                          const IDmaReservations* const reservations = nullptr) noexcept
        : m_registry{registry}, m_backend{backend}, m_reservations{reservations}
    {
    }

    /// Preconditions:
    /// - State is Uninitialized or Quiesced.
    /// - The backend, registry, retained ResourceDomain, Frames, and device
    /// endpoints outlive this object and every returned token use.
    /// - No concurrent operation remains.
    ~IommuDomain() noexcept = default;

    IommuDomain(const IommuDomain&) = delete;
    IommuDomain& operator=(const IommuDomain&) = delete;
    IommuDomain(IommuDomain&&) = delete;
    IommuDomain& operator=(IommuDomain&&) = delete;

    /// Pins and charges the exact ResourceDomain, then claims an empty native
    /// backend. Every ordinary failure restores the initial state.
    [[nodiscard]] IommuDomainFailure Initialize(Resource::ResourceDomainObjectId resourceDomainIdentity,
                                                Resource::ResourceDomain& resourceDomain,
                                                const IommuDomainConfiguration& configuration) noexcept;

    /// Installs one exact Frame subrange at a nonoverlapping device address. The caller has already
    /// authorized Map on both object capabilities. The requested device address must suit the
    /// domain on every backend; the receipt's DmaStart is the one the backend chose
    /// (IIommuDomainBackend::ChooseDeviceAddress), which the driver programs and UnmapAt names: the
    /// requested one on a translating backend, the physical address on an identity one.
    [[nodiscard]] Foundation::Result<IommuMappingReceipt, IommuDomainFailure>
    Map(const IommuMappingRequest& request) noexcept;

    /// Removes exactly the translation generation named by mapping. Storage
    /// and alias policy remain retained when device quiescence is required;
    /// otherwise the backend's architectural DMA drain permits their release. A backend failure is terminal because
    /// the software and native translation may have diverged.
    [[nodiscard]] IommuDomainFailure Unmap(IommuMappingToken mapping) noexcept;

    /// A requester that caches DMA addresses needs trusted device quiescence even behind a draining
    /// translation backend.
    void RequireDmaQuiescence() noexcept;
    [[nodiscard]] bool RequiresDmaQuiescence() const noexcept;

    /// Unmap of the active translation starting at start (IommuControl).
    /// InvalidMapping when none does; retained storage has no active token.
    [[nodiscard]] IommuDomainFailure UnmapAt(DmaAddress start) noexcept;

    /// The backend's record of this domain's DMA faults; zero while no
    /// backend is claimed.
    [[nodiscard]] Hal::Device::IommuBackendFaults GetFaults() noexcept;

    /// Attaches one platform-authenticated DMA requester. The trusted device
    /// assignment path has disabled bus mastering and authorized this pairing.
    [[nodiscard]] Foundation::Result<IommuAttachmentReceipt, IommuDomainFailure>
    Attach(Hal::Device::IIommuDevice& device) noexcept;

    /// Detaches an exact attachment while bus mastering and outstanding DMA
    /// are stopped by the device owner.
    [[nodiscard]] IommuDomainFailure Detach(IommuAttachmentToken attachment) noexcept;

    /// Terminally revokes every translation and attachment, including when a failed driver cannot
    /// return individual tokens. The backend is released first; only then are Frame policies, pins,
    /// and device claims released. The trusted device owner has already disabled bus mastering and
    /// drained outstanding DMA before this call. With a parker, which a drain gives when no one can
    /// prove the DMA ended yet, every mapping's pin and Frame policy go to it instead of being
    /// released; a parker that cannot take them all refuses the call, nothing changed.
    [[nodiscard]] IommuDomainFailure Quiesce(IIommuStorageParker* parker = nullptr) noexcept;

    /// Releases what a parked mapping held: its Frame's DMA mapping-policy
    /// reservation, then its pin.
    static void ReleaseParkedMapping(Object::ObjectPin& pin, Memory::Frame& frame, Object::ObjectRegistry& registry,
                                     Object::ObjectId frameObject, Memory::PhysicalAddress physicalStart,
                                     Memory::PageCount pages, Memory::PagePermissions permissions) noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] IommuDomainSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

private:
    struct MappingRecord
    {
        Object::ObjectPin FramePin;
        Memory::Frame* FrameStorage{nullptr};
        Memory::FrameId FrameIdentity{};
        DmaAddress DmaStart{};
        Memory::PhysicalAddress PhysicalStart{};
        Memory::PageCount Pages{};
        DmaPermissions Permissions{};
        Foundation::UInt64 Sequence{0U};
        bool Retired{false};
    };

    struct AttachmentRecord
    {
        Hal::Device::IIommuDevice* Device{nullptr};
        Foundation::UInt64 Sequence{0U};
    };

    [[nodiscard]] static IommuDomainFailure
    MakeFailure(IommuDomainError error,
                Hal::Device::IommuBackendOwnershipError ownership = Hal::Device::IommuBackendOwnershipError::None,
                Hal::Device::IommuBackendError backend = Hal::Device::IommuBackendError::None,
                Hal::Device::IommuDeviceError device = Hal::Device::IommuDeviceError::None,
                Object::ObjectPinError pin = Object::ObjectPinError::Ok,
                Memory::FrameMappingPolicyError framePolicy = Memory::FrameMappingPolicyError::None,
                Resource::ResourceDomainError resource = Resource::ResourceDomainError::None) noexcept;
    [[nodiscard]] static bool ConfigurationIsValid(const IommuDomainConfiguration& configuration) noexcept;
    [[nodiscard]] static bool RequestReservedIsZero(const IommuMappingRequest& request) noexcept;
    [[nodiscard]] static Memory::PagePermissions GetFramePolicyPermissions(DmaPermissions permissions) noexcept;
    [[nodiscard]] static bool BackendFailureIsOrdinary(Hal::Device::IommuBackendError error) noexcept;
    [[nodiscard]] bool DmaRangeIsValidLocked(DmaAddress start, Memory::PageCount pages) const noexcept;
    [[nodiscard]] bool DmaRangeOverlapsLocked(DmaAddress start, Memory::PageCount pages) const noexcept;
    /// Whether [start, start + pages) overlaps a firmware reservation of an
    /// attached requester.
    [[nodiscard]] bool ReservationOverlapsLocked(DmaAddress start, Memory::PageCount pages) const noexcept;
    /// Whether a present mapping overlaps a reservation of requester.
    [[nodiscard]] bool MappingOverlapsReservationLocked(Foundation::UInt32 requester) const noexcept;
    [[nodiscard]] MappingRecord* FindFreeMappingLocked() noexcept;
    [[nodiscard]] MappingRecord* FindRetiredMappingLocked(const IommuMappingRequest& request) noexcept;
    [[nodiscard]] MappingRecord* FindMappingLocked(IommuMappingToken mapping) noexcept;
    [[nodiscard]] IommuDomainFailure UnmapLocked(MappingRecord& record) noexcept;
    [[nodiscard]] AttachmentRecord* FindFreeAttachmentLocked() noexcept;
    [[nodiscard]] AttachmentRecord* FindAttachmentLocked(IommuAttachmentToken attachment) noexcept;
    [[nodiscard]] bool DeviceIsAttachedLocked(const Hal::Device::IIommuDevice& device) const noexcept;
    void PublishMappingLocked(MappingRecord& record, const IommuMappingRequest& request, DmaAddress deviceStart,
                              Memory::PhysicalAddress physicalStart, Foundation::UInt64 sequence) noexcept;
    void ReleaseMappingLocked(MappingRecord& record) noexcept;
    void ForgetMappingLocked(MappingRecord& record) noexcept;
    void PublishAttachmentLocked(AttachmentRecord& record, Hal::Device::IIommuDevice& device,
                                 Foundation::UInt64 sequence) noexcept;
    [[nodiscard]] Hal::Device::IommuDeviceError ReleaseAttachmentLocked(AttachmentRecord& record) noexcept;
    [[nodiscard]] IommuDomainFailure ReleaseResourceDomainLocked() noexcept;
    [[nodiscard]] IommuDomainSnapshot GetSnapshotLocked() const noexcept;
    void EnterFaultedLocked() noexcept;

    Object::ObjectRegistry& m_registry;
    Hal::Device::IIommuDomainBackend& m_backend;
    const IDmaReservations* m_reservations;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Platform::Synchronization::AtomicUInt64 m_chargeOwnerValue;
    Object::ObjectPin m_resourceDomainPin;
    Resource::ResourceDomain* m_resourceDomain{nullptr};
    Resource::ResourceDomainObjectId m_resourceDomainIdentity{};
    Object::ResourceDomainId m_resourceDomainIdentifier{};
    MappingRecord m_mappings[MaximumMappings]{};
    AttachmentRecord m_attachments[MaximumAttachments]{};
    Foundation::UInt64 m_mappingCount{0U};
    Foundation::UInt64 m_attachmentCount{0U};
    Foundation::UInt64 m_lastMappingSequence{0U};
    Foundation::UInt64 m_lastAttachmentSequence{0U};
    IommuDomainConfiguration m_configuration{};
    IommuDomainState m_state{IommuDomainState::Uninitialized};
    bool m_dmaQuiescenceRequired{false};
    bool m_backendClaimed{false};
    bool m_resourceReserved{false};
};

static_assert(sizeof(IommuDomainId) == 16U);
static_assert(alignof(IommuDomainId) == 8U);
static_assert(sizeof(IommuMappingToken) == 16U);
static_assert(alignof(IommuMappingToken) == 8U);
static_assert(sizeof(IommuAttachmentToken) == 16U);
static_assert(alignof(IommuAttachmentToken) == 8U);
static_assert(sizeof(IommuDomainState) == 1U);
static_assert(sizeof(IommuDomainError) == 1U);
static_assert(sizeof(IommuDomainConfiguration) == 8U);
static_assert(alignof(IommuDomainConfiguration) == 1U);
static_assert(sizeof(IommuMappingRequest) == 56U);
static_assert(alignof(IommuMappingRequest) == 8U);
static_assert(sizeof(IommuMappingReceipt) == 64U);
static_assert(alignof(IommuMappingReceipt) == 8U);
static_assert(sizeof(IommuAttachmentReceipt) == 16U);
static_assert(alignof(IommuAttachmentReceipt) == 8U);
static_assert(sizeof(IommuDomainFailure) == 8U);
static_assert(alignof(IommuDomainFailure) == 1U);
static_assert(sizeof(IommuDomainSnapshot) == 72U);
static_assert(alignof(IommuDomainSnapshot) == 8U);
static_assert(
    []() constexpr
    {
        Foundation::UInt64 next{0U};
        return IommuSequencePolicy::TryAdvance(0U, next) && (next == 1U);
    }());
static_assert(
    []() constexpr
    {
        Foundation::UInt64 next{7U};
        return !IommuSequencePolicy::TryAdvance(IommuSequencePolicy::Maximum, next) && (next == 7U);
    }());
} // Namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_IOMMU_DOMAIN_HPP_INCLUDED
