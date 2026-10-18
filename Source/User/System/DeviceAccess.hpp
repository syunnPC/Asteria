// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SYSTEM_DEVICE_ACCESS_HPP_INCLUDED
#define ASTERIA_USER_SYSTEM_DEVICE_ACCESS_HPP_INCLUDED

#include "Abi/SystemCallAbi.hpp"
#include "Foundation/Types.hpp"
#include "User/System/ObjectRequests.hpp"
#include "Platform/Synchronization/DeviceMemoryBarrier.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::System
{
/// MemoryMap and MemoryObject permissions: read, write, user.
inline constexpr Foundation::UInt8 ReadWriteUser{0x0BU};

/// A device register's value, read whole. GCC narrows a volatile read whose
/// value is only tested (a one-byte testb on AMD64), even an atomic one, and
/// a device that takes accesses of four or eight bytes, as edu does, makes
/// QEMU read a narrower one as zero: a status bit then reads clear. The value
/// is passed through an empty asm that needs it whole in a register, so the
/// load cannot fold into the test.
[[nodiscard]] inline Foundation::UInt32 ReadRegister(const volatile Foundation::UInt32* const registers,
                                                     const Foundation::Size word) noexcept
{
    Foundation::UInt32 value = registers[word];
    __asm__ __volatile__("" : "+r"(value));
    return value;
}

/// A full platform fence for coherent DMA storage: the caller's ordinary loads and stores
/// are not moved across it, so a command it built in memory is complete
/// before the register write that issues it, and data the device wrote is
/// read only after the register read that reported it. AMD64 keeps its
/// stores, and its loads, in order with uncached accesses itself; IA-64
/// additionally requires mf, supplied by the common platform adapter.
inline void OrderDmaStorage() noexcept
{
    Platform::Synchronization::DeviceMemoryBarrier();
}

/// Complete scanout writes before a Query reply or after rendering.
inline void OrderScanoutStorage() noexcept
{
    Platform::Synchronization::OrderScanoutStorage();
}

/// A Frame, or a readable and writable MemoryObject of cache policy, of
/// pages charged to the domain that domain (Manage) names; a Frame at a
/// physical address that is a multiple of alignmentPages pages (zero for any).
[[nodiscard, gnu::noinline]] inline Abi::SystemCallResponse
CreateRamFrame(const SystemCallClient& client, const CapabilityHandle domain, const Foundation::UInt64 pages,
               const Foundation::UInt8 alignmentLog2 = 0U, const Foundation::UInt8 limitBits = 32U) noexcept
{
    // Physical memory management; native session buffers use limit 32.
    alignas(8) Foundation::UInt8 request[Abi::ObjectCreateRamFrameRequestSizeBytes]{};
    StoreWord(request + Abi::ObjectCreatePagesOffset, pages);
    request[Abi::ObjectCreateFrameAlignmentOffset] = alignmentLog2;
    request[Abi::ObjectCreateRamFrameLimitOffset] = limitBits;
    return client.CreateObject(domain, Abi::ObjectTypeFrame, AddressOf(request), sizeof(request));
}
[[nodiscard, gnu::noinline]] inline Abi::SystemCallResponse
CreateMemory(const SystemCallClient& client, const CapabilityHandle domain, const Foundation::UInt64 type,
             const Foundation::UInt64 pages, const Foundation::UInt8 cache = Abi::ObjectCreateCacheWriteBack,
             const Foundation::UInt8 alignmentPages = 0U) noexcept
{
    static_assert(Abi::ObjectCreateFrameRequestSizeBytes == Abi::ObjectCreateMemoryObjectRequestSizeBytes);
    alignas(8) Foundation::UInt8 request[Abi::ObjectCreateMemoryObjectRequestSizeBytes]{};
    StoreWord(request + Abi::ObjectCreatePagesOffset, pages);
    if (type == Abi::ObjectTypeMemoryObject)
    {
        request[Abi::ObjectCreatePermissionsOffset] = ReadWriteUser;
        request[Abi::ObjectCreateCachePolicyOffset] = cache;
    }
    else
    {
        request[Abi::ObjectCreateFrameAlignmentOffset] = alignmentPages;
    }
    return client.CreateObject(domain, type, AddressOf(request), sizeof(request));
}

/// A device Frame of the first pages of region (Map), charged to domain.
[[nodiscard, gnu::noinline]] inline Abi::SystemCallResponse
CreateDeviceFrame(const SystemCallClient& client, const CapabilityHandle domain, const CapabilityHandle region,
                  const Foundation::UInt64 pages = 1U) noexcept
{
    alignas(8) Foundation::UInt8 request[Abi::ObjectCreateDeviceFrameRequestSizeBytes]{};
    StoreWord(request + Abi::ObjectCreatePagesOffset, pages);
    StoreHandle(request + Abi::ObjectCreateDeviceFrameRegionOffset, region);
    return client.CreateObject(domain, Abi::ObjectTypeFrame, AddressOf(request), sizeof(request));
}

/// Maps pages of memory over frame at start of space, readable and writable,
/// from page offset of both.
[[nodiscard, gnu::noinline]] inline Abi::SystemCallError
MapWindow(const SystemCallClient& client, const CapabilityHandle space, const CapabilityHandle memory,
          const CapabilityHandle frame, const Foundation::UInt64 start, const Foundation::UInt64 pages,
          const Foundation::UInt64 offset = 0U) noexcept
{
    alignas(8) Foundation::UInt8 request[Abi::FaultResolveRequestSizeBytes]{};
    StoreHandle(request + Abi::FaultResolveAddressSpaceOffset, space);
    StoreHandle(request + Abi::FaultResolveMemoryObjectOffset, memory);
    StoreHandle(request + Abi::FaultResolveFrameOffset, frame);
    StoreWord(request + Abi::FaultResolveVirtualStartOffset, start);
    StoreWord(request + Abi::FaultResolvePageCountOffset, pages);
    StoreWord(request + Abi::FaultResolveMemoryObjectPageOffset, offset);
    StoreWord(request + Abi::FaultResolveFramePageOffset, offset);
    request[Abi::FaultResolvePermissionsOffset] = ReadWriteUser;
    return client.MapMemory(AddressOf(request)).Error;
}
} // Namespace Asteria::User::System

#endif // ASTERIA_USER_SYSTEM_DEVICE_ACCESS_HPP_INCLUDED
