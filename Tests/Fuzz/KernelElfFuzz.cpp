// SPDX-License-Identifier: Apache-2.0
#include "Boot/KernelElf.hpp"

#include "Support/KernelElfImage.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
constinit Asteria::TestSupport::KernelElfImage g_source{};
constinit Asteria::TestSupport::KernelElfImage g_before{};
constinit Asteria::Boot::KernelElfPlan g_plan{};
constinit Asteria::Boot::KernelElfPlan g_previous{};
alignas(0x1000000U) constinit std::uint8_t g_destination[4096U]{};
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) noexcept
{
    const Asteria::Foundation::UInt32 architecture = size != 0U && (data[0U] & 1U) != 0U ? 2U : 1U;
    g_source.Build(architecture);
    // Compact mutations reach header, tables, note, symbols, descriptors,
    // instruction fields and relocations while preserving surrounding structure.
    for (std::size_t index{1U}; index + 2U < size && index < 4096U; index += 3U)
    {
        const std::size_t offset =
            (static_cast<std::size_t>(data[index]) | (static_cast<std::size_t>(data[index + 1U]) << 8U)) %
            sizeof(g_source.Data);
        g_source.Data[offset] ^= data[index + 2U];
    }
    auto source = g_source.View();
    if (size > 1U && (data[0U] & 2U) != 0U)
    {
        source.SizeBytes = static_cast<std::size_t>(data[1U]) * 128U;
    }
    g_before = g_source;
    g_previous = g_plan;
    const auto result = Asteria::Boot::KernelElf::Parse(source, architecture, g_plan);
    if (std::memcmp(&g_source, &g_before, sizeof(g_source)) != 0 ||
        (result != Asteria::Boot::KernelElfError::None && std::memcmp(&g_previous, &g_plan, sizeof(g_plan)) != 0))
    {
        std::abort();
    }
    if (result == Asteria::Boot::KernelElfError::None)
    {
        g_previous = g_plan;
        if (Asteria::Boot::KernelElf::Parse(source, architecture, g_plan) != result ||
            std::memcmp(&g_previous, &g_plan, sizeof(g_plan)) != 0)
        {
            std::abort();
        }
        // A valid destination address with an insufficient extent must fail
        // before touching even one byte. Successful full-image placement and
        // independent relocation expectations live in KernelElfTests.
        std::memset(g_destination, 0xA5, sizeof(g_destination));
        if (Asteria::Boot::KernelElf::Place(g_plan, g_destination, sizeof(g_destination)) !=
            Asteria::Boot::KernelElfError::InvalidDestination)
        {
            std::abort();
        }
        for (const auto byte : g_destination)
        {
            if (byte != 0xA5U)
            {
                std::abort();
            }
        }
    }
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    constexpr std::uint8_t Seeds[][4U]{
        {0U, 0U, 0U, 0U},          {1U, 0U, 0U, 0U},          {2U, 0U, 0U, 0U},          {3U, 0x40U, 0U, 0U},
        {0U, 0x18U, 0x71U, 0xFFU}, {1U, 0x08U, 0x40U, 0x80U}, {1U, 0x08U, 0x72U, 0x80U}, {1U, 0x1CU, 0x71U, 0xFFU},
    };
    emit(Seeds[0U], 0U);
    for (const auto& seed : Seeds)
    {
        emit(seed, sizeof(seed));
    }
}
