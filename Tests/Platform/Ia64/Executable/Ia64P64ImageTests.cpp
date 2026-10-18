// SPDX-License-Identifier: Apache-2.0
#include <cstdio>

#include "Platform/Ia64/Executable/Ia64P64Image.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Executable::Elf::Elf64ByteView;
using Executable::Elf::Elf64ImageError;
using Executable::Elf::Elf64LoadPlanLimits;
using Platform::Ia64::Executable::Ia64ElfMachine;
using Platform::Ia64::Executable::Ia64P64ElfFlags;
using Platform::Ia64::Executable::Ia64P64Image;
using Platform::Ia64::Executable::Ia64P64ImageError;
using Platform::Ia64::Executable::Ia64P64LoadPlan;
using Platform::Ia64::Executable::Ia64P64LoadPlanAccessError;

constexpr Foundation::Size ImageSizeBytes{0x5000U};
constexpr Foundation::Size ProgramHeaderTableOffset{64U};
constexpr Foundation::Size ProgramHeaderSizeBytes{56U};
constexpr Foundation::UInt64 RegionOneBase{0x2000'0000'0400'0000ULL};
constexpr Foundation::UInt64 CodeVirtualStart{RegionOneBase};
constexpr Foundation::UInt64 ReadOnlyVirtualStart{RegionOneBase + 0x2000U};
constexpr Foundation::UInt64 DataVirtualStart{RegionOneBase + 0x4000U};
constexpr Foundation::UInt64 DescriptorAddress{ReadOnlyVirtualStart + 0x20U};
constexpr Foundation::UInt64 EntryBundleAddress{CodeVirtualStart + 0x10U};
constexpr Foundation::UInt64 GlobalPointer{DataVirtualStart + 0x80U};
constexpr Foundation::UInt64 CodeFileOffset{0x1000U};
constexpr Foundation::UInt64 ReadOnlyFileOffset{0x2000U};
constexpr Foundation::UInt64 DataFileOffset{0x3000U};
constexpr Foundation::Size CodeHeaderIndex{0U};
constexpr Foundation::Size ReadOnlyHeaderIndex{1U};
constexpr Foundation::Size DataHeaderIndex{2U};

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

void SetProgramHeader(TestImage& image, const Foundation::Size index, const Foundation::UInt32 flags,
                      const Foundation::UInt64 fileOffset, const Foundation::UInt64 virtualStart,
                      const Foundation::UInt64 fileSizeBytes, const Foundation::UInt64 memorySizeBytes,
                      const Foundation::UInt64 alignmentBytes = 0x1000U) noexcept
{
    Foundation::UInt8* const header = image.Bytes + GetProgramHeaderOffset(index);
    WriteLittleEndian32(header, 1U);
    WriteLittleEndian32(header + 4U, flags);
    WriteLittleEndian64(header + 8U, fileOffset);
    WriteLittleEndian64(header + 16U, virtualStart);
    WriteLittleEndian64(header + 24U, virtualStart);
    WriteLittleEndian64(header + 32U, fileSizeBytes);
    WriteLittleEndian64(header + 40U, memorySizeBytes);
    WriteLittleEndian64(header + 48U, alignmentBytes);
}

void SetEntryDescriptor(TestImage& image, const Foundation::UInt64 entryBundleAddress,
                        const Foundation::UInt64 globalPointer) noexcept
{
    Foundation::UInt8* const descriptor = image.Bytes + ReadOnlyFileOffset + (DescriptorAddress - ReadOnlyVirtualStart);
    WriteLittleEndian64(descriptor, entryBundleAddress);
    WriteLittleEndian64(descriptor + 8U, globalPointer);
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
    WriteLittleEndian16(image.Bytes + 16U, 2U);
    WriteLittleEndian16(image.Bytes + 18U, Ia64ElfMachine);
    WriteLittleEndian32(image.Bytes + 20U, 1U);
    WriteLittleEndian64(image.Bytes + 24U, DescriptorAddress);
    WriteLittleEndian64(image.Bytes + 32U, ProgramHeaderTableOffset);
    WriteLittleEndian32(image.Bytes + 48U, Ia64P64ElfFlags);
    WriteLittleEndian16(image.Bytes + 52U, 64U);
    WriteLittleEndian16(image.Bytes + 54U, 56U);
    WriteLittleEndian16(image.Bytes + 56U, 3U);

    SetProgramHeader(image, CodeHeaderIndex, 5U, CodeFileOffset, CodeVirtualStart, 0x200U, 0x200U);
    SetProgramHeader(image, ReadOnlyHeaderIndex, 4U, ReadOnlyFileOffset, ReadOnlyVirtualStart, 0x200U, 0x200U);
    SetProgramHeader(image, DataHeaderIndex, 6U, DataFileOffset, DataVirtualStart, 0x200U, 0x300U);
    SetEntryDescriptor(image, EntryBundleAddress, GlobalPointer);
}

[[nodiscard]] constexpr Elf64LoadPlanLimits Limits() noexcept
{
    return Elf64LoadPlanLimits{
        .PageSizeBytes = 0x1000U,
        .MaximumSegmentAlignmentBytes = 0x1'0000U,
        .LoadSegmentCapacity = 8U,
    };
}

void CheckPolicyFailure(TestContext& runner, TestImage& image, const Ia64P64ImageError expected,
                        const char* const description, const Foundation::UInt8 implementedBit = 60U,
                        const Foundation::UInt8 userRegion = 1U) noexcept
{
    Ia64P64LoadPlan plan{};
    const auto result = Ia64P64Image::DecodeLoadPlan(Elf64ByteView{.Data = image.Bytes, .SizeBytes = ImageSizeBytes},
                                                     Limits(), implementedBit, userRegion, plan);
    ASTERIA_EXPECT(runner, description, !result.HasValue(), result.GetError().Error == expected);
    ASTERIA_EXPECT(runner, "a policy failure exposes no partial load plan", !plan.IsComplete(),
                   plan.GetPageSizeBytes() == 0U, plan.GetSegmentCount() == 0U);
    ASTERIA_EXPECT(runner, "a policy failure hides image information",
                   plan.GetImageInformation().GetError() == Ia64P64LoadPlanAccessError::PlanIncomplete);
}

void TestValidImage(TestContext& runner) noexcept
{
    TestImage image{};
    MakeValidImage(image);
    Ia64P64LoadPlan plan{};
    const auto result = Ia64P64Image::DecodeLoadPlan(Elf64ByteView{.Data = image.Bytes, .SizeBytes = ImageSizeBytes},
                                                     Limits(), 60U, 1U, plan);
    ASTERIA_EXPECT(runner, "the canonical IA-64 P64 image validates", result.HasValue());
    ASTERIA_EXPECT(runner, "success publishes the complete P64 entry contract", result.HasValue(),
                   result.GetValue().EntryDescriptorAddress == DescriptorAddress,
                   result.GetValue().EntryBundleAddress == EntryBundleAddress,
                   result.GetValue().GlobalPointer == GlobalPointer, result.GetValue().Flags == Ia64P64ElfFlags,
                   result.GetValue().Machine == Ia64ElfMachine, result.GetValue().ProgramHeaderCount == 3U);
    ASTERIA_EXPECT(runner, "success publishes the portable segment plan", plan.IsComplete(),
                   plan.GetPageSizeBytes() == 0x1000U, plan.GetSegmentCount() == 3U);
    const auto first = plan.GetSegment(0U);
    const auto last = plan.GetSegment(2U);
    ASTERIA_EXPECT(runner, "the P64 plan preserves sorted portable segments", first.HasValue(), last.HasValue(),
                   first.GetValue().VirtualStart == CodeVirtualStart, last.GetValue().VirtualStart == DataVirtualStart);
    ASTERIA_EXPECT(runner, "the completed plan exposes the validated P64 information",
                   plan.GetImageInformation().HasValue(),
                   plan.GetImageInformation().GetValue().GlobalPointer == GlobalPointer);
    ASTERIA_EXPECT(runner, "the completed plan rejects an out-of-range segment index",
                   plan.GetSegment(3U).GetError() == Ia64P64LoadPlanAccessError::IndexOutOfBounds);

    image.Bytes[0U] = 0U;
    const auto rejected = Ia64P64Image::DecodeLoadPlan(Elf64ByteView{.Data = image.Bytes, .SizeBytes = ImageSizeBytes},
                                                       Limits(), 60U, 1U, plan);
    ASTERIA_EXPECT(runner, "a later generic failure retains its precise ELF64 cause", !rejected.HasValue(),
                   rejected.GetError().Error == Ia64P64ImageError::Elf64ImageRejected,
                   rejected.GetError().Elf64Error == Elf64ImageError::InvalidMagic);
    ASTERIA_EXPECT(runner, "a later failure invalidates an earlier successful plan", !plan.IsComplete(),
                   plan.GetSegment(0U).GetError() == Ia64P64LoadPlanAccessError::PlanIncomplete);
}

void TestHeaderAndProfileFailures(TestContext& runner) noexcept
{
    TestImage image{};
    MakeValidImage(image);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::InvalidAddressProfile,
                       "implemented virtual-address bit 49 is rejected", 49U);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::InvalidAddressProfile,
                       "implemented virtual-address bit 61 is rejected", 61U);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::InvalidAddressProfile,
                       "Region 0 is rejected as a user image region", 60U, 0U);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::InvalidAddressProfile, "virtual region 8 is rejected", 60U,
                       8U);

    MakeValidImage(image);
    WriteLittleEndian16(image.Bytes + 18U, 0x003EU);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::UnsupportedMachine, "an AMD64 machine number is rejected");

    MakeValidImage(image);
    WriteLittleEndian32(image.Bytes + 48U, 0U);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::UnsupportedFlags,
                       "an ELF without the P64 ABI flag is rejected");

    MakeValidImage(image);
    WriteLittleEndian32(image.Bytes + 48U, Ia64P64ElfFlags | 1U);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::UnsupportedFlags, "unsupported IA-64 ELF flags are rejected");
}

