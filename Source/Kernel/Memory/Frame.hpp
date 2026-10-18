// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_FRAME_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_FRAME_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/MemoryTypes.hpp"
#include "Kernel/Object/IKernelObject.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Object
{
class ObjectRegistry;
}

namespace Asteria::Kernel::Device
{
class IommuDomain;
}

namespace Asteria::Kernel::Memory
{
class AddressSpace;
constexpr Foundation::Size MaximumFrameMappingPolicyRecords{256U};

/// Identifies one Frame generation within one ObjectRegistry.
/// Lifetime:
/// - The originating registry outlives every copy and use of this value.
class FrameId final
{
public:
    constexpr FrameId() noexcept = default;

    constexpr FrameId(const Object::ObjectRegistry& registry, const Object::ObjectId object) noexcept
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

    [[nodiscard]] constexpr bool IsEqualTo(const FrameId other) const noexcept
    {
        return (m_registry == other.m_registry) && (m_object.GetValue() == other.m_object.GetValue());
    }

private:
    const Object::ObjectRegistry* m_registry{nullptr};
    Object::ObjectId m_object{};
};

enum class FrameKind : Foundation::UInt8
{
    Ram = 1U,
    Device = 2U,
};

enum class FrameState : Foundation::UInt8
{
    Uninitialized,
    Ready,
};

enum class FrameError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    InvalidDescriptor,
    AlreadyInitialized,
};

enum class FrameMappingPolicyError : Foundation::UInt8
{
    None,
    InvalidPermissions,
    WriteExecuteConflict,
    ReferenceExhausted,
    PhysicalOwnershipConflict,
};

struct FrameDescriptor
{
    PhysicalAddress PhysicalStart;
    PageCount Pages;
    MemoryCachePolicy CachePolicy;
    FrameKind Kind;
    Foundation::UInt8 Reserved[6];
};

struct FrameSnapshot
{
    FrameDescriptor Descriptor;
    FrameState State;
    Foundation::UInt8 ReadOnly;
    Foundation::UInt8 Reserved[6];
};

/// One write-versus-execute reservation record of a boot Frame: an exact
/// physical sub-range and access (1 writable, 2 executable) with its
/// reference count; a zero count is a free record.
struct FrameMappingPolicyRecord
{
    PhysicalAddress PhysicalStart;
    PageCount Pages;
    Foundation::UInt64 ReferenceCount;
    Foundation::UInt8 Access;
    Foundation::UInt8 Reserved[7];
};

/// Admits a caller to a Frame's mapping reservations: only AddressSpace and
/// IommuDomain, which commit CPU and DMA translations, construct one. A key
/// rather than friendship, so Frame itself declares no friend and its
/// destructor can stay protected for BootFrame and CreatedFrame.
class FrameMappingKey final
{
private:
    friend class AddressSpace;
    friend class Device::IommuDomain;

    constexpr FrameMappingKey() noexcept = default;
};

/// Describes one immutable, physically contiguous range of 4 KiB frames. Frame supplies identity,
/// lifetime, ownership, and one cache policy. It does not allocate pages and does not install a
/// virtual mapping. The allocator or device enumerator proves physical ownership before
/// registration; an AddressSpace or IOMMU mapper later validates mapping authority and alias
/// policy. While any mapping reservation of a Frame is held, the Frame claims its whole physical
/// range in its ObjectRegistry, so no other Frame of that registry can reserve overlapping pages.
/// Its write-versus-execute policy takes one of two forms. A BootFrame keeps
/// MaximumFrameMappingPolicyRecords sub-range records, so disjoint writable and executable
/// sub-ranges of it stay legal. A CreatedFrame, the form FrameObjectPool creates, keeps two
/// counters: a writable reservation is refused while any executable one is held, and the reverse,
/// whatever the sub-ranges, and the counters cannot be exhausted by distinct ranges.
class Frame : public Object::IKernelObject
{
public:
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&&) = delete;
    Frame& operator=(Frame&&) = delete;

    /// Publishes one immutable physical range. Invalid initialization is
    /// failure-atomic and can be retried.
    [[nodiscard]] FrameError Initialize(Object::ResourceDomainId chargeOwner,
                                        const FrameDescriptor& descriptor) noexcept;

    /// Publishes bootstrap data that CPU and DMA mappings may only read.
    /// No writable or executable mapping can be reserved for this identity.
    [[nodiscard]] FrameError InitializeReadOnly(Object::ResourceDomainId chargeOwner,
                                                const FrameDescriptor& descriptor) noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] FrameSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

    /// Reserves Frame-wide W^X policy before one CPU or IOMMU translation
    /// commit. The first reservation of any access also claims the Frame's
    /// whole range for object, this Frame's identity in registry; a claim
    /// held by another Frame of that registry refuses it with
    /// PhysicalOwnershipConflict. The caller releases a reservation, with the
    /// same registry and identity, on an ordinary HAL rejection, on IOMMU
    /// Unmap, or at its root's or domain's quiescence, and otherwise retains
    /// it (see Frame.md); the last release ends the claim. The Frame lock
    /// serializes reservations across every root and IOMMU domain and is held
    /// while the registry lock is taken.
    [[nodiscard]] FrameMappingPolicyError ReserveMappingPolicy(FrameMappingKey key, Object::ObjectRegistry& registry,
                                                               Object::ObjectId object, PhysicalAddress physicalStart,
                                                               PageCount pages, PagePermissions permissions) noexcept;
    void ReleaseMappingPolicy(FrameMappingKey key, Object::ObjectRegistry& registry, Object::ObjectId object,
                              PhysicalAddress physicalStart, PageCount pages, PagePermissions permissions) noexcept;

