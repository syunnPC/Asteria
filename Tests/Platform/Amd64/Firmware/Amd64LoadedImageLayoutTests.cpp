// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <new>

#include "Foundation/Types.hpp"
#include "Platform/Amd64/Firmware/Amd64LoadedImageLayout.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Platform::Amd64::Firmware::Amd64LoadedImageLayout;
using Platform::Amd64::Firmware::Amd64LoadedImageLayoutError;
using Platform::Amd64::Firmware::Amd64LoadedImageLayoutParser;
using Platform::Amd64::Firmware::Amd64LoadedImageSectionPurpose;

constexpr Foundation::Size ImageBytes{0xA000U};
constexpr Foundation::Size LayoutPrefixBytes{(sizeof(Amd64LoadedImageLayout) / 2U) & ~Foundation::Size{7U}};
constexpr Foundation::Size LargeImageBytes{0x20000U};
constexpr Foundation::Size HeaderBytes{0x0400U};
constexpr Foundation::Size PeOffset{0x0080U};
constexpr Foundation::Size PeSignatureSize{4U};
constexpr Foundation::Size CoffHeaderSize{20U};
constexpr Foundation::Size OptionalHeaderSize{240U};
constexpr Foundation::Size OptionalOffset{PeOffset + 24U};
constexpr Foundation::Size SectionTableOffset{OptionalOffset + 240U};
alignas(4096) Foundation::UInt8 g_image[ImageBytes + 1U];
alignas(4096) Foundation::UInt8 g_largeImage[LargeImageBytes];
alignas(65536) Foundation::UInt8 g_relocatedImage[ImageBytes + 4096U];
Foundation::UInt8 g_snapshot[ImageBytes];

union alignas(4096) OutputAliasFixture
{
    Foundation::UInt8 Storage[ImageBytes];
    Amd64LoadedImageLayout Output;
};

struct alignas(4096) OutputBeforeImageFixture final
{
    Foundation::UInt8 Prefix[4096U - sizeof(Amd64LoadedImageLayout)];
    Amd64LoadedImageLayout Output;
    Foundation::UInt8 Image[ImageBytes];
};

struct alignas(4096) OutputAfterImageFixture final
{
    Foundation::UInt8 Image[ImageBytes];
    Amd64LoadedImageLayout Output;
};

struct alignas(4096) OutputInsideImageFixture final
{
    Foundation::UInt8 Prefix[4096U];
    Amd64LoadedImageLayout Output;
    Foundation::UInt8 Tail[ImageBytes - 4096U - sizeof(Amd64LoadedImageLayout)];
};

struct alignas(4096) OutputCrossesImageStartFixture final
{
    Foundation::UInt8 Prefix[4096U - LayoutPrefixBytes];
    Amd64LoadedImageLayout Output;
    Foundation::UInt8 Tail[ImageBytes - (sizeof(Amd64LoadedImageLayout) - LayoutPrefixBytes)];
};

OutputAliasFixture g_outputAlias{};
OutputBeforeImageFixture g_outputBeforeImage{};
OutputAfterImageFixture g_outputAfterImage{};
OutputInsideImageFixture g_outputInsideImage{};
OutputCrossesImageStartFixture g_outputCrossesImageStart{};
static_assert(__builtin_offsetof(OutputBeforeImageFixture, Output) == (4096U - sizeof(Amd64LoadedImageLayout)));
static_assert(__builtin_offsetof(OutputBeforeImageFixture, Image) == 4096U);
static_assert(__builtin_offsetof(OutputAfterImageFixture, Output) == ImageBytes);
static_assert(sizeof(OutputInsideImageFixture) == ImageBytes);
static_assert(__builtin_offsetof(OutputInsideImageFixture, Output) == 4096U);
static_assert(__builtin_offsetof(OutputCrossesImageStartFixture, Output) == (4096U - LayoutPrefixBytes));
static_assert(sizeof(OutputCrossesImageStartFixture) == (4096U + ImageBytes));

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

