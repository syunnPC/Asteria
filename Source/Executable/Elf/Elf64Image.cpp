// SPDX-License-Identifier: Apache-2.0

#include "Executable/Elf/Elf64Image.hpp"

namespace Asteria::Executable::Elf
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::Size Elf64HeaderSizeBytes{64U};
constexpr Foundation::UInt16 Elf64ProgramHeaderSizeBytes{56U};
constexpr Foundation::UInt16 ExtendedProgramHeaderCount{0xFFFFU};
constexpr Foundation::UInt16 ExecutableImageType{2U};
constexpr Foundation::UInt32 LoadProgramHeaderType{1U};
constexpr Foundation::UInt32 DynamicProgramHeaderType{2U};
constexpr Foundation::UInt32 InterpreterProgramHeaderType{3U};
constexpr Foundation::UInt32 ExecuteFlag{1U};
constexpr Foundation::UInt32 WriteFlag{2U};
constexpr Foundation::UInt32 ReadFlag{4U};
constexpr Foundation::UInt32 KnownPermissionFlags{ExecuteFlag | WriteFlag | ReadFlag};

[[nodiscard]] Foundation::UInt16 ReadLittleEndian16(const Foundation::UInt8* const bytes) noexcept
{
    return static_cast<Foundation::UInt16>(bytes[0U]) |
           static_cast<Foundation::UInt16>(static_cast<Foundation::UInt16>(bytes[1U]) << 8U);
}

[[nodiscard]] Foundation::UInt32 ReadLittleEndian32(const Foundation::UInt8* const bytes) noexcept
{
    return static_cast<Foundation::UInt32>(bytes[0U]) | (static_cast<Foundation::UInt32>(bytes[1U]) << 8U) |
           (static_cast<Foundation::UInt32>(bytes[2U]) << 16U) | (static_cast<Foundation::UInt32>(bytes[3U]) << 24U);
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

[[nodiscard]] bool IsPowerOfTwo(const Foundation::UInt64 value) noexcept
{
    return (value != 0U) && ((value & (value - 1U)) == 0U);
}

[[nodiscard]] bool TryAdd(const Foundation::UInt64 first, const Foundation::UInt64 second,
                          Foundation::UInt64& result) noexcept
{
    if (first > (MaximumUInt64 - second))
    {
        return false;
    }
    result = first + second;
    return true;
}

[[nodiscard]] Foundation::UInt32 GetPowerOfTwoShift(Foundation::UInt64 value) noexcept
{
    Foundation::UInt32 shift{0U};
    while (value > 1U)
    {
        value >>= 1U;
        ++shift;
    }
    return shift;
}

[[nodiscard]] bool TryNormalizePageRange(const Foundation::UInt64 virtualStart, const Foundation::UInt64 memoryEnd,
                                         const Foundation::UInt64 memorySizeBytes,
                                         const Foundation::UInt64 pageSizeBytes, Foundation::UInt64& pageStart,
                                         Foundation::UInt64& pageCount, Foundation::UInt64& pageEnd) noexcept
{
    const Foundation::UInt64 pageMask = pageSizeBytes - 1U;
    pageStart = virtualStart & ~pageMask;
    if (memorySizeBytes == 0U)
    {
        pageCount = 0U;
        pageEnd = pageStart;
        return true;
    }

    const Foundation::UInt64 remainder = memoryEnd & pageMask;
    pageEnd = memoryEnd;
    if (remainder != 0U)
    {
        const Foundation::UInt64 padding = pageSizeBytes - remainder;
        if (!TryAdd(memoryEnd, padding, pageEnd))
        {
            return false;
        }
    }
    pageCount = (pageEnd - pageStart) >> GetPowerOfTwoShift(pageSizeBytes);
    return true;
}

[[nodiscard]] bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                 const Foundation::UInt64 secondStart, const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] Elf64SegmentPermissions DecodePermissions(const Foundation::UInt32 flags) noexcept
{
    Elf64SegmentPermissions permissions{};
    if ((flags & ReadFlag) != 0U)
    {
        permissions = permissions.With(Elf64SegmentPermission::Read);
    }
    if ((flags & WriteFlag) != 0U)
    {
        permissions = permissions.With(Elf64SegmentPermission::Write);
    }
    if ((flags & ExecuteFlag) != 0U)
    {
        permissions = permissions.With(Elf64SegmentPermission::Execute);
    }
    return permissions;
}