protected:
    /// A Frame with the two counters (CreatedFrame).
    constexpr Frame() noexcept = default;

    /// A Frame with records (BootFrame): records outlives this Frame.
    constexpr explicit Frame(FrameMappingPolicyRecord (&records)[MaximumFrameMappingPolicyRecords]) noexcept
        : m_mappingPolicies{records}
    {
    }

    /// Preconditions:
    /// - No lease, pin, mapper, or concurrent operation retains this object.
    ~Frame() noexcept = default;

private:
    [[nodiscard]] FrameError InitializeWithAccess(Object::ResourceDomainId chargeOwner,
                                                  const FrameDescriptor& descriptor, bool readOnly) noexcept;
    [[nodiscard]] FrameSnapshot GetSnapshotLocked() const noexcept;
    /// A BootFrame's record check: WriteExecuteConflict against an
    /// overlapping record of the opposite access, ReferenceExhausted when
    /// neither a record of the exact key with room nor a free one is left;
    /// otherwise matching or empty names the record to count.
    [[nodiscard]] FrameMappingPolicyError FindRecordLocked(PhysicalAddress physicalStart, PageCount pages,
                                                           Foundation::UInt8 access,
                                                           FrameMappingPolicyRecord*& matching,
                                                           FrameMappingPolicyRecord*& empty) noexcept;

    mutable Platform::Synchronization::SpinLock m_synchronization;
    // Reservations of every access. Each root and IOMMU domain holds at most
    // one per mapping, which bounds the count far below UInt32 exhaustion; it
    // also fills the padding after the one-byte lock.
    Foundation::UInt32 m_activeReservations{0U};
    // A CreatedFrame's writable and executable reservations, each at most
    // m_activeReservations; a BootFrame keeps them zero.
    Foundation::UInt32 m_writableReservations{0U};
    Foundation::UInt32 m_executableReservations{0U};
    Platform::Synchronization::AtomicUInt64 m_chargeOwnerValue;
    FrameDescriptor m_descriptor{};
    // A BootFrame's records; null for a CreatedFrame.
    FrameMappingPolicyRecord* m_mappingPolicies{nullptr};
    FrameState m_state{FrameState::Uninitialized};
    bool m_readOnly{false};
};

/// A Frame the kernel builds at boot or for a device it publishes: its
/// mappings may hold disjoint writable and executable sub-ranges at once,
/// up to MaximumFrameMappingPolicyRecords distinct ones (Frame.md).
class BootFrame final : public Frame
{
public:
    constexpr BootFrame() noexcept : Frame{m_records}
    {
    }

    ~BootFrame() noexcept = default;

    BootFrame(const BootFrame&) = delete;
    BootFrame& operator=(const BootFrame&) = delete;
    BootFrame(BootFrame&&) = delete;
    BootFrame& operator=(BootFrame&&) = delete;

private:
    FrameMappingPolicyRecord m_records[MaximumFrameMappingPolicyRecords]{};
};

/// A Frame user code created through FrameObjectPool: two W^X counters and no records, so 4,096 of
/// them fit the pool's slab.
class CreatedFrame final : public Frame
{
public:
    constexpr CreatedFrame() noexcept = default;
    ~CreatedFrame() noexcept = default;

    CreatedFrame(const CreatedFrame&) = delete;
    CreatedFrame& operator=(const CreatedFrame&) = delete;
    CreatedFrame(CreatedFrame&&) = delete;
    CreatedFrame& operator=(CreatedFrame&&) = delete;
};

static_assert(sizeof(FrameId) == 16U);
static_assert(alignof(FrameId) == 8U);
static_assert(sizeof(FrameKind) == 1U);
static_assert(sizeof(FrameState) == 1U);
static_assert(sizeof(FrameError) == 1U);
static_assert(sizeof(FrameMappingPolicyError) == 1U);
static_assert(sizeof(FrameDescriptor) == 24U);
static_assert(alignof(FrameDescriptor) == 8U);
static_assert(sizeof(FrameSnapshot) == 32U);
static_assert(alignof(FrameSnapshot) == 8U);
static_assert(sizeof(FrameMappingPolicyRecord) == 32U);
static_assert(sizeof(BootFrame) ==
              sizeof(Frame) + (MaximumFrameMappingPolicyRecords * sizeof(FrameMappingPolicyRecord)));
static_assert(sizeof(CreatedFrame) == sizeof(Frame));
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_FRAME_HPP_INCLUDED
