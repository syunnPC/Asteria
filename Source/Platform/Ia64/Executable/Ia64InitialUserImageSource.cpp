// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Executable/Ia64InitialUserImageSource.hpp"

namespace Asteria::Platform::Ia64::Executable
{
namespace
{
using ImageViewResult = Foundation::Result<::Asteria::Executable::Elf::Elf64ByteView, Ia64InitialUserImageSourceError>;
constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 PageBytes{4096U};

bool RegionValid(const Boot::BootFileRegion& region) noexcept
{
    return region.PhysicalStart != 0U && (region.PhysicalStart & (PageBytes - 1U)) == 0U && region.PageCount != 0U &&
           region.PageCount <= Maximum / PageBytes && region.PhysicalStart <= Maximum - region.PageCount * PageBytes;
}
} // namespace

::Asteria::Executable::Elf::Elf64ByteView Ia64InitialUserImageSource::Get(const Boot::BootFileRegion& region,
                                                                          const Foundation::Size index) noexcept
{
    if (!RegionValid(region) || index >= Boot::BootFileCount)
    {
        return {};
    }
    const Boot::BootFileRecord& file = region.Files[index];
    const Foundation::UInt64 bytes{region.PageCount * PageBytes};
    if (file.Offset > bytes || file.SizeBytes == 0U || file.SizeBytes > bytes - file.Offset)
    {
        return {};
    }
    // A validated LoaderData receipt denotes live identity-addressable bytes.
    return {reinterpret_cast<const Foundation::UInt8*>(region.PhysicalStart + file.Offset),
            static_cast<Foundation::Size>(file.SizeBytes)};
}

ImageViewResult Ia64InitialUserImageSource::CreateView(const Boot::BootFileRegion& region,
                                                       const ::Asteria::Executable::Elf::Elf64ByteView source) noexcept
{
    if (!RegionValid(region))
    {
        return ImageViewResult::Failure(Ia64InitialUserImageSourceError::InvalidRegionExtent);
    }
    const Foundation::UIntPointer start = reinterpret_cast<Foundation::UIntPointer>(source.Data);
    if (start == 0U || source.SizeBytes == 0U || start > Maximum - source.SizeBytes)
    {
        return ImageViewResult::Failure(Ia64InitialUserImageSourceError::InvalidImageExtent);
    }
    if (start < region.PhysicalStart || start + source.SizeBytes > region.PhysicalStart + region.PageCount * PageBytes)
    {
        return ImageViewResult::Failure(Ia64InitialUserImageSourceError::ImageOutsideRegion);
    }
    return ImageViewResult::Success(source);
}
} // namespace Asteria::Platform::Ia64::Executable