void WriteUInt64(Foundation::UInt8* const bytes, const Foundation::Size offset, const Foundation::UInt64 value) noexcept
{
    for (Foundation::UInt8 index{0U}; index < 8U; ++index)
    {
        bytes[offset + index] = static_cast<Foundation::UInt8>(value >> (static_cast<Foundation::UInt64>(index) * 8U));
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

void WriteSection(Foundation::UInt8* const image, const Foundation::Size index, const char* const name,
                  const Foundation::Size nameLength, const Foundation::UInt32 virtualSize,
                  const Foundation::UInt32 virtualAddress, const Foundation::UInt32 rawSize,
                  const Foundation::UInt32 rawOffset, const Foundation::UInt32 characteristics) noexcept
{
    const Foundation::Size offset = SectionTableOffset + (index * 40U);
    WriteName(image, offset, name, nameLength);
    WriteUInt32(image, offset + 8U, virtualSize);
    WriteUInt32(image, offset + 12U, virtualAddress);
    WriteUInt32(image, offset + 16U, rawSize);
    WriteUInt32(image, offset + 20U, rawOffset);
    WriteUInt32(image, offset + 36U, characteristics);
}

void ResetImage(Foundation::UInt8* const image) noexcept
{
    for (Foundation::Size index{0U}; index < ImageBytes; ++index)
    {
        image[index] = 0U;
    }
    image[0U] = static_cast<Foundation::UInt8>('M');
    image[1U] = static_cast<Foundation::UInt8>('Z');
    WriteUInt32(image, 60U, PeOffset);
    image[PeOffset] = static_cast<Foundation::UInt8>('P');
    image[PeOffset + 1U] = static_cast<Foundation::UInt8>('E');
    WriteUInt16(image, PeOffset + 4U, 0x8664U);
    WriteUInt16(image, PeOffset + 6U, 7U);
    WriteUInt16(image, PeOffset + 20U, 240U);
    WriteUInt16(image, PeOffset + 22U, 0x022EU);

    WriteUInt16(image, OptionalOffset, 0x020BU);
    WriteUInt32(image, OptionalOffset + 4U, 0x1800U);
    WriteUInt32(image, OptionalOffset + 16U, 0x1170U);
    WriteUInt32(image, OptionalOffset + 20U, 0x1000U);
    WriteUInt64(image, OptionalOffset + 24U, 0x0400'0000U);
    WriteUInt32(image, OptionalOffset + 32U, 0x1000U);
    WriteUInt32(image, OptionalOffset + 36U, 0x0200U);
    WriteUInt32(image, OptionalOffset + 56U, ImageBytes);
    WriteUInt32(image, OptionalOffset + 60U, HeaderBytes);
    WriteUInt16(image, OptionalOffset + 68U, 10U);
    WriteUInt16(image, OptionalOffset + 70U, 0x0160U);
    WriteUInt32(image, OptionalOffset + 108U, 16U);
    WriteUInt32(image, OptionalOffset + 112U + (1U * 8U), 0x7000U);
    WriteUInt32(image, OptionalOffset + 112U + (1U * 8U) + 4U, 24U);
    WriteUInt32(image, OptionalOffset + 112U + (5U * 8U), 0x8000U);
    WriteUInt32(image, OptionalOffset + 112U + (5U * 8U) + 4U, 0x011CU);

    WriteSection(image, 0U, ".text", 5U, 0x1800U, 0x1000U, 0x1800U, 0x0400U, 0x6000'0020U);
    WriteSection(image, 1U, ".rdata", 6U, 32U, 0x3000U, 0x0200U, 0x1C00U, 0x4000'0040U);
    WriteSection(image, 2U, ".rodata", 7U, 0x0400U, 0x4000U, 0x0400U, 0x1E00U, 0x4000'0040U);
    WriteSection(image, 3U, ".bss", 4U, 0x2000U, 0x5000U, 0U, 0U, 0xC000'0080U);
    WriteSection(image, 4U, ".idata", 6U, 24U, 0x7000U, 0x0200U, 0x2200U, 0x4000'0040U);
    WriteSection(image, 5U, ".reloc", 6U, 0x0180U, 0x8000U, 0x0200U, 0x2400U, 0x4200'0040U);
    WriteSection(image, 6U, ".data", 5U, 16U, 0x9000U, 0x0200U, 0x2600U, 0xC000'0040U);
}

[[nodiscard]] Amd64LoadedImageLayout LayoutSentinel() noexcept
{
    Amd64LoadedImageLayout layout{
        .ImageBase = 0x1122'3344'5566'7788ULL,
        .ImageSize = 0x8877'6655'4433'2211ULL,
        .Sections = {},
    };
    for (Foundation::Size index{0U}; index < Platform::Amd64::Firmware::Amd64LoadedImageSectionCount; ++index)
    {
        layout.Sections[index].VirtualStart = 0x1000U + index;
        layout.Sections[index].PageCount = 0x2000U + index;
        layout.Sections[index].Purpose = Amd64LoadedImageSectionPurpose::WritableData;
        layout.Sections[index].Reserved = static_cast<Foundation::UInt32>(0xA500U + index);
    }
    return layout;
}

[[nodiscard]] bool LayoutsEqual(const Amd64LoadedImageLayout& first, const Amd64LoadedImageLayout& second) noexcept
{
    if ((first.ImageBase != second.ImageBase) || (first.ImageSize != second.ImageSize))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < Platform::Amd64::Firmware::Amd64LoadedImageSectionCount; ++index)
    {
        if ((first.Sections[index].VirtualStart != second.Sections[index].VirtualStart) ||
            (first.Sections[index].PageCount != second.Sections[index].PageCount) ||
            (first.Sections[index].Purpose != second.Sections[index].Purpose) ||
            (first.Sections[index].Reserved != second.Sections[index].Reserved))
        {
            return false;
        }
    }
    return true;
}

void CaptureImageSnapshot() noexcept
{
    for (Foundation::Size index{0U}; index < ImageBytes; ++index)
    {
        g_snapshot[index] = g_image[index];
    }
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

[[nodiscard]] bool HasError(const Amd64LoadedImageLayoutError expected) noexcept
{
    CaptureImageSnapshot();
    Amd64LoadedImageLayout output = LayoutSentinel();
    const Amd64LoadedImageLayout expectedOutput = output;
    const Amd64LoadedImageLayoutError error = Amd64LoadedImageLayoutParser::Parse(g_image, ImageBytes, output);
    return (error == expected) && LayoutsEqual(output, expectedOutput) && ImageMatchesSnapshot();
}

void TestExactLayout(TestContext& runner) noexcept
{
    ResetImage(g_image);
    CaptureImageSnapshot();
    Amd64LoadedImageLayout output = LayoutSentinel();
    const Amd64LoadedImageLayoutError error = Amd64LoadedImageLayoutParser::Parse(g_image, ImageBytes, output);
    const Foundation::UInt64 base = static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_image));
    bool exact = (error == Amd64LoadedImageLayoutError::None) && (output.ImageBase == base) &&
                 (output.ImageSize == ImageBytes) && (output.Sections[0U].VirtualStart == (base + 0x1000U)) &&
                 (output.Sections[0U].PageCount == 2U) &&
                 (output.Sections[0U].Purpose == Amd64LoadedImageSectionPurpose::Code) &&
                 (output.Sections[1U].VirtualStart == (base + 0x3000U)) && (output.Sections[1U].PageCount == 1U) &&
                 (output.Sections[1U].Purpose == Amd64LoadedImageSectionPurpose::ReadOnlyData) &&
                 (output.Sections[2U].VirtualStart == (base + 0x4000U)) && (output.Sections[2U].PageCount == 1U) &&
                 (output.Sections[2U].Purpose == Amd64LoadedImageSectionPurpose::ReadOnlyData) &&
                 (output.Sections[3U].VirtualStart == (base + 0x5000U)) && (output.Sections[3U].PageCount == 2U) &&
                 (output.Sections[3U].Purpose == Amd64LoadedImageSectionPurpose::WritableData) &&
                 (output.Sections[4U].VirtualStart == (base + 0x7000U)) && (output.Sections[4U].PageCount == 1U) &&
                 (output.Sections[4U].Purpose == Amd64LoadedImageSectionPurpose::ReadOnlyData) &&
                 (output.Sections[5U].VirtualStart == (base + 0x8000U)) && (output.Sections[5U].PageCount == 1U) &&
                 (output.Sections[5U].Purpose == Amd64LoadedImageSectionPurpose::ReadOnlyData);
    for (Foundation::Size index{0U}; exact && (index < 7U); ++index)
    {
        exact = output.Sections[index].Reserved == 0U;
    }
    ASTERIA_EXPECT(runner, "the canonical AMD64 image yields exact owned page ranges", exact);
    ASTERIA_EXPECT(runner, "successful parsing leaves every loaded-image byte unchanged", ImageMatchesSnapshot());
}

void TestInputAndOutputBoundaries(TestContext& runner) noexcept
{
    Amd64LoadedImageLayout output = LayoutSentinel();
    const Amd64LoadedImageLayout expectedOutput = output;
    ASTERIA_EXPECT(runner, "a null image is rejected without changing output",
                   Amd64LoadedImageLayoutParser::Parse(nullptr, ImageBytes, output) ==
                       Amd64LoadedImageLayoutError::NullImage,
                   LayoutsEqual(output, expectedOutput));

    ResetImage(g_image);
    output = LayoutSentinel();
    ASTERIA_EXPECT(
        runner, "image size bounds and page alignment are enforced atomically",
        Amd64LoadedImageLayoutParser::Parse(g_image, HeaderBytes - 1U, output) ==
            Amd64LoadedImageLayoutError::ImageSizeOutOfRange,
        Amd64LoadedImageLayoutParser::Parse(g_image, ImageBytes - 1U, output) ==
            Amd64LoadedImageLayoutError::ImageSizeOutOfRange,
        Amd64LoadedImageLayoutParser::Parse(g_image, Platform::Amd64::Firmware::MaximumAmd64LoadedImageBytes + 0x1000U,
                                            output) == Amd64LoadedImageLayoutError::ImageSizeOutOfRange,
        LayoutsEqual(output, expectedOutput));

    constexpr Foundation::UIntPointer maximumAlignedAddress{~Foundation::UIntPointer{0U} &
                                                            ~Foundation::UIntPointer{0x0FFFU}};
    output = LayoutSentinel();
    ASTERIA_EXPECT(runner, "the runtime image extent is page-aligned and non-wrapping",
                   Amd64LoadedImageLayoutParser::Parse(g_image + 1U, ImageBytes, output) ==
                       Amd64LoadedImageLayoutError::InvalidImageGeometry,
                   Amd64LoadedImageLayoutParser::Parse(reinterpret_cast<const void*>(maximumAlignedAddress), ImageBytes,
                                                       output) == Amd64LoadedImageLayoutError::InvalidImageGeometry,
                   LayoutsEqual(output, expectedOutput));

    auto* const aliasedOutput =
        ::new (static_cast<void*>(&g_outputAlias.Output)) Amd64LoadedImageLayout{LayoutSentinel()};
    const Amd64LoadedImageLayout expectedAliasedOutput = *aliasedOutput;
    ASTERIA_EXPECT(
        runner, "an output overlapping the image is rejected before image access",
        Amd64LoadedImageLayoutParser::Parse(static_cast<const void*>(&g_outputAlias), ImageBytes, *aliasedOutput) ==
            Amd64LoadedImageLayoutError::OutputAliasesImage,
        LayoutsEqual(*aliasedOutput, expectedAliasedOutput));

    g_outputInsideImage.Output = LayoutSentinel();
    const Amd64LoadedImageLayout expectedInsideOutput = g_outputInsideImage.Output;
    ASTERIA_EXPECT(runner, "an output beginning inside the image is rejected before image access",
                   Amd64LoadedImageLayoutParser::Parse(static_cast<const void*>(&g_outputInsideImage), ImageBytes,
                                                       g_outputInsideImage.Output) ==
                       Amd64LoadedImageLayoutError::OutputAliasesImage,
                   LayoutsEqual(g_outputInsideImage.Output, expectedInsideOutput));

    g_outputCrossesImageStart.Output = LayoutSentinel();
    const Amd64LoadedImageLayout expectedCrossingOutput = g_outputCrossesImageStart.Output;
    const auto* const crossedImage = reinterpret_cast<const Foundation::UInt8*>(&g_outputCrossesImageStart) + 4096U;
    ASTERIA_EXPECT(runner, "an output crossing the image start is rejected before image access",
                   Amd64LoadedImageLayoutParser::Parse(crossedImage, ImageBytes, g_outputCrossesImageStart.Output) ==
                       Amd64LoadedImageLayoutError::OutputAliasesImage,
                   LayoutsEqual(g_outputCrossesImageStart.Output, expectedCrossingOutput));

    ResetImage(g_outputBeforeImage.Image);
    g_outputBeforeImage.Output = LayoutSentinel();
    ASTERIA_EXPECT(
        runner, "an output ending exactly where the image begins is accepted",
        Amd64LoadedImageLayoutParser::Parse(g_outputBeforeImage.Image, ImageBytes, g_outputBeforeImage.Output) ==
            Amd64LoadedImageLayoutError::None,
        g_outputBeforeImage.Output.ImageBase == reinterpret_cast<Foundation::UIntPointer>(g_outputBeforeImage.Image));

    ResetImage(g_outputAfterImage.Image);
    g_outputAfterImage.Output = LayoutSentinel();
    ASTERIA_EXPECT(
        runner, "an output beginning exactly where the image ends is accepted",
        Amd64LoadedImageLayoutParser::Parse(g_outputAfterImage.Image, ImageBytes, g_outputAfterImage.Output) ==
            Amd64LoadedImageLayoutError::None,
        g_outputAfterImage.Output.ImageBase == reinterpret_cast<Foundation::UIntPointer>(g_outputAfterImage.Image));
}

void TestHeaderFailures(TestContext& runner) noexcept
{
    ResetImage(g_image);
    g_image[0U] = 0U;
    ASTERIA_EXPECT(runner, "the DOS signature is required", HasError(Amd64LoadedImageLayoutError::InvalidDosHeader));

    ResetImage(g_image);
    g_image[PeOffset] = 0U;
    ASTERIA_EXPECT(runner, "the PE signature is required", HasError(Amd64LoadedImageLayoutError::InvalidPeHeader));

    ResetImage(g_image);
    WriteUInt16(g_image, PeOffset + 4U, 0x0200U);
    ASTERIA_EXPECT(runner, "a non-AMD64 COFF machine is rejected",
                   HasError(Amd64LoadedImageLayoutError::InvalidCoffHeader));

    ResetImage(g_image);
    WriteUInt16(g_image, OptionalOffset, 0x010BU);
    ASTERIA_EXPECT(runner, "a non-PE32+ optional header is rejected",
                   HasError(Amd64LoadedImageLayoutError::InvalidOptionalHeader));

    ResetImage(g_image);
    WriteUInt16(g_image, OptionalOffset + 68U, 11U);
    ASTERIA_EXPECT(runner, "a non-EFI-application subsystem is rejected",
                   HasError(Amd64LoadedImageLayoutError::InvalidOptionalHeader));

    ResetImage(g_image);
    WriteUInt64(g_image, OptionalOffset + 24U, 0x0400'1000U);
    const bool preferredBase = HasError(Amd64LoadedImageLayoutError::InvalidImageGeometry);
    Foundation::UInt8* const relocatedImage = g_relocatedImage + 4096U;
    ResetImage(relocatedImage);
    const Foundation::UInt64 relocatedBase =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(relocatedImage));
    WriteUInt64(relocatedImage, OptionalOffset + 24U, relocatedBase);
    Amd64LoadedImageLayout relocatedOutput = LayoutSentinel();
    const bool relocatedBaseAccepted =
        (Amd64LoadedImageLayoutParser::Parse(relocatedImage, ImageBytes, relocatedOutput) ==
         Amd64LoadedImageLayoutError::None) &&
        (relocatedOutput.ImageBase == relocatedBase) && (relocatedOutput.ImageSize == ImageBytes);
    ResetImage(g_image);
    WriteUInt32(g_image, OptionalOffset + 56U, ImageBytes - 0x1000U);
    ASTERIA_EXPECT(runner, "preferred or loader-rebased ImageBase and loaded SizeOfImage are exact", preferredBase,
                   relocatedBaseAccepted, HasError(Amd64LoadedImageLayoutError::InvalidImageGeometry));
}

void TestSectionFailures(TestContext& runner) noexcept
{
    ResetImage(g_image);
    g_image[SectionTableOffset] = static_cast<Foundation::UInt8>('X');
    ASTERIA_EXPECT(runner, "all six canonical section names and their order are required",
                   HasError(Amd64LoadedImageLayoutError::InvalidSectionName));

    ResetImage(g_image);
    WriteUInt32(g_image, SectionTableOffset + 12U, 0x2000U);
    ASTERIA_EXPECT(runner, "virtual section gaps and overlaps are rejected",
                   HasError(Amd64LoadedImageLayoutError::InvalidSectionGeometry));

    ResetImage(g_image);
    WriteUInt32(g_image, SectionTableOffset + 36U, 0xE000'0020U);
    ASTERIA_EXPECT(runner, "a writable executable text section is rejected",
                   HasError(Amd64LoadedImageLayoutError::InvalidSectionProtection));

    ResetImage(g_image);
    WriteUInt32(g_image, SectionTableOffset + (3U * 40U) + 16U, 0x0200U);
    ASTERIA_EXPECT(runner, "BSS may not have raw backing",
                   HasError(Amd64LoadedImageLayoutError::InvalidSectionGeometry));

    ResetImage(g_image);
    WriteUInt32(g_image, SectionTableOffset + 16U, 0x1900U);
    WriteUInt32(g_image, OptionalOffset + 4U, 0x1900U);
    WriteUInt32(g_image, SectionTableOffset + (1U * 40U) + 20U, 0x1D00U);
    WriteUInt32(g_image, SectionTableOffset + (2U * 40U) + 20U, 0x1F00U);
    WriteUInt32(g_image, SectionTableOffset + (4U * 40U) + 20U, 0x2300U);
    WriteUInt32(g_image, SectionTableOffset + (5U * 40U) + 20U, 0x2500U);
    const bool misalignedRaw = HasError(Amd64LoadedImageLayoutError::InvalidSectionGeometry);
    ResetImage(g_image);
    WriteUInt32(g_image, SectionTableOffset + 16U, 0x1600U);
    WriteUInt32(g_image, OptionalOffset + 4U, 0x1600U);
    WriteUInt32(g_image, SectionTableOffset + (1U * 40U) + 20U, 0x1A00U);
    WriteUInt32(g_image, SectionTableOffset + (2U * 40U) + 20U, 0x1C00U);
    WriteUInt32(g_image, SectionTableOffset + (4U * 40U) + 20U, 0x2000U);
    WriteUInt32(g_image, SectionTableOffset + (5U * 40U) + 20U, 0x2200U);
    const bool truncatedRaw = HasError(Amd64LoadedImageLayoutError::InvalidSectionGeometry);
    ResetImage(g_image);
    WriteUInt32(g_image, SectionTableOffset + 20U, 0x0600U);
    const bool noncontiguousRaw = HasError(Amd64LoadedImageLayoutError::InvalidSectionGeometry);
    ResetImage(g_image);
    WriteUInt32(g_image, SectionTableOffset + 24U, 1U);
    ASTERIA_EXPECT(runner, "raw extents and reserved COFF section fields are canonical", misalignedRaw, truncatedRaw,
                   noncontiguousRaw, HasError(Amd64LoadedImageLayoutError::InvalidSectionGeometry));

    ResetImage(g_image);
    WriteUInt32(g_image, SectionTableOffset + (3U * 40U) + 8U, 0x1000U);
    WriteUInt32(g_image, SectionTableOffset + (4U * 40U) + 12U, 0x6000U);
    WriteUInt32(g_image, SectionTableOffset + (5U * 40U) + 12U, 0x7000U);
    WriteUInt32(g_image, SectionTableOffset + (6U * 40U) + 12U, 0x8000U);
    WriteUInt32(g_image, OptionalOffset + 112U + (1U * 8U), 0x6000U);
    WriteUInt32(g_image, OptionalOffset + 112U + (5U * 8U), 0x7000U);
    ASTERIA_EXPECT(runner, "the final virtual section extent must equal SizeOfImage",
                   HasError(Amd64LoadedImageLayoutError::InvalidImageGeometry));

    ResetImage(g_image);
    WriteUInt32(g_image, SectionTableOffset + (5U * 40U) + 8U, 0x1200U);
    WriteUInt32(g_image, SectionTableOffset + (5U * 40U) + 16U, 0x1200U);
    ASTERIA_EXPECT(runner, "a section extending past SizeOfImage retains section-error provenance",
                   HasError(Amd64LoadedImageLayoutError::InvalidSectionGeometry));
}

void TestDirectoryAndEntrypointFailures(TestContext& runner) noexcept
{
    ResetImage(g_image);
    WriteUInt32(g_image, OptionalOffset + 112U, 0x3000U);
    ASTERIA_EXPECT(runner, "unexpected PE data directories are rejected",
                   HasError(Amd64LoadedImageLayoutError::InvalidDirectory));

    ResetImage(g_image);
    WriteUInt32(g_image, OptionalOffset + 112U + (1U * 8U), 0x6000U);
    const bool importDirectory = HasError(Amd64LoadedImageLayoutError::InvalidDirectory);
    ResetImage(g_image);
    g_image[0x7000U] = 1U;
    const bool nonemptyImports = HasError(Amd64LoadedImageLayoutError::InvalidDirectory);
    ResetImage(g_image);
    WriteUInt32(g_image, OptionalOffset + 112U + (5U * 8U) + 4U, 0U);
    const bool emptyRelocations = HasError(Amd64LoadedImageLayoutError::InvalidDirectory);
    ResetImage(g_image);
    WriteUInt32(g_image, OptionalOffset + 112U + (5U * 8U) + 4U, 0x0200U);
    ASTERIA_EXPECT(runner, "import and relocation directories must match their exact sections", importDirectory,
                   nonemptyImports, emptyRelocations, HasError(Amd64LoadedImageLayoutError::InvalidDirectory));

    ResetImage(g_image);
    WriteUInt32(g_image, OptionalOffset + 16U, 0x3000U);
    const bool outsideText = HasError(Amd64LoadedImageLayoutError::InvalidEntrypoint);
    ResetImage(g_image);
    WriteUInt32(g_image, OptionalOffset + 16U, 0x1171U);
    const bool unalignedEntry = HasError(Amd64LoadedImageLayoutError::InvalidEntrypoint);
    ResetImage(g_image);
    WriteUInt32(g_image, OptionalOffset + 20U, 0x2000U);
    ASTERIA_EXPECT(runner, "entrypoint and BaseOfCode are bound to executable text", outsideText, unalignedEntry,
                   HasError(Amd64LoadedImageLayoutError::InvalidEntrypoint));
}

void TestCanonicalFieldFailures(TestContext& runner) noexcept
{
    ResetImage(g_image);
    g_image[1U] = static_cast<Foundation::UInt8>('X');
    ASTERIA_EXPECT(runner, "both DOS signature bytes are canonical",
                   HasError(Amd64LoadedImageLayoutError::InvalidDosHeader));

    ResetImage(g_image);
    constexpr Foundation::Size completePeHeaderBytes{PeSignatureSize + CoffHeaderSize + OptionalHeaderSize +
                                                     (6U * 40U)};
    for (Foundation::Size index{completePeHeaderBytes}; index > 0U; --index)
    {
        g_image[PeOffset + index] = g_image[PeOffset + index - 1U];
    }
    WriteUInt32(g_image, 60U, PeOffset + 1U);
    const bool unalignedPeOffset = HasError(Amd64LoadedImageLayoutError::InvalidPeHeader);
    ResetImage(g_image);
    g_image[PeOffset + 2U] = 1U;
    const bool unterminatedPeSignature = HasError(Amd64LoadedImageLayoutError::InvalidPeHeader);
    ASTERIA_EXPECT(runner, "the PE offset and complete four-byte signature are canonical", unalignedPeOffset,
                   unterminatedPeSignature);

    ResetImage(g_image);
    constexpr Foundation::Size truncatedOptionalOffset{ImageBytes - (PeSignatureSize + CoffHeaderSize)};
    for (Foundation::Size index{0U}; index < (PeSignatureSize + CoffHeaderSize); ++index)
    {
        g_image[truncatedOptionalOffset + index] = g_image[PeOffset + index];
    }
    WriteUInt32(g_image, 60U, truncatedOptionalOffset);
    ASTERIA_EXPECT(runner, "the complete optional header must be readable",
                   HasError(Amd64LoadedImageLayoutError::InvalidOptionalHeader));

    bool coffFields{true};
    ResetImage(g_image);
    WriteUInt16(g_image, PeOffset + 6U, 5U);
    coffFields = coffFields && HasError(Amd64LoadedImageLayoutError::InvalidCoffHeader);
    ResetImage(g_image);
    WriteUInt16(g_image, PeOffset + 20U, 239U);
    coffFields = coffFields && HasError(Amd64LoadedImageLayoutError::InvalidCoffHeader);
    ResetImage(g_image);
    WriteUInt16(g_image, PeOffset + 22U, 0x022FU);
    coffFields = coffFields && HasError(Amd64LoadedImageLayoutError::InvalidCoffHeader);
    ASTERIA_EXPECT(runner, "the mapping-relevant COFF fields are enforced", coffFields);

    bool optionalGeometry{true};
    ResetImage(g_image);
    WriteUInt32(g_image, OptionalOffset + 32U, 0x2000U);
    optionalGeometry = optionalGeometry && HasError(Amd64LoadedImageLayoutError::InvalidOptionalHeader);
    ResetImage(g_image);
    WriteUInt32(g_image, OptionalOffset + 36U, 0x0400U);
    optionalGeometry = optionalGeometry && HasError(Amd64LoadedImageLayoutError::InvalidOptionalHeader);
    ResetImage(g_image);
    WriteUInt32(g_image, OptionalOffset + 60U, 0x0600U);
    optionalGeometry = optionalGeometry && HasError(Amd64LoadedImageLayoutError::InvalidOptionalHeader);
    ResetImage(g_image);
    WriteUInt16(g_image, OptionalOffset + 70U, 0x0140U);
    optionalGeometry = optionalGeometry && HasError(Amd64LoadedImageLayoutError::InvalidOptionalHeader);
    ASTERIA_EXPECT(runner, "optional-header alignments, header size, and DLL flags are exact", optionalGeometry);

    ResetImage(g_image);
    WriteUInt32(g_image, OptionalOffset + 104U, 1U);
    const bool loaderFlags = HasError(Amd64LoadedImageLayoutError::InvalidOptionalHeader);
    ResetImage(g_image);
    WriteUInt32(g_image, OptionalOffset + 108U, 15U);
    ASTERIA_EXPECT(runner, "loader flags and directory count retain valid PE framing", loaderFlags,
                   HasError(Amd64LoadedImageLayoutError::InvalidOptionalHeader));

    ResetImage(g_image);
    WriteUInt64(g_image, OptionalOffset + 24U, 0U);
    const bool zeroPreferredBase = HasError(Amd64LoadedImageLayoutError::InvalidImageGeometry);
    ResetImage(g_largeImage);
    WriteUInt32(g_largeImage, OptionalOffset + 56U, LargeImageBytes);
    WriteUInt32(g_largeImage, SectionTableOffset + (3U * 40U) + 8U, 0x19000U);
    WriteUInt32(g_largeImage, SectionTableOffset + (4U * 40U) + 12U, 0x1E000U);
    WriteUInt32(g_largeImage, SectionTableOffset + (5U * 40U) + 12U, 0x1F000U);
    WriteUInt32(g_largeImage, OptionalOffset + 112U + (1U * 8U), 0x1E000U);
    WriteUInt32(g_largeImage, OptionalOffset + 112U + (5U * 8U), 0x1F000U);
    WriteUInt64(g_largeImage, OptionalOffset + 24U, 0xFFFF'FFFF'FFFF'0000ULL);
    Amd64LoadedImageLayout wrappingOutput = LayoutSentinel();
    const Amd64LoadedImageLayout expectedWrappingOutput = wrappingOutput;
    const bool wrappingPreferredExtent =
        (Amd64LoadedImageLayoutParser::Parse(g_largeImage, LargeImageBytes, wrappingOutput) ==
         Amd64LoadedImageLayoutError::InvalidImageGeometry) &&
        LayoutsEqual(wrappingOutput, expectedWrappingOutput);
    ResetImage(g_image);
    WriteUInt32(g_image, OptionalOffset + 56U, 0x1000U);
    Amd64LoadedImageLayout output = LayoutSentinel();
    const Amd64LoadedImageLayout expectedOutput = output;
    const bool undersizedLoadedImage = (Amd64LoadedImageLayoutParser::Parse(g_image, 0x1000U, output) ==
                                        Amd64LoadedImageLayoutError::InvalidImageGeometry) &&
                                       LayoutsEqual(output, expectedOutput);
    ASTERIA_EXPECT(runner, "preferred and loaded image extents are nonzero and non-wrapping", zeroPreferredBase,
                   wrappingPreferredExtent, undersizedLoadedImage);

    ResetImage(g_image);
    constexpr Foundation::Size latePeOffset{0x0300U};
    constexpr Foundation::Size peHeaderBytes{PeSignatureSize + CoffHeaderSize + OptionalHeaderSize};
    for (Foundation::Size index{0U}; index < peHeaderBytes; ++index)
    {
        g_image[latePeOffset + index] = g_image[PeOffset + index];
    }
    WriteUInt32(g_image, 60U, latePeOffset);
    ASTERIA_EXPECT(runner, "the section table must fit completely within loaded headers",
                   HasError(Amd64LoadedImageLayoutError::InvalidSectionTable));

    ResetImage(g_image);
    g_image[SectionTableOffset + 5U] = 1U;
    const bool namePadding = HasError(Amd64LoadedImageLayoutError::InvalidSectionName);
    ResetImage(g_image);
    WriteUInt32(g_image, SectionTableOffset + 8U, 0U);
    const bool zeroVirtualSize = HasError(Amd64LoadedImageLayoutError::InvalidSectionGeometry);
    ASTERIA_EXPECT(runner, "section-name padding and nonzero virtual extents are canonical", namePadding,
                   zeroVirtualSize);

    bool reservedSectionFields{true};
    constexpr Foundation::Size reservedOffsets[4U]{24U, 28U, 32U, 34U};
    for (const Foundation::Size offset : reservedOffsets)
    {
        ResetImage(g_image);
        if (offset < 32U)
        {
            WriteUInt32(g_image, SectionTableOffset + offset, 1U);
        }
        else
        {
            WriteUInt16(g_image, SectionTableOffset + offset, 1U);
        }
        reservedSectionFields = reservedSectionFields && HasError(Amd64LoadedImageLayoutError::InvalidSectionGeometry);
    }
    ASTERIA_EXPECT(runner, "all relocation and line-number section fields are zero", reservedSectionFields);

    ResetImage(g_image);
    WriteUInt32(g_image, SectionTableOffset + 16U, 0xFFFFF000U);
    WriteUInt32(g_image, OptionalOffset + 4U, 0xFFFFF000U);
    WriteUInt32(g_image, SectionTableOffset + (1U * 40U) + 12U, 0U);
    WriteUInt32(g_image, SectionTableOffset + (1U * 40U) + 20U, 0xFFFFF400U);
    WriteUInt32(g_image, SectionTableOffset + (2U * 40U) + 12U, 0x1000U);
    WriteUInt32(g_image, SectionTableOffset + (2U * 40U) + 20U, 0xFFFFF600U);
    WriteUInt32(g_image, SectionTableOffset + (3U * 40U) + 12U, 0x2000U);
    WriteUInt32(g_image, SectionTableOffset + (3U * 40U) + 8U, 0x5000U);
    WriteUInt32(g_image, SectionTableOffset + (4U * 40U) + 20U, 0xFFFFFA00U);
    WriteUInt32(g_image, SectionTableOffset + (5U * 40U) + 20U, 0xFFFFFC00U);
    ASTERIA_EXPECT(runner, "section-end arithmetic may not wrap the 32-bit RVA space",
                   HasError(Amd64LoadedImageLayoutError::InvalidSectionGeometry));

    ResetImage(g_image);
    WriteUInt32(g_image, OptionalOffset + 112U + (1U * 8U) + 4U, 23U);
    const bool importSize = HasError(Amd64LoadedImageLayoutError::InvalidDirectory);
    ResetImage(g_image);
    WriteUInt32(g_image, OptionalOffset + 112U + (5U * 8U), 0x7000U);
    const bool relocationRva = HasError(Amd64LoadedImageLayoutError::InvalidDirectory);
    ASTERIA_EXPECT(runner, "directory RVAs and sizes are bound to their exact sections", importSize, relocationRva);

    ResetImage(g_image);
    WriteUInt32(g_image, OptionalOffset + 16U, 0U);
    ASTERIA_EXPECT(runner, "the entrypoint may not precede executable text",
                   HasError(Amd64LoadedImageLayoutError::InvalidEntrypoint));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Amd64LoadedImageLayoutTests"};
    Asteria::Test::TestExactLayout(runner);
    Asteria::Test::TestInputAndOutputBoundaries(runner);
    Asteria::Test::TestHeaderFailures(runner);
    Asteria::Test::TestSectionFailures(runner);
    Asteria::Test::TestDirectoryAndEntrypointFailures(runner);
    Asteria::Test::TestCanonicalFieldFailures(runner);
    return runner.Finish();
}
