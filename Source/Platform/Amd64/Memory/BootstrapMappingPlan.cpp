// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Memory/BootstrapMappingPlan.hpp"

namespace Asteria::Platform::Amd64::Memory
{
namespace
{
using Firmware::Amd64LoadedImageLayout;
using Firmware::Amd64LoadedImageSectionPurpose;
using Kernel::Memory::InitialMapping;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::InitialMappingPlan;
using Kernel::Memory::InitialMappingPlanError;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MappingPermissions;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::PhysicalPageRange;
using Kernel::Memory::VirtualAddress;

constexpr Foundation::UInt64 PageBytes{Kernel::Memory::MappingPageSizeBytes};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 LowerCanonicalAddressLimit{0x0000'8000'0000'0000ULL};
constexpr Foundation::UInt64 FirmwareMemoryRuntimeAttribute{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 FirmwareMemoryWriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt32 FirmwareDescriptorVersion{1U};
constexpr Foundation::UInt32 MinimumFirmwareDescriptorSize{40U};
constexpr Foundation::UInt32 LoaderDataMemoryType{2U};
constexpr Foundation::UInt32 ConventionalMemoryType{7U};
constexpr Foundation::UInt32 MaximumStandardMemoryType{15U};
constexpr Foundation::UInt32 MinimumOemMemoryType{0x7000'0000U};
constexpr Foundation::UInt64 MaximumPhysicalPageCount{Foundation::UInt64{1U} << 52U};

[[nodiscard]] constexpr MappingPermissions ReadOnly() noexcept
{
    return MappingPermissions{MappingPermission::Read};
}

[[nodiscard]] constexpr MappingPermissions PermissionsFor(const Amd64LoadedImageSectionPurpose purpose) noexcept
{
    switch (purpose)
    {
    case Amd64LoadedImageSectionPurpose::Code:
        return ReadOnly().With(MappingPermission::Execute);
    case Amd64LoadedImageSectionPurpose::ReadOnlyData:
        return ReadOnly();
    case Amd64LoadedImageSectionPurpose::WritableData:
        return ReadOnly().With(MappingPermission::Write);
    }
    return MappingPermissions{};
}

[[nodiscard]] constexpr InitialMappingOwner OwnerFor(const Amd64LoadedImageSectionPurpose purpose) noexcept
{
    switch (purpose)
    {
    case Amd64LoadedImageSectionPurpose::Code:
        return InitialMappingOwner::NucleusCode;
    case Amd64LoadedImageSectionPurpose::ReadOnlyData:
        return InitialMappingOwner::NucleusReadOnlyData;
    case Amd64LoadedImageSectionPurpose::WritableData:
        return InitialMappingOwner::NucleusWritableData;
    }
    return InitialMappingOwner::Device;
}

[[nodiscard]] constexpr bool TryGetRangeEnd(const Foundation::UInt64 start, const Foundation::UInt64 pages,
                                            Foundation::UInt64& end) noexcept
{
    if ((pages == 0U) || ((start & (PageBytes - 1U)) != 0U) || (pages > (MaximumUInt64 >> 12U)))
    {
        return false;
    }
    const Foundation::UInt64 bytes = pages << 12U;
    if (start > (MaximumUInt64 - bytes))
    {
        return false;
    }
    end = start + bytes;
    return true;
}

[[nodiscard]] constexpr bool IsLowerCanonicalRange(const Foundation::UInt64 start,
                                                   const Foundation::UInt64 end) noexcept
{
    return (start < end) && (end <= LowerCanonicalAddressLimit);
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] constexpr bool TryGetPageInterval(const Foundation::UInt64 start, const Foundation::UInt64 pages,
                                                Foundation::UInt64& firstPage, Foundation::UInt64& endPage) noexcept
{
    if ((pages == 0U) || ((start & (PageBytes - 1U)) != 0U))
    {
        return false;
    }
    firstPage = start >> 12U;
    if ((firstPage >= MaximumPhysicalPageCount) || (pages > (MaximumPhysicalPageCount - firstPage)))
    {
        return false;
    }
    endPage = firstPage + pages;
    return true;
}

[[nodiscard]] constexpr Boot::PhysicalMemoryKind
ConvertFirmwareMemoryType(const Foundation::UInt32 firmwareType) noexcept
{
    switch (firmwareType)
    {
    case 1U:
    case 2U:
        return Boot::PhysicalMemoryKind::Loader;
    case 3U:
    case 4U:
        return Boot::PhysicalMemoryKind::BootServices;
    case 5U:
    case 6U:
        return Boot::PhysicalMemoryKind::RuntimeServices;
    case ConventionalMemoryType:
        return Boot::PhysicalMemoryKind::Conventional;
    case 8U:
        return Boot::PhysicalMemoryKind::Unusable;
    case 9U:
        return Boot::PhysicalMemoryKind::AcpiReclaimable;
    case 10U:
        return Boot::PhysicalMemoryKind::AcpiNonVolatile;
    case 11U:
    case 12U:
        return Boot::PhysicalMemoryKind::MemoryMappedIo;
    case 13U:
        return Boot::PhysicalMemoryKind::PalCode;
    case 14U:
        return Boot::PhysicalMemoryKind::Persistent;
    case 15U:
        return Boot::PhysicalMemoryKind::Unaccepted;
    case 0U:
    default:
        return Boot::PhysicalMemoryKind::Reserved;
    }
}

[[nodiscard]] constexpr bool IsFirmwareMemoryTypeValid(const Foundation::UInt32 firmwareType) noexcept
{
    return (firmwareType <= MaximumStandardMemoryType) || (firmwareType >= MinimumOemMemoryType);
}

[[nodiscard]] constexpr bool IsImageRamRegion(const Boot::PhysicalMemoryRegion& region) noexcept
{
    switch (region.FirmwareType)
    {
    case 1U:
    case 2U:
        return region.Kind == Boot::PhysicalMemoryKind::Loader;
    default:
        return false;
    }
}

[[nodiscard]] bool IsBootInfoShapeValid(const Boot::BootInfo& bootInfo) noexcept
{
    if ((bootInfo.Signature != Boot::BootInfoSignature) || (bootInfo.Version != Boot::BootInfoVersion) ||
        (bootInfo.SizeBytes != sizeof(Boot::BootInfo)) || (bootInfo.MemoryRegionCount == 0U) ||
        (bootInfo.MemoryRegionCount > Boot::MaximumMemoryRegions) ||
        (bootInfo.FirmwareDescriptorVersion != FirmwareDescriptorVersion) ||
        (bootInfo.FirmwareDescriptorSize < MinimumFirmwareDescriptorSize) || (bootInfo.Reserved != 0U) ||
        (bootInfo.FirmwareMemoryMapSizeBytes !=
         (static_cast<Foundation::UInt64>(bootInfo.MemoryRegionCount) * bootInfo.FirmwareDescriptorSize)))
    {
        return false;
    }

    Foundation::UInt64 previousEndPage{0U};
    for (Foundation::Size index{0U}; index < bootInfo.MemoryRegionCount; ++index)
    {
        const Boot::PhysicalMemoryRegion& region = bootInfo.MemoryRegions[index];
        Foundation::UInt64 firstPage{0U};
        Foundation::UInt64 endPage{0U};
        Foundation::UInt64 virtualFirstPage{0U};
        Foundation::UInt64 virtualEndPage{0U};
        if (!TryGetPageInterval(region.PhysicalStart, region.PageCount, firstPage, endPage) ||
            ((region.VirtualStart != 0U) &&
             !TryGetPageInterval(region.VirtualStart, region.PageCount, virtualFirstPage, virtualEndPage)) ||
            !IsFirmwareMemoryTypeValid(region.FirmwareType) ||
            (region.Kind != ConvertFirmwareMemoryType(region.FirmwareType)) ||
            ((region.Kind == Boot::PhysicalMemoryKind::Conventional) &&
             ((region.Attributes & FirmwareMemoryRuntimeAttribute) != 0U)) ||
            ((index != 0U) && (firstPage < previousEndPage)))
        {
            return false;
        }
        previousEndPage = endPage;
    }
    return true;
}

enum class ProvenancePurpose : Foundation::UInt8
{
    Image,
    Reserved,
};

[[nodiscard]] bool IsRegionCompatible(const Boot::PhysicalMemoryRegion& region, const ProvenancePurpose purpose,
                                      const PhysicalPageRange& reservedPages) noexcept
{
    if (((region.Attributes & FirmwareMemoryWriteBackAttribute) == 0U) ||
        ((region.Attributes & FirmwareMemoryRuntimeAttribute) != 0U))
    {
        return false;
    }
    if (purpose == ProvenancePurpose::Image)
    {
        return IsImageRamRegion(region);
    }
    return (region.FirmwareType == LoaderDataMemoryType) && (region.Kind == Boot::PhysicalMemoryKind::Loader) &&
           (region.Attributes == reservedPages.Attributes);
}

[[nodiscard]] bool HasMemoryProvenance(const Boot::BootInfo& bootInfo, const Foundation::UInt64 start,
                                       const Foundation::UInt64 pages, const ProvenancePurpose purpose,
                                       const PhysicalPageRange& reservedPages) noexcept
{
    Foundation::UInt64 cursorPage{0U};
    Foundation::UInt64 targetEndPage{0U};
    if (!TryGetPageInterval(start, pages, cursorPage, targetEndPage))
    {
        return false;
    }

    for (Foundation::Size index{0U}; index < bootInfo.MemoryRegionCount; ++index)
    {
        const Boot::PhysicalMemoryRegion& region = bootInfo.MemoryRegions[index];
        Foundation::UInt64 regionStartPage{0U};
        Foundation::UInt64 regionEndPage{0U};
        if (!TryGetPageInterval(region.PhysicalStart, region.PageCount, regionStartPage, regionEndPage))
        {
            return false;
        }
        if (regionEndPage <= cursorPage)
        {
            continue;
        }
        if ((regionStartPage > cursorPage) || !IsRegionCompatible(region, purpose, reservedPages))
        {
            return false;
        }
        cursorPage = regionEndPage < targetEndPage ? regionEndPage : targetEndPage;
        if (cursorPage == targetEndPage)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool IsImageLayoutValid(const Amd64LoadedImageLayout& image) noexcept
{
    Foundation::UInt64 imageEnd{0U};
    if ((image.ImageBase == 0U) || (image.ImageSize < (2U * PageBytes)) ||
        (image.ImageSize > Firmware::MaximumAmd64LoadedImageBytes) || ((image.ImageSize & (PageBytes - 1U)) != 0U) ||
        !TryGetRangeEnd(image.ImageBase, image.ImageSize >> 12U, imageEnd) ||
        !IsLowerCanonicalRange(image.ImageBase, imageEnd))
    {
        return false;
    }

    constexpr Amd64LoadedImageSectionPurpose ExpectedPurposes[Firmware::Amd64LoadedImageSectionCount]{
        Amd64LoadedImageSectionPurpose::Code,         Amd64LoadedImageSectionPurpose::ReadOnlyData,
        Amd64LoadedImageSectionPurpose::ReadOnlyData, Amd64LoadedImageSectionPurpose::WritableData,
        Amd64LoadedImageSectionPurpose::ReadOnlyData, Amd64LoadedImageSectionPurpose::ReadOnlyData,
        Amd64LoadedImageSectionPurpose::WritableData,
    };
    Foundation::UInt64 expectedStart = image.ImageBase + PageBytes;
    for (Foundation::Size index{0U}; index < Firmware::Amd64LoadedImageSectionCount; ++index)
    {
        const Firmware::Amd64LoadedImageSection& section = image.Sections[index];
        Foundation::UInt64 sectionEnd{0U};
        if ((section.VirtualStart != expectedStart) || (section.Purpose != ExpectedPurposes[index]) ||
            (section.Reserved != 0U) || !TryGetRangeEnd(section.VirtualStart, section.PageCount, sectionEnd) ||
            (sectionEnd > imageEnd))
        {
            return false;
        }
        expectedStart = sectionEnd;
    }
    return expectedStart == imageEnd;
}

[[nodiscard]] BootstrapMappingPlanError ValidateInputs(const Amd64LoadedImageLayout& image,
                                                       const Boot::BootInfo& finalBootInfo,
                                                       const PhysicalPageRange& reservedPages,
                                                       BootstrapMappingPlanSummary& summary) noexcept
{
    if (!IsImageLayoutValid(image))
    {
        return BootstrapMappingPlanError::InvalidImageLayout;
    }
    if (!IsBootInfoShapeValid(finalBootInfo))
    {
        return BootstrapMappingPlanError::InvalidBootInfo;
    }

    Foundation::UInt64 reservedEnd{0U};
    if ((reservedPages.PhysicalStart == 0U) || (reservedPages.PageCount != BootstrapReservedPageCount) ||
        (reservedPages.Reserved != 0U) || (reservedPages.FirmwareType != LoaderDataMemoryType) ||
        ((reservedPages.Attributes & FirmwareMemoryWriteBackAttribute) == 0U) ||
        ((reservedPages.Attributes & FirmwareMemoryRuntimeAttribute) != 0U) ||
        ((reservedPages.PhysicalStart & ((BootstrapReservedAlignmentPages * PageBytes) - 1U)) != 0U) ||
        !TryGetRangeEnd(reservedPages.PhysicalStart, reservedPages.PageCount, reservedEnd) ||
        !IsLowerCanonicalRange(reservedPages.PhysicalStart, reservedEnd))
    {
        return BootstrapMappingPlanError::InvalidReservedPages;
    }

    const Foundation::UInt64 imageEnd = image.ImageBase + image.ImageSize;
    if (RangesOverlap(reservedPages.PhysicalStart, reservedEnd, image.ImageBase, imageEnd))
    {
        return BootstrapMappingPlanError::ReservedPagesOverlapImage;
    }
    if (!HasMemoryProvenance(finalBootInfo, image.ImageBase, image.ImageSize >> 12U, ProvenancePurpose::Image,
                             reservedPages))
    {
        return BootstrapMappingPlanError::InvalidImageMemoryProvenance;
    }
    if (!HasMemoryProvenance(finalBootInfo, reservedPages.PhysicalStart, reservedPages.PageCount,
                             ProvenancePurpose::Reserved, reservedPages))
    {
        return BootstrapMappingPlanError::InvalidReservedMemoryProvenance;
    }

    summary = BootstrapMappingPlanSummary{
        .ReservedPhysicalStart = reservedPages.PhysicalStart,
        .StackPhysicalStart = reservedPages.PhysicalStart + (BootstrapStackPageOffset << 12U),
        .PageTableStoragePhysicalStart = reservedPages.PhysicalStart + (BootstrapPageTableStoragePageOffset << 12U),
        .DescriptorTableStoragePhysicalStart =
            reservedPages.PhysicalStart + (BootstrapDescriptorTableStoragePageOffset << 12U),
        .MappingCount = static_cast<Foundation::UInt16>(BootstrapInitialMappingCount),
        .Reserved16 = 0U,
        .Reserved32 = 0U,
    };
    return BootstrapMappingPlanError::None;
}

[[nodiscard]] constexpr InitialMapping MakeMapping(const Foundation::UInt64 start, const Foundation::UInt64 pages,
                                                   const MappingPermissions permissions,
                                                   const InitialMappingOwner owner) noexcept
{
    return InitialMapping{
        .VirtualStart = VirtualAddress{start},
        .PhysicalStart = PhysicalAddress{start},
        .Pages = PageCount{pages},
        .Permissions = permissions,
        .CachePolicy = MemoryCachePolicy::WriteBack,
        .Owner = owner,
        .Reserved = 0U,
    };
}

[[nodiscard]] InitialMappingPlanError
AddReservedMappings(InitialMappingPlan& output, const Foundation::UInt64 reservedStart, Foundation::Size& mappingIndex,
                    Foundation::UInt64& stackStart, Foundation::UInt64& tableStorageStart,
                    Foundation::UInt64& descriptorTableStorageStart) noexcept
{
    stackStart = reservedStart + (BootstrapStackPageOffset << 12U);
    InitialMappingPlanError error =
        output.AddMapping(MakeMapping(stackStart, BootstrapStackPageCount, ReadOnly().With(MappingPermission::Write),
                                      InitialMappingOwner::NucleusStack));
    if (error != InitialMappingPlanError::None)
    {
        return error;
    }
    ++mappingIndex;

    tableStorageStart = reservedStart + (BootstrapPageTableStoragePageOffset << 12U);
    error = output.AddMapping(MakeMapping(tableStorageStart, BootstrapPageTableStoragePageCount,
                                          ReadOnly().With(MappingPermission::Write), InitialMappingOwner::PageTable));
    if (error != InitialMappingPlanError::None)
    {
        return error;
    }
    ++mappingIndex;

    descriptorTableStorageStart = reservedStart + (BootstrapDescriptorTableStoragePageOffset << 12U);
    error = output.AddMapping(MakeMapping(descriptorTableStorageStart, BootstrapDescriptorTableStoragePageCount,
                                          ReadOnly().With(MappingPermission::Write),
                                          InitialMappingOwner::NucleusDescriptorTables));
    if (error == InitialMappingPlanError::None)
    {
        ++mappingIndex;
    }
    return error;
}
} // namespace

BootstrapMappingPlanError BootstrapMappingPlanBuilder::Validate(const Amd64LoadedImageLayout& image,
                                                                const Boot::BootInfo& finalBootInfo,
                                                                const PhysicalPageRange& reservedPages,
                                                                BootstrapMappingPlanSummary& outputSummary) noexcept
{
    BootstrapMappingPlanSummary summary{};
    const BootstrapMappingPlanError error = ValidateInputs(image, finalBootInfo, reservedPages, summary);
    if (error != BootstrapMappingPlanError::None)
    {
        return error;
    }
    outputSummary = summary;
    return BootstrapMappingPlanError::None;
}

BootstrapMappingPlanError BootstrapMappingPlanBuilder::Build(const Amd64LoadedImageLayout& image,
                                                             const Boot::BootInfo& finalBootInfo,
                                                             const PhysicalPageRange& reservedPages,
                                                             InitialMappingPlan& outputPlan,
                                                             BootstrapMappingPlanSummary& outputSummary) noexcept
{
    if ((outputPlan.GetMappingCount() != 0U) || outputPlan.IsSealed())
    {
        return BootstrapMappingPlanError::OutputNotEmpty;
    }
    BootstrapMappingPlanSummary validatedSummary{};
    const BootstrapMappingPlanError validationError =
        ValidateInputs(image, finalBootInfo, reservedPages, validatedSummary);
    if (validationError != BootstrapMappingPlanError::None)
    {
        return validationError;
    }

    Foundation::Size mappingIndex{0U};
    for (Foundation::Size index{0U}; index < Firmware::Amd64LoadedImageSectionCount; ++index)
    {
        const Firmware::Amd64LoadedImageSection& section = image.Sections[index];
        const InitialMappingPlanError mappingError = outputPlan.AddMapping(MakeMapping(
            section.VirtualStart, section.PageCount, PermissionsFor(section.Purpose), OwnerFor(section.Purpose)));
        if (mappingError != InitialMappingPlanError::None)
        {
            return BootstrapMappingPlanError::InternalInvariantViolation;
        }
        ++mappingIndex;
    }

    Foundation::UInt64 stackStart{0U};
    Foundation::UInt64 tableStorageStart{0U};
    Foundation::UInt64 descriptorTableStorageStart{0U};
    const InitialMappingPlanError reservedMappingError =
        AddReservedMappings(outputPlan, reservedPages.PhysicalStart, mappingIndex, stackStart, tableStorageStart,
                            descriptorTableStorageStart);
    if ((reservedMappingError != InitialMappingPlanError::None) || (mappingIndex != BootstrapInitialMappingCount))
    {
        return BootstrapMappingPlanError::InternalInvariantViolation;
    }

    const InitialMappingPlanError sealError = outputPlan.Seal();
    if (sealError != InitialMappingPlanError::None)
    {
        return BootstrapMappingPlanError::InternalInvariantViolation;
    }

    if ((stackStart != validatedSummary.StackPhysicalStart) ||
        (tableStorageStart != validatedSummary.PageTableStoragePhysicalStart) ||
        (descriptorTableStorageStart != validatedSummary.DescriptorTableStoragePhysicalStart))
    {
        return BootstrapMappingPlanError::InternalInvariantViolation;
    }
    outputSummary = validatedSummary;
    return BootstrapMappingPlanError::None;
}
} // namespace Asteria::Platform::Amd64::Memory
