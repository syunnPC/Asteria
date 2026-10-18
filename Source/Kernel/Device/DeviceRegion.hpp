// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_DEVICE_REGION_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_DEVICE_REGION_HPP_INCLUDED

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

namespace Asteria::Kernel::Device
{
class DeviceManager;

/// Identifies one DeviceRegion generation within one ObjectRegistry.
/// The originating registry outlives every copy and use of this value.
class DeviceRegionId final
{
public:
    constexpr DeviceRegionId() noexcept = default;

    constexpr DeviceRegionId(const Object::ObjectRegistry& registry, const Object::ObjectId object) noexcept
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

    [[nodiscard]] constexpr bool IsEqualTo(const DeviceRegionId other) const noexcept
    {
        return (m_registry == other.m_registry) && (m_object.GetValue() == other.m_object.GetValue());
    }

private:
    const Object::ObjectRegistry* m_registry{nullptr};
    Object::ObjectId m_object{};
};

enum class DeviceRegionKind : Foundation::UInt8
{
    MemoryMappedIo = 1U,
    PortIo = 2U,
};

enum class DeviceRegionPermission : Foundation::UInt8
{
    Read = Foundation::UInt8{1U} << 0U,
    Write = Foundation::UInt8{1U} << 1U,
};

class DeviceRegionPermissions final
{
public:
    constexpr DeviceRegionPermissions() noexcept = default;

    explicit constexpr DeviceRegionPermissions(const DeviceRegionPermission permission) noexcept
        : m_bits{static_cast<Foundation::UInt8>(permission)}
    {
    }

    [[nodiscard]] constexpr DeviceRegionPermissions With(const DeviceRegionPermission permission) const noexcept
    {
        return DeviceRegionPermissions{
            static_cast<Foundation::UInt8>(m_bits | static_cast<Foundation::UInt8>(permission)),
            RawValueTag{},
        };
    }

    [[nodiscard]] constexpr bool Contains(const DeviceRegionPermission permission) const noexcept
    {
        const Foundation::UInt8 bit = static_cast<Foundation::UInt8>(permission);
        return (m_bits & bit) == bit;
    }

    [[nodiscard]] constexpr bool HasOnlyKnownBits() const noexcept
    {
        return (m_bits & static_cast<Foundation::UInt8>(~KnownBits)) == 0U;
    }

    [[nodiscard]] constexpr bool IsEmpty() const noexcept
    {
        return m_bits == 0U;
    }

    [[nodiscard]] constexpr Foundation::UInt8 GetBits() const noexcept
    {
        return m_bits;
    }

private:
    struct RawValueTag final
    {
    };

    constexpr DeviceRegionPermissions(const Foundation::UInt8 bits, RawValueTag) noexcept : m_bits{bits}
    {
    }

    static constexpr Foundation::UInt8 KnownBits = static_cast<Foundation::UInt8>(DeviceRegionPermission::Read) |
                                                   static_cast<Foundation::UInt8>(DeviceRegionPermission::Write);

    Foundation::UInt8 m_bits{0U};
};

/// Encodes both the natural byte width and its descriptor mask bit.
enum class DeviceAccessWidth : Foundation::UInt8
{
    Byte = Foundation::UInt8{1U} << 0U,
    Word = Foundation::UInt8{1U} << 1U,
    DoubleWord = Foundation::UInt8{1U} << 2U,
    QuadWord = Foundation::UInt8{1U} << 3U,
};

class DeviceAccessWidths final
{
public:
    constexpr DeviceAccessWidths() noexcept = default;

    explicit constexpr DeviceAccessWidths(const DeviceAccessWidth width) noexcept
        : m_bits{static_cast<Foundation::UInt8>(width)}
    {
    }

    [[nodiscard]] constexpr DeviceAccessWidths With(const DeviceAccessWidth width) const noexcept
    {
        return DeviceAccessWidths{
            static_cast<Foundation::UInt8>(m_bits | static_cast<Foundation::UInt8>(width)),
            RawValueTag{},
        };
    }

    [[nodiscard]] constexpr bool Contains(const DeviceAccessWidth width) const noexcept
    {
        const Foundation::UInt8 bit = static_cast<Foundation::UInt8>(width);
        return (m_bits & bit) == bit;
    }

    [[nodiscard]] constexpr bool HasOnlyKnownBits() const noexcept
    {
        return (m_bits & static_cast<Foundation::UInt8>(~KnownBits)) == 0U;
    }

    [[nodiscard]] constexpr bool IsEmpty() const noexcept
    {
        return m_bits == 0U;
    }

    [[nodiscard]] constexpr Foundation::UInt8 GetBits() const noexcept
    {
        return m_bits;
    }

private:
    struct RawValueTag final
    {
    };

