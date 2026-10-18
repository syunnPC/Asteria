// SPDX-License-Identifier: Apache-2.0
#include "User/Loader/ExecutableFormats.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Support/StaticPieImage.hpp"
#include "Support/TestContext.hpp"

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Foundation::UInt8;
using TestSupport::PieLoad;
using TestSupport::PieRules;
using TestSupport::PieStore;
using TestSupport::StaticPieImage;
using TestSupport::TestContext;
using User::Loader::ApplyExecutableRelocations;
using User::Loader::DescribeExecutable;
using User::Loader::ExecutableImage;
using User::Loader::ExecutableKind;
using User::Loader::LoaderError;
using User::Loader::RelocationWriter;

constexpr Size SegmentCapacity{32U * 4096U};
struct Staging
{
    UInt8 Bytes[9U][SegmentCapacity]{};
    Size Writes{};
    Size FailAt{~Size{0U}};
};
Staging g_first;
Staging g_second;

[[nodiscard]] bool Write(void* const context, const Size segment, const UInt64 offset, const UInt8* const bytes,
                         const Size count) noexcept
{
    auto& staging = *static_cast<Staging*>(context);
    if ((segment >= 9U) || (offset > SegmentCapacity) || (count > SegmentCapacity - offset) ||
        (staging.Writes == staging.FailAt))
    {
        return false;
    }
    ++staging.Writes;
    std::memcpy(staging.Bytes[segment] + offset, bytes, count);
    return true;
}

void Stage(const UInt8* const bytes, const ExecutableImage& image, Staging& staging) noexcept
{
    staging.Writes = 0U;
    staging.FailAt = ~Size{0U};
    std::memset(staging.Bytes, 0, sizeof(staging.Bytes));
    for (Size index{0U}; index < image.SegmentCount; ++index)
    {
        const auto& segment = image.Segments[index];
        std::memcpy(staging.Bytes[index] + segment.PageOffset, bytes + segment.FileOffset, segment.FileBytes);
    }
}

[[nodiscard]] RelocationWriter Writer(Staging& staging, const UInt64 descriptorAddress = 0U) noexcept
{
    return RelocationWriter{.Context = &staging,
                            .Write = Write,
                            .DescriptorAddress = descriptorAddress,
                            .DescriptorBytes = (descriptorAddress == 0U) ? 0U : 4096U};
}

[[nodiscard]] UInt64 Word(const ExecutableImage& image, const Staging& staging, const UInt64 address) noexcept
{
    for (Size index{0U}; index < image.SegmentCount; ++index)
    {
        const auto& segment = image.Segments[index];
        if ((address >= segment.VirtualStart) && (address - segment.VirtualStart < segment.Pages * 4096U))
        {
            return PieLoad(staging.Bytes[index] + address - segment.VirtualStart);
        }
    }
    return ~UInt64{0U};
}