void TestSegmentAddressFailures(TestContext& runner) noexcept
{
    TestImage image{};
    MakeValidImage(image);
    SetProgramHeader(image, DataHeaderIndex, 6U, DataFileOffset, 0x4000'0000'0400'4000ULL, 0x200U, 0x300U);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::InvalidLoadSegmentAddress,
                       "a load segment in another virtual region is rejected");

    MakeValidImage(image);
    SetProgramHeader(image, DataHeaderIndex, 6U, DataFileOffset, DataVirtualStart | (Foundation::UInt64{1U} << 54U),
                     0x200U, 0x300U);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::InvalidLoadSegmentAddress,
                       "a noncanonical segment address is rejected", 53U);

    MakeValidImage(image);
    constexpr Foundation::UInt64 CanonicalEdge{RegionOneBase + (Foundation::UInt64{1U} << 53U) - 0x800U};
    SetProgramHeader(image, DataHeaderIndex, 6U, 0x3800U, CanonicalEdge, 0x200U, 0x1000U);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::InvalidLoadSegmentAddress,
                       "a segment crossing the PAL address hole is rejected", 53U);

    MakeValidImage(image);
    constexpr Foundation::UInt64 HighCanonicalLobeStart{0x3FE0'0000'0000'0000ULL};
    constexpr Foundation::UInt64 ThroughHoleSizeBytes{HighCanonicalLobeStart + 0x1000U - DataVirtualStart};
    SetProgramHeader(image, DataHeaderIndex, 6U, DataFileOffset, DataVirtualStart, 0x200U, ThroughHoleSizeBytes);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::InvalidLoadSegmentAddress,
                       "a segment spanning both canonical lobes is rejected", 53U);

    MakeValidImage(image);
    WriteLittleEndian16(image.Bytes + 56U, 4U);
    SetProgramHeader(image, 3U, 4U, 0U, 0U, 0U, 0U, 1U);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::InvalidLoadSegmentAddress,
                       "a zero-length load header still requires a valid user address");
}

