// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_EXECUTABLE_ELF_ELF64_IMAGE_HPP_INCLUDED
#define ASTERIA_EXECUTABLE_ELF_ELF64_IMAGE_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Executable::Elf
{
constexpr Foundation::Size MaximumElf64LoadSegments{32U};

struct Elf64ByteView
{
    const Foundation::UInt8* Data;
    Foundation::Size SizeBytes;
};

struct Elf64LoadPlanLimits
{
    Foundation::UInt64 PageSizeBytes;
    Foundation::UInt64 MaximumSegmentAlignmentBytes;
    Foundation::Size LoadSegmentCapacity;
};

enum class Elf64SegmentPermission : Foundation::UInt8
{
    Read = Foundation::UInt8{1U} << 0U,
    Write = Foundation::UInt8{1U} << 1U,
    Execute = Foundation::UInt8{1U} << 2U,
};

class Elf64SegmentPermissions final
{
public:
    constexpr Elf64SegmentPermissions() noexcept = default;

    explicit constexpr Elf64SegmentPermissions(const Elf64SegmentPermission permission) noexcept
        : m_bits{static_cast<Foundation::UInt8>(permission)}
    {
    }

    [[nodiscard]] constexpr Elf64SegmentPermissions With(const Elf64SegmentPermission permission) const noexcept
    {
        return Elf64SegmentPermissions{
            static_cast<Foundation::UInt8>(m_bits | static_cast<Foundation::UInt8>(permission)),
            RawValueTag{},
        };
    }

    [[nodiscard]] constexpr bool Contains(const Elf64SegmentPermission permission) const noexcept
    {
        const Foundation::UInt8 bit = static_cast<Foundation::UInt8>(permission);
        return (m_bits & bit) == bit;
    }

    [[nodiscard]] constexpr Foundation::UInt8 GetBits() const noexcept
    {
        return m_bits;
    }

private:
    struct RawValueTag final
    {
    };

    constexpr Elf64SegmentPermissions(const Foundation::UInt8 bits, RawValueTag) noexcept : m_bits{bits}
    {
    }

    Foundation::UInt8 m_bits{0U};
};

struct Elf64LoadSegment
{
    Foundation::UInt64 FileOffset;
    Foundation::UInt64 FileSizeBytes;
    Foundation::UInt64 VirtualStart;
    Foundation::UInt64 MemorySizeBytes;
    Foundation::UInt64 BssStart;
    Foundation::UInt64 BssSizeBytes;
    Foundation::UInt64 PageStart;
    Foundation::UInt64 PageCount;
    Foundation::UInt64 AlignmentBytes;
    Elf64SegmentPermissions Permissions;
};

struct Elf64ImageInformation
{
    Foundation::UInt64 EntryAddress;
    Foundation::UInt32 Flags;
    Foundation::UInt16 Machine;
    Foundation::UInt16 ProgramHeaderCount;
};

enum class Elf64LoadPlanAccessError : Foundation::UInt32
{
    None,
    PlanIncomplete,
    IndexOutOfBounds,
};

enum class Elf64ImageError : Foundation::UInt32
{
    None,
    InvalidByteView,
    InvalidLimits,
    HeaderTooSmall,
    InvalidMagic,
    UnsupportedClass,
    UnsupportedEndianness,
    UnsupportedVersion,
    UnsupportedImageType,
    InvalidHeaderSize,
    InvalidProgramHeaderSize,
    UnsupportedExtendedProgramHeaderCount,
    ProgramHeaderTableOverflow,
    ProgramHeaderTableOutsideImage,
    InterpreterUnsupported,
    RuntimeRelocationUnsupported,
    LoadSegmentCapacityExceeded,
    FileSizeExceedsMemorySize,
    FileRangeOverflow,
    FileRangeOutsideImage,
    VirtualRangeOverflow,
    UnsupportedSegmentAlignment,
    SegmentAlignmentMismatch,
    UnknownSegmentPermissions,
    WriteExecuteSegment,
    NormalizedPageRangeOverflow,
    NormalizedPageOverlap,
    EntryNotFileBacked,
};

class Elf64LoadPlan final
{
public:
    constexpr Elf64LoadPlan() noexcept = default;

    [[nodiscard]] constexpr bool IsComplete() const noexcept
    {
        return m_isComplete;
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetPageSizeBytes() const noexcept
    {
        return m_isComplete ? m_pageSizeBytes : 0U;
    }

    [[nodiscard]] constexpr Foundation::Size GetSegmentCount() const noexcept
    {
        return m_isComplete ? m_segmentCount : 0U;
    }

    [[nodiscard]] Foundation::Result<Elf64ImageInformation, Elf64LoadPlanAccessError>
    GetImageInformation() const noexcept;

    [[nodiscard]] Foundation::Result<Elf64LoadSegment, Elf64LoadPlanAccessError>
    GetSegment(Foundation::Size index) const noexcept;

private:
    friend class Elf64Image;

    Elf64LoadSegment m_segments[MaximumElf64LoadSegments]{};
    Elf64ImageInformation m_imageInformation{};
    Foundation::UInt64 m_pageSizeBytes{0U};
    Foundation::Size m_segmentCount{0U};
    bool m_isComplete{false};
};

/// Decodes the functional loading contract of a little-endian ELF64 ET_EXEC image.
///
/// The decoder reads external fields byte by byte. It does not retain image storage,
/// allocate, relocate, copy, zero, map, or execute the image. Input and output must not
/// overlap and the input must remain immutable for the call. Failure leaves output
/// incomplete and exposes no partial segment. Success orders segments by normalized
/// page address and returns the same image information retained by output.
class Elf64Image final
{
public:
    Elf64Image() = delete;

    [[nodiscard]] static Foundation::Result<Elf64ImageInformation, Elf64ImageError>
    DecodeLoadPlan(Elf64ByteView image, const Elf64LoadPlanLimits& limits, Elf64LoadPlan& output) noexcept;
};

static_assert(sizeof(Elf64ByteView) == 16U);
static_assert(alignof(Elf64ByteView) == 8U);
static_assert(sizeof(Elf64LoadPlanLimits) == 24U);
static_assert(alignof(Elf64LoadPlanLimits) == 8U);
static_assert(sizeof(Elf64SegmentPermission) == 1U);
static_assert(sizeof(Elf64SegmentPermissions) == 1U);
static_assert(sizeof(Elf64LoadSegment) == 80U);
static_assert(alignof(Elf64LoadSegment) == 8U);
static_assert(sizeof(Elf64ImageInformation) == 16U);
static_assert(alignof(Elf64ImageInformation) == 8U);
static_assert(sizeof(Elf64LoadPlanAccessError) == 4U);
static_assert(sizeof(Elf64ImageError) == 4U);
} // namespace Asteria::Executable::Elf

#endif // ASTERIA_EXECUTABLE_ELF_ELF64_IMAGE_HPP_INCLUDED
