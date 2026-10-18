// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Executable/Ia64P64Image.hpp"

namespace Asteria::Platform::Ia64::Executable
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 Ia64RegionShift{61U};
constexpr Foundation::UInt64 P64EntryDescriptorSizeBytes{16U};
constexpr Foundation::UInt64 Ia64BundleSizeBytes{16U};
constexpr Foundation::UInt64 GlobalPointerAlignmentBytes{8U};

using Elf64LoadSegment = ::Asteria::Executable::Elf::Elf64LoadSegment;
using Elf64SegmentPermission = ::Asteria::Executable::Elf::Elf64SegmentPermission;
using ImageResult = Foundation::Result<Ia64P64ImageInformation, Ia64P64ImageFailure>;

[[nodiscard]] constexpr ImageResult Fail(const Ia64P64ImageError error) noexcept
{
    return ImageResult::Failure(Ia64P64ImageFailure{
        .Error = error,
        .Elf64Error = ::Asteria::Executable::Elf::Elf64ImageError::None,
    });
}

[[nodiscard]] constexpr ImageResult FailElf64(const ::Asteria::Executable::Elf::Elf64ImageError error) noexcept
{
    return ImageResult::Failure(Ia64P64ImageFailure{
        .Error = Ia64P64ImageError::Elf64ImageRejected,
        .Elf64Error = error,
    });
}

[[nodiscard]] constexpr bool TryAdd(const Foundation::UInt64 first, const Foundation::UInt64 second,
                                    Foundation::UInt64& result) noexcept
{
    if (first > (MaximumUInt64 - second))
    {
        return false;
    }
    result = first + second;
    return true;
}

[[nodiscard]] constexpr bool TryMultiplyPowerOfTwo(const Foundation::UInt64 value, Foundation::UInt64 powerOfTwo,
                                                   Foundation::UInt64& result) noexcept
{
    Foundation::UInt32 shift{0U};
    while (powerOfTwo > 1U)
    {
        powerOfTwo >>= 1U;
        ++shift;
    }
    if (value > (MaximumUInt64 >> shift))
    {
        return false;
    }
    result = value << shift;
    return true;
}

[[nodiscard]] constexpr bool IsAligned(const Foundation::UInt64 value, const Foundation::UInt64 alignment) noexcept
{
    return (value & (alignment - 1U)) == 0U;
}

[[nodiscard]] constexpr bool IsVirtualAddressImplemented(const Foundation::UInt64 address,
                                                         const Foundation::UInt8 implementedMostSignificantBit) noexcept
{
    if (implementedMostSignificantBit == MaximumIa64ImplementedVirtualAddressBit)
    {
        return true;
    }

    const Foundation::UInt8 unimplementedBitCount =
        static_cast<Foundation::UInt8>(MaximumIa64ImplementedVirtualAddressBit - implementedMostSignificantBit);
    const Foundation::UInt64 mask = (Foundation::UInt64{1U} << unimplementedBitCount) - 1U;
    const Foundation::UInt64 actual = (address >> (implementedMostSignificantBit + 1U)) & mask;
    const Foundation::UInt64 expected =
        ((address & (Foundation::UInt64{1U} << implementedMostSignificantBit)) != 0U) ? mask : 0U;
    return actual == expected;
}

[[nodiscard]] constexpr bool IsImplementedUserRange(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes,
                                                    const Foundation::UInt8 implementedMostSignificantBit,
                                                    const Foundation::UInt8 userVirtualRegion) noexcept
{
    Foundation::UInt64 end{0U};
    if ((sizeBytes == 0U) || !TryAdd(start, sizeBytes, end) || ((start >> Ia64RegionShift) != userVirtualRegion) ||
        (((end - 1U) >> Ia64RegionShift) != userVirtualRegion) ||
        !IsVirtualAddressImplemented(start, implementedMostSignificantBit) ||
        !IsVirtualAddressImplemented(end - 1U, implementedMostSignificantBit))
    {
        return false;
    }

    if (implementedMostSignificantBit == MaximumIa64ImplementedVirtualAddressBit)
    {
        return true;
    }
    const Foundation::UInt64 canonicalLobeBit = Foundation::UInt64{1U} << implementedMostSignificantBit;
    return ((start ^ (end - 1U)) & canonicalLobeBit) == 0U;
}

[[nodiscard]] constexpr bool IsImplementedUserAddress(const Foundation::UInt64 address,
                                                      const Foundation::UInt8 implementedMostSignificantBit,
                                                      const Foundation::UInt8 userVirtualRegion) noexcept
{
    return ((address >> Ia64RegionShift) == userVirtualRegion) &&
           IsVirtualAddressImplemented(address, implementedMostSignificantBit);
}