    constexpr DeviceAccessWidths(const Foundation::UInt8 bits, RawValueTag) noexcept : m_bits{bits}
    {
    }

    static constexpr Foundation::UInt8 KnownBits = static_cast<Foundation::UInt8>(DeviceAccessWidth::Byte) |
                                                   static_cast<Foundation::UInt8>(DeviceAccessWidth::Word) |
                                                   static_cast<Foundation::UInt8>(DeviceAccessWidth::DoubleWord) |
                                                   static_cast<Foundation::UInt8>(DeviceAccessWidth::QuadWord);

    Foundation::UInt8 m_bits{0U};
};

enum class DeviceAccessOperation : Foundation::UInt8
{
    Read,
    Write,
};

enum class DeviceRegionState : Foundation::UInt8
{
    Uninitialized,
    Ready,
    Quiesced,
};

enum class DeviceRegionError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    InvalidDescriptor,
    AlreadyInitialized,
    ResourceDomainPinRejected,
    ResourceDomainInstanceMismatch,
    ResourceDomainRejected,
    NotReady,
    Quiesced,
    InvalidOperation,
    WriteValueDenied,
    UnsupportedAccess,
    UnsupportedWidth,
    RangeExceeded,
    MisalignedAccess,
    MappingNotSupported,
    MappingAccessDenied,
    MappingCacheMismatch,
    ReferenceExhausted,
    AccessReferenceUnderflow,
    MappingReferenceUnderflow,
    InUse,
    InvariantViolation,
};

/// Immutable publisher-owned lists; storage must outlive the region. An absent
/// offset is unrestricted. A listed offset accepts only its width and values;
/// overlapping wider writes are refused. At most 32 lists and 32 values per list.
struct DevicePortWriteList
{
    Foundation::UInt64 Offset;
    DeviceAccessWidth Width;
    const Foundation::UInt32* Values;
    Foundation::Size Count;
};

struct DevicePortWritePolicy
{
    const DevicePortWriteList* Lists{nullptr};
    Foundation::Size Count{0U};
};

struct DeviceRegionDescriptor
{
    Foundation::UInt64 BaseAddress;
    Foundation::UInt64 LengthBytes;
    DeviceRegionKind Kind;
    DeviceRegionPermissions Permissions;
    DeviceAccessWidths Widths;
    Memory::MemoryCachePolicy CachePolicy;
    Foundation::UInt8 Reserved[4];
};

struct DeviceRegionFailure
{
    DeviceRegionError Error;
    Object::ObjectPinError Pin;
    Resource::ResourceDomainError Resource;
    Foundation::UInt8 Reserved[5];
};

struct DeviceRegionSnapshot
{
    DeviceRegionDescriptor Descriptor;
    Resource::ResourceDomainObjectId ResourceDomainIdentity;
    Object::ResourceDomainId ResourceDomain;
    Foundation::UInt64 ActiveMappings;
    Foundation::UInt64 ActiveRegisterAccesses;
    DeviceRegionState State;
    Foundation::UInt8 Reserved[7];
};

/// Owns one bounded MMIO or port-I/O authority range. The DeviceManager derives regions only from
/// discovered platform resources. A region supplies policy and lifetime; target HAL code performs
/// the actual volatile register operation, and AddressSpace performs page-table edits.
class DeviceRegion final : public Object::IKernelObject
{
public:
    constexpr DeviceRegion() noexcept = default;

    /// Preconditions:
    /// - State is Uninitialized or Quiesced.
    /// - No retained mapping, register access, object pin, or concurrent call
    /// remains.
    ~DeviceRegion() noexcept = default;

    DeviceRegion(const DeviceRegion&) = delete;
    DeviceRegion& operator=(const DeviceRegion&) = delete;
    DeviceRegion(DeviceRegion&&) = delete;
    DeviceRegion& operator=(DeviceRegion&&) = delete;

    [[nodiscard]] static bool DescriptorIsValid(const DeviceRegionDescriptor& descriptor) noexcept;

    /// Reserves metadata in the exact ResourceDomain and publishes an
    /// immutable hardware range. Every failure is side-effect free.
    [[nodiscard]] DeviceRegionFailure Initialize(Object::ObjectRegistry& registry,
                                                 Resource::ResourceDomainObjectId resourceDomainIdentity,
                                                 Resource::ResourceDomain& resourceDomain,
                                                 const DeviceRegionDescriptor& descriptor,
                                                 DevicePortWritePolicy writes = {}) noexcept;

