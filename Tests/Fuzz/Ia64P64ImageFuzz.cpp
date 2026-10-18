// SPDX-License-Identifier: Apache-2.0
// Fuzz harness for the IA-64 P64 load-plan decoder (Ia64P64Image).
//
// Input layout: three profile bytes followed by the image.
//   byte 0: implemented virtual-address most significant bit, 47 + (b % 16),
//           so values below 50 and above 60 exercise profile rejection.
//   byte 1: user virtual region, b % 9 (regions 0 and 8 are invalid).
//   byte 2: page shift 12 + (b & 7) and load-segment capacity 1 + (b >> 3).
//
// Properties checked for every input:
// - Decoding never reads outside the image (sanitizers report violations).
// - A failure leaves the plan incomplete, even when it held a complete result
//   before the call, and exposes neither segments nor information. An
//   Elf64ImageRejected failure carries exactly the portable decoder's error;
//   every other policy failure happens only for images the portable decoder
//   accepts, and InvalidAddressProfile exactly for an invalid profile.
// - A success is deterministic, retains the returned information, and exposes
//   exactly the portable decoder's segments for the same image.
// - Every accepted mapping and its page range lie in the selected user region
//   and in implemented virtual addresses without crossing the implemented
//   hole; the entry descriptor is aligned, lies in readable file-backed bytes
//   and decodes to the returned bundle address and global pointer; the bundle
//   is aligned file-backed executable code; the global pointer is aligned
//   readable memory; the header machine and flags are the P64 values.

#include "Platform/Ia64/Executable/Ia64P64Image.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{
using Asteria::Executable::Elf::Elf64ByteView;
using Asteria::Executable::Elf::Elf64Image;
using Asteria::Executable::Elf::Elf64ImageError;
using Asteria::Executable::Elf::Elf64LoadPlan;
using Asteria::Executable::Elf::Elf64LoadPlanLimits;
using Asteria::Executable::Elf::Elf64LoadSegment;
using Asteria::Executable::Elf::Elf64SegmentPermission;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::UInt8;
using Asteria::Platform::Ia64::Executable::Ia64P64Image;
using Asteria::Platform::Ia64::Executable::Ia64P64ImageError;
using Asteria::Platform::Ia64::Executable::Ia64P64ImageInformation;
using Asteria::Platform::Ia64::Executable::Ia64P64LoadPlan;
using Asteria::Platform::Ia64::Executable::Ia64P64LoadPlanAccessError;

constexpr Size ProfilePrefixBytes{3U};
constexpr Size HeaderBytes{64U};
constexpr Size ProgramHeaderBytes{56U};
constexpr UInt64 MaximumUInt64{~UInt64{0U}};
constexpr UInt32 LoadType{1U};
constexpr UInt32 ExecuteFlag{1U};
constexpr UInt32 WriteFlag{2U};
constexpr UInt32 ReadFlag{4U};

struct Profile
{
    UInt8 ImplementedBit;
    UInt8 Region;
    Elf64LoadPlanLimits Limits;
};

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

[[nodiscard]] Profile DecodeProfile(const UInt8* const prefix) noexcept
{
    return Profile{
        .ImplementedBit = static_cast<UInt8>(47U + (prefix[0] % 16U)),
        .Region = static_cast<UInt8>(prefix[1] % 9U),
        .Limits =
            Elf64LoadPlanLimits{
                .PageSizeBytes = UInt64{1U} << (12U + (prefix[2] & 7U)),
                .MaximumSegmentAlignmentBytes = 0x1'0000U,
                .LoadSegmentCapacity = static_cast<Size>(1U + (prefix[2] >> 3U)),
            },
    };
}

[[nodiscard]] bool ProfileIsValid(const Profile& profile) noexcept
{
    return (profile.ImplementedBit >= 50U) && (profile.ImplementedBit <= 60U) && (profile.Region >= 1U) &&
           (profile.Region <= 7U);
}