[[nodiscard]] bool SegmentPrecedes(const Elf64LoadSegment& first, const Elf64LoadSegment& second) noexcept
{
    return (first.PageStart < second.PageStart) ||
           ((first.PageStart == second.PageStart) && (first.VirtualStart < second.VirtualStart));
}
} // namespace

Foundation::Result<Elf64ImageInformation, Elf64LoadPlanAccessError> Elf64LoadPlan::GetImageInformation() const noexcept
{
    if (!m_isComplete)
    {
        return Foundation::Result<Elf64ImageInformation, Elf64LoadPlanAccessError>::Failure(
            Elf64LoadPlanAccessError::PlanIncomplete);
    }
    return Foundation::Result<Elf64ImageInformation, Elf64LoadPlanAccessError>::Success(m_imageInformation);
}

Foundation::Result<Elf64LoadSegment, Elf64LoadPlanAccessError>
Elf64LoadPlan::GetSegment(const Foundation::Size index) const noexcept
{
    if (!m_isComplete)
    {
        return Foundation::Result<Elf64LoadSegment, Elf64LoadPlanAccessError>::Failure(
            Elf64LoadPlanAccessError::PlanIncomplete);
    }
    if (index >= m_segmentCount)
    {
        return Foundation::Result<Elf64LoadSegment, Elf64LoadPlanAccessError>::Failure(
            Elf64LoadPlanAccessError::IndexOutOfBounds);
    }
    return Foundation::Result<Elf64LoadSegment, Elf64LoadPlanAccessError>::Success(m_segments[index]);
}

