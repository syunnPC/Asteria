// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_FIRMWARE_AMD64_LOADED_IMAGE_LAYOUT_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_FIRMWARE_AMD64_LOADED_IMAGE_LAYOUT_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Amd64::Firmware
{
constexpr Foundation::Size Amd64LoadedImageSectionCount{7U};
/// Largest loaded image accepted by the boot path, including static runtime storage.
constexpr Foundation::Size MaximumAmd64LoadedImageBytes{4U * 1024U * 1024U};

enum class Amd64LoadedImageLayoutError : Foundation::UInt32
{
    None,
    NullImage,
    ImageSizeOutOfRange,
    OutputAliasesImage,
    InvalidDosHeader,
    InvalidPeHeader,
    InvalidCoffHeader,
    InvalidOptionalHeader,
    InvalidImageGeometry,
    InvalidSectionTable,
    InvalidSectionName,
    InvalidSectionGeometry,
    InvalidSectionProtection,
    InvalidDirectory,
    InvalidEntrypoint,
};

enum class Amd64LoadedImageSectionPurpose : Foundation::UInt32
{
    Code,
    ReadOnlyData,
    WritableData,
};

struct Amd64LoadedImageSection
{
    Foundation::UInt64 VirtualStart;
    Foundation::UInt64 PageCount;
    Amd64LoadedImageSectionPurpose Purpose;
    Foundation::UInt32 Reserved;
};

struct Amd64LoadedImageLayout
{
    Foundation::UInt64 ImageBase;
    Foundation::UInt64 ImageSize;
    Amd64LoadedImageSection Sections[Amd64LoadedImageSectionCount];
};

/// Validates the mapping-relevant canonical in-memory AMD64 PE32+ application
/// profile and derives the six section ownership ranges used by later mapping
/// planning.
/// The explicit output remains unchanged on failure and is assigned only after
/// the complete image validates. The complete input range must stay readable
/// and stable for the call. The caller must ensure that output shares no
/// backing storage with any image byte through an alternate virtual alias; the
/// parser also rejects direct address-range overlap.
class Amd64LoadedImageLayoutParser final
{
public:
    [[nodiscard]] static Amd64LoadedImageLayoutError Parse(const void* imageBase, Foundation::Size imageSize,
                                                           Amd64LoadedImageLayout& output) noexcept;

private:
    Amd64LoadedImageLayoutParser() = delete;
};

using Amd64LoadedImageLayoutParseFunction = Amd64LoadedImageLayoutError (*)(const void*, Foundation::Size,
                                                                            Amd64LoadedImageLayout&) noexcept;

static_assert(__is_same(decltype(&Amd64LoadedImageLayoutParser::Parse), Amd64LoadedImageLayoutParseFunction));
static_assert(sizeof(Amd64LoadedImageSectionPurpose) == 4U);
static_assert(sizeof(Amd64LoadedImageSection) == 24U);
static_assert(alignof(Amd64LoadedImageSection) == 8U);
static_assert(__is_aggregate(Amd64LoadedImageSection));
static_assert(__is_standard_layout(Amd64LoadedImageSection));
static_assert(__is_trivially_copyable(Amd64LoadedImageSection));
static_assert(__builtin_offsetof(Amd64LoadedImageSection, VirtualStart) == 0U);
static_assert(__builtin_offsetof(Amd64LoadedImageSection, PageCount) == 8U);
static_assert(__builtin_offsetof(Amd64LoadedImageSection, Purpose) == 16U);
static_assert(__builtin_offsetof(Amd64LoadedImageSection, Reserved) == 20U);
static_assert(__builtin_offsetof(Amd64LoadedImageSection, Reserved) + sizeof(Amd64LoadedImageSection::Reserved) ==
              sizeof(Amd64LoadedImageSection));
static_assert(sizeof(Amd64LoadedImageLayout) == 184U);
static_assert(alignof(Amd64LoadedImageLayout) == 8U);
static_assert(__is_aggregate(Amd64LoadedImageLayout));
static_assert(__is_standard_layout(Amd64LoadedImageLayout));
static_assert(__is_trivially_copyable(Amd64LoadedImageLayout));
static_assert(__builtin_offsetof(Amd64LoadedImageLayout, ImageBase) == 0U);
static_assert(__builtin_offsetof(Amd64LoadedImageLayout, ImageSize) == 8U);
static_assert(__builtin_offsetof(Amd64LoadedImageLayout, Sections) == 16U);
static_assert(__builtin_offsetof(Amd64LoadedImageLayout, Sections) + sizeof(Amd64LoadedImageLayout::Sections) ==
              sizeof(Amd64LoadedImageLayout));
static_assert(sizeof(Amd64LoadedImageLayoutError) == 4U);
} // Namespace Asteria::Platform::Amd64::Firmware

#endif // ASTERIA_PLATFORM_AMD64_FIRMWARE_AMD64_LOADED_IMAGE_LAYOUT_HPP_INCLUDED
