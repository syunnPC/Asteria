// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/BootstrapMappingPlan.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
using Firmware::Ia64LoadedImageLayout;
using Firmware::Ia64LoadedImageSectionPurpose;
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
constexpr Foundation::UInt64 Ia64VirtualRegionSizeBytes{Foundation::UInt64{1U} << 61U};
constexpr Foundation::UInt64 FirmwareMemoryRuntimeAttribute{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 FirmwareMemoryWriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt32 FirmwareDescriptorVersion{1U};
constexpr Foundation::UInt32 MinimumFirmwareDescriptorSize{40U};
constexpr Foundation::UInt32 ConventionalMemoryType{7U};
constexpr Foundation::UInt32 MaximumStandardMemoryType{15U};
constexpr Foundation::UInt32 MinimumOemMemoryType{0x7000'0000U};
constexpr Foundation::UInt64 MaximumPhysicalPageCount{Foundation::UInt64{1U} << 52U};

[[nodiscard]] constexpr BootstrapMappingPlanFailure
Failure(const BootstrapMappingPlanError error,
        const InitialMappingPlanError mappingError = InitialMappingPlanError::None,
        const Foundation::Size mappingIndex = 0U) noexcept
{
    return BootstrapMappingPlanFailure{
        .Error = error,
        .MappingError = mappingError,
        .MappingIndex = mappingIndex,
    };
}

[[nodiscard]] Foundation::Result<BootstrapMappingPlanSummary, BootstrapMappingPlanFailure>
Fail(const BootstrapMappingPlanFailure& failure) noexcept
{
    return Foundation::Result<BootstrapMappingPlanSummary, BootstrapMappingPlanFailure>::Failure(failure);
}

[[nodiscard]] constexpr MappingPermissions ReadOnly() noexcept
{
    return MappingPermissions{MappingPermission::Read};
}

[[nodiscard]] constexpr MappingPermissions PermissionsFor(const Ia64LoadedImageSectionPurpose purpose) noexcept
{
    switch (purpose)
    {
    case Ia64LoadedImageSectionPurpose::Code:
        return ReadOnly().With(MappingPermission::Execute);
    case Ia64LoadedImageSectionPurpose::ReadOnlyData:
        return ReadOnly();
    case Ia64LoadedImageSectionPurpose::WritableData:
        return ReadOnly().With(MappingPermission::Write);
    }
    return MappingPermissions{};
}

[[nodiscard]] constexpr InitialMappingOwner OwnerFor(const Ia64LoadedImageSectionPurpose purpose) noexcept
{
    switch (purpose)
    {
    case Ia64LoadedImageSectionPurpose::Code:
        return InitialMappingOwner::NucleusCode;
    case Ia64LoadedImageSectionPurpose::ReadOnlyData:
        return InitialMappingOwner::NucleusReadOnlyData;
    case Ia64LoadedImageSectionPurpose::WritableData:
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

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] constexpr bool IsRangeInVirtualRegionZero(const Foundation::UInt64 start,
                                                        const Foundation::UInt64 end) noexcept
{
    return (start < Ia64VirtualRegionSizeBytes) && (end <= Ia64VirtualRegionSizeBytes);
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
    AlignedImage,
    Reserved,
    KernelElf,
};

/// reservedPages names the allocated reservation (bootstrap or run-time
/// memory) whose attributes a Reserved region must carry.
[[nodiscard]] bool IsRegionCompatible(const Boot::PhysicalMemoryRegion& region, const ProvenancePurpose purpose,
                                      const PhysicalPageRange& reservedPages) noexcept
{
    if (((region.Attributes & FirmwareMemoryWriteBackAttribute) == 0U) ||
        ((region.Attributes & FirmwareMemoryRuntimeAttribute) != 0U))
    {
        return false;
    }
    if (purpose == ProvenancePurpose::KernelElf)
    {
        return region.FirmwareType == 1U && region.Kind == Boot::PhysicalMemoryKind::Loader;
    }
    if (purpose == ProvenancePurpose::AlignedImage)
    {
        return (region.FirmwareType == 1U) && (region.Kind == Boot::PhysicalMemoryKind::Loader);
    }
    if (purpose == ProvenancePurpose::Image)
    {
        return IsImageRamRegion(region);
    }
    return (region.FirmwareType == reservedPages.FirmwareType) &&
           ((region.FirmwareType == ConventionalMemoryType && region.Kind == Boot::PhysicalMemoryKind::Conventional) ||
            (region.FirmwareType == 2U && region.Kind == Boot::PhysicalMemoryKind::Loader)) &&
           (region.Attributes == reservedPages.Attributes);
}

/// A run-time memory reservation is one page of an architected insertable
/// size, aligned to that size, of Conventional write-back memory without the
/// runtime attribute, inside region 0.
[[nodiscard]] bool IsRuntimeMemoryReservationShapeValid(const PhysicalPageRange& range,
                                                        Foundation::UInt64& end) noexcept
{
    // One page count that is a power of two and, in pages, an architected
    // size: at most 256 MiB, so the byte size below cannot wrap.
    constexpr Foundation::UInt64 ArchitectedPageCounts{ArchitectedMinimumInsertablePageSizeMask >> 12U};
    if ((range.PhysicalStart == 0U) || ((range.PageCount & (range.PageCount - 1U)) != 0U) ||
        ((range.PageCount & ArchitectedPageCounts) == 0U) || (range.Reserved != 0U) ||
        (range.FirmwareType != ConventionalMemoryType && range.FirmwareType != 2U) ||
        ((range.Attributes & FirmwareMemoryWriteBackAttribute) == 0U) ||
        ((range.Attributes & FirmwareMemoryRuntimeAttribute) != 0U))
    {
        return false;
    }
    const Foundation::UInt64 bytes = range.PageCount << 12U;
    return ((range.PhysicalStart & (bytes - 1U)) == 0U) && TryGetRangeEnd(range.PhysicalStart, range.PageCount, end) &&
           IsRangeInVirtualRegionZero(range.PhysicalStart, end);
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

[[nodiscard]] bool IsImageLayoutValid(const Ia64LoadedImageLayout& image) noexcept
{
    if ((image.ImageBase == 0U) || ((image.ImageBase & (PageBytes - 1U)) != 0U) ||
        (image.ImageSize < (2U * PageBytes)) || ((image.ImageSize & (PageBytes - 1U)) != 0U) ||
        (image.ImageBase > (MaximumUInt64 - image.ImageSize)) ||
        !IsRangeInVirtualRegionZero(image.ImageBase, image.ImageBase + image.ImageSize))
    {
        return false;
    }

    if ((image.Placement != Firmware::Ia64LoadedImagePlacement::Firmware) &&
        (image.Placement != Firmware::Ia64LoadedImagePlacement::AlignedCopy) &&
        (image.Placement != Firmware::Ia64LoadedImagePlacement::KernelElf))
    {
        return false;
    }
    if (image.Placement != Firmware::Ia64LoadedImagePlacement::Firmware)
    {
        constexpr Foundation::UInt64 GroupBytes{0x400000U};
        if ((image.ImageBase & (Firmware::Ia64RelocatedImageAlignmentBytes - 1U)) != 0U)
        {
            return false;
        }
        const Foundation::UInt64 code = image.Sections[0U].VirtualStart;
        const Foundation::UInt64 readOnly = image.Sections[1U].VirtualStart;
        const Foundation::UInt64 writable = image.Sections[4U].VirtualStart;
        if (((code | readOnly | writable) & (GroupBytes - 1U)) != 0U ||
            (image.Sections[0U].PageCount != (GroupBytes / PageBytes)) || ((readOnly + GroupBytes) != writable) ||
            ((writable + GroupBytes) != (image.ImageBase + image.ImageSize)))
        {
            return false;
        }
    }
    constexpr Ia64LoadedImageSectionPurpose ExpectedPurposes[Firmware::Ia64LoadedImageSectionCount]{
        Ia64LoadedImageSectionPurpose::Code,         Ia64LoadedImageSectionPurpose::ReadOnlyData,
        Ia64LoadedImageSectionPurpose::ReadOnlyData, Ia64LoadedImageSectionPurpose::ReadOnlyData,
        Ia64LoadedImageSectionPurpose::WritableData, Ia64LoadedImageSectionPurpose::WritableData,
    };
    const Foundation::UInt64 minimumSectionStart = image.ImageBase + PageBytes;
    Foundation::UInt64 expectedStart = image.Sections[0U].VirtualStart;
    if ((expectedStart < minimumSectionStart) || (expectedStart >= (image.ImageBase + image.ImageSize)))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < Firmware::Ia64LoadedImageSectionCount; ++index)
    {
        const Firmware::Ia64LoadedImageSection& section = image.Sections[index];
        Foundation::UInt64 sectionEnd{0U};
        if ((section.VirtualStart != expectedStart) || (section.Purpose != ExpectedPurposes[index]) ||
            (section.Reserved != 0U) || !TryGetRangeEnd(section.VirtualStart, section.PageCount, sectionEnd) ||
            (sectionEnd > (image.ImageBase + image.ImageSize)))
        {
            return false;
        }
        expectedStart = sectionEnd;
    }
    return expectedStart == (image.ImageBase + image.ImageSize);
}

[[nodiscard]] constexpr InitialMapping MakeMapping(const Foundation::UInt64 start, const Foundation::UInt64 pages,
                                                   const MappingPermissions permissions,
                                                   const InitialMappingOwner owner) noexcept
{
    return InitialMapping{
        .VirtualStart = VirtualAddress{start},
        .PhysicalStart = PhysicalAddress{start},
        .Pages = PageCount{pages},
        .Permissions = permissions.With(MappingPermission::Global),
        .CachePolicy = MemoryCachePolicy::WriteBack,
        .Owner = owner,
        .Reserved = 0U,
    };
}

[[nodiscard]] InitialMappingPlanError AddLoadedImageMappings(InitialMappingPlan& output,
                                                             const Ia64LoadedImageLayout& image,
                                                             Foundation::Size& mappingIndex) noexcept
{
    constexpr Foundation::Size groupFirstSections[BootstrapLoadedImageMappingCount]{0U, 1U, 4U};
    constexpr Foundation::Size groupEndSections[BootstrapLoadedImageMappingCount]{1U, 4U, 6U};

    for (Foundation::Size groupIndex{0U}; groupIndex < BootstrapLoadedImageMappingCount; ++groupIndex)
    {
        const Foundation::Size firstSection = groupFirstSections[groupIndex];
        const Foundation::Size endSection = groupEndSections[groupIndex];
        Foundation::UInt64 pageCount{0U};
        for (Foundation::Size sectionIndex{firstSection}; sectionIndex < endSection; ++sectionIndex)
        {
            pageCount += image.Sections[sectionIndex].PageCount;
        }

        const Firmware::Ia64LoadedImageSection& first = image.Sections[firstSection];
        const InitialMappingPlanError error = output.AddMapping(
            MakeMapping(first.VirtualStart, pageCount, PermissionsFor(first.Purpose), OwnerFor(first.Purpose)));
        if (error != InitialMappingPlanError::None)
        {
            return error;
        }
        ++mappingIndex;
    }
    return InitialMappingPlanError::None;
}

[[nodiscard]] InitialMappingPlanError AddReservedMappings(InitialMappingPlan& output,
                                                          const Foundation::UInt64 reservedStart,
                                                          Foundation::Size& mappingIndex,
                                                          Foundation::UInt64& translationStorageStart) noexcept
{
    const Foundation::UInt64 stackStart = reservedStart + (BootstrapStackPageOffset << 12U);
    InitialMappingPlanError error =
        output.AddMapping(MakeMapping(stackStart, BootstrapStackPageCount, ReadOnly().With(MappingPermission::Write),
                                      InitialMappingOwner::NucleusStack));
    if (error != InitialMappingPlanError::None)
    {
        return error;
    }
    ++mappingIndex;

    translationStorageStart = reservedStart + (BootstrapTranslationStoragePageOffset << 12U);
    error = output.AddMapping(MakeMapping(translationStorageStart, BootstrapTranslationStoragePageCount,
                                          ReadOnly().With(MappingPermission::Write), InitialMappingOwner::PageTable));
    if (error != InitialMappingPlanError::None)
    {
        return error;
    }
    ++mappingIndex;

    const Foundation::UInt64 interruptVectorTableStart =
        reservedStart + (BootstrapInterruptVectorTablePageOffset << 12U);
    error = output.AddMapping(MakeMapping(interruptVectorTableStart, BootstrapInterruptVectorTablePageCount,
                                          ReadOnly().With(MappingPermission::Execute),
                                          InitialMappingOwner::NucleusInterruptVectorTable));
    if (error != InitialMappingPlanError::None)
    {
        return error;
    }
    ++mappingIndex;

    const Foundation::UInt64 rseBackingStoreStart = reservedStart + (BootstrapRseBackingStorePageOffset << 12U);
    error = output.AddMapping(MakeMapping(rseBackingStoreStart, BootstrapRseBackingStorePageCount,
                                          ReadOnly().With(MappingPermission::Write),
                                          InitialMappingOwner::NucleusRegisterBackingStore));
    if (error == InitialMappingPlanError::None)
    {
        ++mappingIndex;
    }
    return error;
}

[[nodiscard]] InitialMappingPlanError AddRuntimeMemoryMapping(InitialMappingPlan& output,
                                                              const PhysicalPageRange& runtimeMemoryPages,
                                                              Foundation::Size& mappingIndex,
                                                              const bool consolidated) noexcept
{
    const InitialMappingPlanError error = output.AddMapping(MakeMapping(
        runtimeMemoryPages.PhysicalStart, runtimeMemoryPages.PageCount, ReadOnly().With(MappingPermission::Write),
        consolidated ? InitialMappingOwner::NucleusBootstrapRuntimeData : InitialMappingOwner::NucleusWritableData));
    if (error == InitialMappingPlanError::None)
    {
        ++mappingIndex;
    }
    return error;
}
} // namespace

Foundation::Result<BootstrapMappingPlanSummary, BootstrapMappingPlanFailure>
BootstrapMappingPlanBuilder::Build(const Ia64LoadedImageLayout& image, const Boot::BootInfo& finalBootInfo,
                                   const PhysicalPageRange& reservedPages, const PhysicalPageRange& runtimeMemoryPages,
                                   InitialMappingPlan& output) noexcept
{
    if ((output.GetMappingCount() != 0U) || output.IsSealed())
    {
        return Fail(Failure(BootstrapMappingPlanError::OutputNotEmpty));
    }
    if (!IsImageLayoutValid(image))
    {
        return Fail(Failure(BootstrapMappingPlanError::InvalidImageLayout));
    }
    if (!IsBootInfoShapeValid(finalBootInfo))
    {
        return Fail(Failure(BootstrapMappingPlanError::InvalidBootInfo));
    }
    Foundation::UInt64 reservedEnd{0U};
    if ((reservedPages.PhysicalStart == 0U) || (reservedPages.PageCount != BootstrapReservedPageCount) ||
        (reservedPages.Reserved != 0U) ||
        (reservedPages.FirmwareType != ConventionalMemoryType &&
         !(image.Placement == Firmware::Ia64LoadedImagePlacement::KernelElf && reservedPages.FirmwareType == 2U)) ||
        ((reservedPages.Attributes & FirmwareMemoryWriteBackAttribute) == 0U) ||
        ((reservedPages.Attributes & FirmwareMemoryRuntimeAttribute) != 0U) ||
        ((reservedPages.PhysicalStart & ((BootstrapReservedAlignmentPages * PageBytes) - 1U)) != 0U) ||
        !TryGetRangeEnd(reservedPages.PhysicalStart, reservedPages.PageCount, reservedEnd) ||
        !IsRangeInVirtualRegionZero(reservedPages.PhysicalStart, reservedEnd))
    {
        return Fail(Failure(BootstrapMappingPlanError::InvalidReservedPages));
    }
    if (RangesOverlap(reservedPages.PhysicalStart, reservedEnd, image.ImageBase, image.ImageBase + image.ImageSize))
    {
        return Fail(Failure(BootstrapMappingPlanError::ReservedPagesOverlapImage));
    }
    Foundation::UInt64 runtimeMemoryEnd{0U};
    if (runtimeMemoryPages.FirmwareType != reservedPages.FirmwareType ||
        !IsRuntimeMemoryReservationShapeValid(runtimeMemoryPages, runtimeMemoryEnd))
    {
        return Fail(Failure(BootstrapMappingPlanError::InvalidRuntimeMemoryPages));
    }
    if (RangesOverlap(runtimeMemoryPages.PhysicalStart, runtimeMemoryEnd, image.ImageBase,
                      image.ImageBase + image.ImageSize) ||
        (RangesOverlap(runtimeMemoryPages.PhysicalStart, runtimeMemoryEnd, reservedPages.PhysicalStart, reservedEnd) &&
         !((reservedPages.PhysicalStart == runtimeMemoryPages.PhysicalStart) && (reservedEnd < runtimeMemoryEnd))))
    {
        return Fail(Failure(BootstrapMappingPlanError::RuntimeMemoryPagesOverlap));
    }
    if (!HasMemoryProvenance(
            finalBootInfo, image.ImageBase, image.ImageSize >> 12U,
            image.Placement == Firmware::Ia64LoadedImagePlacement::KernelElf     ? ProvenancePurpose::KernelElf
            : image.Placement == Firmware::Ia64LoadedImagePlacement::AlignedCopy ? ProvenancePurpose::AlignedImage
                                                                                 : ProvenancePurpose::Image,
            reservedPages))
    {
        return Fail(Failure(BootstrapMappingPlanError::InvalidImageMemoryProvenance));
    }
    if (!HasMemoryProvenance(finalBootInfo, reservedPages.PhysicalStart, reservedPages.PageCount,
                             ProvenancePurpose::Reserved, reservedPages))
    {
        return Fail(Failure(BootstrapMappingPlanError::InvalidReservedMemoryProvenance));
    }
    if (!HasMemoryProvenance(finalBootInfo, runtimeMemoryPages.PhysicalStart, runtimeMemoryPages.PageCount,
                             ProvenancePurpose::Reserved, runtimeMemoryPages))
    {
        return Fail(Failure(BootstrapMappingPlanError::InvalidRuntimeMemoryProvenance));
    }

    const bool consolidated = reservedPages.PhysicalStart == runtimeMemoryPages.PhysicalStart;
    const Foundation::Size expectedMappingCount =
        consolidated ? BootstrapConsolidatedMappingCount : BootstrapInitialMappingCount;
    Foundation::Size mappingIndex{0U};
    InitialMappingPlanError mappingError = AddLoadedImageMappings(output, image, mappingIndex);
    if (mappingError != InitialMappingPlanError::None)
    {
        return Fail(Failure(BootstrapMappingPlanError::InternalInvariantViolation, mappingError, mappingIndex));
    }

    Foundation::UInt64 translationStorageStart{0U};
    if (!consolidated)
    {
        mappingError = AddReservedMappings(output, reservedPages.PhysicalStart, mappingIndex, translationStorageStart);
        if (mappingError != InitialMappingPlanError::None)
        {
            return Fail(Failure(BootstrapMappingPlanError::InternalInvariantViolation, mappingError, mappingIndex));
        }
    }
    else
    {
        translationStorageStart = reservedPages.PhysicalStart + (BootstrapTranslationStoragePageOffset << 12U);
    }
    mappingError = AddRuntimeMemoryMapping(output, runtimeMemoryPages, mappingIndex, consolidated);
    if (mappingError != InitialMappingPlanError::None)
    {
        return Fail(Failure(BootstrapMappingPlanError::InternalInvariantViolation, mappingError, mappingIndex));
    }
    if (mappingIndex != expectedMappingCount)
    {
        return Fail(Failure(BootstrapMappingPlanError::InternalInvariantViolation,
                            InitialMappingPlanError::InvalidMapping, mappingIndex));
    }

    const InitialMappingPlanError sealError = output.Seal();
    if (sealError != InitialMappingPlanError::None)
    {
        return Fail(
            Failure(BootstrapMappingPlanError::InternalInvariantViolation, sealError, BootstrapInitialMappingCount));
    }

    return Foundation::Result<BootstrapMappingPlanSummary, BootstrapMappingPlanFailure>::Success(
        BootstrapMappingPlanSummary{
            .StackPhysicalStart = reservedPages.PhysicalStart,
            .TranslationStoragePhysicalStart = translationStorageStart,
            .MappingCount = static_cast<Foundation::UInt16>(expectedMappingCount),
            .Reserved16 = 0U,
            .Reserved32 = 0U,
            .RseBackingStorePhysicalStart =
                consolidated
                    ? runtimeMemoryEnd - (BootstrapRseBackingStorePageCount * Kernel::Memory::MappingPageSizeBytes)
                    : reservedPages.PhysicalStart +
                          (BootstrapRseBackingStorePageOffset * Kernel::Memory::MappingPageSizeBytes),
        });
}
} // namespace Asteria::Platform::Ia64::Memory
