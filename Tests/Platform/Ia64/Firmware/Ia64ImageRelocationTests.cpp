// SPDX-License-Identifier: Apache-2.0
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Firmware/Ia64ImageRelocation.hpp"

#include "Support/TestContext.hpp"

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt64;
using Platform::Ia64::Firmware::Ia64ImageRelocation;
using Platform::Ia64::Firmware::Ia64ImageRelocationError;
constexpr Size Capacity{0x1000000U};
constexpr Size SyntheticBytes{0x7000U};
constexpr Size Optional{0x98U};
constexpr Size Sections{Optional + 240U};
alignas(Capacity) UInt8 g_source[Capacity + 0x10000U]{};
alignas(Capacity) UInt8 g_copy[Capacity]{};

[[nodiscard]] UInt64 Read(const UInt8* bytes, Size count) noexcept
{
    UInt64 result{0U};
    for (Size index{0U}; index < count; ++index)
    {
        result |= static_cast<UInt64>(bytes[index]) << (8U * index);
    }
    return result;
}
void Write(UInt8* bytes, UInt64 value, Size count) noexcept
{
    for (Size index{0U}; index < count; ++index)
    {
        bytes[index] = static_cast<UInt8>(value & 0xFFU);
        value >>= 8U;
    }
}
[[nodiscard]] UInt64 Address(const void* pointer) noexcept
{
    return reinterpret_cast<Foundation::UIntPointer>(pointer);
}

// Independent bit-coordinate model of the ISA's scattered immediate. It
// changes one bundle bit at a time, leaving all other bits untouched.
[[nodiscard]] constexpr Size ImmediateBit(Size bit) noexcept
{
    if (bit < 7U)
    {
        return 100U + bit;
    }
    if (bit < 16U)
    {
        return 114U + bit - 7U;
    }
    if (bit < 21U)
    {
        return 109U + bit - 16U;
    }
    if (bit == 21U)
    {
        return 108U;
    }
    if (bit < 63U)
    {
        return 46U + bit - 22U;
    }
    return 123U;
}
[[nodiscard]] UInt64 ReadImmediate(const UInt8* bundle) noexcept
{
    UInt64 result{0U};
    for (Size bit{0U}; bit < 64U; ++bit)
    {
        const Size location = ImmediateBit(bit);
        result |= static_cast<UInt64>((bundle[location / 8U] >> (location % 8U)) & 1U) << bit;
    }
    return result;
}
void WriteImmediate(UInt8* bundle, UInt64 value) noexcept
{
    for (Size bit{0U}; bit < 64U; ++bit)
    {
        const Size location = ImmediateBit(bit);
        const UInt8 mask = static_cast<UInt8>(1U << (location % 8U));
        bundle[location / 8U] =
            static_cast<UInt8>((bundle[location / 8U] & ~mask) | (((value >> bit) & 1U) != 0U ? mask : 0U));
    }
}