[[nodiscard]] bool IsLoadSegmentAddressValid(const Elf64LoadSegment& segment, const Foundation::UInt64 pageSizeBytes,
                                             const Foundation::UInt8 implementedMostSignificantBit,
                                             const Foundation::UInt8 userVirtualRegion) noexcept
{
    if (segment.MemorySizeBytes == 0U)
    {
        return (segment.PageCount == 0U) &&
               IsImplementedUserAddress(segment.VirtualStart, implementedMostSignificantBit, userVirtualRegion);
    }

    Foundation::UInt64 pageSize{0U};
    return IsImplementedUserRange(segment.VirtualStart, segment.MemorySizeBytes, implementedMostSignificantBit,
                                  userVirtualRegion) &&
           (segment.PageCount != 0U) && TryMultiplyPowerOfTwo(segment.PageCount, pageSizeBytes, pageSize) &&
           IsImplementedUserRange(segment.PageStart, pageSize, implementedMostSignificantBit, userVirtualRegion);
}

[[nodiscard]] constexpr bool SegmentContainsRange(const Elf64LoadSegment& segment, const Foundation::UInt64 start,
                                                  const Foundation::UInt64 sizeBytes,
                                                  const bool requireFileBacked) noexcept
{
    const Foundation::UInt64 availableSize = requireFileBacked ? segment.FileSizeBytes : segment.MemorySizeBytes;
    Foundation::UInt64 segmentEnd{0U};
    Foundation::UInt64 rangeEnd{0U};
    return TryAdd(segment.VirtualStart, availableSize, segmentEnd) && TryAdd(start, sizeBytes, rangeEnd) &&
           (start >= segment.VirtualStart) && (rangeEnd <= segmentEnd);
}

[[nodiscard]] bool FindSegmentContainingRange(const Ia64P64LoadPlan& plan, const Foundation::UInt64 start,
                                              const Foundation::UInt64 sizeBytes,
                                              const Elf64SegmentPermission requiredPermission,
                                              const bool requireFileBacked, Elf64LoadSegment& selected) noexcept
{
    for (Foundation::Size index{0U}; index < plan.GetSegmentCount(); ++index)
    {
        const auto segmentResult = plan.GetSegment(index);
        if (!segmentResult.HasValue())
        {
            return false;
        }
        const Elf64LoadSegment& segment = segmentResult.GetValue();
        if (segment.Permissions.Contains(requiredPermission) &&
            SegmentContainsRange(segment, start, sizeBytes, requireFileBacked))
        {
            selected = segment;
            return true;
        }
    }
    return false;
}

[[nodiscard]] Foundation::UInt64 ReadLittleEndian64(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::UInt32 index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}
} // namespace

Foundation::Result<Ia64P64ImageInformation, Ia64P64LoadPlanAccessError>
Ia64P64LoadPlan::GetImageInformation() const noexcept
{
    if (!m_isComplete)
    {
        return Foundation::Result<Ia64P64ImageInformation, Ia64P64LoadPlanAccessError>::Failure(
            Ia64P64LoadPlanAccessError::PlanIncomplete);
    }
    return Foundation::Result<Ia64P64ImageInformation, Ia64P64LoadPlanAccessError>::Success(m_imageInformation);
}

Foundation::Result<::Asteria::Executable::Elf::Elf64LoadSegment, Ia64P64LoadPlanAccessError>
Ia64P64LoadPlan::GetSegment(const Foundation::Size index) const noexcept
{
    if (!m_isComplete)
    {
        return Foundation::Result<::Asteria::Executable::Elf::Elf64LoadSegment, Ia64P64LoadPlanAccessError>::Failure(
            Ia64P64LoadPlanAccessError::PlanIncomplete);
    }
    const auto segment = m_elf64LoadPlan.GetSegment(index);
    if (!segment.HasValue())
    {
        return Foundation::Result<::Asteria::Executable::Elf::Elf64LoadSegment, Ia64P64LoadPlanAccessError>::Failure(
            Ia64P64LoadPlanAccessError::IndexOutOfBounds);
    }
    return Foundation::Result<::Asteria::Executable::Elf::Elf64LoadSegment, Ia64P64LoadPlanAccessError>::Success(
        segment.GetValue());
}

