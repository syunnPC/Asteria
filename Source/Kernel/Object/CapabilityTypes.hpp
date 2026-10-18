// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_OBJECT_CAPABILITY_TYPES_HPP_INCLUDED
#define ASTERIA_KERNEL_OBJECT_CAPABILITY_TYPES_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Object
{
class CapabilitySpace;

class ObjectId final
{
public:
    constexpr ObjectId() noexcept = default;

    explicit constexpr ObjectId(const Foundation::UInt64 value) noexcept : m_value{value}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetValue() const noexcept
    {
        return m_value;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return m_value != 0U;
    }

private:
    Foundation::UInt64 m_value{0U};
};

class ResourceDomainId final
{
public:
    constexpr ResourceDomainId() noexcept = default;

    explicit constexpr ResourceDomainId(const Foundation::UInt64 value) noexcept : m_value{value}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetValue() const noexcept
    {
        return m_value;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return m_value != 0U;
    }

private:
    Foundation::UInt64 m_value{0U};
};

class RevocationDomainId final
{
public:
    constexpr RevocationDomainId() noexcept = default;

    explicit constexpr RevocationDomainId(const Foundation::UInt64 value) noexcept : m_value{value}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetValue() const noexcept
    {
        return m_value;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return m_value != 0U;
    }

private:
    Foundation::UInt64 m_value{0U};
};

static_assert(sizeof(RevocationDomainId) == 8U);

class MonotonicTime final
{
public:
    constexpr MonotonicTime() noexcept = default;

    explicit constexpr MonotonicTime(const Foundation::UInt64 ticks) noexcept : m_ticks{ticks}
    {
    }

    [[nodiscard]] static constexpr MonotonicTime Infinite() noexcept
    {
        return MonotonicTime{~Foundation::UInt64{0U}};
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetTicks() const noexcept
    {
        return m_ticks;
    }

    [[nodiscard]] constexpr bool IsInfinite() const noexcept
    {
        return m_ticks == Infinite().GetTicks();
    }

private:
    Foundation::UInt64 m_ticks{0U};
};

class CapabilityHandle final
{
public:
    constexpr CapabilityHandle() noexcept = default;

    constexpr CapabilityHandle(const Foundation::UInt64 selector, const Foundation::UInt64 authenticator) noexcept
        : m_selector{selector}, m_authenticator{authenticator}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetSelector() const noexcept
    {
        return m_selector;
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetAuthenticator() const noexcept
    {
        return m_authenticator;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return m_selector != 0U;
    }

private:
    Foundation::UInt64 m_selector{0U};
    Foundation::UInt64 m_authenticator{0U};
};

static_assert(sizeof(CapabilityHandle) == 16U);

class CapabilityGenerationPolicy final
{
public:
    static constexpr Foundation::UInt32 SelectorIndexBits{8U};
    static constexpr Foundation::UInt64 SelectorIndexMask{(Foundation::UInt64{1U} << SelectorIndexBits) - 1U};
    static constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U} >> SelectorIndexBits};

    CapabilityGenerationPolicy() = delete;

    [[nodiscard]] static constexpr bool CanAdvance(const Foundation::UInt64 generation) noexcept
    {
        return generation < Maximum;
    }

    /// Preconditions:
    /// - CanAdvance(generation) is true.
    [[nodiscard]] static constexpr Foundation::UInt64 Advance(const Foundation::UInt64 generation) noexcept
    {
        return generation + 1U;
    }
};

static_assert(CapabilityGenerationPolicy::CanAdvance(CapabilityGenerationPolicy::Maximum - 1U));
static_assert(!CapabilityGenerationPolicy::CanAdvance(CapabilityGenerationPolicy::Maximum));
static_assert(CapabilityGenerationPolicy::Maximum == (~Foundation::UInt64{0U} >> 8U));
static_assert(CapabilityGenerationPolicy::SelectorIndexMask == 0xFFU);
static_assert(CapabilityGenerationPolicy::Advance(CapabilityGenerationPolicy::Maximum - 1U) ==
              CapabilityGenerationPolicy::Maximum);

class CapabilitySpaceIdentifier final
{
public:
    constexpr CapabilitySpaceIdentifier() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetValue() const noexcept
    {
        return m_value;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return m_value != 0U;
    }

private:
    friend class CapabilitySpace;

    explicit constexpr CapabilitySpaceIdentifier(const Foundation::UInt64 value) noexcept : m_value{value}
    {
    }

    Foundation::UInt64 m_value{0U};
};

enum class ObjectType : Foundation::UInt8
{
    Invalid = 0U,
    ProtectionDomain = 1U,
    AddressSpace = 2U,
    CapabilitySpace = 3U,
    Thread = 4U,
    SchedulingContext = 5U,
    ResourceDomain = 6U,
    MemoryObject = 7U,
    Frame = 8U,
    Endpoint = 9U,
    ReplyToken = 10U,
    Notification = 11U,
    WaitSet = 12U,
    Timer = 13U,
    Interrupt = 14U,
    DeviceRegion = 15U,
    IommuDomain = 16U,
    FaultChannel = 17U,
    CpuSet = 18U,
    Vcpu = 19U,
    FaultToken = 20U,
    PciFunction = 21U,
    DiagnosticChannel = 22U,
    InterruptLine = 23U,
};

enum class CapabilityRight : Foundation::UInt16
{
    Inspect = Foundation::UInt16{1U} << 0U,
    Invoke = Foundation::UInt16{1U} << 1U,
    Read = Foundation::UInt16{1U} << 2U,
    Write = Foundation::UInt16{1U} << 3U,
    Map = Foundation::UInt16{1U} << 4U,
    Wait = Foundation::UInt16{1U} << 5U,
    Signal = Foundation::UInt16{1U} << 6U,
    Transfer = Foundation::UInt16{1U} << 7U,
    Duplicate = Foundation::UInt16{1U} << 8U,
    Derive = Foundation::UInt16{1U} << 9U,
    Revoke = Foundation::UInt16{1U} << 10U,
    Manage = Foundation::UInt16{1U} << 11U,
    Debug = Foundation::UInt16{1U} << 12U,
    /// ResourceDomain authority to create executable MemoryObjects.
    Code = Foundation::UInt16{1U} << 13U,
};

enum class CapabilityOperation : Foundation::UInt8
{
    Inspect,
    Invoke,
    Read,
    Write,
    Map,
    Wait,
    Signal,
    Transfer,
    Duplicate,
    Derive,
    Revoke,
    Manage,
    Debug,
};

class CapabilityRights final
{
public:
    constexpr CapabilityRights() noexcept = default;

    explicit constexpr CapabilityRights(const CapabilityRight right) noexcept
        : m_bits{static_cast<Foundation::UInt16>(right)}
    {
    }

    [[nodiscard]] static constexpr CapabilityRights All() noexcept
    {
        return CapabilityRights{KnownBits, RawValueTag{}};
    }

    [[nodiscard]] constexpr CapabilityRights With(const CapabilityRight right) const noexcept
    {
        return CapabilityRights{
            static_cast<Foundation::UInt16>(m_bits | static_cast<Foundation::UInt16>(right)),
            RawValueTag{},
        };
    }

    [[nodiscard]] constexpr bool ContainsAll(const CapabilityRights required) const noexcept
    {
        return (m_bits & required.m_bits) == required.m_bits;
    }

    [[nodiscard]] constexpr bool IsSubsetOf(const CapabilityRights available) const noexcept
    {
        return available.ContainsAll(*this);
    }

    [[nodiscard]] constexpr bool IsEqualTo(const CapabilityRights other) const noexcept
    {
        return m_bits == other.m_bits;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_bits & static_cast<Foundation::UInt16>(~KnownBits)) == 0U;
    }

    [[nodiscard]] constexpr Foundation::UInt16 GetRawValue() const noexcept
    {
        return m_bits;
    }

private:
    struct RawValueTag
    {
    };

    static constexpr Foundation::UInt16 KnownBits{0x3FFFU};

    constexpr CapabilityRights(const Foundation::UInt16 bits, const RawValueTag) noexcept : m_bits{bits}
    {
    }

    Foundation::UInt16 m_bits{0U};
};

