// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Firmware/Ia64LoadedImageLayout.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Platform::Ia64::Firmware::Ia64LoadedImageLayout;
using Platform::Ia64::Firmware::Ia64LoadedImageBaseAlignmentBytes;
using Platform::Ia64::Firmware::Ia64LoadedImageLayoutError;
using Platform::Ia64::Firmware::Ia64LoadedImageLayoutParser;
using Platform::Ia64::Firmware::Ia64LoadedImageSectionPurpose;

constexpr Foundation::Size ImageBytes{0x10000U};
constexpr Foundation::Size LargeImageBytes{0x235000U};
constexpr Foundation::Size HeaderBytes{0x400U};
constexpr Foundation::Size PeOffset{0x80U};
constexpr Foundation::Size OptionalOffset{PeOffset + 24U};
constexpr Foundation::Size SectionTableOffset{OptionalOffset + 240U};
alignas(Ia64LoadedImageBaseAlignmentBytes) Foundation::UInt8 g_image[ImageBytes + Ia64LoadedImageBaseAlignmentBytes];
alignas(Ia64LoadedImageBaseAlignmentBytes) Foundation::UInt8 g_largeImage[LargeImageBytes];
Foundation::UInt8 g_snapshot[ImageBytes];

void WriteUInt16(Foundation::UInt8* const bytes, const Foundation::Size offset, const Foundation::UInt16 value) noexcept
{
    bytes[offset] = static_cast<Foundation::UInt8>(value);
    bytes[offset + 1U] = static_cast<Foundation::UInt8>(value >> 8U);
}

void WriteUInt32(Foundation::UInt8* const bytes, const Foundation::Size offset, const Foundation::UInt32 value) noexcept
{
    for (Foundation::UInt8 index{0U}; index < 4U; ++index)
    {
        bytes[offset + index] = static_cast<Foundation::UInt8>(value >> (static_cast<Foundation::UInt32>(index) * 8U));
    }
}

void WriteName(Foundation::UInt8* const bytes, const Foundation::Size offset, const char* const name,
               const Foundation::Size length) noexcept
{
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        bytes[offset + index] = index < length ? static_cast<Foundation::UInt8>(name[index]) : Foundation::UInt8{0U};
    }
}

void WriteSection(const Foundation::Size index, const char* const name, const Foundation::Size nameLength,
                  const Foundation::UInt32 virtualSize, const Foundation::UInt32 virtualAddress,
                  const Foundation::UInt32 rawSize, const Foundation::UInt32 characteristics) noexcept
{
    const Foundation::Size offset = SectionTableOffset + (index * 40U);
    WriteName(g_image, offset, name, nameLength);
    WriteUInt32(g_image, offset + 8U, virtualSize);
    WriteUInt32(g_image, offset + 12U, virtualAddress);
    WriteUInt32(g_image, offset + 16U, rawSize);
    WriteUInt32(g_image, offset + 36U, characteristics);
}

