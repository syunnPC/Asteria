// SPDX-License-Identifier: Apache-2.0
#include <cstdio>

#include "Executable/Elf/Elf64Image.hpp"
#include "Foundation/Types.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
constexpr Foundation::Size ImageSizeBytes{4096U};
constexpr Foundation::Size ProgramHeaderTableOffset{64U};
constexpr Foundation::Size ProgramHeaderSizeBytes{56U};
constexpr Foundation::Size DataHeaderIndex{0U};
constexpr Foundation::Size AuxiliaryHeaderIndex{1U};
constexpr Foundation::Size CodeHeaderIndex{2U};
constexpr Foundation::Size ReadOnlyHeaderIndex{3U};

struct TestImage
{
    Foundation::UInt8 Bytes[ImageSizeBytes];
};

[[nodiscard]] constexpr Foundation::Size GetProgramHeaderOffset(const Foundation::Size index) noexcept
{
    return ProgramHeaderTableOffset + (index * ProgramHeaderSizeBytes);
}

void WriteLittleEndian16(Foundation::UInt8* const bytes, const Foundation::UInt16 value) noexcept
{
    bytes[0U] = static_cast<Foundation::UInt8>(value);
    bytes[1U] = static_cast<Foundation::UInt8>(value >> 8U);
}

void WriteLittleEndian32(Foundation::UInt8* const bytes, const Foundation::UInt32 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

void WriteLittleEndian64(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

void SetProgramHeader(TestImage& image, const Foundation::Size index, const Foundation::UInt32 type,
                      const Foundation::UInt32 flags, const Foundation::UInt64 fileOffset,
                      const Foundation::UInt64 virtualStart, const Foundation::UInt64 fileSizeBytes,
                      const Foundation::UInt64 memorySizeBytes, const Foundation::UInt64 alignmentBytes) noexcept
{
    Foundation::UInt8* const header = image.Bytes + GetProgramHeaderOffset(index);
    WriteLittleEndian32(header, type);
    WriteLittleEndian32(header + 4U, flags);
    WriteLittleEndian64(header + 8U, fileOffset);
    WriteLittleEndian64(header + 16U, virtualStart);
    WriteLittleEndian64(header + 24U, 0xFFFF'FFFF'FFFF'FFFFULL);
    WriteLittleEndian64(header + 32U, fileSizeBytes);
    WriteLittleEndian64(header + 40U, memorySizeBytes);
    WriteLittleEndian64(header + 48U, alignmentBytes);
}

void MakeValidImage(TestImage& image) noexcept
{
    image = TestImage{};
    image.Bytes[0U] = 0x7FU;
    image.Bytes[1U] = static_cast<Foundation::UInt8>('E');
    image.Bytes[2U] = static_cast<Foundation::UInt8>('L');
    image.Bytes[3U] = static_cast<Foundation::UInt8>('F');
    image.Bytes[4U] = 2U;
    image.Bytes[5U] = 1U;
    image.Bytes[6U] = 1U;
    image.Bytes[7U] = 0xFFU;
    WriteLittleEndian16(image.Bytes + 16U, 2U);
    WriteLittleEndian16(image.Bytes + 18U, 0x003EU);
    WriteLittleEndian32(image.Bytes + 20U, 1U);
    WriteLittleEndian64(image.Bytes + 24U, 0x0040'0210U);
    WriteLittleEndian64(image.Bytes + 32U, ProgramHeaderTableOffset);
    WriteLittleEndian64(image.Bytes + 40U, 0xFFFF'FFFF'FFFF'FFF0ULL);
    WriteLittleEndian32(image.Bytes + 48U, 0xA5A5'5A5AU);
    WriteLittleEndian16(image.Bytes + 52U, 64U);
    WriteLittleEndian16(image.Bytes + 54U, 56U);
    WriteLittleEndian16(image.Bytes + 56U, 4U);
    WriteLittleEndian16(image.Bytes + 58U, 1U);
    WriteLittleEndian16(image.Bytes + 60U, 0xFFFFU);
    WriteLittleEndian16(image.Bytes + 62U, 0xFFFFU);

    SetProgramHeader(image, DataHeaderIndex, 1U, 6U, 0x600U, 0x0040'4600U, 0x100U, 0x300U, 0x1000U);
    SetProgramHeader(image, AuxiliaryHeaderIndex, 4U, 0xFFFF'FFFFU, 0xFFFF'FFFF'FFFF'FFFFULL, 0xFFFF'FFFF'FFFF'FFFFULL,
                     0xFFFF'FFFF'FFFF'FFFFULL, 0xFFFF'FFFF'FFFF'FFFFULL, 3U);
    SetProgramHeader(image, CodeHeaderIndex, 1U, 5U, 0x200U, 0x0040'0200U, 0x80U, 0x80U, 0x1000U);
    SetProgramHeader(image, ReadOnlyHeaderIndex, 1U, 4U, 0x400U, 0x0040'2400U, 0x100U, 0x100U, 0x1000U);
}

[[nodiscard]] constexpr Executable::Elf::Elf64LoadPlanLimits MakeLimits() noexcept
{
    return Executable::Elf::Elf64LoadPlanLimits{
        .PageSizeBytes = 0x1000U,
        .MaximumSegmentAlignmentBytes = 0x1'0000U,
        .LoadSegmentCapacity = 3U,
    };
}

void CheckError(TestContext& runner, const TestImage& image, const Executable::Elf::Elf64LoadPlanLimits& limits,
                const Executable::Elf::Elf64ImageError expected, const char* const description) noexcept
{
    Executable::Elf::Elf64LoadPlan plan{};
    const Foundation::Result<Executable::Elf::Elf64ImageInformation, Executable::Elf::Elf64ImageError> result =
        Executable::Elf::Elf64Image::DecodeLoadPlan(
            Executable::Elf::Elf64ByteView{
                .Data = image.Bytes,
                .SizeBytes = sizeof(image.Bytes),
            },
            limits, plan);
    ASTERIA_EXPECT(runner, description, !result.HasValue(), result.GetError() == expected);
    ASTERIA_EXPECT(runner, "a decode failure exposes no partial plan", !plan.IsComplete(),
                   plan.GetSegmentCount() == 0U);
}

void TestSuccessfulPlan(TestContext& runner) noexcept
{
    TestImage image{};
    MakeValidImage(image);
    Executable::Elf::Elf64LoadPlan plan{};
    const Foundation::Result<Executable::Elf::Elf64ImageInformation, Executable::Elf::Elf64ImageError> result =
        Executable::Elf::Elf64Image::DecodeLoadPlan(
            Executable::Elf::Elf64ByteView{
                .Data = image.Bytes,
                .SizeBytes = sizeof(image.Bytes),
            },
            MakeLimits(), plan);
    ASTERIA_EXPECT(runner, "a valid ET_EXEC image produces a plan", result.HasValue());
    ASTERIA_EXPECT(runner, "a successful plan is complete", plan.IsComplete());
    ASTERIA_EXPECT(runner, "the selected page geometry is retained", plan.GetPageSizeBytes() == 0x1000U);
    ASTERIA_EXPECT(runner, "only PT_LOAD entries are retained", plan.GetSegmentCount() == 3U);
    if (!result.HasValue() || (plan.GetSegmentCount() != 3U))
    {
        return;
    }

    const Executable::Elf::Elf64ImageInformation& information = result.GetValue();
    ASTERIA_EXPECT(runner, "machine, flags, entry, and header count are exposed without platform policy",
                   information.Machine == 0x003EU, information.Flags == 0xA5A5'5A5AU,
                   information.EntryAddress == 0x0040'0210U, information.ProgramHeaderCount == 4U);
    const Foundation::Result<Executable::Elf::Elf64ImageInformation, Executable::Elf::Elf64LoadPlanAccessError>
        retainedInformation = plan.GetImageInformation();
    ASTERIA_EXPECT(runner, "the completed plan retains image information", retainedInformation.HasValue(),
                   retainedInformation.GetValue().Flags == information.Flags);

    const Foundation::Result<Executable::Elf::Elf64LoadSegment, Executable::Elf::Elf64LoadPlanAccessError> codeResult =
        plan.GetSegment(0U);
    const Foundation::Result<Executable::Elf::Elf64LoadSegment, Executable::Elf::Elf64LoadPlanAccessError>
        readOnlyResult = plan.GetSegment(1U);
    const Foundation::Result<Executable::Elf::Elf64LoadSegment, Executable::Elf::Elf64LoadPlanAccessError> dataResult =
        plan.GetSegment(2U);
    ASTERIA_EXPECT(runner, "the executable segment carries its file, virtual, page, and RX contract",
                   codeResult.HasValue(), codeResult.GetValue().FileOffset == 0x200U,
                   codeResult.GetValue().VirtualStart == 0x0040'0200U, codeResult.GetValue().PageStart == 0x0040'0000U,
                   codeResult.GetValue().PageCount == 1U,
                   codeResult.GetValue().Permissions.Contains(Executable::Elf::Elf64SegmentPermission::Read),
                   codeResult.GetValue().Permissions.Contains(Executable::Elf::Elf64SegmentPermission::Execute),
                   !codeResult.GetValue().Permissions.Contains(Executable::Elf::Elf64SegmentPermission::Write));
    ASTERIA_EXPECT(runner, "segments are sorted by page address with normalized read permission",
                   readOnlyResult.HasValue(), readOnlyResult.GetValue().VirtualStart == 0x0040'2400U,
                   readOnlyResult.GetValue().AlignmentBytes == 0x1000U,
                   readOnlyResult.GetValue().Permissions.GetBits() == 1U);
    ASTERIA_EXPECT(runner, "the writable segment separates file-backed bytes from BSS", dataResult.HasValue(),
                   dataResult.GetValue().FileSizeBytes == 0x100U, dataResult.GetValue().MemorySizeBytes == 0x300U,
                   dataResult.GetValue().BssStart == 0x0040'4700U, dataResult.GetValue().BssSizeBytes == 0x200U,
                   dataResult.GetValue().Permissions.GetBits() == 3U);
    ASTERIA_EXPECT(runner, "segment access is bounds checked", !plan.GetSegment(3U).HasValue(),
                   plan.GetSegment(3U).GetError() == Executable::Elf::Elf64LoadPlanAccessError::IndexOutOfBounds);
}

void TestPermittedFlexibleFields(TestContext& runner) noexcept
{
    TestImage image{};
    MakeValidImage(image);
    SetProgramHeader(image, AuxiliaryHeaderIndex, 1U, 0U, ImageSizeBytes, 0x0040'1234U, 0U, 0U, 0U);
    Executable::Elf::Elf64LoadPlanLimits limits = MakeLimits();
    limits.LoadSegmentCapacity = 4U;
    Executable::Elf::Elf64LoadPlan plan{};
    const Foundation::Result<Executable::Elf::Elf64ImageInformation, Executable::Elf::Elf64ImageError> result =
        Executable::Elf::Elf64Image::DecodeLoadPlan(
            Executable::Elf::Elf64ByteView{
                .Data = image.Bytes,
                .SizeBytes = sizeof(image.Bytes),
            },
            limits, plan);
    ASTERIA_EXPECT(runner, "zero-length PT_LOAD and zero alignment remain valid ELF flexibility", result.HasValue(),
                   plan.GetSegmentCount() == 4U);
    const Foundation::Result<Executable::Elf::Elf64LoadSegment, Executable::Elf::Elf64LoadPlanAccessError> emptyResult =
        plan.GetSegment(1U);
    ASTERIA_EXPECT(runner, "an empty segment has an empty normalized page extent", emptyResult.HasValue(),
                   emptyResult.GetValue().PageCount == 0U, emptyResult.GetValue().AlignmentBytes == 0U);
}

void TestDescriptorStyleEntry(TestContext& runner) noexcept
{
    TestImage image{};
    MakeValidImage(image);
    WriteLittleEndian64(image.Bytes + 24U, 0x0040'2410U);
    Executable::Elf::Elf64LoadPlan plan{};
    const Foundation::Result<Executable::Elf::Elf64ImageInformation, Executable::Elf::Elf64ImageError> result =
        Executable::Elf::Elf64Image::DecodeLoadPlan(
            Executable::Elf::Elf64ByteView{
                .Data = image.Bytes,
                .SizeBytes = sizeof(image.Bytes),
            },
            MakeLimits(), plan);
    ASTERIA_EXPECT(runner, "a descriptor-style entry in a read-only file-backed segment is accepted", result.HasValue(),
                   result.GetValue().EntryAddress == 0x0040'2410U);
}

void TestExecuteOnlyDirectEntry(TestContext& runner) noexcept
{
    TestImage image{};
    MakeValidImage(image);
    WriteLittleEndian32(image.Bytes + GetProgramHeaderOffset(CodeHeaderIndex) + 4U, 1U);
    Executable::Elf::Elf64LoadPlan plan{};
    const Foundation::Result<Executable::Elf::Elf64ImageInformation, Executable::Elf::Elf64ImageError> result =
        Executable::Elf::Elf64Image::DecodeLoadPlan(
            Executable::Elf::Elf64ByteView{
                .Data = image.Bytes,
                .SizeBytes = sizeof(image.Bytes),
            },
            MakeLimits(), plan);
    ASTERIA_EXPECT(runner, "a direct entry in execute-only file-backed bytes is accepted", result.HasValue(),
                   plan.GetSegment(0U).HasValue(), plan.GetSegment(0U).GetValue().Permissions.GetBits() == 4U);
}

void TestInputAndHeaderFailures(TestContext& runner) noexcept
{
    TestImage image{};
    MakeValidImage(image);
    Executable::Elf::Elf64LoadPlan plan{};
    const Foundation::Result<Executable::Elf::Elf64ImageInformation, Executable::Elf::Elf64ImageError> nullResult =
        Executable::Elf::Elf64Image::DecodeLoadPlan(
            Executable::Elf::Elf64ByteView{
                .Data = nullptr,
                .SizeBytes = 1U,
            },
            MakeLimits(), plan);
    ASTERIA_EXPECT(runner, "a nonempty null byte view is rejected", !nullResult.HasValue(),
                   nullResult.GetError() == Executable::Elf::Elf64ImageError::InvalidByteView);

    const Foundation::Result<Executable::Elf::Elf64ImageInformation, Executable::Elf::Elf64ImageError> shortResult =
        Executable::Elf::Elf64Image::DecodeLoadPlan(
            Executable::Elf::Elf64ByteView{
                .Data = nullptr,
                .SizeBytes = 0U,
            },
            MakeLimits(), plan);
    ASTERIA_EXPECT(runner, "an empty byte view is a bounded short header", !shortResult.HasValue(),
                   shortResult.GetError() == Executable::Elf::Elf64ImageError::HeaderTooSmall);

    Executable::Elf::Elf64LoadPlanLimits limits = MakeLimits();
    limits.PageSizeBytes = 3U;
    CheckError(runner, image, limits, Executable::Elf::Elf64ImageError::InvalidLimits,
               "page size must be a power of two");
    limits = MakeLimits();
    limits.MaximumSegmentAlignmentBytes = 3U;
    CheckError(runner, image, limits, Executable::Elf::Elf64ImageError::InvalidLimits,
               "the supported alignment ceiling must be a power of two");
    limits = MakeLimits();
    limits.LoadSegmentCapacity = Executable::Elf::MaximumElf64LoadSegments + 1U;
    CheckError(runner, image, limits, Executable::Elf::Elf64ImageError::InvalidLimits,
               "caller capacity cannot exceed fixed plan storage");

    MakeValidImage(image);
    image.Bytes[0U] = 0U;
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::InvalidMagic, "ELF magic is required");
    MakeValidImage(image);
    image.Bytes[4U] = 1U;
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::UnsupportedClass,
               "ELF32 input is rejected");
    MakeValidImage(image);
    image.Bytes[5U] = 2U;
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::UnsupportedEndianness,
               "big-endian input is rejected");
    MakeValidImage(image);
    image.Bytes[6U] = 0U;
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::UnsupportedVersion,
               "the identification version must be current");
    MakeValidImage(image);
    WriteLittleEndian32(image.Bytes + 20U, 0U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::UnsupportedVersion,
               "the ELF header version must be current");
    MakeValidImage(image);
    WriteLittleEndian16(image.Bytes + 16U, 3U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::UnsupportedImageType,
               "ET_DYN input is not a fixed executable image");
    MakeValidImage(image);
    WriteLittleEndian16(image.Bytes + 52U, 63U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::InvalidHeaderSize,
               "the decoded ELF64 header size is required");
    MakeValidImage(image);
    WriteLittleEndian16(image.Bytes + 54U, 55U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::InvalidProgramHeaderSize,
               "the decoded ELF64 program-header size is required");
    MakeValidImage(image);
    WriteLittleEndian16(image.Bytes + 56U, 0xFFFFU);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::UnsupportedExtendedProgramHeaderCount,
               "section-table-dependent extended program-header counts are rejected");
    MakeValidImage(image);
    WriteLittleEndian64(image.Bytes + 32U, 0xFFFF'FFFF'FFFF'FFF0ULL);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::ProgramHeaderTableOverflow,
               "program-header arithmetic cannot wrap");
    MakeValidImage(image);
    WriteLittleEndian64(image.Bytes + 32U, ImageSizeBytes - 64U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::ProgramHeaderTableOutsideImage,
               "the complete program-header table must be contained");
}

void TestSegmentFailures(TestContext& runner) noexcept
{
    TestImage image{};
    MakeValidImage(image);
    SetProgramHeader(image, AuxiliaryHeaderIndex, 3U, 0U, 0U, 0U, 0U, 0U, 0U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::InterpreterUnsupported,
               "PT_INTERP is outside the freestanding loader contract");
    MakeValidImage(image);
    SetProgramHeader(image, AuxiliaryHeaderIndex, 2U, 0U, 0U, 0U, 0U, 0U, 0U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::RuntimeRelocationUnsupported,
               "PT_DYNAMIC runtime relocation dependence is rejected");

    MakeValidImage(image);
    Executable::Elf::Elf64LoadPlanLimits limits = MakeLimits();
    limits.LoadSegmentCapacity = 2U;
    CheckError(runner, image, limits, Executable::Elf::Elf64ImageError::LoadSegmentCapacityExceeded,
               "PT_LOAD count is bounded by caller capacity");
    MakeValidImage(image);
    WriteLittleEndian64(image.Bytes + GetProgramHeaderOffset(CodeHeaderIndex) + 32U, 0x81U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::FileSizeExceedsMemorySize,
               "file bytes cannot exceed the memory extent");
    MakeValidImage(image);
    WriteLittleEndian64(image.Bytes + GetProgramHeaderOffset(DataHeaderIndex) + 8U, 0xFFFF'FFFF'FFFF'FFF0ULL);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::FileRangeOverflow,
               "a PT_LOAD file range cannot wrap");
    MakeValidImage(image);
    WriteLittleEndian64(image.Bytes + GetProgramHeaderOffset(DataHeaderIndex) + 8U, ImageSizeBytes - 0x80U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::FileRangeOutsideImage,
               "every file-backed byte must be inside the view");
    MakeValidImage(image);
    WriteLittleEndian64(image.Bytes + GetProgramHeaderOffset(CodeHeaderIndex) + 16U, 0xFFFF'FFFF'FFFF'FFC0ULL);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::VirtualRangeOverflow,
               "a PT_LOAD virtual memory extent cannot wrap");
    MakeValidImage(image);
    WriteLittleEndian64(image.Bytes + GetProgramHeaderOffset(CodeHeaderIndex) + 48U, 3U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::UnsupportedSegmentAlignment,
               "non-power-of-two segment alignment is rejected");
    MakeValidImage(image);
    WriteLittleEndian64(image.Bytes + GetProgramHeaderOffset(CodeHeaderIndex) + 48U, 0x2'0000U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::UnsupportedSegmentAlignment,
               "segment alignment cannot exceed caller support");
    MakeValidImage(image);
    WriteLittleEndian64(image.Bytes + GetProgramHeaderOffset(CodeHeaderIndex) + 16U, 0x0040'0201U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::SegmentAlignmentMismatch,
               "file offset and virtual address must be congruent");
    MakeValidImage(image);
    WriteLittleEndian32(image.Bytes + GetProgramHeaderOffset(CodeHeaderIndex) + 4U, 13U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::UnknownSegmentPermissions,
               "unknown PF bits are rejected");
    MakeValidImage(image);
    WriteLittleEndian32(image.Bytes + GetProgramHeaderOffset(CodeHeaderIndex) + 4U, 7U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::WriteExecuteSegment,
               "writable executable segments are rejected");
    MakeValidImage(image);
    WriteLittleEndian64(image.Bytes + GetProgramHeaderOffset(CodeHeaderIndex) + 16U, 0xFFFF'FFFF'FFFF'F800ULL);
    WriteLittleEndian64(image.Bytes + GetProgramHeaderOffset(CodeHeaderIndex) + 48U, 1U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::NormalizedPageRangeOverflow,
               "normalizing a representable virtual extent cannot wrap");
    MakeValidImage(image);
    WriteLittleEndian64(image.Bytes + GetProgramHeaderOffset(ReadOnlyHeaderIndex) + 16U, 0x0040'0400U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::NormalizedPageOverlap,
               "distinct byte ranges cannot share a normalized mapping page");
}

void TestEntryFailuresAndOutputInvalidation(TestContext& runner) noexcept
{
    TestImage image{};
    MakeValidImage(image);
    WriteLittleEndian64(image.Bytes + 24U, 0x0040'0280U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::EntryNotFileBacked,
               "entry cannot equal a file-range end");
    MakeValidImage(image);
    WriteLittleEndian64(image.Bytes + 24U, 0x0090'0000U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::EntryNotFileBacked,
               "entry must select some PT_LOAD file bytes");
    MakeValidImage(image);
    WriteLittleEndian64(image.Bytes + 24U, 0x0040'0260U);
    WriteLittleEndian64(image.Bytes + GetProgramHeaderOffset(CodeHeaderIndex) + 32U, 0x40U);
    CheckError(runner, image, MakeLimits(), Executable::Elf::Elf64ImageError::EntryNotFileBacked,
               "entry cannot select BSS bytes");

    MakeValidImage(image);
    Executable::Elf::Elf64LoadPlan plan{};
    const Foundation::Result<Executable::Elf::Elf64ImageInformation, Executable::Elf::Elf64ImageError> validResult =
        Executable::Elf::Elf64Image::DecodeLoadPlan(
            Executable::Elf::Elf64ByteView{
                .Data = image.Bytes,
                .SizeBytes = sizeof(image.Bytes),
            },
            MakeLimits(), plan);
    image.Bytes[0U] = 0U;
    const Foundation::Result<Executable::Elf::Elf64ImageInformation, Executable::Elf::Elf64ImageError> invalidResult =
        Executable::Elf::Elf64Image::DecodeLoadPlan(
            Executable::Elf::Elf64ByteView{
                .Data = image.Bytes,
                .SizeBytes = sizeof(image.Bytes),
            },
            MakeLimits(), plan);
    ASTERIA_EXPECT(runner, "a failed reuse invalidates the previously completed output", validResult.HasValue(),
                   !invalidResult.HasValue(), !plan.IsComplete(), plan.GetSegmentCount() == 0U);
    ASTERIA_EXPECT(runner, "incomplete plans expose neither metadata nor stale segments",
                   !plan.GetImageInformation().HasValue(),
                   plan.GetImageInformation().GetError() == Executable::Elf::Elf64LoadPlanAccessError::PlanIncomplete,
                   !plan.GetSegment(0U).HasValue(),
                   plan.GetSegment(0U).GetError() == Executable::Elf::Elf64LoadPlanAccessError::PlanIncomplete);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Elf64ImageTests"};
    Asteria::Test::TestSuccessfulPlan(runner);
    Asteria::Test::TestPermittedFlexibleFields(runner);
    Asteria::Test::TestDescriptorStyleEntry(runner);
    Asteria::Test::TestExecuteOnlyDirectEntry(runner);
    Asteria::Test::TestInputAndHeaderFailures(runner);
    Asteria::Test::TestSegmentFailures(runner);
    Asteria::Test::TestEntryFailuresAndOutputInvalidation(runner);
    return runner.Finish();
}
