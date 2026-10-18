// SPDX-License-Identifier: Apache-2.0
// Fuzz harness for the portable ELF64 load-plan decoder (Elf64Image).
//
// Input layout: three limit bytes followed by the image.
//   byte 0: page-size shift (bits 0-4); bit 7 multiplies the page size by
//           three, which makes it invalid.
//   byte 1: maximum segment alignment shift (bits 0-5).
//   byte 2: load-segment capacity modulo 40, so capacities above the
//           32-entry plan storage are rejected as invalid limits.
//
// Properties checked for every input:
// - Decoding never reads outside the image (sanitizers report violations).
// - A failure leaves the plan incomplete, even when the plan held a complete
//   result before the call, and exposes neither segments nor information.
// - A success is deterministic and returns the information the plan retains,
//   which matches the ELF header fields.
// - An accepted plan holds exactly the image's PT_LOAD headers (every segment
//   matches one header, and the counts agree), the image has no PT_INTERP or
//   PT_DYNAMIC header, and the segments are ordered by normalized page
//   address.
// - Every accepted segment has its file range inside the image, a file size
//   within its memory size, the derived BSS range, a supported alignment that
//   the file offset and virtual address share, no write+execute permission,
//   and a page range that covers its memory exactly; nonempty page ranges do
//   not overlap, and the entry point is file-backed.

#include "Executable/Elf/Elf64Image.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{
using Asteria::Executable::Elf::Elf64ByteView;
using Asteria::Executable::Elf::Elf64Image;
using Asteria::Executable::Elf::Elf64ImageError;
using Asteria::Executable::Elf::Elf64ImageInformation;
using Asteria::Executable::Elf::Elf64LoadPlan;
using Asteria::Executable::Elf::Elf64LoadPlanAccessError;
using Asteria::Executable::Elf::Elf64LoadPlanLimits;
using Asteria::Executable::Elf::Elf64LoadSegment;
using Asteria::Executable::Elf::Elf64SegmentPermission;
using Asteria::Executable::Elf::MaximumElf64LoadSegments;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::UInt8;

constexpr Size LimitPrefixBytes{3U};
constexpr Size HeaderBytes{64U};
constexpr Size ProgramHeaderBytes{56U};
constexpr UInt64 MaximumUInt64{~UInt64{0U}};
constexpr UInt32 LoadType{1U};
constexpr UInt32 DynamicType{2U};
constexpr UInt32 InterpreterType{3U};
constexpr UInt32 ExecuteFlag{1U};
constexpr UInt32 WriteFlag{2U};
constexpr UInt32 ReadFlag{4U};

[[noreturn]] void Violation(const char* const property)
{
    static_cast<void>(std::fprintf(stderr, "FUZZ PROPERTY VIOLATED: %s\n", property));
    std::abort();
}

void Require(const bool condition, const char* const property)
{
    if (!condition)
    {
        Violation(property);
    }
}

[[nodiscard]] UInt64 ReadLittleEndian(const UInt8* const bytes, const Size width) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < width; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

