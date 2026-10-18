// SPDX-License-Identifier: Apache-2.0
#include "Boot/Uefi/FatReportVolume.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
namespace Uefi = Asteria::Boot::Uefi;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
struct Media
{
    UInt32 Id;
    Uefi::Boolean Removable;
    Uefi::Boolean Present;
    Uefi::Boolean Partition;
    Uefi::Boolean ReadOnly;
    Uefi::Boolean Caching;
    UInt32 SectorBytes;
    UInt32 Alignment;
    UInt64 Last;
};
struct Block;
using Read = Uefi::Status(ASTERIA_UEFI_API*)(Block*, UInt32, UInt64, Size, void*);
using Write = Uefi::Status(ASTERIA_UEFI_API*)(Block*, UInt32, UInt64, Size, const void*);
using Flush = Uefi::Status(ASTERIA_UEFI_API*)(Block*);
struct Block
{
    UInt64 Revision;
    Media* Medium;
    void* Reset;
    Read ReadFunction;
    Write WriteFunction;
    Flush FlushFunction;
};
constinit Media g_media{1U, 0U, 1U, 1U, 0U, 0U, 512U, 4096U, 131071U};
constinit Block g_block{};
constinit Uefi::BootServices g_services{};
const UInt8* g_input{nullptr};
Size g_length{0U};
Size g_pages{0U};
void Require(const bool good) noexcept
{
    if (!good)
    {
        std::abort();
    }
}
Uefi::Status ASTERIA_UEFI_API Allocate(Uefi::AllocateType, Uefi::MemoryType, Size count, UInt64* address)
{
    *address = reinterpret_cast<UInt64>(std::aligned_alloc(4096U, count * 4096U));
    if (*address == 0U)
    {
        return Uefi::NotFound;
    }
    g_pages += count;
    return Uefi::Success;
}
Uefi::Status ASTERIA_UEFI_API Free(UInt64 address, Size count)
{
    std::free(reinterpret_cast<void*>(address));
    Require(count <= g_pages);
    g_pages -= count;
    return Uefi::Success;
}
Uefi::Status ASTERIA_UEFI_API Protocol(Uefi::Handle, Uefi::Guid*, void** interface)
{
    *interface = &g_block;
    return Uefi::Success;
}
Uefi::Status ASTERIA_UEFI_API ReadSector(Block*, UInt32 id, UInt64 sector, Size bytes, void* output)
{
    Require(id == 1U && sector <= g_media.Last && bytes == 512U);
    Require(reinterpret_cast<UInt64>(output) % 4096U == 0U);
    // Sparse synthetic volume: both FAT copies share input, then four
    // directory sectors. Every other sector is zero, without large storage.
    Size slot{6U};
    if (sector == 0U)
    {
        slot = 0U;
    }
    else if (sector == 32U || sector == 1056U)
    {
        slot = 1U;
    }
    else if (sector >= 2080U && sector < 2084U)
    {
        slot = static_cast<Size>(sector - 2080U) + 2U;
    }
    auto* const destination{static_cast<UInt8*>(output)};
    for (Size i{0U}; i < bytes; ++i)
    {
        const Size offset{slot * 512U + i};
        destination[i] = slot < 6U && offset < g_length ? g_input[offset] : 0U;
    }
    return Uefi::Success;
}
Uefi::Status ASTERIA_UEFI_API WriteSector(Block*, UInt32, UInt64, Size, const void*)
{
    // Mount and marker lookup must never mutate even malformed media.
    std::abort();
}
Uefi::Status ASTERIA_UEFI_API FlushDisk(Block*)
{
    std::abort();
}
UInt32 Probe(Uefi::BootServices& services)
{
    Uefi::FatReportVolume owner{services};
    Uefi::IAcpiReportVolume* const volume{owner.Open(nullptr)};
    const UInt32 result{volume == nullptr ? 0U : 1U + static_cast<UInt32>(volume->Exists("NOAML"))};
    Require(owner.Release() && g_pages == 0U);
    return result;
}
constinit UInt8 g_seed[3072U]{};
void Put(Size offset, UInt32 value, Size count)
{
    for (Size i{0U}; i < count; ++i)
    {
        g_seed[offset + i] = static_cast<UInt8>(value & 255U);
        value >>= 8U;
    }
}
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) noexcept
{
    if (size > sizeof(g_seed))
    {
        return 0;
    }
    g_input = data;
    g_length = size;
    g_block = {0U, &g_media, nullptr, ReadSector, WriteSector, FlushDisk};
    g_services.AllocatePagesFunction = Allocate;
    g_services.FreePagesFunction = Free;
    g_services.HandleProtocolFunction = Protocol;
    const UInt32 result{Probe(g_services)};
    Require(Probe(g_services) == result);
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*emit)(const std::uint8_t*, std::size_t)) noexcept
{
    emit(g_seed, 0U);
    emit(g_seed, sizeof(g_seed));
    Put(11U, 512U, 2U);
    Put(13U, 1U, 1U);
    Put(14U, 32U, 2U);
    Put(16U, 2U, 1U);
    Put(32U, 131072U, 4U);
    Put(36U, 1024U, 4U);
    Put(44U, 2U, 4U);
    Put(510U, 0xAA55U, 2U);
    for (Size i{0U}; i < 6U; ++i)
    {
        Put(512U + i * 4U, 0x0FFF'FFFFU, 4U);
    }
    constexpr const char* names[]{"EFI        ", "ASTERIA    ", "ACPI       "};
    for (Size i{0U}; i < 3U; ++i)
    {
        const Size offset{(i + 2U) * 512U};
        std::memcpy(g_seed + offset, names[i], 11U);
        Put(offset + 11U, 0x10U, 1U);
        Put(offset + 26U, static_cast<UInt32>(i + 3U), 2U);
    }
    emit(g_seed, sizeof(g_seed));
    Put(512U + 4U, 0U, 4U);
    emit(g_seed, sizeof(g_seed));
}