void TestEntryDescriptorFailures(TestContext& runner) noexcept
{
    TestImage image{};
    MakeValidImage(image);
    WriteLittleEndian64(image.Bytes + 24U, DescriptorAddress + 8U);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::InvalidEntryDescriptorAddress,
                       "a misaligned P64 entry descriptor is rejected");

    MakeValidImage(image);
    SetProgramHeader(image, ReadOnlyHeaderIndex, 0U, ReadOnlyFileOffset, ReadOnlyVirtualStart, 0x200U, 0x200U);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::EntryDescriptorNotReadableFileBacked,
                       "a non-readable P64 entry descriptor is rejected");

    MakeValidImage(image);
    SetProgramHeader(image, ReadOnlyHeaderIndex, 4U, ReadOnlyFileOffset, ReadOnlyVirtualStart, 0x28U, 0x200U);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::EntryDescriptorNotReadableFileBacked,
                       "a partially file-backed P64 descriptor is rejected");
}

void TestEntryBundleFailures(TestContext& runner) noexcept
{
    TestImage image{};
    MakeValidImage(image);
    SetEntryDescriptor(image, EntryBundleAddress + 1U, GlobalPointer);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::InvalidEntryBundleAddress,
                       "a non-bundle-aligned entry address is rejected");

    MakeValidImage(image);
    SetEntryDescriptor(image, ReadOnlyVirtualStart + 0x40U, GlobalPointer);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::EntryBundleNotExecutableFileBacked,
                       "an entry in readable non-executable bytes is rejected");

    MakeValidImage(image);
    SetProgramHeader(image, CodeHeaderIndex, 5U, CodeFileOffset, CodeVirtualStart, 0x18U, 0x200U);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::EntryBundleNotExecutableFileBacked,
                       "an entry bundle straddling file-backed code is rejected");
}