    /// Releases the ResourceDomain reservation after all active uses end.
    /// Quiesced is terminal for public callers and the current registry
    /// generation; only the owning manager can recycle collected storage.
    [[nodiscard]] DeviceRegionFailure Quiesce() noexcept;

    /// Validates and retains one bounded register operation. The target HAL
    /// performs exactly this operation before ReleaseRegisterAccess.
    [[nodiscard]] DeviceRegionError RetainRegisterAccess(Foundation::UInt64 offsetBytes, DeviceAccessWidth width,
                                                         DeviceAccessOperation operation,
                                                         Foundation::UInt32 value) noexcept;

    [[nodiscard]] DeviceRegionError ReleaseRegisterAccess() noexcept;

    /// Checks a prospective MMIO mapping without retaining it. A later owner
    /// must call RetainMapping and handle intervening lifecycle changes.
    [[nodiscard]] DeviceRegionError ValidateMapping(Memory::PhysicalAddress physicalStart, Memory::PageCount pages,
                                                    Memory::PagePermissions permissions,
                                                    Memory::MemoryCachePolicy cachePolicy) const noexcept;

    /// Validates and retains one page-aligned MMIO mapping. Device-backed
    /// AddressSpace records pair every success with ReleaseMapping.
    [[nodiscard]] DeviceRegionError RetainMapping(Memory::PhysicalAddress physicalStart, Memory::PageCount pages,
                                                  Memory::PagePermissions permissions,
                                                  Memory::MemoryCachePolicy cachePolicy) noexcept;

    [[nodiscard]] DeviceRegionError ReleaseMapping() noexcept;

    /// Lock-free last retained mapping policy, initially the publication policy.
    /// Terminal drawing uses it after freezing writers; it survives the last unmap.
    [[nodiscard]] Memory::MemoryCachePolicy GetMappingCachePolicy() const noexcept;

    /// Terminal-only, lock-free single CAS: latches the last policy against
    /// future type changes. False on a racing selection, with output untouched;
    /// the stopper must then skip drawing. Same-policy mappings remain allowed.
    [[nodiscard]] bool FreezeMappingCachePolicy(Memory::MemoryCachePolicy& output) noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] DeviceRegionSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

private:
    friend class DeviceManager;

    [[nodiscard]] static DeviceRegionFailure
    MakeFailure(DeviceRegionError error, Object::ObjectPinError pin = Object::ObjectPinError::Ok,
                Resource::ResourceDomainError resource = Resource::ResourceDomainError::None) noexcept;
    [[nodiscard]] DeviceRegionError ValidateMappingLocked(Memory::PhysicalAddress physicalStart,
                                                          Memory::PageCount pages, Memory::PagePermissions permissions,
                                                          Memory::MemoryCachePolicy cachePolicy) const noexcept;
    [[nodiscard]] DeviceRegionError ResetForDeviceManagerReuse() noexcept;

    mutable Platform::Synchronization::SpinLock m_synchronization;
    Platform::Synchronization::AtomicUInt64 m_chargeOwnerValue;
    Platform::Synchronization::AtomicUInt64 m_mappingCachePolicy;
    Object::ObjectPin m_resourceDomainPin;
    Resource::ResourceDomain* m_resourceDomain{nullptr};
    DeviceRegionDescriptor m_descriptor{};
    DevicePortWritePolicy m_writes{};
    Resource::ResourceDomainObjectId m_resourceDomainIdentity{};
    Object::ResourceDomainId m_resourceDomainIdentifier{};
    Foundation::UInt64 m_activeMappings{0U};
    Foundation::UInt64 m_activeRegisterAccesses{0U};
    DeviceRegionState m_state{DeviceRegionState::Uninitialized};
};

static_assert(sizeof(DeviceRegionId) == 16U);
static_assert(alignof(DeviceRegionId) == 8U);
static_assert(sizeof(DeviceRegionKind) == 1U);
static_assert(sizeof(DeviceRegionPermissions) == 1U);
static_assert(sizeof(DeviceAccessWidth) == 1U);
static_assert(sizeof(DeviceAccessWidths) == 1U);
static_assert(sizeof(DeviceAccessOperation) == 1U);
static_assert(sizeof(DeviceRegionState) == 1U);
static_assert(sizeof(DeviceRegionError) == 1U);
static_assert(sizeof(DeviceRegionDescriptor) == 24U);
static_assert(alignof(DeviceRegionDescriptor) == 8U);
static_assert(sizeof(DeviceRegionFailure) == 8U);
static_assert(alignof(DeviceRegionFailure) == 1U);
static_assert(sizeof(DeviceRegionSnapshot) == 72U);
static_assert(alignof(DeviceRegionSnapshot) == 8U);
} // Namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_DEVICE_REGION_HPP_INCLUDED