void TestRealSample(TestContext& context, const bool ia64) noexcept
{
    const char* const directory = std::getenv("ASTERIA_PIE_SAMPLE_DIR");
    ASTERIA_EXPECT(context, "real sample directory is configured", directory != nullptr);
    if (directory == nullptr)
    {
        return;
    }
    char path[1024U]{};
    const int length = std::snprintf(path, sizeof(path), "%s/%s.elf", directory, ia64 ? "ia64" : "amd64");
    ASTERIA_EXPECT(context, "sample path fits", length > 0, static_cast<Size>(length) < sizeof(path));
    std::FILE* const file = std::fopen(path, "rb");
    ASTERIA_EXPECT(context, "real toolchain sample exists", file != nullptr);
    if (file == nullptr)
    {
        return;
    }
    UInt8 bytes[65536U]{};
    const Size count = std::fread(bytes, 1U, sizeof(bytes), file);
    const bool closed = std::fclose(file) == 0;
    ExecutableImage image{};
    const LoaderError error = DescribeExecutable(bytes, count, PieRules(ia64), image);
    ASTERIA_EXPECT(context, "GNU ld static PIE is described", closed, count < sizeof(bytes), error == LoaderError::None,
                   image.Identity.Kind == ExecutableKind::Relocatable, image.Relocations.Bytes >= 72U,
                   image.Span.Start == 0U, image.Alignment >= 4096U);
    if (error != LoaderError::None)
    {
        return;
    }
    const UInt64 firstBase = ia64 ? 0x2000'0000'0100'0000ULL : 0x4000'0000U;
    const UInt64 delta{0x20'0000U};
    Stage(bytes, image, g_first);
    Stage(bytes, image, g_second);
    const auto first = ApplyExecutableRelocations(bytes, count, image, firstBase, Writer(g_first));
    const auto second = ApplyExecutableRelocations(bytes, count, image, firstBase + delta, Writer(g_second));
    ASTERIA_EXPECT(context, "real samples relocate at two bases with relocated entry and gp",
                   first.Error == LoaderError::None, second.Error == LoaderError::None,
                   first.Entry == firstBase + image.Entry, second.Entry - first.Entry == delta,
                   ia64 ? (second.GlobalPointer - first.GlobalPointer == delta) : first.GlobalPointer == 0U,
                   ia64 ? first.EntryDescriptor == firstBase + image.EntryDescriptor : first.EntryDescriptor == 0U);
    for (UInt64 offset{0U}; offset < image.Relocations.Bytes; offset += 24U)
    {
        const UInt8* const rela = bytes + image.Relocations.FileOffset + offset;
        const UInt64 address = PieLoad(rela);
        const UInt64 addend = PieLoad(rela + 16U);
        ASTERIA_EXPECT(context, "every relocated word equals bias plus addend and differs exactly by delta",
                       Word(image, g_first, address) == firstBase + addend,
                       Word(image, g_second, address) - Word(image, g_first, address) == delta);
        if ((PieLoad(rela + 8U) & 0xFFFF'FFFFU) == 0x81U)
        {
            ASTERIA_EXPECT(context, "GNU ld .opd receives the correct relocated gp",
                           Word(image, g_first, address + 8U) == first.GlobalPointer,
                           Word(image, g_second, address + 8U) - Word(image, g_first, address + 8U) == delta);
        }
    }
    if (ia64)
    {
        ASTERIA_EXPECT(context, "Asteria entry descriptor is staged with its relocated bundle and gp",
                       Word(image, g_first, image.EntryDescriptor) == first.Entry,
                       Word(image, g_first, image.EntryDescriptor + 8U) == first.GlobalPointer);
    }
    else
    {
        ASTERIA_EXPECT(context, "AMD64 GNU ld RELRO is retained for the mapper", image.Relro.Bytes != 0U);
    }
}

void TestSymbols(TestContext& context) noexcept
{
    StaticPieImage sample{true};
    sample.Tag(1U, 8U, 72U);
    sample.Tag(3U, 0x6FFF'FFF9U, 0U);
    sample.Rela(0U, 0x2B20U, (UInt64{1U} << 32U) | 0x47U, 0U);
    sample.Rela(1U, 0x2B28U, (UInt64{1U} << 32U) | 0x47U, 0U);
    sample.Rela(2U, 0x2B30U, (UInt64{1U} << 32U) | 0x27U, 16U);
    ExecutableImage image{};
    const auto error = DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(true), image);
    ASTERIA_EXPECT(context, "defined local FPTR and DIR symbols are described", error == LoaderError::None);
    if (error != LoaderError::None)
    {
        return;
    }
    constexpr UInt64 base{0x2000'0000'0100'0000ULL};
    constexpr UInt64 descriptors{base + 0x10'0000U};
    Stage(sample.Bytes, image, g_first);
    auto result = ApplyExecutableRelocations(sample.Bytes, sizeof(sample.Bytes), image, base, Writer(g_first));
    ASTERIA_EXPECT(context, "FPTR without descriptor capacity refuses before writing",
                   result.Error == LoaderError::ImageTooLarge, g_first.Writes == 0U);
    result = ApplyExecutableRelocations(sample.Bytes, sizeof(sample.Bytes), image, base, Writer(g_first, descriptors));
    ASTERIA_EXPECT(context, "FPTR uses one canonical entry slot and DIR honors its signed addend",
                   result.Error == LoaderError::None, Word(image, g_first, 0x2B20U) == descriptors,
                   Word(image, g_first, 0x2B28U) == descriptors, Word(image, g_first, 0x2B30U) == base + 0x310U,
                   PieLoad(g_first.Bytes[image.SegmentCount]) == base + 0x300U,
                   PieLoad(g_first.Bytes[image.SegmentCount] + 8U) == base + 0x2C00U);
    constexpr UInt64 delta{0x20'0000U};
    Stage(sample.Bytes, image, g_second);
    const auto second = ApplyExecutableRelocations(sample.Bytes, sizeof(sample.Bytes), image, base + delta,
                                                   Writer(g_second, descriptors + delta));
    ASTERIA_EXPECT(context, "local DIR and caller-area FPTR words also differ exactly by the base delta",
                   second.Error == LoaderError::None, second.Entry - result.Entry == delta,
                   second.GlobalPointer - result.GlobalPointer == delta,
                   Word(image, g_second, 0x2B20U) - Word(image, g_first, 0x2B20U) == delta,
                   Word(image, g_second, 0x2B28U) - Word(image, g_first, 0x2B28U) == delta,
                   Word(image, g_second, 0x2B30U) - Word(image, g_first, 0x2B30U) == delta,
                   PieLoad(g_second.Bytes[image.SegmentCount]) - PieLoad(g_first.Bytes[image.SegmentCount]) == delta,
                   PieLoad(g_second.Bytes[image.SegmentCount] + 8U) - PieLoad(g_first.Bytes[image.SegmentCount] + 8U) ==
                       delta);
    sample.Rela(1U, 0x2B28U, (UInt64{1U} << 32U) | 0x47U, 16U);
    ASTERIA_EXPECT(context, "FPTR with a nonzero addend describes another aligned code entry",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(true), image) == LoaderError::None);
    Stage(sample.Bytes, image, g_first);
    result = ApplyExecutableRelocations(sample.Bytes, sizeof(sample.Bytes), image, base, Writer(g_first, descriptors));
    ASTERIA_EXPECT(context, "FPTR addends get distinct canonical descriptors with the image's gp",
                   result.Error == LoaderError::None, Word(image, g_first, 0x2B20U) == descriptors,
                   Word(image, g_first, 0x2B28U) == descriptors + 16U,
                   PieLoad(g_first.Bytes[image.SegmentCount] + 16U) == base + 0x310U,
                   PieLoad(g_first.Bytes[image.SegmentCount] + 24U) == result.GlobalPointer);
    sample.Rela(1U, 0x2B28U, (UInt64{1U} << 32U) | 0x47U, 0U);
    sample.Tag(1U, 8U, 96U);
    sample.Rela(3U, 0x2B40U, 0x81U, 0x300U);
    ASTERIA_EXPECT(context, "an image-owned .opd descriptor is admitted",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(true), image) == LoaderError::None);
    Stage(sample.Bytes, image, g_first);
    result = ApplyExecutableRelocations(sample.Bytes, sizeof(sample.Bytes), image, base, Writer(g_first));
    ASTERIA_EXPECT(context, "FPTR reuses GNU ld's image-owned descriptor", result.Error == LoaderError::None,
                   Word(image, g_first, 0x2B20U) == base + 0x2B40U, Word(image, g_first, 0x2B40U) == base + 0x300U,
                   Word(image, g_first, 0x2B48U) == base + 0x2C00U);
    PieStore(sample.Bytes + 24U, 0x2B40U);
    ASTERIA_EXPECT(context, "an initially zero .opd entry is described through its IPLTLSB initializer",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(true), image) == LoaderError::None,
                   image.Entry == 0x300U, image.EntryDescriptor == 0x2B40U);
    Stage(sample.Bytes, image, g_first);
    result = ApplyExecutableRelocations(sample.Bytes, sizeof(sample.Bytes), image, base, Writer(g_first));
    ASTERIA_EXPECT(context, "the relocated entry descriptor reports its executable bundle and gp",
                   result.Error == LoaderError::None, result.EntryDescriptor == base + 0x2B40U,
                   Word(image, g_first, image.EntryDescriptor) == result.Entry,
                   Word(image, g_first, image.EntryDescriptor + 8U) == result.GlobalPointer);
}

void TestDynamicRefusals(TestContext& context) noexcept
{
    struct Mutation
    {
        Size Offset;
        UInt64 Value;
        LoaderError Error;
    };
    constexpr Mutation mutations[]{
        {.Offset = 0x800U, .Value = 22U, .Error = LoaderError::UnsupportedFormat},     // TEXTREL
        {.Offset = 0x800U, .Value = 17U, .Error = LoaderError::UnsupportedFormat},     // REL
        {.Offset = 0x800U, .Value = 1U, .Error = LoaderError::UnsupportedFormat},      // NEEDED
        {.Offset = 0x840U, .Value = 30U, .Error = LoaderError::UnsupportedFormat},     // DF_TEXTREL (flags value 8+4)
        {.Offset = 0x848U, .Value = 4U, .Error = LoaderError::UnsupportedFormat},      // unknown FLAGS_1 bit
        {.Offset = 0x808U, .Value = ~UInt64{0U}, .Error = LoaderError::Malformed},     // RELA pointer
        {.Offset = 0x818U, .Value = ~UInt64{0U}, .Error = LoaderError::Malformed},     // RELASZ
        {.Offset = 0x828U, .Value = 16U, .Error = LoaderError::Malformed},             // RELAENT
        {.Offset = 0x838U, .Value = 2U, .Error = LoaderError::Malformed},              // RELACOUNT
        {.Offset = 0x908U, .Value = 0xFFFFU, .Error = LoaderError::UnsupportedFormat}, // unknown relocation
        {.Offset = 0x900U, .Value = 0x2FFFU, .Error = LoaderError::Malformed}, // unaligned and crosses memory end
        {.Offset = 0x900U, .Value = 0x3000U, .Error = LoaderError::Malformed}, // page padding is not segment bytes
        {.Offset = 0x900U, .Value = 0x300U, .Error = LoaderError::UnsupportedFormat}, // text relocation
        {.Offset = 0x858U, .Value = ~UInt64{0U}, .Error = LoaderError::Malformed},    // SYMTAB
        {.Offset = 0x878U, .Value = ~UInt64{0U}, .Error = LoaderError::Malformed},    // STRTAB
        {.Offset = 0x868U, .Value = 16U, .Error = LoaderError::Malformed},            // SYMENT
    };
    for (const Mutation& mutation : mutations)
    {
        StaticPieImage sample;
        if (mutation.Offset == 0x840U)
        {
            sample.Tag(4U, 30U, 12U);
        }
        PieStore(sample.Bytes + mutation.Offset, mutation.Value);
        ExecutableImage image{};
        ASTERIA_EXPECT(context, "unsupported dynamic behavior and malformed ranges fail atomically",
                       DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(false), image) == mutation.Error,
                       image.SegmentCount == 0U, image.Relocations.Bytes == 0U,
                       (mutation.Offset != 0x908U) || (image.RejectedRelocationType == 0xFFFFU));
    }
    StaticPieImage sample;
    ExecutableImage image{};
    sample.Header(2U, 3U, 4U, 0x800U, 0x2800U, 16U, 16U);
    ASTERIA_EXPECT(context, "PT_INTERP stays refused",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(false), image) ==
                       LoaderError::UnsupportedFormat);
    sample = StaticPieImage{};
    sample.Tag(10U, 23U, 0x2900U);
    sample.Tag(11U, 2U, 24U);
    sample.Tag(12U, 20U, 17U);
    ASTERIA_EXPECT(context, "REL PLT tables stay refused even with NOW",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(false), image) ==
                       LoaderError::UnsupportedFormat);
    sample.Tag(4U, 0x6FFF'FFFBU, 0x0800'0000U);
    sample.Tag(12U, 20U, 7U);
    ASTERIA_EXPECT(context, "nonempty PLT requires eager binding",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(false), image) ==
                       LoaderError::UnsupportedFormat);
    sample = StaticPieImage{};
    sample.Header(2U, 2U, 6U, 0x800U, 0x2800U, 0x100U, 0x100U);
    for (Size index{10U}; index < 16U; ++index)
    {
        sample.Tag(index, 21U, 0U);
    }
    ASTERIA_EXPECT(context, "dynamic traversal requires bounded DT_NULL",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(false), image) ==
                       LoaderError::Malformed);
    sample = StaticPieImage{};
    PieStore(sample.Bytes + 56U, 4U, 2U);
    sample.Header(3U, 7U, 4U, 0U, 0U, 0U, 1U);
    ASTERIA_EXPECT(context, "nonempty PT_TLS is deferred",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(false), image) ==
                       LoaderError::UnsupportedFormat);
    sample.Header(3U, 7U, 4U, 0U, 0U, 0U, 0U);
    ASTERIA_EXPECT(context, "empty PT_TLS carries no state",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(false), image) == LoaderError::None);
}

