// SPDX-License-Identifier: Apache-2.0
#include "Platform/Ia64/Executable/Ia64InitialUserImageSource.hpp"

#include "Support/TestContext.hpp"

int main()
{
    Asteria::TestSupport::TestContext runner{"Ia64InitialUserImageSourceTests"};
    using Asteria::Platform::Ia64::Executable::Ia64InitialUserImageSource;
    using Asteria::Platform::Ia64::Executable::Ia64InitialUserImageSourceError;
    Asteria::Boot::BootFileRegion region{};
    region.PhysicalStart = 0x10000U;
    region.PageCount = 2U;
    region.Files[0].Offset = 4096U;
    region.Files[0].SizeBytes = 2048U;
    const auto view = Ia64InitialUserImageSource::Get(region, 0U);
    ASTERIA_EXPECT(runner, "whole recorded file",
                   reinterpret_cast<Asteria::Foundation::UIntPointer>(view.Data) == 0x11000U, view.SizeBytes == 2048U);
    ASTERIA_EXPECT(runner, "source in retained region",
                   Ia64InitialUserImageSource::CreateView(region, view).HasValue());
    ASTERIA_EXPECT(runner, "absent file", Ia64InitialUserImageSource::Get(region, 1U).SizeBytes == 0U);
    ASTERIA_EXPECT(runner, "bad index", Ia64InitialUserImageSource::Get(region, 3U).SizeBytes == 0U);
    region.Files[0].Offset = 8192U;
    ASTERIA_EXPECT(runner, "file outside receipt", Ia64InitialUserImageSource::Get(region, 0U).SizeBytes == 0U);
    region.PhysicalStart += 4096U;
    region.PageCount = 1U;
    ASTERIA_EXPECT(runner, "exact region boundary", Ia64InitialUserImageSource::CreateView(region, view).HasValue());
    region.PhysicalStart += 4096U;
    ASTERIA_EXPECT(runner, "view before region",
                   Ia64InitialUserImageSource::CreateView(region, view).GetError() ==
                       Ia64InitialUserImageSourceError::ImageOutsideRegion);
    region.PhysicalStart = 0x10000U;
    ASTERIA_EXPECT(runner, "view after region",
                   Ia64InitialUserImageSource::CreateView(region, view).GetError() ==
                       Ia64InitialUserImageSourceError::ImageOutsideRegion);
    region.PageCount = ~Asteria::Foundation::UInt64{0U};
    ASTERIA_EXPECT(runner, "receipt overflow",
                   Ia64InitialUserImageSource::CreateView(region, view).GetError() ==
                       Ia64InitialUserImageSourceError::InvalidRegionExtent);
    region.PageCount = 1U;
    region.PhysicalStart = ~Asteria::Foundation::UInt64{4095U};
    ASTERIA_EXPECT(runner, "receipt end overflow",
                   Ia64InitialUserImageSource::CreateView(region, view).GetError() ==
                       Ia64InitialUserImageSourceError::InvalidRegionExtent);
    region.PhysicalStart = 1U;
    ASTERIA_EXPECT(runner, "unaligned receipt",
                   Ia64InitialUserImageSource::CreateView(region, view).GetError() ==
                       Ia64InitialUserImageSourceError::InvalidRegionExtent);
    region.PhysicalStart = 0x10000U;
    ASTERIA_EXPECT(runner, "empty view",
                   Ia64InitialUserImageSource::CreateView(region, {}).GetError() ==
                       Ia64InitialUserImageSourceError::InvalidImageExtent);
    const Asteria::Executable::Elf::Elf64ByteView overflow{
        reinterpret_cast<const Asteria::Foundation::UInt8*>(~Asteria::Foundation::UIntPointer{0U}), 1U};
    ASTERIA_EXPECT(runner, "view end overflow",
                   Ia64InitialUserImageSource::CreateView(region, overflow).GetError() ==
                       Ia64InitialUserImageSourceError::InvalidImageExtent);
    return runner.Finish();
}
