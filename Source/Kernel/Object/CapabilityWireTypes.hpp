// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_OBJECT_CAPABILITY_WIRE_TYPES_HPP_INCLUDED
#define ASTERIA_KERNEL_OBJECT_CAPABILITY_WIRE_TYPES_HPP_INCLUDED

#include "Kernel/Object/CapabilityTypes.hpp"

namespace Asteria::Kernel::Object
{
/// Returns the stable wire value for a known object type, or zero for Invalid
/// and values outside the object-type contract.
[[nodiscard]] constexpr Foundation::UInt8 EncodeObjectType(const ObjectType type) noexcept
{
    return IsKnownObjectType(type) ? static_cast<Foundation::UInt8>(type) : 0U;
}

/// Decodes one stable object-type wire value without accepting zero or holes.
[[nodiscard]] constexpr bool TryDecodeObjectType(const Foundation::UInt8 encoded, ObjectType& type) noexcept
{
    const ObjectType candidate = static_cast<ObjectType>(encoded);
    if (!IsKnownObjectType(candidate))
    {
        type = ObjectType::Invalid;
        return false;
    }
    type = candidate;
    return true;
}

/// Decodes the fixed sixteen-bit capability-rights field. Unknown bits fail
/// closed and leave the output empty.
[[nodiscard]] constexpr bool TryDecodeCapabilityRights(const Foundation::UInt16 bits, CapabilityRights& rights) noexcept
{
    if ((bits & static_cast<Foundation::UInt16>(~CapabilityRights::All().GetRawValue())) != 0U)
    {
        rights = CapabilityRights{};
        return false;
    }

    constexpr CapabilityRight knownRights[]{
        CapabilityRight::Inspect,   CapabilityRight::Invoke, CapabilityRight::Read,   CapabilityRight::Write,
        CapabilityRight::Map,       CapabilityRight::Wait,   CapabilityRight::Signal, CapabilityRight::Transfer,
        CapabilityRight::Duplicate, CapabilityRight::Derive, CapabilityRight::Revoke, CapabilityRight::Manage,
        CapabilityRight::Debug,     CapabilityRight::Code,
    };
    CapabilityRights decoded{};
    for (const CapabilityRight right : knownRights)
    {
        if ((bits & static_cast<Foundation::UInt16>(right)) != 0U)
        {
            decoded = decoded.With(right);
        }
    }
    rights = decoded;
    return true;
}

static_assert(EncodeObjectType(ObjectType::Invalid) == 0U);
static_assert(EncodeObjectType(ObjectType::MemoryObject) == 7U);
static_assert(EncodeObjectType(ObjectType::PciFunction) == 21U);
static_assert(EncodeObjectType(ObjectType::DiagnosticChannel) == 22U);
} // namespace Asteria::Kernel::Object

#endif // ASTERIA_KERNEL_OBJECT_CAPABILITY_WIRE_TYPES_HPP_INCLUDED