void TestGlobalPointerPolicy(TestContext& runner) noexcept
{
    TestImage image{};
    MakeValidImage(image);
    SetEntryDescriptor(image, EntryBundleAddress, GlobalPointer + 1U);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::InvalidGlobalPointerAddress,
                       "a misaligned global pointer is rejected");

    MakeValidImage(image);
    SetEntryDescriptor(image, EntryBundleAddress, DataVirtualStart + 0x1000U);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::GlobalPointerNotReadable,
                       "a global pointer outside every mapping is rejected");

    MakeValidImage(image);
    SetProgramHeader(image, CodeHeaderIndex, 1U, CodeFileOffset, CodeVirtualStart, 0x200U, 0x200U);
    SetEntryDescriptor(image, EntryBundleAddress, EntryBundleAddress);
    CheckPolicyFailure(runner, image, Ia64P64ImageError::GlobalPointerNotReadable,
                       "an execute-only mapping cannot contain the global pointer");

    MakeValidImage(image);
    constexpr Foundation::UInt64 BssGlobalPointer{DataVirtualStart + 0x280U};
    SetEntryDescriptor(image, EntryBundleAddress, BssGlobalPointer);
    Ia64P64LoadPlan plan{};
    const auto result = Ia64P64Image::DecodeLoadPlan(Elf64ByteView{.Data = image.Bytes, .SizeBytes = ImageSizeBytes},
                                                     Limits(), 53U, 1U, plan);
    ASTERIA_EXPECT(runner, "readable zero-fill memory may contain the global pointer", result.HasValue(),
                   result.GetValue().GlobalPointer == BssGlobalPointer);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64P64ImageTests"};
    Asteria::Test::TestValidImage(runner);
    Asteria::Test::TestHeaderAndProfileFailures(runner);
    Asteria::Test::TestSegmentAddressFailures(runner);
    Asteria::Test::TestEntryDescriptorFailures(runner);
    Asteria::Test::TestEntryBundleFailures(runner);
    Asteria::Test::TestGlobalPointerPolicy(runner);
    return runner.Finish();
}
