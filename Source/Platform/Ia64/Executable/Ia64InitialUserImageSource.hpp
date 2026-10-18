// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_EXECUTABLE_IA64_INITIAL_USER_IMAGE_SOURCE_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_EXECUTABLE_IA64_INITIAL_USER_IMAGE_SOURCE_HPP_INCLUDED

#include "Boot/BootFiles.hpp"
#include "Executable/Elf/Elf64Image.hpp"
#include "Foundation/Result.hpp"

namespace Asteria::Platform::Ia64::Executable
{
enum class Ia64InitialUserImageSourceError : Foundation::UInt32
{
    None,
    InvalidRegionExtent,
    InvalidImageExtent,
    ImageOutsideRegion,
};

/// Borrows whole files from retained identity-addressed LoaderData before
/// translation takeover. No allocation, copy, mapping or ELF decoding.
class Ia64InitialUserImageSource final
{
public:
    Ia64InitialUserImageSource() = delete;

    /// Returns an empty view for an invalid index, absent file or invalid extent.
    [[nodiscard]] static ::Asteria::Executable::Elf::Elf64ByteView Get(const Boot::BootFileRegion& region,
                                                                       Foundation::Size index) noexcept;

    /// Checks numerical bounds against the recorded boot region. The view must
    /// denote live, readable bytes in the firmware's identity address space.
    [[nodiscard]] static Foundation::Result<::Asteria::Executable::Elf::Elf64ByteView, Ia64InitialUserImageSourceError>
    CreateView(const Boot::BootFileRegion& region, ::Asteria::Executable::Elf::Elf64ByteView source) noexcept;
};

static_assert(sizeof(Ia64InitialUserImageSourceError) == 4U);
} // namespace Asteria::Platform::Ia64::Executable

#endif // ASTERIA_PLATFORM_IA64_EXECUTABLE_IA64_INITIAL_USER_IMAGE_SOURCE_HPP_INCLUDED