void WriteLittleEndian(UInt8* const bytes, const UInt64 value, const Size width) noexcept
{
    for (Size index{0U}; index < width; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

[[nodiscard]] Elf64LoadPlanLimits DecodeLimits(const UInt8* const prefix) noexcept
{
    UInt64 pageSize = UInt64{1U} << (prefix[0] & 0x1FU);
    if ((prefix[0] & 0x80U) != 0U)
    {
        pageSize *= 3U;
    }
    return Elf64LoadPlanLimits{
        .PageSizeBytes = pageSize,
        .MaximumSegmentAlignmentBytes = UInt64{1U} << (prefix[1] & 0x3FU),
        .LoadSegmentCapacity = static_cast<Size>(prefix[2] % 40U),
    };
}

[[nodiscard]] bool SegmentsEqual(const Elf64LoadSegment& left, const Elf64LoadSegment& right) noexcept
{
    return (left.FileOffset == right.FileOffset) && (left.FileSizeBytes == right.FileSizeBytes) &&
           (left.VirtualStart == right.VirtualStart) && (left.MemorySizeBytes == right.MemorySizeBytes) &&
           (left.BssStart == right.BssStart) && (left.BssSizeBytes == right.BssSizeBytes) &&
           (left.PageStart == right.PageStart) && (left.PageCount == right.PageCount) &&
           (left.AlignmentBytes == right.AlignmentBytes) && (left.Permissions.GetBits() == right.Permissions.GetBits());
}

[[nodiscard]] bool InformationEqual(const Elf64ImageInformation& left, const Elf64ImageInformation& right) noexcept
{
    return (left.EntryAddress == right.EntryAddress) && (left.Flags == right.Flags) &&
           (left.Machine == right.Machine) && (left.ProgramHeaderCount == right.ProgramHeaderCount);
}

[[nodiscard]] UInt8 PermissionBits(const UInt32 flags) noexcept
{
    UInt8 bits{0U};
    if ((flags & ReadFlag) != 0U)
    {
        bits = static_cast<UInt8>(bits | static_cast<UInt8>(Elf64SegmentPermission::Read));
    }
    if ((flags & WriteFlag) != 0U)
    {
        bits = static_cast<UInt8>(bits | static_cast<UInt8>(Elf64SegmentPermission::Write));
    }
    if ((flags & ExecuteFlag) != 0U)
    {
        bits = static_cast<UInt8>(bits | static_cast<UInt8>(Elf64SegmentPermission::Execute));
    }
    return bits;
}

void CheckIncomplete(const Elf64LoadPlan& plan)
{
    Require(!plan.IsComplete(), "a failure leaves the plan incomplete");
    Require((plan.GetSegmentCount() == 0U) && (plan.GetPageSizeBytes() == 0U),
            "an incomplete plan exposes no segment count or page size");
    const auto information = plan.GetImageInformation();
    const auto segment = plan.GetSegment(0U);
    Require(!information.HasValue() && (information.GetError() == Elf64LoadPlanAccessError::PlanIncomplete) &&
                !segment.HasValue() && (segment.GetError() == Elf64LoadPlanAccessError::PlanIncomplete),
            "an incomplete plan exposes no information or segment");
}

void CheckSegment(const Elf64LoadSegment& segment, const Elf64LoadPlanLimits& limits, const UInt64 imageSize)
{
    Require(segment.FileSizeBytes <= segment.MemorySizeBytes, "file size is within memory size");
    Require((segment.FileOffset <= imageSize) && (segment.FileSizeBytes <= (imageSize - segment.FileOffset)),
            "the file range lies inside the image");
    Require(segment.VirtualStart <= (MaximumUInt64 - segment.MemorySizeBytes), "the memory range does not wrap");
    Require((segment.BssStart == (segment.VirtualStart + segment.FileSizeBytes)) &&
                (segment.BssSizeBytes == (segment.MemorySizeBytes - segment.FileSizeBytes)),
            "the BSS range follows the file-backed bytes");
    if (segment.AlignmentBytes > 1U)
    {
        const UInt64 mask = segment.AlignmentBytes - 1U;
        Require(((segment.AlignmentBytes & mask) == 0U) &&
                    (segment.AlignmentBytes <= limits.MaximumSegmentAlignmentBytes) &&
                    ((segment.FileOffset & mask) == (segment.VirtualStart & mask)),
                "the segment alignment is supported and shared by offset and address");
    }
    Require(!(segment.Permissions.Contains(Elf64SegmentPermission::Write) &&
              segment.Permissions.Contains(Elf64SegmentPermission::Execute)),
            "no segment is writable and executable");

    const UInt64 pageSize = limits.PageSizeBytes;
    Require(segment.PageStart == (segment.VirtualStart & ~(pageSize - 1U)), "the page start is the aligned address");
    if (segment.MemorySizeBytes == 0U)
    {
        Require(segment.PageCount == 0U, "an empty segment has no pages");
        return;
    }
    Require((segment.PageCount != 0U) && (segment.PageCount <= ((MaximumUInt64 - segment.PageStart) / pageSize)),
            "a nonempty segment has pages inside the address space");
    const UInt64 memoryEnd = segment.VirtualStart + segment.MemorySizeBytes;
    const UInt64 lastPage = segment.PageStart + ((segment.PageCount - 1U) * pageSize);
    Require((lastPage < memoryEnd) && ((memoryEnd - lastPage) <= pageSize),
            "the page range covers the memory range exactly");
}

void CheckAcceptedPlan(const UInt8* const image, const Size imageSize, const Elf64LoadPlanLimits& limits,
                       const Elf64ImageInformation& information, const Elf64LoadPlan& plan)
{
    Require(plan.IsComplete() && (plan.GetPageSizeBytes() == limits.PageSizeBytes), "a success completes the plan");
    const auto retained = plan.GetImageInformation();
    Require(retained.HasValue() && InformationEqual(retained.GetValue(), information),
            "the plan retains the returned information");
    Require((information.EntryAddress == ReadLittleEndian(image + 24U, 8U)) &&
                (information.Flags == ReadLittleEndian(image + 48U, 4U)) &&
                (information.Machine == ReadLittleEndian(image + 18U, 2U)) &&
                (information.ProgramHeaderCount == ReadLittleEndian(image + 56U, 2U)),
            "the information matches the ELF header");

    const Size segmentCount = plan.GetSegmentCount();
    Require((segmentCount <= limits.LoadSegmentCapacity) && (segmentCount <= MaximumElf64LoadSegments),
            "the plan respects the segment capacity");
    const auto outOfBounds = plan.GetSegment(segmentCount);
    Require(!outOfBounds.HasValue() && (outOfBounds.GetError() == Elf64LoadPlanAccessError::IndexOutOfBounds),
            "a segment index past the count is rejected");

    Elf64LoadSegment segments[MaximumElf64LoadSegments]{};
    bool matched[MaximumElf64LoadSegments]{};
    bool entryIsFileBacked{false};
    for (Size index{0U}; index < segmentCount; ++index)
    {
        const auto segment = plan.GetSegment(index);
        Require(segment.HasValue(), "every counted segment is readable");
        segments[index] = segment.GetValue();
        CheckSegment(segments[index], limits, imageSize);
        if ((information.EntryAddress >= segments[index].VirtualStart) &&
            (information.EntryAddress < segments[index].BssStart))
        {
            entryIsFileBacked = true;
        }
        if (index != 0U)
        {
            const Elf64LoadSegment& previous = segments[index - 1U];
            Require((previous.PageStart < segments[index].PageStart) ||
                        ((previous.PageStart == segments[index].PageStart) &&
                         (previous.VirtualStart <= segments[index].VirtualStart)),
                    "segments are ordered by normalized page address");
        }
        for (Size other{0U}; other < index; ++other)
        {
            if ((segments[index].PageCount == 0U) || (segments[other].PageCount == 0U))
            {
                continue;
            }
            const UInt64 end = segments[index].PageStart + (segments[index].PageCount * limits.PageSizeBytes);
            const UInt64 otherEnd = segments[other].PageStart + (segments[other].PageCount * limits.PageSizeBytes);
            Require((end <= segments[other].PageStart) || (otherEnd <= segments[index].PageStart),
                    "nonempty page ranges do not overlap");
        }
    }
    Require(entryIsFileBacked, "the entry point is file-backed");

    // The table bounds are checked here too, so a decoder that accepted an
    // out-of-image table is reported as a property violation, not a harness
    // overflow.
    const UInt64 tableOffset = ReadLittleEndian(image + 32U, 8U);
    const UInt64 headerCount = ReadLittleEndian(image + 56U, 2U);
    Require((tableOffset <= imageSize) && ((headerCount * ProgramHeaderBytes) <= (imageSize - tableOffset)),
            "the accepted program header table lies inside the image");
    Size loadHeaders{0U};
    for (UInt64 header{0U}; header < headerCount; ++header)
    {
        const UInt8* const entry = image + tableOffset + (header * ProgramHeaderBytes);
        const UInt32 type = static_cast<UInt32>(ReadLittleEndian(entry, 4U));
        Require((type != DynamicType) && (type != InterpreterType),
                "an accepted image has no PT_DYNAMIC or PT_INTERP header");
        if (type != LoadType)
        {
            continue;
        }
        ++loadHeaders;
        bool found{false};
        for (Size index{0U}; (index < segmentCount) && !found; ++index)
        {
            const Elf64LoadSegment& segment = segments[index];
            if (!matched[index] && (segment.FileOffset == ReadLittleEndian(entry + 8U, 8U)) &&
                (segment.VirtualStart == ReadLittleEndian(entry + 16U, 8U)) &&
                (segment.FileSizeBytes == ReadLittleEndian(entry + 32U, 8U)) &&
                (segment.MemorySizeBytes == ReadLittleEndian(entry + 40U, 8U)) &&
                (segment.AlignmentBytes == ReadLittleEndian(entry + 48U, 8U)) &&
                (segment.Permissions.GetBits() ==
                 PermissionBits(static_cast<UInt32>(ReadLittleEndian(entry + 4U, 4U)))))
            {
                matched[index] = true;
                found = true;
            }
        }
        Require(found, "every PT_LOAD header appears in the plan");
    }
    Require(loadHeaders == segmentCount, "the plan holds exactly the PT_LOAD headers");
}

// Seed construction. Program headers follow the ELF header; segment bytes
// follow the table.
struct SeedSegment
{
    UInt32 Type;
    UInt32 Flags;
    UInt64 FileOffset;
    UInt64 VirtualStart;
    UInt64 FileSizeBytes;
    UInt64 MemorySizeBytes;
    UInt64 AlignmentBytes;
};

constexpr Size MaximumSeedBytes{4096U};
UInt8 g_seed[MaximumSeedBytes];

// Writes the limit prefix and an image into g_seed; returns the input size.
[[nodiscard]] Size BuildImage(const UInt8 pageShift, const UInt8 alignmentShift, const UInt8 capacity,
                              const UInt64 entry, const SeedSegment* const segments, const Size segmentCount,
                              const Size imageSize) noexcept
{
    for (UInt8& value : g_seed)
    {
        value = 0U;
    }
    g_seed[0] = pageShift;
    g_seed[1] = alignmentShift;
    g_seed[2] = capacity;
    UInt8* const image = g_seed + LimitPrefixBytes;
    image[0] = 0x7FU;
    image[1] = static_cast<UInt8>('E');
    image[2] = static_cast<UInt8>('L');
    image[3] = static_cast<UInt8>('F');
    image[4] = 2U;
    image[5] = 1U;
    image[6] = 1U;
    WriteLittleEndian(image + 16U, 2U, 2U);
    WriteLittleEndian(image + 18U, 0x3EU, 2U);
    WriteLittleEndian(image + 20U, 1U, 4U);
    WriteLittleEndian(image + 24U, entry, 8U);
    WriteLittleEndian(image + 32U, HeaderBytes, 8U);
    WriteLittleEndian(image + 48U, 0x10U, 4U);
    WriteLittleEndian(image + 52U, HeaderBytes, 2U);
    WriteLittleEndian(image + 54U, ProgramHeaderBytes, 2U);
    WriteLittleEndian(image + 56U, segmentCount, 2U);
    for (Size index{0U}; index < segmentCount; ++index)
    {
        UInt8* const header = image + HeaderBytes + (index * ProgramHeaderBytes);
        WriteLittleEndian(header, segments[index].Type, 4U);
        WriteLittleEndian(header + 4U, segments[index].Flags, 4U);
        WriteLittleEndian(header + 8U, segments[index].FileOffset, 8U);
        WriteLittleEndian(header + 16U, segments[index].VirtualStart, 8U);
        WriteLittleEndian(header + 24U, segments[index].VirtualStart, 8U);
        WriteLittleEndian(header + 32U, segments[index].FileSizeBytes, 8U);
        WriteLittleEndian(header + 40U, segments[index].MemorySizeBytes, 8U);
        WriteLittleEndian(header + 48U, segments[index].AlignmentBytes, 8U);
    }
    for (Size index{HeaderBytes + (segmentCount * ProgramHeaderBytes)}; index < imageSize; ++index)
    {
        image[index] = static_cast<UInt8>(index * 7U);
    }
    return LimitPrefixBytes + imageSize;
}

// The smallest accepted image: one code segment right after the table.
constexpr SeedSegment SingleCode[]{{LoadType, ReadFlag | ExecuteFlag, 0x78U, 0x1078U, 0x10U, 0x10U, 8U}};
constexpr Size SingleCodeImageBytes{0x88U};

// Decoding starts from a complete plan, so a failure must retract it.
struct PrimedPlan
{
    UInt8 Image[SingleCodeImageBytes];
    Elf64LoadPlan Plan;
};

PrimedPlan g_primed{};

void PrimePlan()
{
    static bool built{false};
    if (!built)
    {
        static_cast<void>(BuildImage(12U, 12U, 1U, 0x1078U, SingleCode, 1U, SingleCodeImageBytes));
        for (Size index{0U}; index < SingleCodeImageBytes; ++index)
        {
            g_primed.Image[index] = g_seed[LimitPrefixBytes + index];
        }
        built = true;
    }
    const Elf64LoadPlanLimits limits{
        .PageSizeBytes = 0x1000U,
        .MaximumSegmentAlignmentBytes = 0x1000U,
        .LoadSegmentCapacity = 1U,
    };
    Require(Elf64Image::DecodeLoadPlan(Elf64ByteView{.Data = g_primed.Image, .SizeBytes = SingleCodeImageBytes}, limits,
                                       g_primed.Plan)
                .HasValue(),
            "the priming image decodes");
}
} // namespace

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    // A code, read-only, and data segment in unsorted order with BSS, an
    // empty segment, and a note header; 4 KiB pages.
    constexpr SeedSegment mixed[]{
        {LoadType, ReadFlag | WriteFlag, 0x300U, 0x0040'4300U, 0x40U, 0x200U, 0x1000U},
        {4U, ReadFlag, 0x2C0U, 0U, 0x10U, 0x10U, 4U},
        {LoadType, ReadFlag | ExecuteFlag, 0x280U, 0x0040'0280U, 0x40U, 0x40U, 0x1000U},
        {LoadType, ReadFlag, 0x2C0U, 0x0040'22C0U, 0x40U, 0x40U, 0x1000U},
        {LoadType, ReadFlag, 0x0U, 0x0050'0000U, 0U, 0U, 0U},
    };
    emit(g_seed, BuildImage(12U, 16U, 8U, 0x0040'0290U, mixed, 5U, 0x380U));
    emit(g_seed, BuildImage(12U, 12U, 1U, 0x1078U, SingleCode, 1U, SingleCodeImageBytes));
    // IA-64 region-one layout with 16 KiB pages and a large BSS.
    constexpr SeedSegment regionOne[]{
        {LoadType, ReadFlag | ExecuteFlag, 0x100U, 0x2000'0000'0400'0100ULL, 0x100U, 0x100U, 0x100U},
        {LoadType, ReadFlag | WriteFlag, 0x200U, 0x2000'0000'0400'8200ULL, 0x80U, 0x10'0000U, 0x100U},
    };
    emit(g_seed, BuildImage(14U, 16U, 32U, 0x2000'0000'0400'0110ULL, regionOne, 2U, 0x280U));
    // Thirty-two segments, the plan capacity, inserted in descending order.
    SeedSegment many[MaximumElf64LoadSegments]{};
    for (Size index{0U}; index < MaximumElf64LoadSegments; ++index)
    {
        many[index] = SeedSegment{
            LoadType, (index == (MaximumElf64LoadSegments - 1U)) ? (ReadFlag | ExecuteFlag) : ReadFlag,
            0x740U,   0x10'0000U - (index * 0x1000U) + 0x740U,
            0x10U,    0x20U,
            0x40U,
        };
    }
    emit(g_seed, BuildImage(12U, 12U, 32U, 0x10'0000U - (31U * 0x1000U) + 0x740U, many, 32U, 0x760U));
    // Near misses: capacity exceeded, overlapping pages, entry in BSS, a
    // write+execute segment, a dynamic header, and invalid limits.
    emit(g_seed, BuildImage(12U, 16U, 2U, 0x0040'0290U, mixed, 5U, 0x380U));
    constexpr SeedSegment overlapping[]{
        {LoadType, ReadFlag | ExecuteFlag, 0x100U, 0x7100U, 0x20U, 0x20U, 0x100U},
        {LoadType, ReadFlag, 0x180U, 0x7F80U, 0x20U, 0x100U, 0x80U},
    };
    emit(g_seed, BuildImage(12U, 16U, 4U, 0x7100U, overlapping, 2U, 0x200U));
    constexpr SeedSegment entryInBss[]{{LoadType, ReadFlag | ExecuteFlag, 0x78U, 0x1078U, 0x10U, 0x40U, 8U}};
    emit(g_seed, BuildImage(12U, 12U, 1U, 0x1090U, entryInBss, 1U, 0x88U));
    constexpr SeedSegment writableCode[]{
        {LoadType, ReadFlag | WriteFlag | ExecuteFlag, 0x78U, 0x1078U, 0x10U, 0x10U, 8U}};
    emit(g_seed, BuildImage(12U, 12U, 1U, 0x1078U, writableCode, 1U, 0x88U));
    constexpr SeedSegment dynamic[]{
        {LoadType, ReadFlag | ExecuteFlag, 0xB0U, 0x10B0U, 0x10U, 0x10U, 8U},
        {DynamicType, ReadFlag, 0xB0U, 0x10B0U, 0x10U, 0x10U, 8U},
    };
    emit(g_seed, BuildImage(12U, 12U, 2U, 0x10B0U, dynamic, 2U, 0xC0U));
    emit(g_seed, BuildImage(0x8CU, 12U, 1U, 0x1078U, SingleCode, 1U, SingleCodeImageBytes));
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    if (size < LimitPrefixBytes)
    {
        return 0;
    }
    const Elf64LoadPlanLimits limits = DecodeLimits(data);
    const Elf64ByteView image{.Data = data + LimitPrefixBytes, .SizeBytes = size - LimitPrefixBytes};

    PrimePlan();
    Elf64LoadPlan& plan = g_primed.Plan;
    const auto result = Elf64Image::DecodeLoadPlan(image, limits, plan);
    if (!result.HasValue())
    {
        Require(result.GetError() != Elf64ImageError::None, "a failure reports an error");
        CheckIncomplete(plan);
        return 0;
    }
    if (image.SizeBytes < HeaderBytes)
    {
        Violation("an accepted image holds a complete ELF header");
    }
    CheckAcceptedPlan(image.Data, image.SizeBytes, limits, result.GetValue(), plan);

    Elf64LoadPlan again{};
    const auto repeated = Elf64Image::DecodeLoadPlan(image, limits, again);
    Require(repeated.HasValue() && InformationEqual(repeated.GetValue(), result.GetValue()) &&
                (again.GetSegmentCount() == plan.GetSegmentCount()),
            "decoding is deterministic");
    for (Size index{0U}; index < plan.GetSegmentCount(); ++index)
    {
        Require(SegmentsEqual(plan.GetSegment(index).GetValue(), again.GetSegment(index).GetValue()),
                "decoding yields identical segments");
    }
    return 0;
}
