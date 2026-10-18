// SPDX-License-Identifier: Apache-2.0
// Fuzz harness for the IA-64 loaded-image layout parser (Ia64LoadedImageLayoutParser::Parse).
// Input: a geometry byte, then the image header bytes. The image is mapped for every input at a
// fixed 64-KiB-aligned address plus a page offset, so the page after its last page is unmapped; the
// rest of the image is zero. geometry bits 0-4: image size in pages minus one (1-32 pages) geometry
// bits 5-6: base offset in pages (nonzero breaks 64-KiB alignment) geometry bit 7: pass a size that
// is not a multiple of 4 KiB Properties checked for every input: - A null image is rejected as
// NullImage. - Parse reads nothing past the mapped image (the next page is unmapped), and parsing
// is deterministic. - A base without the loader's 64-KiB alignment is rejected. - An accepted
// layout is the canonical PE32+ profile of the image: the base and size are the arguments, the six
// sections carry the canonical names and purposes, start after the header page, are page aligned,
// contiguous, and end at the image end, and the entry point and base of code in the optional header
// name the.opd and.text sections.

#include "Platform/Ia64/Firmware/Ia64LoadedImageLayout.hpp"
#include "Platform/Ia64/Firmware/Ia64ImageRelocation.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/mman.h>

#include "Foundation/Types.hpp"

