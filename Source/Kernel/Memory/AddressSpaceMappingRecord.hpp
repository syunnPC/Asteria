// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_ADDRESS_SPACE_MAPPING_RECORD_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_ADDRESS_SPACE_MAPPING_RECORD_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Device/DeviceRegion.hpp"
#include "Kernel/Memory/Frame.hpp"
#include "Kernel/Memory/MemoryObject.hpp"

namespace Asteria::Kernel::Memory
{
constexpr Foundation::Size MaximumRuntimeAddressSpaceMappings{256U};

/// Authority-free, architecture-neutral description of one page-table range.
/// Object identities describe retained ownership; they grant no authority.
struct AddressSpaceMappingRecord
{
    MemoryObjectId MemoryObject;
    Device::DeviceRegionId DeviceRegion;
    FrameId Frame;
    VirtualAddress VirtualStart;
    PhysicalAddress PhysicalStart;
    PageCount Pages;
    Foundation::UInt64 MemoryObjectOffsetPages;
    PagePermissions Permissions;
    MemoryCachePolicy CachePolicy;
    FrameKind Kind;
    MemoryCommitment Commitment;
    bool RuntimeMapping;
    Foundation::UInt8 Reserved[3];
};

static_assert(sizeof(AddressSpaceMappingRecord) == 88U);
static_assert(alignof(AddressSpaceMappingRecord) == 8U);
static_assert(__is_aggregate(AddressSpaceMappingRecord));
static_assert(__is_standard_layout(AddressSpaceMappingRecord));
static_assert(__is_trivially_copyable(AddressSpaceMappingRecord));
static_assert(__builtin_offsetof(AddressSpaceMappingRecord, MemoryObject) == 0U);
static_assert(__builtin_offsetof(AddressSpaceMappingRecord, DeviceRegion) == 16U);
static_assert(__builtin_offsetof(AddressSpaceMappingRecord, Frame) == 32U);
static_assert(__builtin_offsetof(AddressSpaceMappingRecord, VirtualStart) == 48U);
static_assert(__builtin_offsetof(AddressSpaceMappingRecord, PhysicalStart) == 56U);
static_assert(__builtin_offsetof(AddressSpaceMappingRecord, Pages) == 64U);
static_assert(__builtin_offsetof(AddressSpaceMappingRecord, MemoryObjectOffsetPages) == 72U);
static_assert(__builtin_offsetof(AddressSpaceMappingRecord, Permissions) == 80U);
static_assert(__builtin_offsetof(AddressSpaceMappingRecord, CachePolicy) == 81U);
static_assert(__builtin_offsetof(AddressSpaceMappingRecord, Kind) == 82U);
static_assert(__builtin_offsetof(AddressSpaceMappingRecord, Commitment) == 83U);
static_assert(__builtin_offsetof(AddressSpaceMappingRecord, RuntimeMapping) == 84U);
static_assert(__builtin_offsetof(AddressSpaceMappingRecord, Reserved) == 85U);
} // namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_ADDRESS_SPACE_MAPPING_RECORD_HPP_INCLUDED