Foundation::Result<Elf64ImageInformation, Elf64ImageError>
Elf64Image::DecodeLoadPlan(const Elf64ByteView image, const Elf64LoadPlanLimits& limits, Elf64LoadPlan& output) noexcept
{
    output.m_isComplete = false;
    output.m_segmentCount = 0U;
    output.m_pageSizeBytes = 0U;
    output.m_imageInformation = Elf64ImageInformation{};

    if ((image.Data == nullptr) && (image.SizeBytes != 0U))
    {
        return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(Elf64ImageError::InvalidByteView);
    }
    if (!IsPowerOfTwo(limits.PageSizeBytes) || !IsPowerOfTwo(limits.MaximumSegmentAlignmentBytes) ||
        (limits.LoadSegmentCapacity > MaximumElf64LoadSegments))
    {
        return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(Elf64ImageError::InvalidLimits);
    }
    if (image.SizeBytes < Elf64HeaderSizeBytes)
    {
        return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(Elf64ImageError::HeaderTooSmall);
    }

    if ((image.Data[0U] != 0x7FU) || (image.Data[1U] != static_cast<Foundation::UInt8>('E')) ||
        (image.Data[2U] != static_cast<Foundation::UInt8>('L')) ||
        (image.Data[3U] != static_cast<Foundation::UInt8>('F')))
    {
        return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(Elf64ImageError::InvalidMagic);
    }
    if (image.Data[4U] != 2U)
    {
        return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(Elf64ImageError::UnsupportedClass);
    }
    if (image.Data[5U] != 1U)
    {
        return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
            Elf64ImageError::UnsupportedEndianness);
    }
    if ((image.Data[6U] != 1U) || (ReadLittleEndian32(image.Data + 20U) != 1U))
    {
        return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(Elf64ImageError::UnsupportedVersion);
    }
    if (ReadLittleEndian16(image.Data + 16U) != ExecutableImageType)
    {
        return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
            Elf64ImageError::UnsupportedImageType);
    }
    if (ReadLittleEndian16(image.Data + 52U) != Elf64HeaderSizeBytes)
    {
        return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(Elf64ImageError::InvalidHeaderSize);
    }
    if (ReadLittleEndian16(image.Data + 54U) != Elf64ProgramHeaderSizeBytes)
    {
        return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
            Elf64ImageError::InvalidProgramHeaderSize);
    }

    const Foundation::UInt16 programHeaderCount = ReadLittleEndian16(image.Data + 56U);
    if (programHeaderCount == ExtendedProgramHeaderCount)
    {
        return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
            Elf64ImageError::UnsupportedExtendedProgramHeaderCount);
    }

    const Foundation::UInt64 programHeaderOffset = ReadLittleEndian64(image.Data + 32U);
    const Foundation::UInt64 extendedProgramHeaderCount = static_cast<Foundation::UInt64>(programHeaderCount);
    if (extendedProgramHeaderCount > (MaximumUInt64 / Elf64ProgramHeaderSizeBytes))
    {
        return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
            Elf64ImageError::ProgramHeaderTableOverflow);
    }
    const Foundation::UInt64 programHeaderTableSize = extendedProgramHeaderCount * Elf64ProgramHeaderSizeBytes;
    if (programHeaderOffset > (MaximumUInt64 - programHeaderTableSize))
    {
        return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
            Elf64ImageError::ProgramHeaderTableOverflow);
    }
    const Foundation::UInt64 imageSizeBytes = static_cast<Foundation::UInt64>(image.SizeBytes);
    if ((programHeaderOffset > imageSizeBytes) || (programHeaderTableSize > (imageSizeBytes - programHeaderOffset)))
    {
        return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
            Elf64ImageError::ProgramHeaderTableOutsideImage);
    }

    const Foundation::UInt64 entryAddress = ReadLittleEndian64(image.Data + 24U);
    for (Foundation::UInt16 index{0U}; index < programHeaderCount; ++index)
    {
        const Foundation::UInt64 headerOffset =
            programHeaderOffset + (static_cast<Foundation::UInt64>(index) * Elf64ProgramHeaderSizeBytes);
        const Foundation::UInt8* const header = image.Data + static_cast<Foundation::Size>(headerOffset);
        const Foundation::UInt32 type = ReadLittleEndian32(header);
        if (type == InterpreterProgramHeaderType)
        {
            return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
                Elf64ImageError::InterpreterUnsupported);
        }
        if (type == DynamicProgramHeaderType)
        {
            return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
                Elf64ImageError::RuntimeRelocationUnsupported);
        }
        if (type != LoadProgramHeaderType)
        {
            continue;
        }
        if (output.m_segmentCount >= limits.LoadSegmentCapacity)
        {
            return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
                Elf64ImageError::LoadSegmentCapacityExceeded);
        }

        const Foundation::UInt32 flags = ReadLittleEndian32(header + 4U);
        const Foundation::UInt64 fileOffset = ReadLittleEndian64(header + 8U);
        const Foundation::UInt64 virtualStart = ReadLittleEndian64(header + 16U);
        const Foundation::UInt64 fileSizeBytes = ReadLittleEndian64(header + 32U);
        const Foundation::UInt64 memorySizeBytes = ReadLittleEndian64(header + 40U);
        const Foundation::UInt64 alignmentBytes = ReadLittleEndian64(header + 48U);
        if (fileSizeBytes > memorySizeBytes)
        {
            return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
                Elf64ImageError::FileSizeExceedsMemorySize);
        }

        Foundation::UInt64 fileEnd{0U};
        if (!TryAdd(fileOffset, fileSizeBytes, fileEnd))
        {
            return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
                Elf64ImageError::FileRangeOverflow);
        }
        if ((fileOffset > imageSizeBytes) || (fileEnd > imageSizeBytes))
        {
            return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
                Elf64ImageError::FileRangeOutsideImage);
        }

        Foundation::UInt64 memoryEnd{0U};
        if (!TryAdd(virtualStart, memorySizeBytes, memoryEnd))
        {
            return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
                Elf64ImageError::VirtualRangeOverflow);
        }
        if ((alignmentBytes > 1U) &&
            (!IsPowerOfTwo(alignmentBytes) || (alignmentBytes > limits.MaximumSegmentAlignmentBytes)))
        {
            return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
                Elf64ImageError::UnsupportedSegmentAlignment);
        }
        if ((alignmentBytes > 1U) && ((fileOffset & (alignmentBytes - 1U)) != (virtualStart & (alignmentBytes - 1U))))
        {
            return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
                Elf64ImageError::SegmentAlignmentMismatch);
        }
        if ((flags & ~KnownPermissionFlags) != 0U)
        {
            return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
                Elf64ImageError::UnknownSegmentPermissions);
        }
        if (((flags & WriteFlag) != 0U) && ((flags & ExecuteFlag) != 0U))
        {
            return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
                Elf64ImageError::WriteExecuteSegment);
        }

        Foundation::UInt64 pageStart{0U};
        Foundation::UInt64 pageCount{0U};
        Foundation::UInt64 pageEnd{0U};
        if (!TryNormalizePageRange(virtualStart, memoryEnd, memorySizeBytes, limits.PageSizeBytes, pageStart, pageCount,
                                   pageEnd))
        {
            return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
                Elf64ImageError::NormalizedPageRangeOverflow);
        }

        for (Foundation::Size currentIndex{0U}; currentIndex < output.m_segmentCount; ++currentIndex)
        {
            const Elf64LoadSegment& current = output.m_segments[currentIndex];
            const Foundation::UInt64 currentPageEnd = current.PageStart + (current.PageCount * limits.PageSizeBytes);
            if ((pageCount != 0U) && (current.PageCount != 0U) &&
                RangesOverlap(pageStart, pageEnd, current.PageStart, currentPageEnd))
            {
                return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(
                    Elf64ImageError::NormalizedPageOverlap);
            }
        }

        const Elf64LoadSegment segment{
            .FileOffset = fileOffset,
            .FileSizeBytes = fileSizeBytes,
            .VirtualStart = virtualStart,
            .MemorySizeBytes = memorySizeBytes,
            .BssStart = virtualStart + fileSizeBytes,
            .BssSizeBytes = memorySizeBytes - fileSizeBytes,
            .PageStart = pageStart,
            .PageCount = pageCount,
            .AlignmentBytes = alignmentBytes,
            .Permissions = DecodePermissions(flags),
        };
        Foundation::Size destination{0U};
        while ((destination < output.m_segmentCount) && SegmentPrecedes(output.m_segments[destination], segment))
        {
            ++destination;
        }
        for (Foundation::Size currentIndex{output.m_segmentCount}; currentIndex > destination; --currentIndex)
        {
            output.m_segments[currentIndex] = output.m_segments[currentIndex - 1U];
        }
        output.m_segments[destination] = segment;
        ++output.m_segmentCount;
    }

    bool entryIsFileBacked{false};
    for (Foundation::Size index{0U}; index < output.m_segmentCount; ++index)
    {
        const Elf64LoadSegment& segment = output.m_segments[index];
        if ((entryAddress >= segment.VirtualStart) && (entryAddress < segment.BssStart))
        {
            entryIsFileBacked = true;
            break;
        }
    }
    if (!entryIsFileBacked)
    {
        return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Failure(Elf64ImageError::EntryNotFileBacked);
    }

    const Elf64ImageInformation information{
        .EntryAddress = entryAddress,
        .Flags = ReadLittleEndian32(image.Data + 48U),
        .Machine = ReadLittleEndian16(image.Data + 18U),
        .ProgramHeaderCount = programHeaderCount,
    };
    output.m_imageInformation = information;
    output.m_pageSizeBytes = limits.PageSizeBytes;
    output.m_isComplete = true;
    return Foundation::Result<Elf64ImageInformation, Elf64ImageError>::Success(information);
}
} // namespace Asteria::Executable::Elf
