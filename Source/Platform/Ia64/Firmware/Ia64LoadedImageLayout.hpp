// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_FIRMWARE_IA64_LOADED_IMAGE_LAYOUT_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_FIRMWARE_IA64_LOADED_IMAGE_LAYOUT_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
constexpr Foundation::Size Ia64LoadedImageSectionCount{6U};
/// The target IA-64 EFI loader places applications only at 64-KiB boundaries.
constexpr Foundation::UInt64 Ia64LoadedImageBaseAlignmentBytes{0x1'0000U};

constexpr Foundation::UInt64 Ia64RelocatedImageAlignmentBytes{0x100'0000U};

enum class Ia64LoadedImagePlacement : Foundation::UInt64
{
    Firmware,
    AlignedCopy,
    KernelElf,
};

enum class Ia64LoadedImageLayoutError : Foundation::UInt32
{
    None,
    NullImage,
    ImageSizeOutOfRange,
    InvalidDosHeader,
    InvalidPeHeader,
    InvalidCoffHeader,
    InvalidOptionalHeader,
    InvalidImageGeometry,
    InvalidSectionTable,
    InvalidSectionName,
    InvalidSectionGeometry,
    InvalidSectionProtection,
    InvalidEntrypoint,
};

enum class Ia64LoadedImageSectionPurpose : Foundation::UInt32
{
    Code,
    ReadOnlyData,
    WritableData,
};

struct Ia64LoadedImageSection
{
    Foundation::UInt64 VirtualStart;
    Foundation::UInt64 PageCount;
    Ia64LoadedImageSectionPurpose Purpose;
    Foundation::UInt32 Reserved;
};

struct Ia64LoadedImageLayout
{
    Foundation::UInt64 ImageBase;
    Foundation::UInt64 ImageSize;
    Ia64LoadedImagePlacement Placement{Ia64LoadedImagePlacement::Firmware};
    Ia64LoadedImageSection Sections[Ia64LoadedImageSectionCount];
};

/// Validates the canonical in-memory PE32+ image profile emitted for IA-64.
/// The returned ranges exclude the header page, allocate no storage, retain no
/// source pointer, and remain valid only while the loaded image stays resident.
/// Firmware placement requires the target loader's 64-KiB alignment;
/// AlignedCopy additionally requires 16 MiB. The bootstrap planner binds its
/// fixed permission-group geometry and LoaderCode receipt.
/// image and section extents remain 4-KiB aligned independently.
/// The complete input range must remain readable and stable for the duration of
/// Parse; callers must exclude concurrent mutation.
class Ia64LoadedImageLayoutParser final
{
public:
    [[nodiscard]] static Foundation::Result<Ia64LoadedImageLayout, Ia64LoadedImageLayoutError>
    Parse(const void* imageBase, Foundation::Size imageSize,
          Ia64LoadedImagePlacement placement = Ia64LoadedImagePlacement::Firmware) noexcept;

private:
    Ia64LoadedImageLayoutParser() = delete;
};

static_assert(sizeof(Ia64LoadedImageSectionPurpose) == 4U);
static_assert(sizeof(Ia64LoadedImagePlacement) == 8U);
static_assert(Ia64RelocatedImageAlignmentBytes == 16U * 1024U * 1024U);
static_assert(Ia64LoadedImageBaseAlignmentBytes == 64U * 1024U);
static_assert((Ia64LoadedImageBaseAlignmentBytes & (Ia64LoadedImageBaseAlignmentBytes - 1U)) == 0U);
static_assert(sizeof(Ia64LoadedImageSection) == 24U);
static_assert(alignof(Ia64LoadedImageSection) == 8U);
static_assert(sizeof(Ia64LoadedImageLayout) == 168U);
static_assert(alignof(Ia64LoadedImageLayout) == 8U);
static_assert(sizeof(Ia64LoadedImageLayoutError) == 4U);
} // namespace Asteria::Platform::Ia64::Firmware

#endif // ASTERIA_PLATFORM_IA64_FIRMWARE_IA64_LOADED_IMAGE_LAYOUT_HPP_INCLUDED