struct CapabilityDescriptor
{
    ObjectId Object;
    ObjectType Type;
    CapabilityRights Rights;
    RevocationDomainId RevocationDomain;
    MonotonicTime LeaseDeadline;
    Foundation::UInt64 SealedObjectSelector;
    ResourceDomainId ChargeOwner;
};

enum class CapabilityError : Foundation::UInt8
{
    Ok,
    InvalidArgument,
    InvalidConfiguration,
    InvalidHandle,
    ObjectNotFound,
    ResourceExhausted,
    SpaceExhausted,
    RightsDenied,
    RightsAmplification,
    NoAttenuation,
    TypeMismatch,
    Revoked,
    Expired,
    LeaseExtension,
    RevocationDomainExhausted,
};

[[nodiscard]] constexpr bool IsKnownObjectType(const ObjectType type) noexcept
{
    switch (type)
    {
    case ObjectType::Invalid:
        return false;

    case ObjectType::ProtectionDomain:
    case ObjectType::AddressSpace:
    case ObjectType::CapabilitySpace:
    case ObjectType::Thread:
    case ObjectType::SchedulingContext:
    case ObjectType::ResourceDomain:
    case ObjectType::MemoryObject:
    case ObjectType::Frame:
    case ObjectType::Endpoint:
    case ObjectType::ReplyToken:
    case ObjectType::Notification:
    case ObjectType::WaitSet:
    case ObjectType::Timer:
    case ObjectType::Interrupt:
    case ObjectType::DeviceRegion:
    case ObjectType::IommuDomain:
    case ObjectType::FaultChannel:
    case ObjectType::CpuSet:
    case ObjectType::Vcpu:
    case ObjectType::FaultToken:
    case ObjectType::PciFunction:
    case ObjectType::InterruptLine:
    case ObjectType::DiagnosticChannel:
        return true;
    }

    return false;
}
} // Namespace Asteria::Kernel::Object

#endif // ASTERIA_KERNEL_OBJECT_CAPABILITY_TYPES_HPP_INCLUDED
