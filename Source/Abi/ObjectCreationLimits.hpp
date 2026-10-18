// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ABI_OBJECT_CREATION_LIMITS_HPP_INCLUDED
#define ASTERIA_ABI_OBJECT_CREATION_LIMITS_HPP_INCLUDED
#include "Foundation/Types.hpp"
namespace Asteria::Abi
{
// Order of the ResourceDomain request's eight halfwords.
enum class LimitedObject : Foundation::UInt8
{
    ProtectionDomain,
    AddressSpace,
    CapabilitySpace,
    Endpoint,
    WaitSet,
    FaultChannel,
    Frame,
    MemoryObject,
    Count,
};
inline constexpr Foundation::Size LimitedObjectCount{8U};
inline constexpr Foundation::Size FaultChannelMaximumCount{4U};
// One compositor and one restart overlap.
inline constexpr Foundation::UInt16 LargePageTableArenaMaximumCount{2U};
static_assert(LimitedObjectCount == static_cast<Foundation::Size>(LimitedObject::Count));
inline constexpr Foundation::UInt16 ObjectPoolCapacities[LimitedObjectCount]{
    16U, 16U, 16U, 48U, 16U, FaultChannelMaximumCount, 4096U, 4096U};
struct ObjectCreationLimits
{
    Foundation::UInt16 Counts[LimitedObjectCount]{};
    constexpr ObjectCreationLimits() noexcept
    {
        for (Foundation::Size i{0U}; i < LimitedObjectCount; ++i)
        {
            Counts[i] = ObjectPoolCapacities[i];
        }
    }
    // Boot roots grant the pool capacity.
    Foundation::UInt16 LargePageTableArenas{LargePageTableArenaMaximumCount};
};
static_assert(sizeof(ObjectCreationLimits) == 18U);
}
#endif