namespace
{
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UIntPointer;
using Asteria::Platform::Ia64::Firmware::Ia64LoadedImageBaseAlignmentBytes;
using Asteria::Platform::Ia64::Firmware::Ia64LoadedImageLayout;
using Asteria::Platform::Ia64::Firmware::Ia64LoadedImageLayoutError;
using Asteria::Platform::Ia64::Firmware::Ia64LoadedImageLayoutParser;
using Asteria::Platform::Ia64::Firmware::Ia64LoadedImageSectionCount;
using Asteria::Platform::Ia64::Firmware::Ia64LoadedImageSectionPurpose;

constexpr Size PageBytes{4096U};
constexpr UIntPointer ImageAreaAddress{0x4200'0000U};
constexpr UInt8 PageCountMask{0x1FU};
constexpr UInt8 UnalignedSizeFlag{1U << 7U};
constexpr Size PeOffset{0x40U};
constexpr Size OptionalOffset{PeOffset + 24U};
constexpr Size SectionTableOffset{OptionalOffset + 240U};

constexpr char SectionNames[Ia64LoadedImageSectionCount][8U]{".text", ".rodata", ".opd", ".reloc", ".data", ".bss"};
constexpr Ia64LoadedImageSectionPurpose SectionPurposes[Ia64LoadedImageSectionCount]{
    Ia64LoadedImageSectionPurpose::Code,         Ia64LoadedImageSectionPurpose::ReadOnlyData,
    Ia64LoadedImageSectionPurpose::ReadOnlyData, Ia64LoadedImageSectionPurpose::ReadOnlyData,
    Ia64LoadedImageSectionPurpose::WritableData, Ia64LoadedImageSectionPurpose::WritableData,
};

constexpr Size MaximumImageBytes{32U * PageBytes};
constinit UInt8 g_seed[MaximumImageBytes]{};
constinit UInt8 g_seedInput[1U + 0x400U + 16U + 16U + 64U]{};
alignas(Asteria::Platform::Ia64::Firmware::Ia64RelocatedImageAlignmentBytes) constinit UInt8
    g_destination[MaximumImageBytes]{};
constinit UInt8 g_sourceSnapshot[MaximumImageBytes]{};
constinit UInt8 g_destinationSnapshot[MaximumImageBytes]{};

[[noreturn]] void Violation(const char* const property)
{
    static_cast<void>(std::fprintf(stderr, "FUZZ PROPERTY VIOLATED: %s\n", property));
    std::abort();
}

[[nodiscard]] UInt32 ReadUInt32(const UInt8* const bytes) noexcept
{
    return static_cast<UInt32>(bytes[0U]) | (static_cast<UInt32>(bytes[1U]) << 8U) |
           (static_cast<UInt32>(bytes[2U]) << 16U) | (static_cast<UInt32>(bytes[3U]) << 24U);
}

void WriteLittleEndian(UInt8* const bytes, const UInt32 value, const Size byteCount) noexcept
{
    for (Size index{0U}; index < byteCount; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

void CheckLayout(const UInt8* const image, const Size imageSize, const Ia64LoadedImageLayout& layout) noexcept
{
    const UInt64 base = static_cast<UInt64>(reinterpret_cast<UIntPointer>(image));
    if ((layout.ImageBase != base) || (layout.ImageSize != imageSize) ||
        ((base & (Ia64LoadedImageBaseAlignmentBytes - 1U)) != 0U) || ((imageSize & (PageBytes - 1U)) != 0U))
    {
        Violation("an accepted layout describes the aligned image it was given");
    }
    // The parser proved that the headers lie inside the image; the canonical
    // profile also keeps them inside the 1-KiB header area.
    const Size optionalOffset = ReadUInt32(image + 60U) + 24U;
    if ((optionalOffset + 240U + (Ia64LoadedImageSectionCount * 40U)) > 0x400U)
    {
        Violation("the headers and section table fit in the 1-KiB header area");
    }
    const UInt8* const sectionTable = image + optionalOffset + 240U;
    UInt64 expectedStart = layout.Sections[0U].VirtualStart;
    if ((expectedStart < (base + PageBytes)) || ((expectedStart & (PageBytes - 1U)) != 0U))
    {
        Violation("the first section starts on a page after the header page");
    }
    for (Size index{0U}; index < Ia64LoadedImageSectionCount; ++index)
    {
        const auto& section = layout.Sections[index];
        if ((section.VirtualStart != expectedStart) || (section.PageCount == 0U) ||
            (section.Purpose != SectionPurposes[index]) || (section.Reserved != 0U) ||
            (std::memcmp(sectionTable + (index * 40U), SectionNames[index], 8U) != 0))
        {
            Violation("sections are the canonical named sections in order, contiguous and nonempty");
        }
        expectedStart += section.PageCount * PageBytes;
    }
    const UInt32 entryRva = ReadUInt32(image + optionalOffset + 16U);
    const UInt32 codeRva = ReadUInt32(image + optionalOffset + 20U);
    if ((expectedStart != (base + imageSize)) || ((base + entryRva) != layout.Sections[2U].VirtualStart) ||
        ((entryRva & 0x0FU) != 0U) || ((base + codeRva) != layout.Sections[0U].VirtualStart))
    {
        Violation("sections end at the image end and the entry point names the .opd section");
    }
}

// Writes the headers of a canonical image whose sections have the given
// virtual sizes; each section starts on the page after the previous one.
void BuildImage(const UInt32* const virtualSizes, const UInt32 textStart, const UInt32 peOffset = PeOffset) noexcept
{
    std::memset(g_seed, 0, sizeof(g_seed));
    g_seed[0U] = static_cast<UInt8>('M');
    g_seed[1U] = static_cast<UInt8>('Z');
    WriteLittleEndian(g_seed + 60U, peOffset, 4U);
    std::memcpy(g_seed + peOffset, "PE\0\0", 4U);
    WriteLittleEndian(g_seed + peOffset + 4U, 0x0200U, 2U);
    WriteLittleEndian(g_seed + peOffset + 6U, Ia64LoadedImageSectionCount, 2U);
    WriteLittleEndian(g_seed + peOffset + 20U, 240U, 2U);
    WriteLittleEndian(g_seed + peOffset + 22U, 0x022EU, 2U);
    UInt8* const optional = g_seed + peOffset + 24U;
    WriteLittleEndian(optional, 0x020BU, 2U);
    WriteLittleEndian(optional + 32U, 0x1000U, 4U);
    WriteLittleEndian(optional + 36U, 0x0200U, 4U);
    WriteLittleEndian(optional + 60U, 0x0400U, 4U);
    WriteLittleEndian(optional + 68U, 10U, 2U);
    WriteLittleEndian(optional + 70U, 0x0160U, 2U);
    WriteLittleEndian(optional + 108U, 16U, 4U);
    constexpr UInt32 Characteristics[Ia64LoadedImageSectionCount]{0x6000'0020U, 0x4000'0040U, 0x4000'0040U,
                                                                  0x4000'0040U, 0xC000'0040U, 0xC000'0080U};
    UInt32 start{textStart};
    for (Size index{0U}; index < Ia64LoadedImageSectionCount; ++index)
    {
        UInt8* const section = optional + 240U + (index * 40U);
        const UInt32 rawSize = index == 5U ? 0U : (virtualSizes[index] + 0x1FFU) & ~UInt32{0x1FFU};
        std::memcpy(section, SectionNames[index], 8U);
        WriteLittleEndian(section + 8U, virtualSizes[index], 4U);
        WriteLittleEndian(section + 12U, start, 4U);
        WriteLittleEndian(section + 16U, rawSize, 4U);
        WriteLittleEndian(section + 36U, Characteristics[index], 4U);
        if (index == 0U)
        {
            WriteLittleEndian(optional + 20U, start, 4U);
        }
        if (index == 2U)
        {
            WriteLittleEndian(optional + 16U, start, 4U);
        }
        const UInt32 loaded = virtualSizes[index] > rawSize ? virtualSizes[index] : rawSize;
        start += (loaded + 0xFFFU) & ~UInt32{0xFFFU};
    }
    WriteLittleEndian(optional + 56U, start, 4U);
}

void EmitImage(void (*const emit)(const std::uint8_t* data, std::size_t size), const UInt8 geometry) noexcept
{
    g_seedInput[0U] = geometry;
    std::memcpy(g_seedInput + 1U, g_seed, 0x400U);
    const Size optional = ReadUInt32(g_seed + 60U) + 24U;
    const Size table = optional + 240U;
    const Size text = ReadUInt32(g_seed + table + 12U);
    const Size opd = ReadUInt32(g_seed + table + 2U * 40U + 12U);
    const Size reloc = ReadUInt32(g_seed + table + 3U * 40U + 12U);
    std::memcpy(g_seedInput + 0x401U, g_seed + text, 16U);
    std::memcpy(g_seedInput + 0x411U, g_seed + opd, 16U);
    std::memcpy(g_seedInput + 0x421U, g_seed + reloc, 64U);
    emit(g_seedInput, sizeof(g_seedInput));
}

[[nodiscard]] UInt8 PagesOf(const UInt32 sizeOfImage) noexcept
{
    return static_cast<UInt8>((sizeOfImage / PageBytes) - 1U);
}
} // namespace

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    constexpr UInt32 SmallSections[Ia64LoadedImageSectionCount]{0x1800U, 0x0400U, 0x0010U, 0x0018U, 0x0008U, 0x2000U};
    constexpr UInt32 LargeSections[Ia64LoadedImageSectionCount]{0xF800U, 0x0100U, 0x0020U, 0x0200U, 0x1000U, 0x0800U};
    BuildImage(SmallSections, 0x1000U);
    const UInt8 smallPages = PagesOf(ReadUInt32(g_seed + OptionalOffset + 56U));
    EmitImage(emit, smallPages);
    EmitImage(emit, static_cast<UInt8>(smallPages | (1U << 5U)));
    EmitImage(emit, static_cast<UInt8>(smallPages | UnalignedSizeFlag));
    EmitImage(emit, static_cast<UInt8>(smallPages + 1U));
    BuildImage(LargeSections, 0x2000U);
    EmitImage(emit, PagesOf(ReadUInt32(g_seed + OptionalOffset + 56U)));
    // The latest PE header offset whose section table still ends inside the
    // 1-KiB header area, and the first one past it.
    constexpr UInt32 LatestPeOffset{0x400U - 24U - 240U - (Ia64LoadedImageSectionCount * 40U)};
    BuildImage(SmallSections, 0x1000U, LatestPeOffset);
    EmitImage(emit, smallPages);
    BuildImage(SmallSections, 0x1000U, LatestPeOffset + 4U);
    EmitImage(emit, smallPages);
    // A complete relocation stream, including both descriptor words and
    // an MLX immediate, exercises successful copies and fixup mutations.
    BuildImage(SmallSections, 0x1000U);
    WriteLittleEndian(g_seed + OptionalOffset + 152U, 0x5000U, 4U);
    WriteLittleEndian(g_seed + OptionalOffset + 156U, 24U, 4U);
    WriteLittleEndian(g_seed + SectionTableOffset + 3U * 40U + 8U, 24U, 4U);
    g_seed[0x1000U] = 5U;
    g_seed[0x100FU] = 0x60U;
    WriteLittleEndian(g_seed + 0x4000U, static_cast<UInt32>(ImageAreaAddress + 0x1000U), 4U);
    WriteLittleEndian(g_seed + 0x4008U, static_cast<UInt32>(ImageAreaAddress + 0x3000U), 4U);
    WriteLittleEndian(g_seed + 0x5000U, 0x1000U, 4U);
    WriteLittleEndian(g_seed + 0x5004U, 12U, 4U);
    WriteLittleEndian(g_seed + 0x5008U, 0x9001U, 2U);
    WriteLittleEndian(g_seed + 0x500CU, 0x4000U, 4U);
    WriteLittleEndian(g_seed + 0x5010U, 12U, 4U);
    WriteLittleEndian(g_seed + 0x5014U, 0xA000U, 2U);
    WriteLittleEndian(g_seed + 0x5016U, 0xA008U, 2U);
    EmitImage(emit, smallPages);
    // Near misses, each rejected by one rule of the canonical profile.
    BuildImage(SmallSections, 0x1000U);
    WriteLittleEndian(g_seed + OptionalOffset + 16U, ReadUInt32(g_seed + OptionalOffset + 16U) + 8U, 4U);
    EmitImage(emit, smallPages);
    BuildImage(SmallSections, 0x1000U);
    WriteLittleEndian(g_seed + SectionTableOffset + (5U * 40U) + 16U, 0x200U, 4U);
    EmitImage(emit, smallPages);
    BuildImage(SmallSections, 0x1000U);
    std::memcpy(g_seed + SectionTableOffset + 40U, ".opd\0\0\0\0", 8U);
    EmitImage(emit, smallPages);
    BuildImage(SmallSections, 0x1000U);
    WriteLittleEndian(g_seed + SectionTableOffset + (4U * 40U) + 36U, 0x4000'0040U, 4U);
    EmitImage(emit, smallPages);
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    const UInt8 geometry = size != 0U ? data[0U] : UInt8{0U};
    const Size mappedBytes = (static_cast<Size>(geometry & PageCountMask) + 1U) * PageBytes;
    const Size imageSize = (geometry & UnalignedSizeFlag) != 0U ? mappedBytes - 0x200U : mappedBytes;
    const Size baseOffset = static_cast<Size>((geometry >> 5U) & 3U) * PageBytes;
    void* const hint = reinterpret_cast<void*>(ImageAreaAddress + baseOffset);
    void* const memory = mmap(hint, mappedBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory != hint)
    {
        static_cast<void>(std::fprintf(stderr, "FAIL: the image is not mapped at its fixed address\n"));
        std::abort();
    }
    auto* const image = static_cast<UInt8*>(memory);
    const Size headerBytes = size > 1U ? size - 1U : 0U;
    std::memcpy(image, data + (size != 0U ? 1U : 0U), headerBytes < imageSize ? headerBytes : imageSize);

    const auto absent = Ia64LoadedImageLayoutParser::Parse(nullptr, imageSize);
    if (absent.HasValue() || (absent.GetError() != Ia64LoadedImageLayoutError::NullImage))
    {
        Violation("a null image is rejected as NullImage");
    }
    const auto result = Ia64LoadedImageLayoutParser::Parse(image, imageSize);
    const auto repeated = Ia64LoadedImageLayoutParser::Parse(image, imageSize);
    // Ia64LoadedImageLayout has no padding (pinned by static_assert).
    if ((result.HasValue() != repeated.HasValue()) ||
        (result.HasValue() ? (std::memcmp(&result.GetValue(), &repeated.GetValue(), sizeof(Ia64LoadedImageLayout)) != 0)
                           : (result.GetError() != repeated.GetError())))
    {
        Violation("parsing is deterministic");
    }
    if (result.HasValue())
    {
        if (baseOffset != 0U)
        {
            Violation("a base without 64-KiB alignment is rejected");
        }
        CheckLayout(image, imageSize, result.GetValue());
    }
    // Compact body controls fit the shared replay driver's 4 KiB seed bound.
    // Stage them only after the parser proves all three complete page extents.
    if (result.HasValue() && (size >= sizeof(g_seedInput)))
    {
        const UInt64 base = reinterpret_cast<UIntPointer>(image);
        const auto& layout = result.GetValue();
        std::memcpy(image + (layout.Sections[0U].VirtualStart - base), data + 0x401U, 16U);
        std::memcpy(image + (layout.Sections[2U].VirtualStart - base), data + 0x411U, 16U);
        std::memcpy(image + (layout.Sections[3U].VirtualStart - base), data + 0x421U, 64U);
    }
    std::memcpy(g_sourceSnapshot, image, imageSize);
    std::memset(g_destination, 0xA5, imageSize);
    const auto copy = Asteria::Platform::Ia64::Firmware::Ia64ImageRelocation::RelocateToAlignment(
        image, imageSize, g_destination, sizeof(g_destination));
    if (std::memcmp(g_sourceSnapshot, image, imageSize) != 0)
    {
        Violation("relocation never changes the source image");
    }
    if (!copy.HasValue())
    {
        for (Size index{0U}; index < imageSize; ++index)
        {
            if (g_destination[index] != 0xA5U)
            {
                Violation("relocation rejection leaves the destination byte-identical");
            }
        }
    }
    std::memcpy(g_destinationSnapshot, g_destination, imageSize);
    const auto copyAgain = Asteria::Platform::Ia64::Firmware::Ia64ImageRelocation::RelocateToAlignment(
        image, imageSize, g_destination, sizeof(g_destination));
    if ((copy.HasValue() != copyAgain.HasValue()) || (copy.GetError() != copyAgain.GetError()) ||
        (std::memcmp(g_destinationSnapshot, g_destination, imageSize) != 0))
    {
        Violation("relocation and its output bytes are deterministic");
    }
    if (munmap(memory, mappedBytes) != 0)
    {
        static_cast<void>(std::fprintf(stderr, "FAIL: cannot unmap the image\n"));
        std::abort();
    }
    return 0;
}