void Reset() noexcept
{
    for (Size index{0U}; index < SyntheticBytes; ++index)
    {
        g_source[index] = 0U;
    }
    g_source[0U] = 'M';
    g_source[1U] = 'Z';
    Write(g_source + 60U, 0x80U, 4U);
    Write(g_source + 0x80U, 0x4550U, 4U);
    Write(g_source + 0x84U, 0x200U, 2U);
    Write(g_source + 0x86U, 6U, 2U);
    Write(g_source + 0x94U, 240U, 2U);
    Write(g_source + 0x96U, 0x22EU, 2U);
    Write(g_source + Optional, 0x20BU, 2U);
    Write(g_source + Optional + 16U, 0x3000U, 4U);
    Write(g_source + Optional + 20U, 0x1000U, 4U);
    Write(g_source + Optional + 32U, 0x1000U, 4U);
    Write(g_source + Optional + 36U, 0x200U, 4U);
    Write(g_source + Optional + 56U, SyntheticBytes, 4U);
    Write(g_source + Optional + 60U, 0x400U, 4U);
    Write(g_source + Optional + 68U, 10U, 2U);
    Write(g_source + Optional + 70U, 0x160U, 2U);
    Write(g_source + Optional + 108U, 16U, 4U);
    Write(g_source + Optional + 152U, 0x4000U, 4U);
    Write(g_source + Optional + 156U, 36U, 4U);
    constexpr char names[6U][8U]{".text", ".rodata", ".opd", ".reloc", ".data", ".bss"};
    constexpr UInt64 flags[6U]{0x60000020U, 0x40000040U, 0x40000040U, 0x40000040U, 0xC0000040U, 0xC0000080U};
    for (Size index{0U}; index < 6U; ++index)
    {
        UInt8* section = g_source + Sections + index * 40U;
        for (Size byte{0U}; byte < 8U; ++byte)
        {
            section[byte] = static_cast<UInt8>(names[index][byte]);
        }
        Write(section + 8U, 0x1000U, 4U);
        Write(section + 12U, (index + 1U) * 0x1000U, 4U);
        Write(section + 16U, index == 5U ? 0U : 0x1000U, 4U);
        Write(section + 36U, flags[index], 4U);
    }
    // MLX movl with unrelated slot-zero bits and destination register intact.
    g_source[0x1000U] = 5U;
    g_source[0x1004U] = 0xA5U;
    g_source[0x100FU] = 0x60U;
    WriteImmediate(g_source + 0x1000U, 0xFEDCBA9876543210ULL);
    Write(g_source + 0x3000U, Address(g_source) + 0x1000U, 8U);
    Write(g_source + 0x3008U, Address(g_source) + 0x2000U, 8U);
    Write(g_source + 0x5000U, ~UInt64{0U}, 8U);
    Write(g_source + 0x4000U, 0x1000U, 4U);
    Write(g_source + 0x4004U, 12U, 4U);
    Write(g_source + 0x4008U, 0x9001U, 2U);
    Write(g_source + 0x400CU, 0x3000U, 4U);
    Write(g_source + 0x4010U, 12U, 4U);
    Write(g_source + 0x4014U, 0xA000U, 2U);
    Write(g_source + 0x4016U, 0xA008U, 2U);
    Write(g_source + 0x4018U, 0x5000U, 4U);
    Write(g_source + 0x401CU, 12U, 4U);
    Write(g_source + 0x4020U, 0xA000U, 2U);
}
[[nodiscard]] UInt64 Hash(const UInt8* bytes, Size count) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < count; ++index)
    {
        value = (value * 131U) ^ bytes[index];
    }
    return value;
}
void TestCopy(TestSupport::TestContext& context) noexcept
{
    Reset();
    const UInt64 original = Hash(g_source, SyntheticBytes);
    const UInt64 delta = Address(g_copy) - Address(g_source);
    const auto result = Ia64ImageRelocation::RelocateToAlignment(g_source, SyntheticBytes, g_copy, Capacity);
    ASTERIA_EXPECT(context, "copy rebases descriptors and scattered immediates without changing source",
                   result.HasValue(), result.GetValue().EntryDescriptor == Address(g_copy) + 0x3000U,
                   Read(g_copy + 0x3000U, 8U) == Address(g_copy) + 0x1000U,
                   Read(g_copy + 0x3008U, 8U) == Address(g_copy) + 0x2000U,
                   Read(g_copy + 0x5000U, 8U) == (~UInt64{0U} + delta),
                   ReadImmediate(g_copy + 0x1000U) == (0xFEDCBA9876543210ULL + delta), g_copy[0x1004U] == 0xA5U,
                   Hash(g_source, SyntheticBytes) == original);
    const auto back = Ia64ImageRelocation::RelocateToAlignment(g_copy, SyntheticBytes, g_source, Capacity);
    ASTERIA_EXPECT(context, "opposite signed delta restores the entire image", back.HasValue(),
                   Hash(g_source, SyntheticBytes) == original);
    UInt64 seed{0xDCDF4FD4U};
    for (Size iteration{0U}; iteration < 512U; ++iteration)
    {
        seed = seed * 6364136223846793005ULL + 1U;
        WriteImmediate(g_source + 0x1000U, seed);
        Write(g_source + 0x5000U, seed, 8U);
        const auto copy = Ia64ImageRelocation::RelocateToAlignment(g_source, SyntheticBytes, g_copy, Capacity);
        ASTERIA_EXPECT(context, "all immediate bits and carry boundaries follow the independent model", copy.HasValue(),
                       ReadImmediate(g_copy + 0x1000U) == seed + delta, Read(g_copy + 0x5000U, 8U) == seed + delta);
    }
}
void TestRejections(TestSupport::TestContext& context) noexcept
{
    for (Size mutation{0U}; mutation < 13U; ++mutation)
    {
        Reset();
        for (Size index{0U}; index < SyntheticBytes; ++index)
        {
            g_copy[index] = 0xA5U;
        }
        const UInt64 before = Hash(g_copy, SyntheticBytes);
        void* destination{g_copy};
        Size capacity{Capacity};
        Foundation::UInt32 descriptor{0U};
        switch (mutation)
        {
        case 0U:
            Write(g_source + 0x4004U, 7U, 4U);
            break;
        case 1U:
            Write(g_source + 0x4008U, 0x8001U, 2U);
            break;
        case 2U:
            Write(g_source + 0x4016U, 0xA000U, 2U);
            break;
        case 3U:
            Write(g_source + 0x4018U, 0x7000U, 4U);
            break;
        case 4U:
            Write(g_source + Optional + 156U, 35U, 4U);
            break;
        case 5U:
            g_source[0x100FU] = 0U;
            break;
        case 6U:
            Write(g_source + 0x4020U, 0xAFFFU, 2U);
            break;
        case 7U:
            destination = g_copy + 0x1000U;
            break;
        case 8U:
            destination = g_source;
            break;
        case 9U:
            capacity = SyntheticBytes - 1U;
            break;
        case 10U:
            descriptor = 0x2000U;
            break;
        case 11U:
            Write(g_source + Optional + 152U, 0x3000U, 4U);
            break;
        case 12U:
            Write(g_source + 0x4018U, 0x4000U, 4U);
            break;
        }
        const auto result =
            Ia64ImageRelocation::RelocateToAlignment(g_source, SyntheticBytes, destination, capacity, descriptor);
        ASTERIA_EXPECT(context, "malformed fixup or placement rejection precedes every destination write",
                       !result.HasValue(), Hash(g_copy, SyntheticBytes) == before);
    }
}

