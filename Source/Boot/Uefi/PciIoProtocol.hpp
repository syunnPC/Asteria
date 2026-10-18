// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_PCI_IO_PROTOCOL_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_PCI_IO_PROTOCOL_HPP_INCLUDED

#include "Boot/Uefi/UefiTypes.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Boot::Uefi
{
inline constexpr Guid PciIoProtocolGuid{
    .Data1 = 0x4CF5B200U,
    .Data2 = 0x68B8U,
    .Data3 = 0x4CA5U,
    .Data4 = {0x9EU, 0xECU, 0xB2U, 0x3EU, 0x3FU, 0x50U, 0x02U, 0x9AU},
};

enum class PciIoWidth : Foundation::UInt32
{
    Uint8,
    Uint16,
    Uint32,
    Uint64,
    FifoUint8,
    FifoUint16,
    FifoUint32,
    FifoUint64,
    FillUint8,
    FillUint16,
    FillUint32,
    FillUint64,
    Maximum,
};

struct PciIoProtocol;

using PciIoConfigurationAccess = Status(ASTERIA_UEFI_API*)(PciIoProtocol*, PciIoWidth, Foundation::UInt32,
                                                           Foundation::Size, void*);
using PciIoGetLocation = Status(ASTERIA_UEFI_API*)(PciIoProtocol*, Foundation::Size*, Foundation::Size*,
                                                   Foundation::Size*, Foundation::Size*);
using PciIoGetBarAttributes = Status(ASTERIA_UEFI_API*)(PciIoProtocol*, Foundation::UInt8, Foundation::UInt64*, void**);

struct PciIoAccess
{
    void* Read;
    void* Write;
};

struct PciIoConfiguration
{
    PciIoConfigurationAccess Read;
    void* Write;
};

struct PciIoProtocol
{
    void* PollMemory;
    void* PollIo;
    PciIoAccess Memory;
    PciIoAccess Io;
    PciIoConfiguration Pci;
    void* CopyMemory;
    void* Map;
    void* Unmap;
    void* AllocateBuffer;
    void* FreeBuffer;
    void* Flush;
    PciIoGetLocation GetLocation;
    void* Attributes;
    PciIoGetBarAttributes GetBarAttributes;
    void* SetBarAttributes;
    Foundation::UInt64 RomSize;
    void* RomImage;
};

static_assert(sizeof(PciIoWidth) == 4U);
static_assert(sizeof(PciIoAccess) == 16U);
static_assert(alignof(PciIoAccess) == 8U);
static_assert(sizeof(PciIoConfiguration) == 16U);
static_assert(alignof(PciIoConfiguration) == 8U);
static_assert(sizeof(PciIoProtocol) == 160U);
static_assert(alignof(PciIoProtocol) == 8U);
static_assert(__builtin_offsetof(PciIoProtocol, Pci) == 48U);
static_assert(__builtin_offsetof(PciIoProtocol, GetLocation) == 112U);
static_assert(__builtin_offsetof(PciIoProtocol, GetBarAttributes) == 128U);
static_assert(__builtin_offsetof(PciIoProtocol, RomSize) == 144U);
static_assert(__builtin_offsetof(PciIoProtocol, RomImage) == 152U);
} // namespace Asteria::Boot::Uefi

#endif // ASTERIA_BOOT_UEFI_PCI_IO_PROTOCOL_HPP_INCLUDED