ImageResult Ia64P64Image::DecodeLoadPlan(const ::Asteria::Executable::Elf::Elf64ByteView image,
                                         const ::Asteria::Executable::Elf::Elf64LoadPlanLimits& limits,
                                         const Foundation::UInt8 implementedVirtualAddressMostSignificantBit,
                                         const Foundation::UInt8 userVirtualRegion, Ia64P64LoadPlan& output) noexcept
{
    output.m_isComplete = false;
    output.m_imageInformation = Ia64P64ImageInformation{};

    if ((implementedVirtualAddressMostSignificantBit < MinimumIa64ImplementedVirtualAddressBit) ||
        (implementedVirtualAddressMostSignificantBit > MaximumIa64ImplementedVirtualAddressBit) ||
        (userVirtualRegion < MinimumIa64UserVirtualRegion) || (userVirtualRegion > MaximumIa64UserVirtualRegion))
    {
        return Fail(Ia64P64ImageError::InvalidAddressProfile);
    }

    const auto elf64Information =
        ::Asteria::Executable::Elf::Elf64Image::DecodeLoadPlan(image, limits, output.m_elf64LoadPlan);
    if (!elf64Information.HasValue())
    {
        return FailElf64(elf64Information.GetError());
    }
    if (elf64Information.GetValue().Machine != Ia64ElfMachine)
    {
        return Fail(Ia64P64ImageError::UnsupportedMachine);
    }
    if (elf64Information.GetValue().Flags != Ia64P64ElfFlags)
    {
        return Fail(Ia64P64ImageError::UnsupportedFlags);
    }

    output.m_isComplete = true;
    for (Foundation::Size index{0U}; index < output.GetSegmentCount(); ++index)
    {
        const auto segment = output.GetSegment(index);
        if (!segment.HasValue() ||
            !IsLoadSegmentAddressValid(segment.GetValue(), output.GetPageSizeBytes(),
                                       implementedVirtualAddressMostSignificantBit, userVirtualRegion))
        {
            output.m_isComplete = false;
            return Fail(Ia64P64ImageError::InvalidLoadSegmentAddress);
        }
    }

    const Foundation::UInt64 descriptorAddress = elf64Information.GetValue().EntryAddress;
    if (!IsAligned(descriptorAddress, P64EntryDescriptorSizeBytes) ||
        !IsImplementedUserRange(descriptorAddress, P64EntryDescriptorSizeBytes,
                                implementedVirtualAddressMostSignificantBit, userVirtualRegion))
    {
        output.m_isComplete = false;
        return Fail(Ia64P64ImageError::InvalidEntryDescriptorAddress);
    }

    Elf64LoadSegment selectedSegment{};
    if (!FindSegmentContainingRange(output, descriptorAddress, P64EntryDescriptorSizeBytes,
                                    Elf64SegmentPermission::Read, true, selectedSegment))
    {
        output.m_isComplete = false;
        return Fail(Ia64P64ImageError::EntryDescriptorNotReadableFileBacked);
    }

    Foundation::UInt64 descriptorFileOffset{0U};
    const Foundation::UInt64 descriptorSegmentOffset = descriptorAddress - selectedSegment.VirtualStart;
    if (!TryAdd(selectedSegment.FileOffset, descriptorSegmentOffset, descriptorFileOffset) ||
        (descriptorFileOffset > static_cast<Foundation::UInt64>(image.SizeBytes)) ||
        (P64EntryDescriptorSizeBytes > (static_cast<Foundation::UInt64>(image.SizeBytes) - descriptorFileOffset)))
    {
        output.m_isComplete = false;
        return Fail(Ia64P64ImageError::EntryDescriptorOutsideImage);
    }

    const Foundation::UInt8* const descriptor = image.Data + static_cast<Foundation::Size>(descriptorFileOffset);
    const Foundation::UInt64 entryBundleAddress = ReadLittleEndian64(descriptor);
    const Foundation::UInt64 globalPointer = ReadLittleEndian64(descriptor + 8U);

    if (!IsAligned(entryBundleAddress, Ia64BundleSizeBytes) ||
        !IsImplementedUserRange(entryBundleAddress, Ia64BundleSizeBytes, implementedVirtualAddressMostSignificantBit,
                                userVirtualRegion))
    {
        output.m_isComplete = false;
        return Fail(Ia64P64ImageError::InvalidEntryBundleAddress);
    }
    if (!FindSegmentContainingRange(output, entryBundleAddress, Ia64BundleSizeBytes, Elf64SegmentPermission::Execute,
                                    true, selectedSegment))
    {
        output.m_isComplete = false;
        return Fail(Ia64P64ImageError::EntryBundleNotExecutableFileBacked);
    }

    if (!IsAligned(globalPointer, GlobalPointerAlignmentBytes) ||
        !IsImplementedUserRange(globalPointer, GlobalPointerAlignmentBytes, implementedVirtualAddressMostSignificantBit,
                                userVirtualRegion))
    {
        output.m_isComplete = false;
        return Fail(Ia64P64ImageError::InvalidGlobalPointerAddress);
    }
    if (!FindSegmentContainingRange(output, globalPointer, GlobalPointerAlignmentBytes, Elf64SegmentPermission::Read,
                                    false, selectedSegment))
    {
        output.m_isComplete = false;
        return Fail(Ia64P64ImageError::GlobalPointerNotReadable);
    }

    const Ia64P64ImageInformation information{
        .EntryDescriptorAddress = descriptorAddress,
        .EntryBundleAddress = entryBundleAddress,
        .GlobalPointer = globalPointer,
        .Flags = elf64Information.GetValue().Flags,
        .Machine = elf64Information.GetValue().Machine,
        .ProgramHeaderCount = elf64Information.GetValue().ProgramHeaderCount,
    };
    output.m_imageInformation = information;
    return ImageResult::Success(information);
}
} // namespace Asteria::Platform::Ia64::Executable
