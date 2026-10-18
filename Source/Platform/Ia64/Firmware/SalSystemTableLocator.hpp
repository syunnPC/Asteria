// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_FIRMWARE_SAL_SYSTEM_TABLE_LOCATOR_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_FIRMWARE_SAL_SYSTEM_TABLE_LOCATOR_HPP_INCLUDED

#include "Boot/Uefi/UefiTypes.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
constexpr Foundation::Size MaximumFirmwareConfigurationTables{4096U};
constexpr Boot::Uefi::Guid SalSystemTableGuid{
    .Data1 = 0xEB9D2D32U,
    .Data2 = 0x2D88U,
    .Data3 = 0x11D3U,
    .Data4 = {0x9AU, 0x16U, 0x00U, 0x90U, 0x27U, 0x3FU, 0xC1U, 0x4DU},
};

enum class SalSystemTableLocateError : Foundation::UInt32
{
    None,
    InvalidConfigurationTableArray,
    SalSystemTableNotFound,
    DuplicateSalSystemTable,
    NullSalSystemTable,
    InvalidMemoryMap,
    TableAddressNotInMemoryMap,
    AmbiguousMemoryDescriptor,
    HeaderOutsideMemoryDescriptor,
    UnsupportedMemoryType,
    MissingRuntimeAttribute,
};

struct SalSystemTableLocation
{
    const Foundation::UInt8* Bytes;
    Foundation::Size AvailableBytes;
    Foundation::UInt64 PhysicalAddress;
    Foundation::UInt64 MemoryAttributes;
    Foundation::UInt32 FirmwareMemoryType;
    Foundation::UInt32 Reserved;
};

/// Locates the unique physical SAL configuration-table address before
/// SetVirtualAddressMap, while UEFI still provides the required identity mapping,
/// and binds its header to one non-overlapping IA-64-permitted memory descriptor.
/// It does not read the table. Both input arrays must remain stable for the call;
/// the returned bytes must be copied or parsed while their boot-time mapping and
/// contents remain stable.
class SalSystemTableLocator final
{
public:
    [[nodiscard]] static Foundation::Result<SalSystemTableLocation, SalSystemTableLocateError>
    Locate(const Boot::Uefi::ConfigurationTable* configurationTables, Foundation::Size configurationTableCount,
           const void* memoryMap, Foundation::Size memoryMapSize, Foundation::Size memoryDescriptorSize,
           Foundation::UInt32 memoryDescriptorVersion) noexcept;

private:
    SalSystemTableLocator() = delete;
};

static_assert(sizeof(SalSystemTableLocation) == 40U);
static_assert(alignof(SalSystemTableLocation) == 8U);
static_assert(sizeof(SalSystemTableLocateError) == 4U);
} // namespace Asteria::Platform::Ia64::Firmware

#endif // ASTERIA_PLATFORM_IA64_FIRMWARE_SAL_SYSTEM_TABLE_LOCATOR_HPP_INCLUDED
