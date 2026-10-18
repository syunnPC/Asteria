// SPDX-License-Identifier: Apache-2.0
#include "Boot/KernelElf.hpp"

#include "Support/KernelElfImage.hpp"
#include "Support/TestContext.hpp"

namespace
{
using Asteria::Boot::KernelElf;
using Asteria::Boot::KernelElfError;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::UInt8;
constinit Asteria::TestSupport::KernelElfImage g_source{};
constinit Asteria::Boot::KernelElfPlan g_plan{};
alignas(0x1000000U) constinit UInt8 g_destination[0x1000000U]{};

[[nodiscard]] UInt64 Read(const Size offset) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(g_destination[offset + index]) << (8U * index);
    }
    return value;
}

constexpr Size ImmediateBits[]{100U, 101U, 102U, 103U, 104U, 105U, 106U, 114U, 115U, 116U, 117U, 118U, 119U,
                               120U, 121U, 122U, 109U, 110U, 111U, 112U, 113U, 108U, 46U,  47U,  48U,  49U,
                               50U,  51U,  52U,  53U,  54U,  55U,  56U,  57U,  58U,  59U,  60U,  61U,  62U,
                               63U,  64U,  65U,  66U,  67U,  68U,  69U,  70U,  71U,  72U,  73U,  74U,  75U,
                               76U,  77U,  78U,  79U,  80U,  81U,  82U,  83U,  84U,  85U,  86U,  123U};

[[nodiscard]] UInt64 Immediate(const Size offset) noexcept
{
    UInt64 value{0U};
    // psABI Figure 4-2 bit positions, read independently one bit at a time.
    for (Size bit{0U}; bit < 64U; ++bit)
    {
        const Size field = ImmediateBits[bit];
        value |= static_cast<UInt64>((static_cast<UInt64>(g_destination[offset + field / 8U]) >> (field % 8U)) & 1U)
                 << bit;
    }
    return value;
}

[[nodiscard]] bool UnrelatedInstructionBits() noexcept
{
    for (Size bit{0U}; bit < 128U; ++bit)
    {
        const bool immediate =
            (bit >= 46U && bit <= 86U) || (bit >= 100U && bit <= 106U) || (bit >= 108U && bit <= 123U);
        if (!immediate)
        {
            const UInt8 mask = static_cast<UInt8>(1U << (bit % 8U));
            if ((g_destination[0x400020U + bit / 8U] & mask) != (g_source.Data[0x1020U + bit / 8U] & mask))
            {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool Unchanged() noexcept
{
    for (const auto byte : g_destination)
    {
        if (byte != 0xA5U)
        {
            return false;
        }
    }
    return true;
}
} // namespace

int main()
{
    Asteria::TestSupport::TestContext context{"KernelElf"};
    const UInt64 placed = reinterpret_cast<Asteria::Foundation::UIntPointer>(g_destination);
    constexpr Asteria::Foundation::UInt32 Architectures[]{1U, 2U};
    constexpr UInt64 PreferredBases[]{0x4000000U, 0x1ABCD00000000U};
    for (const UInt64 linkedBase : PreferredBases)
    {
        for (const Asteria::Foundation::UInt32 architecture : Architectures)
        {
            g_source.Build(architecture, linkedBase);
            ASTERIA_EXPECT(context, "specification fixture parses",
                           KernelElf::Parse(g_source.View(), architecture, g_plan) == KernelElfError::None);
            ASTERIA_EXPECT(context, "validated kernel places",
                           KernelElf::Place(g_plan, g_destination, sizeof(g_destination)) == KernelElfError::None);
            if (architecture == 1U)
            {
                ASTERIA_EXPECT(context, "AMD64 64 and RELATIVE sites use the placed base",
                               Read(0x3000U) == placed + 0x1000U, Read(0x3008U) == placed + 0x7080U);
            }
            else
            {
                ASTERIA_EXPECT(context, "IA-64 data sites preserve descriptor versus code meaning",
                               Read(0x801000U) == placed + 0x400000U, Read(0x801008U) == placed + 0x802000U,
                               Read(0x801010U) == placed + 0x400000U, Read(0x801018U) == placed + 0x880000U);
                ASTERIA_EXPECT(context, "IA-64 generated descriptors relocate both words",
                               Read(0x802000U) == placed + 0x400000U, Read(0x802008U) == placed + 0x880000U,
                               Read(0x802020U) == placed + 0x400020U, Read(0x802028U) == placed + 0x880000U);
            }
            if (architecture == 1U)
            {
                ASTERIA_EXPECT(context, "PC32 remains invariant and BSS is zero",
                               (Read(0x1080U) & 0xFFFFFFFFU) == 0x80U, Read(0x4000U) == 0U);
            }
            else
            {
                ASTERIA_EXPECT(context, "IMM64 relocates all encoded bits and GPREL64I stays invariant",
                               Immediate(0x400020U) == placed + 0xEBB357U, UnrelatedInstructionBits(),
                               Immediate(0x4000E0U) == 0x200U, Read(0x400080U) == (UInt64{8U} << 18U),
                               Read(0xC01000U) == 0U);
            }
            const Size fields[]{48U,     56U,     60U,     0x2004U, 0x2034U, 0x2100U, 0x80CU,
                                0x7108U, 0x7118U, 0x7208U, 0x7020U, 0x7110U, 0x3010U, 0x4010U};
            for (const Size field : fields)
            {
                if (architecture == 1U && (field == 0x4010U || field == 0x3010U))
                {
                    continue;
                }
                g_source.Build(architecture, linkedBase);
                ASTERIA_EXPECT(context, "baseline reparses",
                               KernelElf::Parse(g_source.View(), architecture, g_plan) == KernelElfError::None);
                g_source.Data[field] ^= 0xFFU;
                for (auto& byte : g_destination)
                {
                    byte = 0xA5U;
                }
                ASTERIA_EXPECT(context, "bad metadata or relocation rejects before any destination write",
                               KernelElf::Place(g_plan, g_destination, sizeof(g_destination)) != KernelElfError::None,
                               Unchanged());
            }
        }
    }
    return context.Finish();
}
