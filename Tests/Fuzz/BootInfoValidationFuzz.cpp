// SPDX-License-Identifier: Apache-2.0
#include "Boot/BootInfoValidation.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
constinit Asteria::Boot::BootInfo g_seed{};
constinit Asteria::Boot::BootInfo g_input{};
constinit Asteria::Boot::BootInfo g_before{};

void ResetSeed() noexcept
{
    g_seed = {};
    g_seed.Signature = Asteria::Boot::BootInfoSignature;
    g_seed.Version = Asteria::Boot::BootInfoVersion;
    g_seed.SizeBytes = sizeof(g_seed);
    g_seed.MemoryRegionCount = 1U;
    g_seed.MemoryRegions[0U] = {0x10'0000U, 0U, 4U, 8U, 2U, Asteria::Boot::PhysicalMemoryKind::Loader};
    g_seed.Files.PhysicalStart = 0x10'0000U;
    g_seed.Files.PageCount = 4U;
    g_seed.Files.ManifestBytes = 32U;
    g_seed.Files.Files[Asteria::Boot::BootInitFile] = {4096U, 16U, 0U, {}, {}};
    g_seed.Files.Files[Asteria::Boot::BootUserFile] = {4112U, 16U, 0U, {}, {}};
    Asteria::Boot::SealBootInfo(g_seed);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) noexcept
{
    if (g_seed.Signature == 0U)
    {
        ResetSeed();
    }
    // The compact corpus replaces the untrusted prefix and one selected body
    // slice. Addresses remain inert; file-receipt validation must never follow
    // them, even when a malformed record is resealed with a correct checksum.
    g_input = g_seed;
    const std::size_t prefix = size < 32U ? size : 32U;
    std::memcpy(&g_input, data, prefix);
    if (size > 34U)
    {
        constexpr std::size_t Offsets[]{40U, __builtin_offsetof(Asteria::Boot::BootInfo, Files),
                                        __builtin_offsetof(Asteria::Boot::BootInfo, Kernel),
                                        __builtin_offsetof(Asteria::Boot::BootInfo, InventoryRecord)};
        const std::size_t offset = Offsets[data[32U] & 3U];
        const std::size_t body = size - 34U < sizeof(g_input) - offset ? size - 34U : sizeof(g_input) - offset;
        auto* const bytes = reinterpret_cast<std::uint8_t*>(&g_input);
        std::memcpy(bytes + offset, data + 34U, body);
    }
    if (size > 33U && (data[33U] & 1U) != 0U)
    {
        Asteria::Boot::SealBootInfo(g_input);
    }
    g_before = g_input;
    const bool accepted = Asteria::Boot::ValidateBootInfo(g_input);
    static_cast<void>(Asteria::Boot::ValidateBootFileRegion(g_input));
    if (std::memcmp(&g_before, &g_input, sizeof(g_input)) != 0 ||
        (accepted &&
         (g_input.Signature != Asteria::Boot::BootInfoSignature || g_input.Version != Asteria::Boot::BootInfoVersion ||
          g_input.SizeBytes != sizeof(g_input) || g_input.MemoryRegionCount == 0U ||
          g_input.MemoryRegionCount > Asteria::Boot::MaximumMemoryRegions)))
    {
        std::abort();
    }
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    ResetSeed();
    emit(reinterpret_cast<const std::uint8_t*>(&g_seed), 32U);
    emit(reinterpret_cast<const std::uint8_t*>(&g_seed), 16U);
    emit(reinterpret_cast<const std::uint8_t*>(&g_seed), 0U);
}
