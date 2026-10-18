// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_EXECUTABLE_IA64_P64_IMAGE_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_EXECUTABLE_IA64_P64_IMAGE_HPP_INCLUDED

#include "Executable/Elf/Elf64Image.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Platform::Ia64::Executable
{
inline constexpr Foundation::UInt16 Ia64ElfMachine{50U};
inline constexpr Foundation::UInt32 Ia64P64ElfFlags{0x10U};
inline constexpr Foundation::UInt8 MinimumIa64ImplementedVirtualAddressBit{50U};
inline constexpr Foundation::UInt8 MaximumIa64ImplementedVirtualAddressBit{60U};
inline constexpr Foundation::UInt8 MinimumIa64UserVirtualRegion{1U};
inline constexpr Foundation::UInt8 MaximumIa64UserVirtualRegion{7U};

struct Ia64P64ImageInformation
{
    Foundation::UInt64 EntryDescriptorAddress;
    Foundation::UInt64 EntryBundleAddress;
    Foundation::UInt64 GlobalPointer;
    Foundation::UInt32 Flags;
    Foundation::UInt16 Machine;
    Foundation::UInt16 ProgramHeaderCount;
};

enum class Ia64P64LoadPlanAccessError : Foundation::UInt32
{
    None,
    PlanIncomplete,
    IndexOutOfBounds,
};

enum class Ia64P64ImageError : Foundation::UInt32
{
    None,
    InvalidAddressProfile,
    Elf64ImageRejected,
    UnsupportedMachine,
    UnsupportedFlags,
    InvalidLoadSegmentAddress,
    InvalidEntryDescriptorAddress,
    EntryDescriptorNotReadableFileBacked,
    EntryDescriptorOutsideImage,
    InvalidEntryBundleAddress,
    EntryBundleNotExecutableFileBacked,
    InvalidGlobalPointerAddress,
    GlobalPointerNotReadable,
};

struct Ia64P64ImageFailure
{
    Ia64P64ImageError Error;
    ::Asteria::Executable::Elf::Elf64ImageError Elf64Error;
};

class Ia64P64LoadPlan final
{
public:
    constexpr Ia64P64LoadPlan() noexcept = default;

    [[nodiscard]] constexpr bool IsComplete() const noexcept
    {
        return m_isComplete;
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetPageSizeBytes() const noexcept
    {
        return m_isComplete ? m_elf64LoadPlan.GetPageSizeBytes() : 0U;
    }

    [[nodiscard]] constexpr Foundation::Size GetSegmentCount() const noexcept
    {
        return m_isComplete ? m_elf64LoadPlan.GetSegmentCount() : 0U;
    }

    [[nodiscard]] Foundation::Result<Ia64P64ImageInformation, Ia64P64LoadPlanAccessError>
    GetImageInformation() const noexcept;

    [[nodiscard]] Foundation::Result<::Asteria::Executable::Elf::Elf64LoadSegment, Ia64P64LoadPlanAccessError>
    GetSegment(Foundation::Size index) const noexcept;

private:
    friend class Ia64P64Image;

    ::Asteria::Executable::Elf::Elf64LoadPlan m_elf64LoadPlan{};
    Ia64P64ImageInformation m_imageInformation{};
    bool m_isComplete{false};
};

/// Validates the IA-64 P64 execution contract of a static ELF64 image.
///
/// The decoder owns no image bytes and performs no allocation, mapping, copy,
/// zeroing, relocation, or transfer of control. It first creates the portable
/// ELF64 load plan, then validates every nonempty mapping against one selected
/// user region and the supplied PAL-derived virtual-address width. The P64
/// entry descriptor is decoded byte-wise from file-backed readable bytes. Its
/// bundle address must be file-backed executable code, and its GP must name
/// readable mapped memory. The image and output must not overlap, and the image
/// must remain immutable for the call. Failure exposes neither the portable
/// plan nor a partially decoded entry.
class Ia64P64Image final
{
public:
    Ia64P64Image() = delete;

    [[nodiscard]] static Foundation::Result<Ia64P64ImageInformation, Ia64P64ImageFailure>
    DecodeLoadPlan(::Asteria::Executable::Elf::Elf64ByteView image,
                   const ::Asteria::Executable::Elf::Elf64LoadPlanLimits& limits,
                   Foundation::UInt8 implementedVirtualAddressMostSignificantBit, Foundation::UInt8 userVirtualRegion,
                   Ia64P64LoadPlan& output) noexcept;
};

static_assert(sizeof(Ia64P64ImageInformation) == 32U);
static_assert(alignof(Ia64P64ImageInformation) == 8U);
static_assert(sizeof(Ia64P64LoadPlanAccessError) == 4U);
static_assert(sizeof(Ia64P64ImageError) == 4U);
static_assert(sizeof(Ia64P64ImageFailure) == 8U);
static_assert(alignof(Ia64P64ImageFailure) == 4U);
} // namespace Asteria::Platform::Ia64::Executable

#endif // ASTERIA_PLATFORM_IA64_EXECUTABLE_IA64_P64_IMAGE_HPP_INCLUDED
