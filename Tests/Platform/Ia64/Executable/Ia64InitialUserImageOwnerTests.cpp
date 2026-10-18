// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstring>
#include <new>
#include <type_traits>

#include "Boot/BootInfo.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Platform/Ia64/Executable/Ia64InitialUserImageOwner.hpp"
#include "Platform/Ia64/Ia64UserVirtualLayout.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace
{
constexpr Asteria::Foundation::Size OwnerTestImageSizeBytes{0x5000U};
constexpr Asteria::Foundation::Size OwnerTestBackingPageCount{152U};
constexpr Asteria::Foundation::Size OwnerTestBackingSizeBytes{OwnerTestBackingPageCount * 4096U};
} // namespace

extern "C"
{
alignas(4096U) Asteria::Foundation::UInt8 AsteriaIa64InitialUserImageOwnerTestStorage[OwnerTestBackingSizeBytes]{};
}

namespace Asteria::Test
{
namespace
{
[[nodiscard]] Boot::BootFileRegion SourceRegion() noexcept
{
    Boot::BootFileRegion region{};
    region.PhysicalStart = reinterpret_cast<Foundation::UInt64>(AsteriaIa64InitialUserImageOwnerTestStorage);
    region.PageCount = (20480U + 4095U) / 4096U;
    return region;
}

using Kernel::Memory::BootstrapPageAllocator;
using Kernel::Memory::BootstrapPageAllocatorError;
using Platform::Ia64::Executable::Ia64InitialUserImageNoSegment;
using Platform::Ia64::Executable::Ia64InitialUserImageOwner;
using Platform::Ia64::Executable::Ia64InitialUserImageOwnerAccessError;
using Platform::Ia64::Executable::Ia64InitialUserImageOwnerError;
using Platform::Ia64::Executable::Ia64InitialUserImageOwnerState;
using Platform::Ia64::Executable::Ia64P64ImageError;

constexpr Foundation::Size ProgramHeaderTableOffset{64U};
constexpr Foundation::Size ProgramHeaderSizeBytes{56U};
constexpr Foundation::UInt64 RegionOneBase{0x2000'0000'0000'0000ULL};
constexpr Foundation::UInt64 CodeVirtualStart{RegionOneBase + 0x0400'1100U};
constexpr Foundation::UInt64 ReadOnlyVirtualStart{RegionOneBase + 0x0400'3100U};
constexpr Foundation::UInt64 DataVirtualStart{RegionOneBase + 0x0400'8100U};
constexpr Foundation::UInt64 CodeFileOffset{0x1100U};
constexpr Foundation::UInt64 ReadOnlyFileOffset{0x2100U};
constexpr Foundation::UInt64 DataFileOffset{0x3100U};
constexpr Foundation::UInt64 DescriptorAddress{ReadOnlyVirtualStart + 0x20U};
constexpr Foundation::UInt64 EntryBundleAddress{CodeVirtualStart + 0x20U};
constexpr Foundation::UInt64 GlobalPointer{DataVirtualStart + 0x80U};
constexpr Foundation::UInt64 FirmwareWriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 PageSizeBytes{4096U};
constexpr Foundation::UInt64 MaximumSegmentPages{Platform::Ia64::Executable::Ia64InitialUserImageMaximumSegmentPages};
constexpr Foundation::UInt64 MaximumLoadPages{Platform::Ia64::Executable::Ia64InitialUserImageMaximumLoadPages};
// The aggregate case spreads one page over the limit across three segments
// at 16 MiB steps, two of them at the segment limit; the first ends before
// the memory stack's guard.
static_assert((2U * MaximumSegmentPages) < MaximumLoadPages);
static_assert((MaximumLoadPages - (2U * MaximumSegmentPages) + 1U) <= MaximumSegmentPages);
static_assert((MaximumSegmentPages * PageSizeBytes) <= (Platform::Ia64::Ia64InitialUserMemoryStackGuardedVirtualStart -
                                                        (CodeVirtualStart & ~(PageSizeBytes - 1U))));
constexpr Foundation::UInt64 SuccessAggregateLoadPageCount{7U};
constexpr Foundation::UInt64 SuccessArenaPageCount{15U};
constexpr Foundation::UInt8 ValidPhysicalAddressBits{48U};
constexpr Foundation::UInt8 ValidVirtualAddressMostSignificantBit{60U};

alignas(4096U) Foundation::UInt8 g_arenaStorage[OwnerTestBackingSizeBytes]{};
alignas(4096U) Foundation::UInt8 g_ownerAliasStorage[OwnerTestBackingSizeBytes]{};
alignas(4096U) Foundation::UInt8 g_allocatorAliasStorage[OwnerTestBackingSizeBytes]{};
Foundation::UInt8 g_sourceSnapshot[OwnerTestImageSizeBytes]{};
Boot::BootInfo g_bootInfo{};

static_assert(std::is_standard_layout_v<Platform::Ia64::Executable::Ia64InitialUserImageSegmentPlan>);
static_assert(std::is_trivially_copyable_v<Platform::Ia64::Executable::Ia64InitialUserImageSegmentPlan>);
static_assert(std::is_standard_layout_v<Platform::Ia64::Executable::Ia64InitialUserImageStageSummary>);
static_assert(std::is_trivially_copyable_v<Platform::Ia64::Executable::Ia64InitialUserImageStageSummary>);
static_assert(!std::is_copy_constructible_v<Ia64InitialUserImageOwner>);
static_assert(!std::is_copy_assignable_v<Ia64InitialUserImageOwner>);
static_assert(!std::is_move_constructible_v<Ia64InitialUserImageOwner>);
static_assert(!std::is_move_assignable_v<Ia64InitialUserImageOwner>);

[[nodiscard]] Foundation::Size GetProgramHeaderOffset(const Foundation::Size index) noexcept
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

void SetProgramHeader(const Foundation::Size index, const Foundation::UInt32 flags, const Foundation::UInt64 fileOffset,
                      const Foundation::UInt64 virtualStart, const Foundation::UInt64 fileSizeBytes,
                      const Foundation::UInt64 memorySizeBytes,
                      const Foundation::UInt64 alignmentBytes = PageSizeBytes) noexcept
{
    Foundation::UInt8* const header = AsteriaIa64InitialUserImageOwnerTestStorage + GetProgramHeaderOffset(index);
    WriteLittleEndian32(header, 1U);
    WriteLittleEndian32(header + 4U, flags);
    WriteLittleEndian64(header + 8U, fileOffset);
    WriteLittleEndian64(header + 16U, virtualStart);
    WriteLittleEndian64(header + 24U, virtualStart);
    WriteLittleEndian64(header + 32U, fileSizeBytes);
    WriteLittleEndian64(header + 40U, memorySizeBytes);
    WriteLittleEndian64(header + 48U, alignmentBytes);
}

void SetEntryDescriptor(const Foundation::UInt64 entryBundleAddress = EntryBundleAddress,
                        const Foundation::UInt64 globalPointer = GlobalPointer) noexcept
{
    const Foundation::Size descriptorOffset =
        static_cast<Foundation::Size>(ReadOnlyFileOffset + (DescriptorAddress - ReadOnlyVirtualStart));
    WriteLittleEndian64(AsteriaIa64InitialUserImageOwnerTestStorage + descriptorOffset, entryBundleAddress);
    WriteLittleEndian64(AsteriaIa64InitialUserImageOwnerTestStorage + descriptorOffset + 8U, globalPointer);
}

void FillPayload(const Foundation::UInt64 fileOffset, const Foundation::UInt64 sizeBytes,
                 const Foundation::UInt8 seed) noexcept
{
    for (Foundation::UInt64 index{0U}; index < sizeBytes; ++index)
    {
        AsteriaIa64InitialUserImageOwnerTestStorage[static_cast<Foundation::Size>(fileOffset + index)] =
            static_cast<Foundation::UInt8>(seed + static_cast<Foundation::UInt8>(index));
    }
}

void MakeValidImage() noexcept
{
    static_cast<void>(std::memset(AsteriaIa64InitialUserImageOwnerTestStorage, 0, OwnerTestImageSizeBytes));
    Foundation::UInt8* const image = AsteriaIa64InitialUserImageOwnerTestStorage;
    image[0U] = 0x7FU;
    image[1U] = static_cast<Foundation::UInt8>('E');
    image[2U] = static_cast<Foundation::UInt8>('L');
    image[3U] = static_cast<Foundation::UInt8>('F');
    image[4U] = 2U;
    image[5U] = 1U;
    image[6U] = 1U;
    WriteLittleEndian16(image + 16U, 2U);
    WriteLittleEndian16(image + 18U, Platform::Ia64::Executable::Ia64ElfMachine);
    WriteLittleEndian32(image + 20U, 1U);
    WriteLittleEndian64(image + 24U, DescriptorAddress);
    WriteLittleEndian64(image + 32U, ProgramHeaderTableOffset);
    WriteLittleEndian32(image + 48U, Platform::Ia64::Executable::Ia64P64ElfFlags);
    WriteLittleEndian16(image + 52U, 64U);
    WriteLittleEndian16(image + 54U, 56U);
    WriteLittleEndian16(image + 56U, 3U);

    SetProgramHeader(0U, 5U, CodeFileOffset, CodeVirtualStart, 0x180U, 0x300U);
    SetProgramHeader(1U, 4U, ReadOnlyFileOffset, ReadOnlyVirtualStart, 0x300U, 0x1100U);
    SetProgramHeader(2U, 6U, DataFileOffset, DataVirtualStart, 0x900U, 0x3F00U);
    FillPayload(CodeFileOffset, 0x180U, 0x31U);
    FillPayload(ReadOnlyFileOffset, 0x300U, 0x52U);
    FillPayload(DataFileOffset, 0x900U, 0x73U);
    SetEntryDescriptor();
}

void ResetBootInfo(const Foundation::UInt64 physicalStart, const Foundation::UInt64 pageCount,
                   const Foundation::UInt64 attributes = FirmwareWriteBackAttribute) noexcept
{
    static_cast<void>(std::memset(&g_bootInfo, 0, sizeof(g_bootInfo)));
    g_bootInfo.Signature = Boot::BootInfoSignature;
    g_bootInfo.Version = Boot::BootInfoVersion;
    g_bootInfo.SizeBytes = static_cast<Foundation::UInt32>(sizeof(g_bootInfo));
    g_bootInfo.MemoryRegionCount = 1U;
    g_bootInfo.FirmwareDescriptorVersion = 1U;
    g_bootInfo.FirmwareDescriptorSize = 40U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = 40U;
    g_bootInfo.MemoryRegions[0U] = Boot::PhysicalMemoryRegion{
        .PhysicalStart = physicalStart,
        .VirtualStart = 0U,
        .PageCount = pageCount,
        .Attributes = attributes,
        .FirmwareType = 7U,
        .Kind = Boot::PhysicalMemoryKind::Conventional,
    };
}

[[nodiscard]] bool InitializeAllocator(BootstrapPageAllocator& allocator,
                                       Foundation::UInt8* const storage = g_arenaStorage,
                                       const Foundation::UInt64 pageCount = OwnerTestBackingPageCount,
                                       const Foundation::UInt64 attributes = FirmwareWriteBackAttribute) noexcept
{
    ResetBootInfo(reinterpret_cast<Foundation::UIntPointer>(storage), pageCount, attributes);
    return allocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::None;
}

[[nodiscard]] bool BytesAreZero(const Foundation::UInt8* const bytes, const Foundation::Size sizeBytes) noexcept
{
    for (Foundation::Size index{0U}; index < sizeBytes; ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

void CheckSegmentBytes(TestContext& runner, const Platform::Ia64::Executable::Ia64InitialUserImageSegmentPlan& plan,
                       const char* const description) noexcept
{
    const Foundation::Size arenaOffset = static_cast<Foundation::Size>(
        plan.PhysicalSlice.PhysicalStart - reinterpret_cast<Foundation::UIntPointer>(g_arenaStorage));
    const Foundation::Size payloadOffset =
        static_cast<Foundation::Size>(plan.Segment.VirtualStart - plan.Segment.PageStart);
    const Foundation::Size payloadSize = static_cast<Foundation::Size>(plan.Segment.FileSizeBytes);
    const Foundation::Size bssOffset = static_cast<Foundation::Size>(plan.Segment.BssStart - plan.Segment.PageStart);
    const Foundation::Size bssSize = static_cast<Foundation::Size>(plan.Segment.BssSizeBytes);
    const Foundation::Size sliceSize = static_cast<Foundation::Size>(plan.PhysicalSlice.PageCount * PageSizeBytes);
    const Foundation::Size bssEnd = bssOffset + bssSize;
    ASTERIA_EXPECT(runner, description, BytesAreZero(g_arenaStorage + arenaOffset, payloadOffset),
                   std::memcmp(g_arenaStorage + arenaOffset + payloadOffset,
                               AsteriaIa64InitialUserImageOwnerTestStorage +
                                   static_cast<Foundation::Size>(plan.Segment.FileOffset),
                               payloadSize) == 0,
                   BytesAreZero(g_arenaStorage + arenaOffset + bssOffset, bssSize),
                   BytesAreZero(g_arenaStorage + arenaOffset + bssEnd, sliceSize - bssEnd));
}

void TestSuccessfulStage(TestContext& runner) noexcept
{
    MakeValidImage();
    static_cast<void>(std::memset(g_arenaStorage, 0xA5, sizeof(g_arenaStorage)));
    static_cast<void>(
        std::memcpy(g_sourceSnapshot, AsteriaIa64InitialUserImageOwnerTestStorage, OwnerTestImageSizeBytes));
    BootstrapPageAllocator allocator{};
    ASTERIA_EXPECT(runner, "the success fixture initializes a real bootstrap allocator",
                   InitializeAllocator(allocator));
    Ia64InitialUserImageOwner owner{};
    ASTERIA_EXPECT(runner, "a fresh owner hides its stage summary",
                   owner.GetStageSummary().GetError() == Ia64InitialUserImageOwnerAccessError::NotStaged);
    ASTERIA_EXPECT(runner, "a fresh owner hides segment plans",
                   owner.GetSegmentPlan(0U).GetError() == Ia64InitialUserImageOwnerAccessError::NotStaged);
    ASTERIA_EXPECT(runner, "a fresh owner has no retained failure",
                   owner.GetFailure().GetError() == Ia64InitialUserImageOwnerAccessError::NoFailure);

    const auto stage = owner.Stage(allocator, ValidPhysicalAddressBits, ValidVirtualAddressMostSignificantBit,
                                   {AsteriaIa64InitialUserImageOwnerTestStorage, 20480U}, SourceRegion());
    ASTERIA_EXPECT(runner, "a valid unaligned three-segment image stages once", stage.HasValue(),
                   owner.GetState() == Ia64InitialUserImageOwnerState::Staged);
    if (!stage.HasValue())
    {
        return;
    }

    const auto summaryResult = owner.GetStageSummary();
    const auto firstResult = owner.GetSegmentPlan(0U);
    const auto secondResult = owner.GetSegmentPlan(1U);
    const auto thirdResult = owner.GetSegmentPlan(2U);
    ASTERIA_EXPECT(runner, "the stage summary retains the exact P64 entry and GP contract", summaryResult.HasValue(),
                   summaryResult.GetValue().Image.EntryDescriptorAddress == DescriptorAddress,
                   summaryResult.GetValue().Image.EntryBundleAddress == EntryBundleAddress,
                   summaryResult.GetValue().Image.GlobalPointer == GlobalPointer,
                   summaryResult.GetValue().Image.Flags == Platform::Ia64::Executable::Ia64P64ElfFlags,
                   summaryResult.GetValue().Image.Machine == Platform::Ia64::Executable::Ia64ElfMachine);
    ASTERIA_EXPECT(runner, "the current shape owns one exact fifteen-page arena", summaryResult.HasValue(),
                   summaryResult.GetValue().SegmentCount == 3U,
                   summaryResult.GetValue().AggregateLoadPageCount == SuccessAggregateLoadPageCount,
                   summaryResult.GetValue().TotalBssSizeBytes == 0x4580U,
                   summaryResult.GetValue().AggregateArena.PageCount == SuccessArenaPageCount,
                   summaryResult.GetValue().AggregateArena.PhysicalStart ==
                       reinterpret_cast<Foundation::UIntPointer>(g_arenaStorage),
                   allocator.GetAllocatedPageCount() == SuccessArenaPageCount);
    ASTERIA_EXPECT(runner, "segments retain exact sorted packed physical slices", firstResult.HasValue(),
                   secondResult.HasValue(), thirdResult.HasValue(), firstResult.GetValue().Segment.PageCount == 1U,
                   secondResult.GetValue().Segment.PageCount == 2U, thirdResult.GetValue().Segment.PageCount == 4U,
                   firstResult.GetValue().PhysicalSlice.PhysicalStart ==
                       reinterpret_cast<Foundation::UIntPointer>(g_arenaStorage),
                   secondResult.GetValue().PhysicalSlice.PhysicalStart ==
                       reinterpret_cast<Foundation::UIntPointer>(g_arenaStorage) + PageSizeBytes,
                   thirdResult.GetValue().PhysicalSlice.PhysicalStart ==
                       reinterpret_cast<Foundation::UIntPointer>(g_arenaStorage) + (3U * PageSizeBytes));
    ASTERIA_EXPECT(runner, "the three decoded permission sets remain exact", firstResult.HasValue(),
                   secondResult.HasValue(), thirdResult.HasValue(),
                   firstResult.GetValue().Segment.Permissions.GetBits() == 5U,
                   secondResult.GetValue().Segment.Permissions.GetBits() == 1U,
                   thirdResult.GetValue().Segment.Permissions.GetBits() == 3U);
    ASTERIA_EXPECT(runner, "stack and RSE occupy the final eight pages without guard storage", summaryResult.HasValue(),
                   summaryResult.GetValue().MemoryStack.PhysicalStart ==
                       reinterpret_cast<Foundation::UIntPointer>(g_arenaStorage) + (7U * PageSizeBytes),
                   summaryResult.GetValue().MemoryStack.PageCount == 4U,
                   summaryResult.GetValue().RseBackingStore.PhysicalStart ==
                       reinterpret_cast<Foundation::UIntPointer>(g_arenaStorage) + (11U * PageSizeBytes),
                   summaryResult.GetValue().RseBackingStore.PageCount == 4U);
    CheckSegmentBytes(runner, firstResult.GetValue(), "the code slice has zero prefix, exact payload, BSS, and suffix");
    CheckSegmentBytes(runner, secondResult.GetValue(),
                      "the read-only slice has zero prefix, exact payload, BSS, and suffix");
    CheckSegmentBytes(runner, thirdResult.GetValue(), "the data slice has zero prefix, exact payload, BSS, and suffix");
    ASTERIA_EXPECT(runner, "all stack and RSE bytes remain zero", summaryResult.HasValue(),
                   BytesAreZero(g_arenaStorage + (7U * PageSizeBytes), 8U * PageSizeBytes));
    ASTERIA_EXPECT(
        runner, "staging leaves the complete mutable source fixture unchanged",
        std::memcmp(g_sourceSnapshot, AsteriaIa64InitialUserImageOwnerTestStorage, OwnerTestImageSizeBytes) == 0);
    ASTERIA_EXPECT(runner, "a staged owner rejects an out-of-range segment accessor",
                   owner.GetSegmentPlan(3U).GetError() == Ia64InitialUserImageOwnerAccessError::IndexOutOfBounds);
    ASTERIA_EXPECT(runner, "a staged owner exposes no failure",
                   owner.GetFailure().GetError() == Ia64InitialUserImageOwnerAccessError::NoFailure);
    const auto repeated = owner.Stage(allocator, ValidPhysicalAddressBits, ValidVirtualAddressMostSignificantBit,
                                      {AsteriaIa64InitialUserImageOwnerTestStorage, 20480U}, SourceRegion());
    ASTERIA_EXPECT(runner, "a successful owner is terminal without another allocation", !repeated.HasValue(),
                   repeated.GetError().Error == Ia64InitialUserImageOwnerError::AlreadyAttempted,
                   owner.GetState() == Ia64InitialUserImageOwnerState::Staged,
                   allocator.GetAllocatedPageCount() == SuccessArenaPageCount);
}

void CheckImageFailure(
    TestContext& runner, const Ia64InitialUserImageOwnerError expected, const char* const description,
    const Foundation::UInt8 physicalAddressBits = ValidPhysicalAddressBits,
    const Foundation::UInt8 virtualAddressMostSignificantBit = ValidVirtualAddressMostSignificantBit) noexcept
{
    BootstrapPageAllocator allocator{};
    ASTERIA_EXPECT(runner, "a rejection fixture initializes its real allocator", InitializeAllocator(allocator));
    Ia64InitialUserImageOwner owner{};
    const auto stage = owner.Stage(allocator, physicalAddressBits, virtualAddressMostSignificantBit,
                                   {AsteriaIa64InitialUserImageOwnerTestStorage, 20480U}, SourceRegion());
    ASTERIA_EXPECT(runner, description, !stage.HasValue(), stage.GetError().Error == expected,
                   owner.GetState() == Ia64InitialUserImageOwnerState::Failed);
    const auto failure = owner.GetFailure();
    ASTERIA_EXPECT(runner, "a terminal rejection retains its first failure", failure.HasValue(),
                   failure.GetValue().Error == expected);
    ASTERIA_EXPECT(runner, "a failed owner hides its summary",
                   owner.GetStageSummary().GetError() == Ia64InitialUserImageOwnerAccessError::NotStaged);
    const Foundation::UInt64 allocatedPages = allocator.GetAllocatedPageCount();
    const auto repeated = owner.Stage(allocator, physicalAddressBits, virtualAddressMostSignificantBit,
                                      {AsteriaIa64InitialUserImageOwnerTestStorage, 20480U}, SourceRegion());
    ASTERIA_EXPECT(runner, "a failed owner rejects retry and retains the original cause", !repeated.HasValue(),
                   repeated.GetError().Error == Ia64InitialUserImageOwnerError::AlreadyAttempted,
                   allocator.GetAllocatedPageCount() == allocatedPages, owner.GetFailure().HasValue(),
                   owner.GetFailure().GetValue().Error == expected);
}

void TestDecodeAndPolicyFailures(TestContext& runner) noexcept
{
    MakeValidImage();
    CheckImageFailure(runner, Ia64InitialUserImageOwnerError::P64ImageRejected,
                      "implemented virtual-address bit 49 retains a P64 rejection", ValidPhysicalAddressBits, 49U);

    MakeValidImage();
    AsteriaIa64InitialUserImageOwnerTestStorage[0U] = 0U;
    BootstrapPageAllocator allocator{};
    ASTERIA_EXPECT(runner, "the malformed P64 fixture initializes its allocator", InitializeAllocator(allocator));
    Ia64InitialUserImageOwner owner{};
    const auto invalidMagic = owner.Stage(allocator, ValidPhysicalAddressBits, ValidVirtualAddressMostSignificantBit,
                                          {AsteriaIa64InitialUserImageOwnerTestStorage, 20480U}, SourceRegion());
    ASTERIA_EXPECT(runner, "a generic ELF failure remains nested below the P64 cause", !invalidMagic.HasValue(),
                   invalidMagic.GetError().Error == Ia64InitialUserImageOwnerError::P64ImageRejected,
                   invalidMagic.GetError().P64.Error == Ia64P64ImageError::Elf64ImageRejected,
                   invalidMagic.GetError().P64.Elf64Error == ::Asteria::Executable::Elf::Elf64ImageError::InvalidMagic);

    MakeValidImage();
    CheckImageFailure(runner, Ia64InitialUserImageOwnerError::InvalidPhysicalAddressProfile,
                      "physical-address width 31 is rejected", 31U);
    MakeValidImage();
    CheckImageFailure(runner, Ia64InitialUserImageOwnerError::InvalidPhysicalAddressProfile,
                      "physical-address width 64 is rejected", 64U);

    MakeValidImage();
    WriteLittleEndian16(AsteriaIa64InitialUserImageOwnerTestStorage + 56U, 9U);
    for (Foundation::Size index{3U}; index < 9U; ++index)
    {
        SetProgramHeader(index, 4U, 0U,
                         RegionOneBase + 0x0500'0000U + (static_cast<Foundation::UInt64>(index) * PageSizeBytes), 0U,
                         0U, 1U);
    }
    CheckImageFailure(runner, Ia64InitialUserImageOwnerError::P64ImageRejected,
                      "nine load headers exceed the exact decoder capacity");

    MakeValidImage();
    SetProgramHeader(0U, 5U, CodeFileOffset, CodeVirtualStart, 0x180U, 0x300U, 128U * 1024U);
    CheckImageFailure(runner, Ia64InitialUserImageOwnerError::P64ImageRejected,
                      "segment alignment above 64 KiB is rejected by P64 decoding");

    MakeValidImage();
    SetProgramHeader(2U, 6U, DataFileOffset, DataVirtualStart, 0x900U,
                     ((MaximumSegmentPages + 1U) * PageSizeBytes) - 0x100U);
    CheckImageFailure(runner, Ia64InitialUserImageOwnerError::SegmentPageCapacityExceeded,
                      "a normalized segment one page over the limit is rejected before allocation");

    MakeValidImage();
    SetProgramHeader(0U, 5U, CodeFileOffset, CodeVirtualStart, 0x180U, (MaximumSegmentPages * PageSizeBytes) - 0x100U);
    SetProgramHeader(1U, 4U, ReadOnlyFileOffset, RegionOneBase + 0x0700'1100U, 0x300U,
                     (MaximumSegmentPages * PageSizeBytes) - 0x100U);
    SetProgramHeader(2U, 6U, DataFileOffset, RegionOneBase + 0x0800'1100U, 0x900U,
                     ((MaximumLoadPages - (2U * MaximumSegmentPages) + 1U) * PageSizeBytes) - 0x100U);
    WriteLittleEndian64(AsteriaIa64InitialUserImageOwnerTestStorage + 24U, RegionOneBase + 0x0700'1120U);
    const Foundation::Size descriptorOffset = static_cast<Foundation::Size>(ReadOnlyFileOffset + 0x20U);
    WriteLittleEndian64(AsteriaIa64InitialUserImageOwnerTestStorage + descriptorOffset, EntryBundleAddress);
    WriteLittleEndian64(AsteriaIa64InitialUserImageOwnerTestStorage + descriptorOffset + 8U,
                        RegionOneBase + 0x0800'1180U);
    CheckImageFailure(runner, Ia64InitialUserImageOwnerError::AggregatePageCapacityExceeded,
                      "an aggregate load plan one page over the limit is rejected before allocation");

    MakeValidImage();
    SetProgramHeader(0U, 5U, CodeFileOffset, CodeVirtualStart, 0x180U, 0x180U);
    SetProgramHeader(1U, 4U, ReadOnlyFileOffset, ReadOnlyVirtualStart, 0x300U, 0x300U);
    SetProgramHeader(2U, 6U, DataFileOffset, DataVirtualStart, 0x900U, 0x900U);
    CheckImageFailure(runner, Ia64InitialUserImageOwnerError::MissingRealBss,
                      "page padding alone does not satisfy the real-BSS requirement");
}

void TestGuardCollisions(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 guardedStarts[]{
        Platform::Ia64::Ia64UserExecutionProbeCodeGuardedVirtualStart,
        Platform::Ia64::Ia64UserExecutionProbeMemoryStackGuardedVirtualStart,
        Platform::Ia64::Ia64UserExecutionProbeRseBackingStoreGuardedVirtualStart,
        Platform::Ia64::Ia64InitialUserMemoryStackGuardedVirtualStart,
        Platform::Ia64::Ia64InitialUserRseBackingStoreGuardedVirtualStart,
    };
    for (Foundation::Size index{0U}; index < (sizeof(guardedStarts) / sizeof(guardedStarts[0U])); ++index)
    {
        MakeValidImage();
        const Foundation::UInt64 collidingCodeStart = guardedStarts[index] + 0x100U;
        SetProgramHeader(0U, 5U, CodeFileOffset, collidingCodeStart, 0x180U, 0x300U);
        SetEntryDescriptor(collidingCodeStart + 0x20U, GlobalPointer);
        CheckImageFailure(runner, Ia64InitialUserImageOwnerError::GuardedVirtualRangeConflict,
                          "a normalized segment collision with a guarded corridor is rejected");
    }
}

void TestAllocatorAndGeometryFailures(TestContext& runner) noexcept
{
    {
        MakeValidImage();
        BootstrapPageAllocator uninitialized{};
        Ia64InitialUserImageOwner owner{};
        const auto stage = owner.Stage(uninitialized, ValidPhysicalAddressBits, ValidVirtualAddressMostSignificantBit,
                                       {AsteriaIa64InitialUserImageOwnerTestStorage, 20480U}, SourceRegion());
        ASTERIA_EXPECT(runner, "an uninitialized allocator cause is retained exactly", !stage.HasValue(),
                       stage.GetError().Error == Ia64InitialUserImageOwnerError::ArenaAllocationRejected,
                       stage.GetError().Allocator == BootstrapPageAllocatorError::NotInitialized);
    }

    {
        MakeValidImage();
        BootstrapPageAllocator insufficient{};
        ASTERIA_EXPECT(runner, "the insufficient real allocator initializes",
                       InitializeAllocator(insufficient, g_arenaStorage, 14U));
        Ia64InitialUserImageOwner owner{};
        const auto stage = owner.Stage(insufficient, ValidPhysicalAddressBits, ValidVirtualAddressMostSignificantBit,
                                       {AsteriaIa64InitialUserImageOwnerTestStorage, 20480U}, SourceRegion());
        ASTERIA_EXPECT(runner, "an insufficient allocator cause is retained exactly", !stage.HasValue(),
                       stage.GetError().Allocator == BootstrapPageAllocatorError::InsufficientMemory);
    }

    {
        MakeValidImage();
        BootstrapPageAllocator badAttributes{};
        ASTERIA_EXPECT(runner, "an allocator can expose Conventional memory without write-back",
                       InitializeAllocator(badAttributes, g_arenaStorage, OwnerTestBackingPageCount, 0U));
        Ia64InitialUserImageOwner owner{};
        const auto stage = owner.Stage(badAttributes, ValidPhysicalAddressBits, ValidVirtualAddressMostSignificantBit,
                                       {AsteriaIa64InitialUserImageOwnerTestStorage, 20480U}, SourceRegion());
        ASTERIA_EXPECT(runner, "an arena without write-back attributes is terminally rejected", !stage.HasValue(),
                       stage.GetError().Error == Ia64InitialUserImageOwnerError::InvalidArenaAllocationAttributes);
    }

    {
        MakeValidImage();
        ResetBootInfo(Foundation::UInt64{1U} << 40U, 136U);
        BootstrapPageAllocator abovePal{};
        ASTERIA_EXPECT(runner, "the allocator admits geometry above a narrower PAL limit",
                       abovePal.Initialize(g_bootInfo) == BootstrapPageAllocatorError::None);
        Ia64InitialUserImageOwner owner{};
        const auto stage = owner.Stage(abovePal, 40U, ValidVirtualAddressMostSignificantBit,
                                       {AsteriaIa64InitialUserImageOwnerTestStorage, 20480U}, SourceRegion());
        ASTERIA_EXPECT(runner, "the owner rejects an arena ending above the effective PAL limit", !stage.HasValue(),
                       stage.GetError().Error == Ia64InitialUserImageOwnerError::InvalidArenaAllocation);
    }

    {
        MakeValidImage();
        ResetBootInfo(Foundation::UInt64{1U} << 50U, 136U);
        BootstrapPageAllocator aboveEncoder{};
        ASTERIA_EXPECT(runner, "the allocator admits geometry above the IA-64 encoder limit",
                       aboveEncoder.Initialize(g_bootInfo) == BootstrapPageAllocatorError::None);
        Ia64InitialUserImageOwner owner{};
        const auto stage = owner.Stage(aboveEncoder, 63U, ValidVirtualAddressMostSignificantBit,
                                       {AsteriaIa64InitialUserImageOwnerTestStorage, 20480U}, SourceRegion());
        ASTERIA_EXPECT(runner, "the owner rejects an arena ending above the 50-bit encoder limit", !stage.HasValue(),
                       stage.GetError().Error == Ia64InitialUserImageOwnerError::InvalidArenaAllocation);
    }
}

void TestAliasFailuresBeforeWrites(TestContext& runner) noexcept
{
    MakeValidImage();
    static_cast<void>(
        std::memcpy(g_sourceSnapshot, AsteriaIa64InitialUserImageOwnerTestStorage, OwnerTestImageSizeBytes));
    BootstrapPageAllocator sourceAllocator{};
    ASTERIA_EXPECT(runner, "the source-alias allocator initializes",
                   InitializeAllocator(sourceAllocator, AsteriaIa64InitialUserImageOwnerTestStorage));
    Ia64InitialUserImageOwner sourceOwner{};
    const auto sourceAlias =
        sourceOwner.Stage(sourceAllocator, ValidPhysicalAddressBits, ValidVirtualAddressMostSignificantBit,
                          {AsteriaIa64InitialUserImageOwnerTestStorage, 20480U}, SourceRegion());
    ASTERIA_EXPECT(
        runner, "an arena aliasing source is rejected before any write", !sourceAlias.HasValue(),
        sourceAlias.GetError().Error == Ia64InitialUserImageOwnerError::ArenaAliasesSource,
        std::memcmp(g_sourceSnapshot, AsteriaIa64InitialUserImageOwnerTestStorage, OwnerTestImageSizeBytes) == 0);

    MakeValidImage();
    static_cast<void>(std::memset(g_ownerAliasStorage, 0x5AU, sizeof(g_ownerAliasStorage)));
    auto* const owner = ::new (static_cast<void*>(g_ownerAliasStorage)) Ia64InitialUserImageOwner{};
    BootstrapPageAllocator ownerAllocator{};
    ASTERIA_EXPECT(runner, "the owner-alias allocator initializes",
                   InitializeAllocator(ownerAllocator, g_ownerAliasStorage));
    const Foundation::Size ownerCanaryOffset =
        static_cast<Foundation::Size>(SuccessArenaPageCount * PageSizeBytes) - 1U;
    g_ownerAliasStorage[ownerCanaryOffset] = 0x5AU;
    const auto ownerAlias =
        owner->Stage(ownerAllocator, ValidPhysicalAddressBits, ValidVirtualAddressMostSignificantBit,
                     {AsteriaIa64InitialUserImageOwnerTestStorage, 20480U}, SourceRegion());
    ASTERIA_EXPECT(runner, "an arena aliasing its owner is rejected before any write", !ownerAlias.HasValue(),
                   ownerAlias.GetError().Error == Ia64InitialUserImageOwnerError::ArenaAliasesControlStorage,
                   g_ownerAliasStorage[ownerCanaryOffset] == 0x5AU);

    MakeValidImage();
    static_cast<void>(std::memset(g_allocatorAliasStorage, 0x6BU, sizeof(g_allocatorAliasStorage)));
    auto* const allocator = ::new (static_cast<void*>(g_allocatorAliasStorage)) BootstrapPageAllocator{};
    ASTERIA_EXPECT(runner, "the allocator-alias instance initializes in its backing range",
                   InitializeAllocator(*allocator, g_allocatorAliasStorage));
    const Foundation::Size allocatorCanaryOffset =
        static_cast<Foundation::Size>(SuccessArenaPageCount * PageSizeBytes) - 1U;
    g_allocatorAliasStorage[allocatorCanaryOffset] = 0x6BU;
    Ia64InitialUserImageOwner allocatorOwner{};
    const auto allocatorAlias =
        allocatorOwner.Stage(*allocator, ValidPhysicalAddressBits, ValidVirtualAddressMostSignificantBit,
                             {AsteriaIa64InitialUserImageOwnerTestStorage, 20480U}, SourceRegion());
    ASTERIA_EXPECT(runner, "an arena aliasing its allocator is rejected before any write", !allocatorAlias.HasValue(),
                   allocatorAlias.GetError().Error == Ia64InitialUserImageOwnerError::ArenaAliasesControlStorage,
                   g_allocatorAliasStorage[allocatorCanaryOffset] == 0x6BU);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64InitialUserImageOwnerTests"};
    Asteria::Test::TestSuccessfulStage(runner);
    Asteria::Test::TestDecodeAndPolicyFailures(runner);
    Asteria::Test::TestGuardCollisions(runner);
    Asteria::Test::TestAllocatorAndGeometryFailures(runner);
    Asteria::Test::TestAliasFailuresBeforeWrites(runner);
    return runner.Finish();
}
