// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_MEMORY_OBJECT_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_MEMORY_OBJECT_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/MemoryTypes.hpp"
#include "Kernel/Object/IKernelObject.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Object
{
class ObjectRegistry;
}

namespace Asteria::Kernel::Memory
{
/// Identifies one MemoryObject generation within one ObjectRegistry.
/// The originating registry outlives every copy and use of this value.
class MemoryObjectId final
{
public:
    constexpr MemoryObjectId() noexcept = default;

    constexpr MemoryObjectId(const Object::ObjectRegistry& registry, const Object::ObjectId object) noexcept
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

    [[nodiscard]] constexpr bool IsEqualTo(const MemoryObjectId other) const noexcept
    {
        return (m_registry == other.m_registry) && (m_object.GetValue() == other.m_object.GetValue());
    }

private:
    const Object::ObjectRegistry* m_registry{nullptr};
    Object::ObjectId m_object{};
};

enum class MemoryCommitment : Foundation::UInt8
{
    Committed = 1U,
    Discardable = 2U,
    Reconstructible = 3U,
    BestEffort = 4U,
    Pinned = 5U,
    DeviceDma = 6U,
};

/// What a MemoryObject charges its ResourceDomain: its commitment's pages and its metadata (boot
/// memory), or its metadata only, where a Frame carries the pages that back it (created
/// MemoryObjects, Documentation/Architecture/ResourceDomain.md).
enum class MemoryObjectCharge : Foundation::UInt8
{
    Pages,
    MetadataOnly,
};

enum class MemoryObjectState : Foundation::UInt8
{
    Uninitialized,
    Ready,
    Quiesced,
};

enum class MemoryObjectError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    InvalidDescriptor,
    AlreadyInitialized,
    ResourceDomainPinRejected,
    ResourceDomainInstanceMismatch,
    ResourceDomainRejected,
    MappingRangeExceeded,
    MappingAccessDenied,
    MappingCacheMismatch,
    MappingCommitmentMismatch,
    MappingReferenceExhausted,
    MappingReferenceUnderflow,
    InUse,
    NotReady,
    Quiesced,
    InvariantViolation,
};

struct MemoryObjectDescriptor
{
    PageCount Pages;
    MemoryCachePolicy CachePolicy;
    PagePermissions MaximumPermissions;
    MemoryCommitment Commitment;
    Foundation::UInt8 Reserved[5];
};

struct MemoryObjectFailure
{
    MemoryObjectError Error;
    Object::ObjectPinError Pin;
    Resource::ResourceDomainError Resource;
    Foundation::UInt8 Reserved[5];
};

struct MemoryObjectSnapshot
{
    MemoryObjectDescriptor Descriptor;
    Resource::ResourceDomainObjectId ResourceDomainIdentity;
    Object::ResourceDomainId ResourceDomain;
    Foundation::UInt64 ActiveMappings;
    MemoryObjectState State;
    Foundation::UInt8 Reserved[7];
};

/// Describes one pager-managed logical page range and its immutable policy. The object does not own
/// a page table or choose physical backing. A caller with separately authorized MemoryObject,
/// Frame, and AddressSpace capabilities asks FaultFrameMappingPreparation to combine one logical
/// object slice with one physical Frame slice. Committed policies reserve their guarantee from the
/// ResourceDomain for this object's whole lifetime.
class MemoryObject final : public Object::IKernelObject
{
public:
    constexpr MemoryObject() noexcept = default;

    /// Preconditions:
    /// - State is Uninitialized or Quiesced;
    /// - no mapping preparation, object pin, or concurrent operation remains.
    ~MemoryObject() noexcept = default;

    MemoryObject(const MemoryObject&) = delete;
    MemoryObject& operator=(const MemoryObject&) = delete;
    MemoryObject(MemoryObject&&) = delete;
    MemoryObject& operator=(MemoryObject&&) = delete;

    /// Reserves metadata and, unless charge is MetadataOnly, the selected
    /// commitment in resourceDomain, then publishes an immutable descriptor.
    /// Every failure leaves this object Uninitialized and retains no pin or
    /// resource charge.
    [[nodiscard]] MemoryObjectFailure Initialize(Object::ObjectRegistry& registry,
                                                 Resource::ResourceDomainObjectId resourceDomainIdentity,
                                                 Resource::ResourceDomain& resourceDomain,
                                                 const MemoryObjectDescriptor& descriptor,
                                                 MemoryObjectCharge charge = MemoryObjectCharge::Pages) noexcept;

    /// Releases the complete ResourceDomain reservation and its retained pin.
    /// Quiesced is terminal for this storage instance.
    [[nodiscard]] MemoryObjectFailure Quiesce() noexcept;

    /// Atomically revalidates a prepared logical slice and retains one live
    /// mapping reference. AddressSpace pairs every success with ReleaseMapping
    /// after the native mapping can no longer be used.
    [[nodiscard]] MemoryObjectError RetainMapping(Foundation::UInt64 offsetPages, PageCount pages,
                                                  PagePermissions permissions, MemoryCachePolicy cachePolicy,
                                                  MemoryCommitment commitment) noexcept;

    [[nodiscard]] MemoryObjectError ReleaseMapping() noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] MemoryObjectSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

private:
    [[nodiscard]] static MemoryObjectFailure
    MakeFailure(MemoryObjectError error, Object::ObjectPinError pin = Object::ObjectPinError::Ok,
                Resource::ResourceDomainError resource = Resource::ResourceDomainError::None) noexcept;
    [[nodiscard]] static Resource::ResourceReservation GetReservation(const MemoryObjectDescriptor& descriptor,
                                                                      MemoryObjectCharge charge) noexcept;

    mutable Platform::Synchronization::SpinLock m_synchronization;
    Platform::Synchronization::AtomicUInt64 m_chargeOwnerValue;
    Object::ObjectPin m_resourceDomainPin;
    Resource::ResourceDomain* m_resourceDomain{nullptr};
    MemoryObjectDescriptor m_descriptor{};
    Resource::ResourceDomainObjectId m_resourceDomainIdentity{};
    Object::ResourceDomainId m_resourceDomainIdentifier{};
    Foundation::UInt64 m_activeMappings{0U};
    MemoryObjectState m_state{MemoryObjectState::Uninitialized};
    /// What Initialize reserved, which Quiesce releases.
    MemoryObjectCharge m_charge{MemoryObjectCharge::Pages};
};

static_assert(sizeof(MemoryObjectId) == 16U);
static_assert(alignof(MemoryObjectId) == 8U);
static_assert(sizeof(MemoryCommitment) == 1U);
static_assert(sizeof(MemoryObjectState) == 1U);
static_assert(sizeof(MemoryObjectError) == 1U);
static_assert(sizeof(MemoryObjectDescriptor) == 16U);
static_assert(alignof(MemoryObjectDescriptor) == 8U);
static_assert(sizeof(MemoryObjectFailure) == 8U);
static_assert(alignof(MemoryObjectFailure) == 1U);
static_assert(sizeof(MemoryObjectSnapshot) == 56U);
static_assert(alignof(MemoryObjectSnapshot) == 8U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_MEMORY_OBJECT_HPP_INCLUDED