void ApplyModel(UInt8* image, Size optional, UInt64 delta) noexcept
{
    const Size directory = static_cast<Size>(Read(image + optional + 152U, 4U));
    const Size end = directory + static_cast<Size>(Read(image + optional + 156U, 4U));
    for (Size block{directory}; block < end;)
    {
        const Size page = static_cast<Size>(Read(image + block, 4U));
        const Size blockBytes = static_cast<Size>(Read(image + block + 4U, 4U));
        for (Size entry{block + 8U}; entry < block + blockBytes; entry += 2U)
        {
            const UInt64 encoded = Read(image + entry, 2U);
            const Size rva = page + static_cast<Size>(encoded & 0xFFFU);
            if ((encoded >> 12U) == 10U)
            {
                Write(image + rva, Read(image + rva, 8U) + delta, 8U);
            }
            if ((encoded >> 12U) == 9U)
            {
                UInt8* bundle = image + (rva & ~Size{15U});
                WriteImmediate(bundle, ReadImmediate(bundle) + delta);
            }
        }
        block += blockBytes;
    }
}
void TestRealImage(TestSupport::TestContext& context, const char* path) noexcept
{
    // Reconstruct the real normalized PE as the firmware does, at a nonaligned
    // source residue, then independently model both relocation passes.
    std::FILE* file = std::fopen(path, "rb");
    ASTERIA_EXPECT(context, "real normalized PE opens", file != nullptr);
    if (file == nullptr)
    {
        return;
    }
    UInt8* source = g_source + 0x10000U;
    for (Size index{0U}; index < Capacity; ++index)
    {
        source[index] = 0U;
    }
    const Size read = std::fread(source, 1U, 0x400U, file);
    const Size optional = static_cast<Size>(Read(source + 60U, 4U)) + 24U;
    const Size imageBytes = static_cast<Size>(Read(source + optional + 56U, 4U));
    ASTERIA_EXPECT(context, "real image fits test-owned storage", read == 0x400U, imageBytes <= Capacity);
    if ((read != 0x400U) || (imageBytes > Capacity))
    {
        static_cast<void>(std::fclose(file));
        return;
    }
    const Size table = optional + 240U;
    for (Size index{0U}; index < 6U; ++index)
    {
        const UInt8* section = source + table + index * 40U;
        const Size rva = static_cast<Size>(Read(section + 12U, 4U));
        const Size bytes = static_cast<Size>(Read(section + 16U, 4U));
        const UInt64 offset = Read(section + 20U, 4U);
        if (bytes == 0U)
        {
            continue;
        }
        ASTERIA_EXPECT(context, "real PE raw extent is bounded", rva <= imageBytes, bytes <= imageBytes - rva);
        if ((rva > imageBytes) || (bytes > imageBytes - rva))
        {
            break;
        }
        const int sought = std::fseek(file, static_cast<long>(offset), SEEK_SET);
        const Size loaded = std::fread(source + rva, 1U, bytes, file);
        ASTERIA_EXPECT(context, "real section loads", sought == 0, loaded == bytes);
    }
    static_cast<void>(std::fclose(file));
    ApplyModel(source, optional, Address(source) - Read(source + optional + 24U, 8U));
    const UInt64 before = Hash(source, imageBytes);
    const auto result = Ia64ImageRelocation::RelocateToAlignment(source, imageBytes, g_copy, Capacity);
    ASTERIA_EXPECT(context, "real .reloc stream is accepted and leaves firmware copy intact", result.HasValue(),
                   Hash(source, imageBytes) == before);
    ApplyModel(source, optional, Address(g_copy) - Address(source));
    bool equal{true};
    for (Size index{0U}; index < imageBytes; ++index)
    {
        equal &= source[index] == g_copy[index];
    }
    ASTERIA_EXPECT(context, "every real fixup and unchanged byte matches the independent bit model", equal);
}
} // namespace
} // namespace Asteria::Test

int main(int count, char** arguments)
{
    Asteria::TestSupport::TestContext context{"Ia64ImageRelocationTests"};
    if (count == 2)
    {
        Asteria::Test::TestRealImage(context, arguments[1U]);
    }
    else
    {
        Asteria::Test::TestCopy(context);
        Asteria::Test::TestRejections(context);
    }
    return context.Finish();
}