void ResetImage() noexcept
{
    for (Foundation::Size index{0U}; index < ImageBytes; ++index)
    {
        g_image[index] = 0U;
    }
    g_image[0U] = static_cast<Foundation::UInt8>('M');
    g_image[1U] = static_cast<Foundation::UInt8>('Z');
    WriteUInt32(g_image, 60U, PeOffset);
    g_image[PeOffset] = static_cast<Foundation::UInt8>('P');
    g_image[PeOffset + 1U] = static_cast<Foundation::UInt8>('E');
    WriteUInt16(g_image, PeOffset + 4U, 0x0200U);
    WriteUInt16(g_image, PeOffset + 6U, 6U);
    WriteUInt16(g_image, PeOffset + 20U, 240U);
    WriteUInt16(g_image, PeOffset + 22U, 0x022EU);

    WriteUInt16(g_image, OptionalOffset, 0x020BU);
    WriteUInt32(g_image, OptionalOffset + 16U, 0xB000U);
    WriteUInt32(g_image, OptionalOffset + 20U, 0x8000U);
    WriteUInt32(g_image, OptionalOffset + 32U, 0x1000U);
    WriteUInt32(g_image, OptionalOffset + 36U, 0x0200U);
    WriteUInt32(g_image, OptionalOffset + 56U, ImageBytes);
    WriteUInt32(g_image, OptionalOffset + 60U, HeaderBytes);
    WriteUInt16(g_image, OptionalOffset + 68U, 10U);
    WriteUInt16(g_image, OptionalOffset + 70U, 0x0160U);
    WriteUInt32(g_image, OptionalOffset + 108U, 16U);

    WriteSection(0U, ".text", 5U, 0x1800U, 0x8000U, 0x1800U, 0x6000'0020U);
    WriteSection(1U, ".rodata", 7U, 0x0400U, 0xA000U, 0x0400U, 0x4000'0040U);
    WriteSection(2U, ".opd", 4U, 0x0010U, 0xB000U, 0x0200U, 0x4000'0040U);
    WriteSection(3U, ".reloc", 6U, 0x0018U, 0xC000U, 0x0200U, 0x4000'0040U);
    WriteSection(4U, ".data", 5U, 0x0008U, 0xD000U, 0x0200U, 0xC000'0040U);
    WriteSection(5U, ".bss", 4U, 0x2000U, 0xE000U, 0U, 0xC000'0080U);
}

[[nodiscard]] bool HasError(const Ia64LoadedImageLayoutError expected) noexcept
{
    const auto result = Ia64LoadedImageLayoutParser::Parse(g_image, ImageBytes);
    return !result.HasValue() && (result.GetError() == expected);
}

[[nodiscard]] bool ImageMatchesSnapshot() noexcept
{
    for (Foundation::Size index{0U}; index < ImageBytes; ++index)
    {
        if (g_image[index] != g_snapshot[index])
        {
            return false;
        }
    }
    return true;
}

void TestExactLayout(TestContext& runner) noexcept
{
    ResetImage();
    for (Foundation::Size index{0U}; index < ImageBytes; ++index)
    {
        g_snapshot[index] = g_image[index];
    }
    const auto result = Ia64LoadedImageLayoutParser::Parse(g_image, ImageBytes);
    bool exact{false};
    if (result.HasValue())
    {
        const Ia64LoadedImageLayout& layout = result.GetValue();
        const Foundation::UInt64 base =
            static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_image));
        exact = (layout.ImageBase == base) && (layout.ImageSize == ImageBytes) &&
                (layout.Sections[0U].VirtualStart == (base + 0x8000U)) && (layout.Sections[0U].PageCount == 2U) &&
                (layout.Sections[0U].Purpose == Ia64LoadedImageSectionPurpose::Code) &&
                (layout.Sections[1U].VirtualStart == (base + 0xA000U)) && (layout.Sections[1U].PageCount == 1U) &&
                (layout.Sections[1U].Purpose == Ia64LoadedImageSectionPurpose::ReadOnlyData) &&
                (layout.Sections[2U].VirtualStart == (base + 0xB000U)) &&
                (layout.Sections[3U].VirtualStart == (base + 0xC000U)) &&
                (layout.Sections[4U].VirtualStart == (base + 0xD000U)) &&
                (layout.Sections[4U].Purpose == Ia64LoadedImageSectionPurpose::WritableData) &&
                (layout.Sections[5U].VirtualStart == (base + 0xE000U)) && (layout.Sections[5U].PageCount == 2U) &&
                (layout.Sections[5U].Purpose == Ia64LoadedImageSectionPurpose::WritableData);
        for (Foundation::Size index{0U}; exact && (index < 6U); ++index)
        {
            exact = layout.Sections[index].Reserved == 0U;
        }
    }
    ASTERIA_EXPECT(runner, "the canonical IA-64 image yields exact owned page ranges", exact);
    ASTERIA_EXPECT(runner, "successful parsing leaves every loaded-image byte unchanged", ImageMatchesSnapshot());
}

void TestHeaderFailures(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "a null loaded-image pointer is rejected",
                   !Ia64LoadedImageLayoutParser::Parse(nullptr, ImageBytes).HasValue());
    ResetImage();
    ASTERIA_EXPECT(runner, "loaded-image minimum size and page alignment are enforced",
                   !Ia64LoadedImageLayoutParser::Parse(g_image, HeaderBytes - 1U).HasValue(),
                   !Ia64LoadedImageLayoutParser::Parse(g_image, ImageBytes - 1U).HasValue());

    ResetImage();
    const auto unaligned = Ia64LoadedImageLayoutParser::Parse(g_image + 1U, ImageBytes);
    const auto loaderMisaligned = Ia64LoadedImageLayoutParser::Parse(g_image + 0x1000U, ImageBytes);
    constexpr Foundation::UIntPointer MaximumAlignedAddress{
        ~Foundation::UIntPointer{0U} & ~Foundation::UIntPointer{Ia64LoadedImageBaseAlignmentBytes - 1U}};
    const auto wrapping =
        Ia64LoadedImageLayoutParser::Parse(reinterpret_cast<const void*>(MaximumAlignedAddress), ImageBytes);
    ASTERIA_EXPECT(runner, "the runtime image base obeys the 64-KiB loader contract and cannot wrap",
                   !unaligned.HasValue(), unaligned.GetError() == Ia64LoadedImageLayoutError::InvalidImageGeometry,
                   !loaderMisaligned.HasValue(),
                   loaderMisaligned.GetError() == Ia64LoadedImageLayoutError::InvalidImageGeometry,
                   !wrapping.HasValue(), wrapping.GetError() == Ia64LoadedImageLayoutError::InvalidImageGeometry);

    ResetImage();
    g_image[0U] = 0U;
    ASTERIA_EXPECT(runner, "the DOS signature is required", HasError(Ia64LoadedImageLayoutError::InvalidDosHeader));
    ResetImage();
    g_image[PeOffset] = 0U;
    ASTERIA_EXPECT(runner, "the PE signature is required", HasError(Ia64LoadedImageLayoutError::InvalidPeHeader));
    ResetImage();
    WriteUInt16(g_image, PeOffset + 4U, 0x8664U);
    ASTERIA_EXPECT(runner, "a non-IA-64 COFF machine is rejected",
                   HasError(Ia64LoadedImageLayoutError::InvalidCoffHeader));
    ResetImage();
    WriteUInt16(g_image, OptionalOffset, 0x010BU);
    ASTERIA_EXPECT(runner, "a non-PE32+ optional header is rejected",
                   HasError(Ia64LoadedImageLayoutError::InvalidOptionalHeader));
    ResetImage();
    WriteUInt32(g_image, OptionalOffset + 56U, ImageBytes - 0x1000U);
    ASTERIA_EXPECT(runner, "the loaded extent must equal PE SizeOfImage",
                   HasError(Ia64LoadedImageLayoutError::InvalidImageGeometry));
}

void TestImageAboveFormerFixedCap(TestContext& runner) noexcept
{
    ResetImage();
    for (Foundation::Size index{0U}; index < HeaderBytes; ++index)
    {
        g_largeImage[index] = g_image[index];
    }
    WriteUInt32(g_largeImage, OptionalOffset + 56U, static_cast<Foundation::UInt32>(LargeImageBytes));
    WriteUInt32(g_largeImage, SectionTableOffset + (5U * 40U) + 8U,
                static_cast<Foundation::UInt32>(LargeImageBytes - 0xE000U));
    const auto parsed = Ia64LoadedImageLayoutParser::Parse(g_largeImage, LargeImageBytes);
    ASTERIA_EXPECT(runner, "a valid 0x235000-byte IA-64 image has no arbitrary fixed cap", parsed.HasValue(),
                   parsed.GetValue().ImageSize == LargeImageBytes,
                   parsed.GetValue().Sections[5U].PageCount == ((LargeImageBytes - 0xE000U) / 0x1000U));
}

void TestSectionFailures(TestContext& runner) noexcept
{
    ResetImage();
    g_image[SectionTableOffset] = static_cast<Foundation::UInt8>('X');
    ASTERIA_EXPECT(runner, "all six canonical section names and their order are required",
                   HasError(Ia64LoadedImageLayoutError::InvalidSectionName));

    ResetImage();
    WriteUInt32(g_image, SectionTableOffset + 40U + 12U, 0xB000U);
    ASTERIA_EXPECT(runner, "virtual gaps and overlaps between image sections are rejected",
                   HasError(Ia64LoadedImageLayoutError::InvalidSectionGeometry));

    ResetImage();
    WriteUInt32(g_image, SectionTableOffset + 36U, 0xE000'0020U);
    ASTERIA_EXPECT(runner, "a writable executable text section is rejected",
                   HasError(Ia64LoadedImageLayoutError::InvalidSectionProtection));

    ResetImage();
    WriteUInt32(g_image, SectionTableOffset + (5U * 40U) + 16U, 0x0200U);
    ASTERIA_EXPECT(runner, "BSS may not have raw backing",
                   HasError(Ia64LoadedImageLayoutError::InvalidSectionGeometry));

    ResetImage();
    WriteUInt32(g_image, SectionTableOffset + 16U, 0x1700U);
    const bool misalignedRaw = HasError(Ia64LoadedImageLayoutError::InvalidSectionGeometry);
    ResetImage();
    WriteUInt32(g_image, SectionTableOffset + 16U, 0x1000U);
    const bool shortRaw = HasError(Ia64LoadedImageLayoutError::InvalidSectionGeometry);
    ResetImage();
    WriteUInt32(g_image, SectionTableOffset + 16U, 0U);
    ASTERIA_EXPECT(runner, "every backed section has aligned nonzero raw storage covering its virtual bytes",
                   misalignedRaw, shortRaw, HasError(Ia64LoadedImageLayoutError::InvalidSectionGeometry));

    ResetImage();
    WriteUInt32(g_image, OptionalOffset + 16U, 0xA000U);
    ASTERIA_EXPECT(runner, "the entrypoint must be the aligned OPD descriptor",
                   HasError(Ia64LoadedImageLayoutError::InvalidEntrypoint));

    ResetImage();
    WriteUInt32(g_image, OptionalOffset + 20U, 0x7000U);
    const bool baseOfCode = HasError(Ia64LoadedImageLayoutError::InvalidEntrypoint);
    ResetImage();
    WriteUInt32(g_image, SectionTableOffset + (2U * 40U) + 8U, 8U);
    ASTERIA_EXPECT(runner, "BaseOfCode and the complete 16-byte OPD descriptor are required", baseOfCode,
                   HasError(Ia64LoadedImageLayoutError::InvalidEntrypoint));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64LoadedImageLayoutTests"};
    Asteria::Test::TestExactLayout(runner);
    Asteria::Test::TestHeaderFailures(runner);
    Asteria::Test::TestImageAboveFormerFixedCap(runner);
    Asteria::Test::TestSectionFailures(runner);
    return runner.Finish();
}
