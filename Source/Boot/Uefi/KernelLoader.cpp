// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/KernelLoader.hpp"

#include "Abi/ComponentBootstrap.hpp"
#include "Platform/Firmware/Diagnostic.hpp"
#if defined(ASTERIA_TARGET_IA64)
#include "Platform/Ia64/Firmware/Ia64ImageRelocation.hpp"
#endif

namespace Asteria::Boot::Uefi
{
namespace
{
constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 PageBytes{4096U};
struct Allocation
{
    Foundation::UInt64 Start;
    Foundation::UInt64 Pages;
};
constinit KernelElfPlan g_plan{};
constinit BootImagePlacement g_image{};
constinit Allocation g_imageAllocation{};
constinit Allocation g_allocations[KernelElfReservationCount]{};
constinit BootReservation g_reservations[4U]{};
constinit Allocation g_arenaAllocation{};
constinit BootReservation g_arena{};
// The metadata arena's alignment: 2 MiB.
constexpr Foundation::UInt64 ArenaAlignmentPages{512U};

constinit KernelPrepareFunction g_prepare{nullptr};
constinit KernelEnterFunction g_enter{nullptr};
constinit KernelQueryFunction g_query{nullptr};

[[nodiscard]] bool AllocateAligned(BootServices& services, const Foundation::UInt64 pages,
                                   const Foundation::UInt64 alignment, const Foundation::UInt64 ceiling,
                                   const MemoryType type, Allocation& result) noexcept
{
    if (pages == 0U || pages > 65536U || alignment == 0U || alignment > 65536U || (alignment & (alignment - 1U)) != 0U)
    {
        return false;
    }
    const Foundation::UInt64 total = pages + alignment - 1U;
    Foundation::UInt64 allocation{ceiling};
    if (services.AllocatePagesFunction(AllocateType::MaximumAddress, type, static_cast<Foundation::Size>(total),
                                       &allocation) != Success)
    {
        return false;
    }
    if (allocation == 0U || (allocation & 0xFFFU) != 0U || allocation > Maximum - total * PageBytes ||
        allocation + total * PageBytes - 1U > ceiling)
    {
        static_cast<void>(services.FreePagesFunction(allocation, static_cast<Foundation::Size>(total)));
        return false;
    }
    const Foundation::UInt64 mask = alignment * PageBytes - 1U;
    const Foundation::UInt64 placed = (allocation + mask) & ~mask;
    const Foundation::UInt64 prefix = (placed - allocation) / PageBytes;
    const Foundation::UInt64 suffix = total - prefix - pages;
    if (suffix != 0U &&
        services.FreePagesFunction(placed + pages * PageBytes, static_cast<Foundation::Size>(suffix)) != Success)
    {
        static_cast<void>(services.FreePagesFunction(allocation, static_cast<Foundation::Size>(total)));
        return false;
    }
    if (prefix != 0U && services.FreePagesFunction(allocation, static_cast<Foundation::Size>(prefix)) != Success)
    {
        static_cast<void>(services.FreePagesFunction(allocation, static_cast<Foundation::Size>(prefix + pages)));
        return false;
    }
    result = {placed, pages};
    return true;
}

[[nodiscard]] bool AllocateReservations(BootServices& services) noexcept
{
    for (Foundation::Size index{0U}; index < KernelElfReservationCount; ++index)
    {
        const auto& declaration = g_plan.Reservations[index];
        if (declaration.PageCount == 0U || (declaration.Flags & 1U) != 0U)
        {
            continue;
        }
        const bool guards = (declaration.Flags & 2U) != 0U;
        const Foundation::UInt64 pages = guards ? 2U * declaration.PageCount + 1U : declaration.PageCount;
        if (!AllocateAligned(services, pages, declaration.AlignmentPages, declaration.MaximumAddress,
                             MemoryType::LoaderData, g_allocations[index]))
        {
            return false;
        }
        const Foundation::UInt64 body = g_allocations[index].Start + (guards ? declaration.PageCount * PageBytes : 0U);
        if (body == 0U || (declaration.Kind == 2U && body < PageBytes))
        {
            return false;
        }
        g_reservations[declaration.Kind] = {body, declaration.PageCount, 0U, 2U, 0U};
    }
    for (const auto& declaration : g_plan.Reservations)
    {
        if ((declaration.Flags & 1U) != 0U)
        {
            g_reservations[declaration.Kind] = g_reservations[3U];
            g_reservations[declaration.Kind].PageCount = declaration.PageCount;
        }
    }
    return true;
}

// The arena the kernel sizes for spanGiB GiB, below every fixed user range,
// since every root maps it at its identity address. A kernel that asks for
// none (IA-64) leaves the receipt zero.
[[nodiscard]] bool AllocateMetadataArena(BootServices& services, const Foundation::UInt64 spanGiB) noexcept
{
    const Foundation::UInt64 pages =
        (g_query == nullptr)
            ? 0U
            : g_query(static_cast<Foundation::UInt32>(KernelQuery::MetadataArenaPages),
                      static_cast<Foundation::UInt32>((spanGiB < 0xFFFF'FFFFU) ? spanGiB : 0xFFFF'FFFFU));
    if (pages == 0U)
    {
        return true;
    }
    if (((pages % ArenaAlignmentPages) != 0U) ||
        !AllocateAligned(services, pages, ArenaAlignmentPages, Abi::Amd64FixedUserRangesStart - 1U,
                         MemoryType::LoaderData, g_arenaAllocation))
    {
        return false;
    }
    g_arena = {g_arenaAllocation.Start, pages, 0U, 2U, 0U};
    return true;
}

void PublishEntries() noexcept
{
    // The target ABI interprets an IA-64 function pointer as the identity
    // address of a validated, relocated two-word descriptor; AMD64 uses code.
    // Storage and all entry targets remain alive until the no-return handoff.
    g_prepare = reinterpret_cast<KernelPrepareFunction>(
        static_cast<Foundation::UIntPointer>(g_image.PhysicalStart + g_plan.PrepareRva));
    g_enter = reinterpret_cast<KernelEnterFunction>(
        static_cast<Foundation::UIntPointer>(g_image.PhysicalStart + g_plan.EnterRva));
    g_query = reinterpret_cast<KernelQueryFunction>(
        static_cast<Foundation::UIntPointer>(g_image.PhysicalStart + g_plan.QueryRva));
}
} // Namespace

KernelElfError LoadKernelImage(BootServices& services, const Executable::Elf::Elf64ByteView source) noexcept
{
    if (services.AllocatePagesFunction == nullptr || services.FreePagesFunction == nullptr || g_query != nullptr)
    {
        return KernelElfError::InvalidDestination;
    }
#if defined(ASTERIA_TARGET_AMD64)
    constexpr Foundation::UInt32 Architecture{BootArchitectureAmd64};
#else
    constexpr Foundation::UInt32 Architecture{BootArchitectureIa64};
#endif
    const KernelElfError parsed = KernelElf::Parse(source, Architecture, g_plan);
    if (parsed != KernelElfError::None)
    {
        g_plan.Source = {};
        return parsed;
    }
    if (!AllocateAligned(services, g_plan.Image.SizeBytes / PageBytes, g_plan.Image.AlignmentBytes / PageBytes,
                         0xFFFFFFFFU, MemoryType::LoaderCode, g_imageAllocation))
    {
        g_plan.Source = {};
        return KernelElfError::InvalidDestination;
    }
    const auto placed =
        KernelElf::Place(g_plan, reinterpret_cast<void*>(static_cast<Foundation::UIntPointer>(g_imageAllocation.Start)),
                         static_cast<Foundation::Size>(g_plan.Image.SizeBytes));
    if (placed != KernelElfError::None)
    {
        g_plan.Source = {};
        static_cast<void>(services.FreePagesFunction(g_imageAllocation.Start,
                                                     static_cast<Foundation::Size>(g_imageAllocation.Pages)));
        g_imageAllocation = {};
        return placed;
    }
    g_image = g_plan.Image;
    const Foundation::UInt64 delta = g_imageAllocation.Start - g_image.PhysicalStart;
    g_image.PhysicalStart = g_imageAllocation.Start;
    for (Foundation::Size index{0U}; index < g_image.SectionCount; ++index)
    {
        g_image.Sections[index].PhysicalStart += delta;
    }
#if defined(ASTERIA_TARGET_IA64)
    Platform::Ia64::Firmware::MakeIa64ImageInstructionCoherent(
        reinterpret_cast<void*>(static_cast<Foundation::UIntPointer>(g_imageAllocation.Start)),
        static_cast<Foundation::Size>(g_image.SizeBytes));
#endif
    PublishEntries();
    g_plan.Source = {};
    return KernelElfError::None;
}

bool ReserveLoadedKernelMemory(BootServices& services, const Foundation::UInt64 spanGiB) noexcept
{
    if (g_prepare == nullptr || services.AllocatePagesFunction == nullptr || services.FreePagesFunction == nullptr)
    {
        return false;
    }
    if (AllocateReservations(services) && AllocateMetadataArena(services, spanGiB))
    {
        return true;
    }
    static_cast<void>(ReleaseKernelBootReservations(services));
    return false;
}

const BootImagePlacement& GetPlacedKernelImage() noexcept
{
    return g_image;
}

BootReservation GetKernelBootReservation(const Foundation::UInt32 kind) noexcept
{
    return kind < 4U ? g_reservations[kind] : BootReservation{};
}

BootReservation GetKernelMetadataArena() noexcept
{
    return g_arena;
}

bool ReleaseKernelBootReservations(BootServices& services) noexcept
{
    if (services.FreePagesFunction == nullptr)
    {
        return false;
    }
    bool success{true};
    if (g_arenaAllocation.Pages != 0U)
    {
        if (services.FreePagesFunction(g_arenaAllocation.Start,
                                       static_cast<Foundation::Size>(g_arenaAllocation.Pages)) != Success)
        {
            success = false;
        }
        else
        {
            g_arenaAllocation = {};
            g_arena = {};
        }
    }
    for (auto& allocation : g_allocations)
    {
        if (allocation.Pages != 0U)
        {
            if (services.FreePagesFunction(allocation.Start, static_cast<Foundation::Size>(allocation.Pages)) !=
                Success)
            {
                success = false;
            }
            else
            {
                allocation = {};
            }
        }
    }
    return success;
}

Foundation::UInt64 ReadKernelQuery(const KernelQuery query, const Foundation::UInt32 index) noexcept
{
    return g_query == nullptr ? 0U : g_query(static_cast<Foundation::UInt32>(query), index);
}

const char* ReadKernelDiagnosticString(const KernelQuery query) noexcept
{
    const Foundation::UInt64 address = ReadKernelQuery(query);
    if (address == 0U)
    {
        return nullptr;
    }
    for (Foundation::Size index{0U}; index < g_image.SectionCount; ++index)
    {
        const auto& section = g_image.Sections[index];
        if (section.Purpose != 1U || address < section.PhysicalStart ||
            address - section.PhysicalStart >= section.PageCount * PageBytes)
        {
            continue;
        }
        const Foundation::UInt64 remaining = section.PageCount * PageBytes - (address - section.PhysicalStart);
        const auto* const text = reinterpret_cast<const char*>(static_cast<Foundation::UIntPointer>(address));
        for (Foundation::UInt64 character{0U}; character < remaining && character < 512U; ++character)
        {
            if (text[character] == '\0')
            {
                return text;
            }
        }
    }
    return nullptr;
}

bool PrepareLoadedKernel(BootInfo& info) noexcept
{
    return g_prepare != nullptr && g_prepare(&info) == 1U;
}

[[noreturn]] void EnterLoadedKernel(const BootInfo& info) noexcept
{
    if (g_enter != nullptr)
    {
        g_enter(&info);
    }
    Platform::Firmware::HaltAfterFirmwareExit();
}
} // Namespace Asteria::Boot::Uefi