// An address is implemented when its region offset bits above the implemented
// most significant bit repeat that bit.
[[nodiscard]] bool IsImplemented(const UInt64 address, const UInt8 implementedBit) noexcept
{
    const bool high = ((address >> implementedBit) & 1U) != 0U;
    for (UInt32 bit{implementedBit + 1U}; bit <= 60U; ++bit)
    {
        if ((((address >> bit) & 1U) != 0U) != high)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsUserRange(const UInt64 start, const UInt64 size, const Profile& profile) noexcept
{
    if ((size == 0U) || (start > (MaximumUInt64 - size)))
    {
        return false;
    }
    // Below the full 61-bit implementation a range must also stay in one
    // implemented lobe instead of spanning the unimplemented hole.
    const UInt64 last = start + size - 1U;
    const bool oneLobe = (profile.ImplementedBit == 60U) || ((((start ^ last) >> profile.ImplementedBit) & 1U) == 0U);
    return ((start >> 61U) == profile.Region) && ((last >> 61U) == profile.Region) &&
           IsImplemented(start, profile.ImplementedBit) && IsImplemented(last, profile.ImplementedBit) && oneLobe;
}

[[nodiscard]] bool SegmentContains(const Elf64LoadSegment& segment, const UInt64 start, const UInt64 size,
                                   const bool fileBacked) noexcept
{
    const UInt64 available = fileBacked ? segment.FileSizeBytes : segment.MemorySizeBytes;
    return (start >= segment.VirtualStart) && (size <= available) &&
           ((start - segment.VirtualStart) <= (available - size));
}

[[nodiscard]] bool SegmentsEqual(const Elf64LoadSegment& left, const Elf64LoadSegment& right) noexcept
{
    return (left.FileOffset == right.FileOffset) && (left.FileSizeBytes == right.FileSizeBytes) &&
           (left.VirtualStart == right.VirtualStart) && (left.MemorySizeBytes == right.MemorySizeBytes) &&
           (left.BssStart == right.BssStart) && (left.BssSizeBytes == right.BssSizeBytes) &&
           (left.PageStart == right.PageStart) && (left.PageCount == right.PageCount) &&
           (left.AlignmentBytes == right.AlignmentBytes) && (left.Permissions.GetBits() == right.Permissions.GetBits());
}

[[nodiscard]] bool InformationEqual(const Ia64P64ImageInformation& left, const Ia64P64ImageInformation& right) noexcept
{
    return (left.EntryDescriptorAddress == right.EntryDescriptorAddress) &&
           (left.EntryBundleAddress == right.EntryBundleAddress) && (left.GlobalPointer == right.GlobalPointer) &&
           (left.Flags == right.Flags) && (left.Machine == right.Machine) &&
           (left.ProgramHeaderCount == right.ProgramHeaderCount);
}

void CheckIncomplete(const Ia64P64LoadPlan& plan)
{
    Require(!plan.IsComplete(), "a failure leaves the plan incomplete");
    Require((plan.GetSegmentCount() == 0U) && (plan.GetPageSizeBytes() == 0U),
            "an incomplete plan exposes no segment count or page size");
    const auto information = plan.GetImageInformation();
    const auto segment = plan.GetSegment(0U);
    Require(!information.HasValue() && (information.GetError() == Ia64P64LoadPlanAccessError::PlanIncomplete) &&
                !segment.HasValue() && (segment.GetError() == Ia64P64LoadPlanAccessError::PlanIncomplete),
            "an incomplete plan exposes no information or segment");
}

// Finds the unique segment with the permission that contains the range.
[[nodiscard]] bool FindSegment(const Ia64P64LoadPlan& plan, const UInt64 start, const UInt64 size,
                               const Elf64SegmentPermission permission, const bool fileBacked,
                               Elf64LoadSegment& found) noexcept
{
    for (Size index{0U}; index < plan.GetSegmentCount(); ++index)
    {
        const Elf64LoadSegment segment = plan.GetSegment(index).GetValue();
        if (segment.Permissions.Contains(permission) && SegmentContains(segment, start, size, fileBacked))
        {
            found = segment;
            return true;
        }
    }
    return false;
}

void CheckAccepted(const Elf64ByteView image, const Profile& profile, const Ia64P64ImageInformation& information,
                   const Ia64P64LoadPlan& plan)
{
    Require(ProfileIsValid(profile), "only a valid address profile is accepted");
    Require(plan.IsComplete(), "a success completes the plan");
    const auto retained = plan.GetImageInformation();
    Require(retained.HasValue() && InformationEqual(retained.GetValue(), information),
            "the plan retains the returned information");
    Require((information.Machine == 50U) && (information.Flags == 0x10U), "the image is an IA-64 P64 image");

    Elf64LoadPlan portable{};
    const auto elf = Elf64Image::DecodeLoadPlan(image, profile.Limits, portable);
    Require(elf.HasValue() && (elf.GetValue().EntryAddress == information.EntryDescriptorAddress) &&
                (elf.GetValue().ProgramHeaderCount == information.ProgramHeaderCount) &&
                (portable.GetSegmentCount() == plan.GetSegmentCount()) &&
                (portable.GetPageSizeBytes() == plan.GetPageSizeBytes()),
            "the plan is the portable plan of the same image");

    const UInt64 pageSize = plan.GetPageSizeBytes();
    for (Size index{0U}; index < plan.GetSegmentCount(); ++index)
    {
        const auto segment = plan.GetSegment(index);
        Require(segment.HasValue() && SegmentsEqual(segment.GetValue(), portable.GetSegment(index).GetValue()),
                "every segment equals the portable segment");
        const Elf64LoadSegment& value = segment.GetValue();
        if (value.MemorySizeBytes == 0U)
        {
            Require(((value.VirtualStart >> 61U) == profile.Region) &&
                        IsImplemented(value.VirtualStart, profile.ImplementedBit),
                    "an empty segment names an implemented user address");
            continue;
        }
        Require(IsUserRange(value.VirtualStart, value.MemorySizeBytes, profile) &&
                    (value.PageCount <= (MaximumUInt64 / pageSize)) &&
                    IsUserRange(value.PageStart, value.PageCount * pageSize, profile),
                "every mapping and its pages lie in implemented user addresses");
    }

    const UInt64 descriptorAddress = information.EntryDescriptorAddress;
    Elf64LoadSegment descriptorSegment{};
    Require(((descriptorAddress & 15U) == 0U) && IsUserRange(descriptorAddress, 16U, profile) &&
                FindSegment(plan, descriptorAddress, 16U, Elf64SegmentPermission::Read, true, descriptorSegment),
            "the entry descriptor is aligned readable file-backed data");
    const UInt64 descriptorOffset = descriptorSegment.FileOffset + (descriptorAddress - descriptorSegment.VirtualStart);
    Require((descriptorOffset <= image.SizeBytes) && ((image.SizeBytes - descriptorOffset) >= 16U),
            "the entry descriptor lies inside the image");
    Require((ReadLittleEndian(image.Data + descriptorOffset, 8U) == information.EntryBundleAddress) &&
                (ReadLittleEndian(image.Data + descriptorOffset + 8U, 8U) == information.GlobalPointer),
            "the returned entry is the descriptor contents");

    Elf64LoadSegment ignored{};
    Require(((information.EntryBundleAddress & 15U) == 0U) &&
                IsUserRange(information.EntryBundleAddress, 16U, profile) &&
                FindSegment(plan, information.EntryBundleAddress, 16U, Elf64SegmentPermission::Execute, true, ignored),
            "the entry bundle is aligned file-backed executable code");
    Require(((information.GlobalPointer & 7U) == 0U) && IsUserRange(information.GlobalPointer, 8U, profile) &&
                FindSegment(plan, information.GlobalPointer, 8U, Elf64SegmentPermission::Read, false, ignored),
            "the global pointer is aligned readable memory");
}

// Seed construction: code, read-only data holding the entry descriptor, and
// data with BSS holding the global pointer, all at small offsets from one
// region base. Variants change the profile or one entry field.
constexpr UInt64 CodeOffset{0x100U};
constexpr UInt64 ReadOnlyOffset{0x200U};
constexpr UInt64 DataOffset{0x300U};
constexpr UInt64 CodeDelta{0x0400'0100ULL};
constexpr UInt64 ReadOnlyDelta{0x0400'8200ULL};
constexpr UInt64 DataDelta{0x0401'0300ULL};
constexpr UInt64 SegmentFileBytes{0x40U};
constexpr UInt64 DataMemoryBytes{0x2000U};
constexpr Size SeedImageBytes{0x340U};

struct SeedVariant
{
    UInt8 Prefix[ProfilePrefixBytes];
    UInt64 RegionBase;
    UInt32 Machine;
    UInt32 Flags;
    UInt64 DescriptorDelta;
    UInt64 BundleDelta;
    UInt64 GlobalPointerDelta;
    UInt32 CodeFlags;
};

UInt8 g_seed[ProfilePrefixBytes + SeedImageBytes];

void WriteProgramHeader(UInt8* const header, const UInt32 flags, const UInt64 fileOffset, const UInt64 virtualStart,
                        const UInt64 memoryBytes) noexcept
{
    WriteLittleEndian(header, LoadType, 4U);
    WriteLittleEndian(header + 4U, flags, 4U);
    WriteLittleEndian(header + 8U, fileOffset, 8U);
    WriteLittleEndian(header + 16U, virtualStart, 8U);
    WriteLittleEndian(header + 24U, virtualStart, 8U);
    WriteLittleEndian(header + 32U, SegmentFileBytes, 8U);
    WriteLittleEndian(header + 40U, memoryBytes, 8U);
    WriteLittleEndian(header + 48U, 0x100U, 8U);
}

// Writes the variant into g_seed and returns the input size.
[[nodiscard]] Size BuildImage(const SeedVariant& variant) noexcept
{
    for (UInt8& value : g_seed)
    {
        value = 0U;
    }
    for (Size index{0U}; index < ProfilePrefixBytes; ++index)
    {
        g_seed[index] = variant.Prefix[index];
    }
    UInt8* const image = g_seed + ProfilePrefixBytes;
    image[0] = 0x7FU;
    image[1] = static_cast<UInt8>('E');
    image[2] = static_cast<UInt8>('L');
    image[3] = static_cast<UInt8>('F');
    image[4] = 2U;
    image[5] = 1U;
    image[6] = 1U;
    WriteLittleEndian(image + 16U, 2U, 2U);
    WriteLittleEndian(image + 18U, variant.Machine, 2U);
    WriteLittleEndian(image + 20U, 1U, 4U);
    WriteLittleEndian(image + 24U, variant.RegionBase + variant.DescriptorDelta, 8U);
    WriteLittleEndian(image + 32U, HeaderBytes, 8U);
    WriteLittleEndian(image + 48U, variant.Flags, 4U);
    WriteLittleEndian(image + 52U, HeaderBytes, 2U);
    WriteLittleEndian(image + 54U, ProgramHeaderBytes, 2U);
    WriteLittleEndian(image + 56U, 3U, 2U);
    WriteProgramHeader(image + HeaderBytes, variant.CodeFlags, CodeOffset, variant.RegionBase + CodeDelta,
                       SegmentFileBytes);
    WriteProgramHeader(image + HeaderBytes + ProgramHeaderBytes, ReadFlag, ReadOnlyOffset,
                       variant.RegionBase + ReadOnlyDelta, SegmentFileBytes);
    WriteProgramHeader(image + HeaderBytes + (2U * ProgramHeaderBytes), ReadFlag | WriteFlag, DataOffset,
                       variant.RegionBase + DataDelta, DataMemoryBytes);
    constexpr UInt64 fileBackedDeltas[]{CodeDelta, ReadOnlyDelta, DataDelta};
    constexpr UInt64 fileOffsets[]{CodeOffset, ReadOnlyOffset, DataOffset};
    for (Size segment{0U}; segment < 3U; ++segment)
    {
        if ((variant.DescriptorDelta >= fileBackedDeltas[segment]) &&
            ((variant.DescriptorDelta - fileBackedDeltas[segment]) <= (SegmentFileBytes - 16U)))
        {
            UInt8* const descriptor =
                image + fileOffsets[segment] + (variant.DescriptorDelta - fileBackedDeltas[segment]);
            WriteLittleEndian(descriptor, variant.RegionBase + variant.BundleDelta, 8U);
            WriteLittleEndian(descriptor + 8U, variant.RegionBase + variant.GlobalPointerDelta, 8U);
        }
    }
    return ProfilePrefixBytes + SeedImageBytes;
}

constexpr UInt64 RegionOne{0x2000'0000'0000'0000ULL};
// Region three, upper implemented lobe for a 55-bit implementation.
constexpr UInt64 RegionThreeUpperLobe{0x7F80'0000'0000'0000ULL};
constexpr UInt64 RegionSeven{0xE000'0000'0000'0000ULL};
constexpr SeedVariant ValidRegionOne{
    {13U, 1U, 58U},        RegionOne, 50U, 0x10U, ReadOnlyDelta + 0x20U, CodeDelta + 0x10U, DataDelta + 0x1000U,
    ReadFlag | ExecuteFlag};

// Decoding starts from a complete plan, so a failure must retract it.
struct PrimedPlan
{
    UInt8 Image[SeedImageBytes];
    Ia64P64LoadPlan Plan;
};

PrimedPlan g_primed{};

void PrimePlan()
{
    static bool built{false};
    if (!built)
    {
        static_cast<void>(BuildImage(ValidRegionOne));
        for (Size index{0U}; index < SeedImageBytes; ++index)
        {
            g_primed.Image[index] = g_seed[ProfilePrefixBytes + index];
        }
        built = true;
    }
    const Profile profile = DecodeProfile(ValidRegionOne.Prefix);
    Require(Ia64P64Image::DecodeLoadPlan(Elf64ByteView{.Data = g_primed.Image, .SizeBytes = SeedImageBytes},
                                         profile.Limits, profile.ImplementedBit, profile.Region, g_primed.Plan)
                .HasValue(),
            "the priming image decodes");
}
} // namespace

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    constexpr UInt64 descriptor{ReadOnlyDelta + 0x20U};
    constexpr UInt64 bundle{CodeDelta + 0x10U};
    constexpr UInt64 globalPointer{DataDelta + 0x1000U};
    constexpr UInt32 code{ReadFlag | ExecuteFlag};
    constexpr SeedVariant variants[]{
        ValidRegionOne,
        {{6U, 7U, 56U}, RegionSeven, 50U, 0x10U, descriptor, bundle, globalPointer, code},
        {{8U, 3U, 57U}, RegionThreeUpperLobe, 50U, 0x10U, descriptor, bundle, DataDelta + 0x38U, code},
        // The descriptor in readable code and the global pointer in BSS.
        {{13U, 1U, 58U}, RegionOne, 50U, 0x10U, CodeDelta + 0x20U, bundle, DataDelta + 0x1FF8U, code},
        // Near misses, one policy check each.
        {{13U, 1U, 58U}, RegionOne, 62U, 0x10U, descriptor, bundle, globalPointer, code},
        {{13U, 1U, 58U}, RegionOne, 50U, 0U, descriptor, bundle, globalPointer, code},
        {{13U, 1U, 58U}, RegionOne, 50U, 0x10U, descriptor + 8U, bundle, globalPointer, code},
        {{13U, 1U, 58U}, RegionOne, 50U, 0x10U, CodeDelta + 0x20U, bundle, globalPointer, ExecuteFlag},
        {{13U, 1U, 58U}, RegionOne, 50U, 0x10U, descriptor, bundle + 8U, globalPointer, code},
        {{13U, 1U, 58U}, RegionOne, 50U, 0x10U, descriptor, ReadOnlyDelta, globalPointer, code},
        {{13U, 1U, 58U}, RegionOne, 50U, 0x10U, descriptor, bundle, globalPointer + 4U, code},
        {{13U, 1U, 58U}, RegionOne, 50U, 0x10U, descriptor, bundle, DataDelta + DataMemoryBytes, code},
        {{13U, 1U, 58U}, RegionOne, 50U, 0x10U, DataDelta + 0x100U, bundle, globalPointer, code},
        {{2U, 1U, 58U}, RegionOne, 50U, 0x10U, descriptor, bundle, globalPointer, code},
        {{13U, 2U, 58U}, RegionOne, 50U, 0x10U, descriptor, bundle, globalPointer, code},
        {{6U, 1U, 58U}, RegionOne | (UInt64{1U} << 57U), 50U, 0x10U, descriptor, bundle, globalPointer, code},
    };
    for (const SeedVariant& variant : variants)
    {
        emit(g_seed, BuildImage(variant));
    }
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    if (size < ProfilePrefixBytes)
    {
        return 0;
    }
    const Profile profile = DecodeProfile(data);
    const Elf64ByteView image{.Data = data + ProfilePrefixBytes, .SizeBytes = size - ProfilePrefixBytes};
    PrimePlan();
    Ia64P64LoadPlan& plan = g_primed.Plan;

    const auto result =
        Ia64P64Image::DecodeLoadPlan(image, profile.Limits, profile.ImplementedBit, profile.Region, plan);
    if (!result.HasValue())
    {
        CheckIncomplete(plan);
        const auto failure = result.GetError();
        Require(failure.Error != Ia64P64ImageError::None, "a failure reports an error");
        Require((failure.Error == Ia64P64ImageError::InvalidAddressProfile) == !ProfileIsValid(profile),
                "the address profile is rejected exactly when it is invalid");
        if (failure.Error == Ia64P64ImageError::InvalidAddressProfile)
        {
            return 0;
        }
        Elf64LoadPlan portable{};
        const auto elf = Elf64Image::DecodeLoadPlan(image, profile.Limits, portable);
        if (failure.Error == Ia64P64ImageError::Elf64ImageRejected)
        {
            Require(!elf.HasValue() && (elf.GetError() == failure.Elf64Error),
                    "a portable rejection carries the portable decoder's error");
        }
        else
        {
            Require(elf.HasValue() && (failure.Elf64Error == Elf64ImageError::None),
                    "a P64 policy failure concerns an image the portable decoder accepts");
        }
        return 0;
    }
    CheckAccepted(image, profile, result.GetValue(), plan);

    Ia64P64LoadPlan again{};
    const auto repeated =
        Ia64P64Image::DecodeLoadPlan(image, profile.Limits, profile.ImplementedBit, profile.Region, again);
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
