// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Device/Amd64MmioWindow.hpp"

#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Amd64/Diagnostics/Amd64Stop.hpp"
#include "Platform/Amd64/Diagnostics/Amd64StopStage.hpp"

namespace Asteria::Platform::Amd64::Device
{
namespace
{
using Foundation::UInt64;
constexpr UInt64 AddressMask{0x000F'FFFF'FFFF'F000ULL};
UInt64* g_leaf{nullptr};
UInt64 g_root{0U};
UInt64 g_original{0U};
bool g_open{false};
bool g_active{false};
} // namespace

bool FindAmd64MmioWindowLeaf(const UInt64 root, const Foundation::Size arenaPages, UInt64*& output) noexcept
{
    if (root == 0U || (root & 4095U) != 0U || arenaPages < 4U || arenaPages > 64U || root >= (UInt64{1U} << 47U) ||
        root > (UInt64{1U} << 47U) - arenaPages * 4096U)
    {
        return false;
    }
    UInt64 address{root};
    constexpr Foundation::Size shifts[4U]{39U, 30U, 21U, 12U};
    for (const Foundation::Size shift : shifts)
    {
        if (address < root || address >= root + arenaPages * 4096U)
        {
            return false;
        }
        auto* const table = reinterpret_cast<UInt64*>(static_cast<Foundation::UIntPointer>(address));
        UInt64* const entry = &table[(MmioWindowVirtualAddress >> shift) & 511U];
        if ((*entry & 0x87U) != 3U)
        {
            return false;
        } // Present/RW, no U/S or large/PAT.
        if (shift == 12U)
        {
            if ((*entry & ((UInt64{1U} << 63U) | 0x118U)) != ((UInt64{1U} << 63U) | 0x18U))
            {
                return false;
            }
            output = entry;
            return true;
        }
        address = *entry & AddressMask;
    }
    return false;
}

bool PrepareAmd64MmioWindow(const UInt64 root) noexcept
{
    if (g_leaf != nullptr || !FindAmd64MmioWindowLeaf(root, 64U, g_leaf))
    {
        return false;
    }
    g_root = root;
    g_original = *g_leaf;
    g_open = true;
    return true;
}

bool CloseAmd64MmioWindow() noexcept
{
    if (!g_open || g_active || *g_leaf != g_original)
    {
        return false;
    }
    g_open = false;
    return true;
}

#if defined(ASTERIA_TARGET_AMD64)
[[noreturn]] void WindowFailure() noexcept
{
    Diagnostics::StopAmd64(Kernel::Diagnostics::StopCode::InvariantViolated,
                           static_cast<UInt64>(Diagnostics::Amd64BootStage::MmioWindowInvariant));
}

[[nodiscard]] UInt64 Access(const UInt64 physical, const Foundation::UInt32 value, const bool write) noexcept
{
    UInt64 root{0U};
    UInt64 flags{0U};
    UInt64 cpu{0U};
    __asm__ __volatile__("mov %%cr3,%0; pushfq; popq %1; movq %%gs:16,%2"
                         : "=r"(root), "=r"(flags), "=r"(cpu)
                         :
                         : "memory");
    if (!g_open || g_active || root != g_root || cpu != 0U || (flags & 0x200U) != 0U || (physical & 3U) != 0U ||
        physical > 0x000F'FFFF'FFFF'FFFCULL || *g_leaf != g_original)
    {
        WindowFailure();
    }
    g_active = true;
    auto* const leaf = static_cast<volatile UInt64*>(g_leaf);
    *leaf = (g_original & ~AddressMask) | (physical & AddressMask);
    __asm__ __volatile__("invlpg (%0); mfence" : : "r"(MmioWindowVirtualAddress) : "memory");
    auto* const mapped = reinterpret_cast<volatile Foundation::UInt32*>(
        static_cast<Foundation::UIntPointer>(MmioWindowVirtualAddress + (physical & 4095U)));
    Foundation::UInt32 read{0U};
    if (write)
    {
        *mapped = value;
    }
    else
    {
        read = *mapped;
    }
    __asm__ __volatile__("mfence" : : : "memory");
    *leaf = g_original;
    __asm__ __volatile__("invlpg (%0)" : : "r"(MmioWindowVirtualAddress) : "memory");
    g_active = false;
    return read;
}
#endif
} // namespace Asteria::Platform::Amd64::Device

#if defined(ASTERIA_TARGET_AMD64)
extern "C" Asteria::Foundation::UInt64
AsteriaAmd64ReadUncachedPhysicalDoubleWord(const Asteria::Foundation::UInt64 address) noexcept
{
    return Asteria::Platform::Amd64::Device::Access(address, 0U, false);
}
extern "C" void AsteriaAmd64WriteUncachedPhysicalDoubleWord(const Asteria::Foundation::UInt64 address,
                                                            const Asteria::Foundation::UInt32 value) noexcept
{
    static_cast<void>(Asteria::Platform::Amd64::Device::Access(address, value, true));
}
#endif