void TestWriterAndPermissions(TestContext& context) noexcept
{
    StaticPieImage sample;
    sample.Header(1U, 1U, 4U, 0x800U, 0x2800U, 0x800U, 0x800U);
    ExecutableImage image{};
    ASTERIA_EXPECT(context, "read-only relocation outside RELRO is refused",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(false), image) ==
                       LoaderError::UnsupportedFormat);
    PieStore(sample.Bytes + 56U, 4U, 2U);
    sample.Header(3U, 0x6474'E552U, 4U, 0xB00U, 0x2B00U, 0x100U, 0x100U);
    ASTERIA_EXPECT(context, "read-only relocation inside recorded RELRO is admitted",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(false), image) == LoaderError::None,
                   image.Relro.Start == 0x2B00U, image.Relro.Bytes == 0x100U);
    Stage(sample.Bytes, image, g_first);
    auto result = ApplyExecutableRelocations(sample.Bytes, sizeof(sample.Bytes), image, 0x4000'0000U, Writer(g_first));
    ASTERIA_EXPECT(context, "RELRO destinations are writable only during staging", result.Error == LoaderError::None,
                   Word(image, g_first, 0x2B20U) == 0x4000'2C00U);
    g_first.Writes = 0U;
    result = ApplyExecutableRelocations(sample.Bytes, sizeof(sample.Bytes), image, 1U, Writer(g_first));
    ASTERIA_EXPECT(context, "bad base alignment refuses before writing", result.Error == LoaderError::Malformed,
                   g_first.Writes == 0U);
    result =
        ApplyExecutableRelocations(sample.Bytes, sizeof(sample.Bytes), image, ~UInt64{0U} - 4095U, Writer(g_first));
    ASTERIA_EXPECT(context, "overflowing base refuses before writing", result.Error == LoaderError::Malformed,
                   g_first.Writes == 0U);
    g_first.FailAt = 0U;
    result = ApplyExecutableRelocations(sample.Bytes, sizeof(sample.Bytes), image, 0x4000'0000U, Writer(g_first));
    ASTERIA_EXPECT(context, "writer failure reports Denied", result.Error == LoaderError::Denied);
    g_first.FailAt = ~Size{0U};
    // The applier also diagnoses a raw unknown relocation number. Mutating
    // this test-owned snapshot exercises its defensive check before writing.
    PieStore(sample.Bytes + 0x908U, 0x1234U);
    result = ApplyExecutableRelocations(sample.Bytes, sizeof(sample.Bytes), image, 0x4000'0000U, Writer(g_first));
    ASTERIA_EXPECT(context, "unknown relocation reports its exact number before writing",
                   result.Error == LoaderError::UnsupportedFormat, result.Type == 0x1234U, g_first.Writes == 0U);
}
void TestArithmeticAndMetadata(TestContext& context) noexcept
{
    StaticPieImage sample;
    ExecutableImage image{};
    constexpr UInt64 base{0x4000'0000U};
    // A negative ELF64 addend is a bit representation, not an unsigned address.
    sample.Rela(0U, 0x2B20U, 8U, ~UInt64{15U});
    ASTERIA_EXPECT(context, "signed negative RELATIVE addends survive description",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(false), image) == LoaderError::None);
    Stage(sample.Bytes, image, g_first);
    auto result = ApplyExecutableRelocations(sample.Bytes, sizeof(sample.Bytes), image, base, Writer(g_first));
    ASTERIA_EXPECT(context, "signed addend is subtracted from the chosen bias", result.Error == LoaderError::None,
                   Word(image, g_first, 0x2B20U) == base - 16U);
    sample.Rela(0U, 0x2B20U, 8U, UInt64{1U} << 63U);
    ASTERIA_EXPECT(context, "description defers base-dependent arithmetic",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(false), image) == LoaderError::None);
    Stage(sample.Bytes, image, g_first);
    result = ApplyExecutableRelocations(sample.Bytes, sizeof(sample.Bytes), image, base, Writer(g_first));
    ASTERIA_EXPECT(context, "signed addend underflow refuses before writes", result.Error == LoaderError::Malformed,
                   g_first.Writes == 0U);
    sample = StaticPieImage{};
    sample.Tag(10U, 7U, 0x2900U);
    ASTERIA_EXPECT(context, "duplicate relocation metadata refuses",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(false), image) ==
                       LoaderError::Malformed);
    sample = StaticPieImage{};
    PieStore(sample.Bytes + 64U + 56U + 48U, 3U);
    ASTERIA_EXPECT(context, "segment alignment must be a power of two",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(false), image) ==
                       LoaderError::Malformed);
    sample = StaticPieImage{};
    PieStore(sample.Bytes + 64U + 2U * 56U + 8U, 0x900U);
    ASTERIA_EXPECT(context, "dynamic file and virtual ranges must describe the same bytes",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(false), image) ==
                       LoaderError::Malformed);
    sample = StaticPieImage{};
    auto rules = PieRules(false);
    rules.UserLimit = ~UInt64{0U};
    sample.Header(1U, 1U, 6U, 0xFF0U, ~UInt64{15U}, 8U, 8U);
    ASTERIA_EXPECT(context, "normalizing a segment must not wrap its page end",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), rules, image) == LoaderError::Malformed);
    sample = StaticPieImage{};
    PieStore(sample.Bytes + 56U, 4U, 2U);
    sample.Header(3U, 1U, 6U, 0U, 0x4000U, 0U, 0x100U);
    ASTERIA_EXPECT(context, "PIE permits disjoint writable segments under machine capacity",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(false), image) == LoaderError::None,
                   image.SegmentCount == 3U);
    sample = StaticPieImage{true};
    sample.Tag(3U, 0x6FFF'FFF9U, 0U);
    sample.Rela(0U, 0x2B20U, (UInt64{1U} << 32U) | 0x47U, 0U);
    constexpr Size mutations[]{0xA1EU, 0xA1CU, 0xA18U, 0xA20U};
    constexpr UInt64 values[]{0U, 0x12U, 16U, 0x2B00U};
    for (Size index{0U}; index < 4U; ++index)
    {
        StaticPieImage invalid = sample;
        constexpr Size widths[]{2U, 1U, 4U, 8U};
        PieStore(invalid.Bytes + mutations[index], values[index], widths[index]);
        ASTERIA_EXPECT(context, "undefined, preemptible, bad-name and non-code function symbols refuse",
                       DescribeExecutable(invalid.Bytes, sizeof(invalid.Bytes), PieRules(true), image) ==
                           LoaderError::UnsupportedFormat,
                       image.SegmentCount == 0U);
    }
    sample.Tag(1U, 8U, 48U);
    sample.Rela(1U, 0x2B28U, (UInt64{1U} << 32U) | 0x47U, 0U);
    ASTERIA_EXPECT(context, "two symbol relocations describe",
                   DescribeExecutable(sample.Bytes, sizeof(sample.Bytes), PieRules(true), image) == LoaderError::None);
    Stage(sample.Bytes, image, g_first);
    g_first.FailAt = 2U;
    result = ApplyExecutableRelocations(sample.Bytes, sizeof(sample.Bytes), image, 0x2000'0000'0100'0000ULL,
                                        Writer(g_first, 0x2000'0000'0200'0000ULL));
    ASTERIA_EXPECT(context, "a later callback failure reports partial staging for discard",
                   result.Error == LoaderError::Denied, g_first.Writes == 2U);
}

} // namespace
} // namespace Asteria::Test

int main()
{
    Asteria::TestSupport::TestContext context{"StaticPie"};
    Asteria::Test::TestRealSample(context, false);
    Asteria::Test::TestRealSample(context, true);
    Asteria::Test::TestSymbols(context);
    Asteria::Test::TestDynamicRefusals(context);
    Asteria::Test::TestWriterAndPermissions(context);
    Asteria::Test::TestArithmeticAndMetadata(context);
    return context.Finish();
}
