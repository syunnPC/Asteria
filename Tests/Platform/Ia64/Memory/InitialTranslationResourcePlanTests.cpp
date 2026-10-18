// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <new>

#include "Boot/BootInfo.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Ia64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Ia64/Memory/InitialTranslationResourcePlan.hpp"

#include "Support/Ia64HostedBootGeometry.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
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
using Boot::BootInfo;
using Boot::PhysicalMemoryKind;
using Boot::PhysicalMemoryRegion;
using Platform::Ia64::Firmware::Ia64LoadedImageLayout;
using Platform::Ia64::Firmware::Ia64LoadedImageBaseAlignmentBytes;
using Platform::Ia64::Firmware::Ia64LoadedImageSectionPurpose;
using Platform::Ia64::Firmware::SalSystemTableInformation;
using Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask;
using Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask;
using Platform::Ia64::Memory::BootstrapMappingPlanBuilder;
using Platform::Ia64::Memory::BootstrapReservedPageCount;
using Platform::Ia64::Memory::InitialTranslationEncodingError;
using Platform::Ia64::Memory::InitialTranslationRegionSet;
using Platform::Ia64::Memory::InitialTranslationResourceEntry;
using Platform::Ia64::Memory::InitialTranslationResourcePlan;
using Platform::Ia64::Memory::InitialTranslationResourcePlanError;
using Platform::Ia64::Memory::InitialTranslationResourcePlanner;
using Platform::Ia64::Memory::PalVirtualMemoryCapabilities;
using Platform::Ia64::Memory::RuntimeMemoryReservationPreferredBytes;
using Platform::Ia64::Memory::RegionConfiguration;

constexpr Foundation::UInt64 Base{0xE000'0000'0010'0000ULL};
constexpr Foundation::UInt64 PhysicalBase{0x0100'0000U};
constexpr Foundation::UInt64 PageSizeBytes{4096U};
constexpr Foundation::UInt64 MinimumFinalImageResiduePeriodBytes{16U * 1024U * 1024U};
constexpr Foundation::UInt64 FinalImageSyntheticBase{0x0001'0000'0000'0000U};
constexpr Foundation::Size OutputBytes{sizeof(InitialTranslationResourcePlan)};

constinit InitialMappingPlan g_plan;
constinit InitialMappingPlan g_unsealedPlan;
constinit InitialMappingPlan g_dataCapacityPlan;
constinit InitialMappingPlan g_instructionCapacityPlan;
constinit InitialMappingPlan g_firmwareIdentityPlans[4];
constinit InitialMappingPlan g_crossRegionFirmwareOverlapPlan;
constinit InitialMappingPlan g_regionIdentifierConflictPlan;
constinit InitialTranslationResourcePlan g_output;
constinit InitialTranslationResourcePlan g_deterministicOutput;
constinit InitialTranslationResourcePlan g_failureOutput;
constinit InitialTranslationResourcePlan g_dataCapacityOutput;
constinit InitialTranslationResourcePlan g_instructionCapacityOutput;
constinit InitialMappingPlan g_finalImageMappingPlan;
constinit InitialMappingPlan g_hostedMappingPlan;
constinit InitialTranslationResourcePlan g_hostedTranslationPlan;
constinit BootInfo g_hostedBootInfo{};
constinit InitialTranslationResourcePlan g_finalImageTranslationPlan;
constinit BootInfo g_finalImageBootInfo{};
Foundation::UInt8 g_outputSnapshot[OutputBytes];
alignas(InitialTranslationResourcePlan) Foundation::UInt8 g_patternedOutputStorageA[OutputBytes];
alignas(InitialTranslationResourcePlan) Foundation::UInt8 g_patternedOutputStorageB[OutputBytes];
alignas(InitialTranslationResourcePlan) Foundation::UInt8 g_transientOutputStorage[OutputBytes];

static_assert((MinimumFinalImageResiduePeriodBytes % Ia64LoadedImageBaseAlignmentBytes) == 0U);
static_assert((MinimumFinalImageResiduePeriodBytes / Ia64LoadedImageBaseAlignmentBytes) == 256U);

[[nodiscard]] constexpr MappingPermissions ReadOnly() noexcept
{
    return MappingPermissions{MappingPermission::Read};
}

[[nodiscard]] constexpr MappingPermissions ReadWrite() noexcept
{
    return ReadOnly().With(MappingPermission::Write);
}

[[nodiscard]] constexpr MappingPermissions ReadExecute() noexcept
{
    return ReadOnly().With(MappingPermission::Execute);
}

[[nodiscard]] InitialMapping MakeMapping(const Foundation::UInt64 virtualStart, const Foundation::UInt64 physicalStart,
                                         const Foundation::UInt64 pages, const MappingPermissions permissions,
                                         const InitialMappingOwner owner) noexcept
{
    return InitialMapping{
        .VirtualStart = VirtualAddress{virtualStart},
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Pages = PageCount{pages},
        .Permissions = permissions,
        .CachePolicy = MemoryCachePolicy::WriteBack,
        .Owner = owner,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr PalVirtualMemoryCapabilities FullCapabilities() noexcept
{
    return PalVirtualMemoryCapabilities{
        .Translation =
            Platform::Ia64::Memory::TranslationCapabilities{
                .InsertablePageSizeMask = ArchitectedMinimumInsertablePageSizeMask,
                .PhysicalAddressBits = 50U,
                .ImplementedVirtualAddressMostSignificantBit = 60U,
                .RegionIdentifierBits = 24U,
                .ProtectionKeyBits = 24U,
                .MemoryAttributeMask =
                    static_cast<Foundation::UInt8>((Foundation::UInt8{1U} << 0U) | (Foundation::UInt8{1U} << 4U) |
                                                   (Foundation::UInt8{1U} << 5U) | (Foundation::UInt8{1U} << 6U)),
                .HardwareWalkerImplemented = true,
                .Reserved16 = 0U,
                .Reserved64 = 0U,
            },
        .PurgePageSizeMask = ArchitectedMinimumPurgePageSizeMask,
        .InstructionTranslationRegisterCount = 8U,
        .DataTranslationRegisterCount = 8U,
        .ProtectionKeyRegisterCount = 16U,
        .MaximumConcurrentPurgesEncoding = 1U,
        .HashTagIdentifier = 8U,
        .UniqueTranslationCacheCount = 2U,
        .TranslationCacheLevelCount = 1U,
        .Reserved8 = 0U,
        .Reserved32 = 0U,
    };
}

[[nodiscard]] constexpr SalSystemTableInformation FirmwareInformation() noexcept
{
    return SalSystemTableInformation{
        .PalProcedurePhysicalAddress = 0x0010'0000U,
        .SalProcedurePhysicalAddress = 0x0020'0000U,
        .SalGlobalDataPhysicalAddress = 0x0030'0000U,
        .TranslationRegisterVirtualAddress = 0U,
        .TranslationRegisterEncodedPageSize = 22U << 2U,
        .CoherenceDomainInformationPhysicalAddress = 0U,
        .ApWakeVector = 0xFFU,
        .TotalTableLength = 192U,
        .CoherenceDomainCount = 0U,
        .SalRevision = 0x0340U,
        .EntryCount = 3U,
        .SalAVersion = 0x0100U,
        .SalBVersion = 0x0100U,
        .MemoryDescriptorCount = 0U,
        .PlatformFeatures = 0U,
        .HasPlatformFeatures = true,
        .HasTranslationRegister = true,
        .HasPurgeCoherenceInformation = false,
        .HasApWakeVector = true,
        .Reserved8 = 0U,
    };
}

[[nodiscard]] constexpr InitialTranslationRegionSet PrivateRegions() noexcept
{
    InitialTranslationRegionSet regions{};
    for (Foundation::Size index{0U}; index < Platform::Ia64::Memory::InitialTranslationRegionCount; ++index)
    {
        regions.Regions[index] = RegionConfiguration{
            .RegionIdentifier = static_cast<Foundation::UInt32>(0x2000U + index),
            .ProtectionKey = 0U,
            .VhptWalkerEnabled = false,
            .SharedAcrossAddressSpaces = false,
            .Reserved16 = 0U,
            .Reserved32 = 0U,
        };
    }
    return regions;
}

[[nodiscard]] constexpr InitialTranslationRegionSet FinalImageRegions() noexcept
{
    InitialTranslationRegionSet regions = PrivateRegions();
    regions.Regions[0U].SharedAcrossAddressSpaces = true;
    return regions;
}

[[nodiscard]] bool Add(InitialMappingPlan& plan, const InitialMapping& mapping) noexcept
{
    return plan.AddMapping(mapping) == InitialMappingPlanError::None;
}

[[nodiscard]] bool TextIsEqual(const char* const first, const char* const second) noexcept
{
    if ((first == nullptr) || (second == nullptr))
    {
        return false;
    }
    Foundation::Size index{0U};
    while ((first[index] != '\0') && (second[index] != '\0'))
    {
        if (first[index] != second[index])
        {
            return false;
        }
        ++index;
    }
    return first[index] == second[index];
}

[[nodiscard]] bool ParseHexadecimal(const char* text, Foundation::UInt64& value) noexcept
{
    if (text == nullptr)
    {
        return false;
    }
    if ((text[0U] == '0') && ((text[1U] == 'x') || (text[1U] == 'X')))
    {
        text += 2U;
    }
    if (*text == '\0')
    {
        return false;
    }

    Foundation::UInt64 parsed{0U};
    for (; *text != '\0'; ++text)
    {
        Foundation::UInt8 digit{0U};
        if ((*text >= '0') && (*text <= '9'))
        {
            digit = static_cast<Foundation::UInt8>(*text - '0');
        }
        else if ((*text >= 'a') && (*text <= 'f'))
        {
            digit = static_cast<Foundation::UInt8>(*text - 'a' + 10);
        }
        else if ((*text >= 'A') && (*text <= 'F'))
        {
            digit = static_cast<Foundation::UInt8>(*text - 'A' + 10);
        }
        else
        {
            return false;
        }
        if (parsed > ((~Foundation::UInt64{0U} - digit) >> 4U))
        {
            return false;
        }
        parsed = (parsed << 4U) | digit;
    }
    value = parsed;
    return true;
}

[[nodiscard]] bool TryAddUnsigned(const Foundation::UInt64 first, const Foundation::UInt64 second,
                                  Foundation::UInt64& result) noexcept
{
    if (first > (~Foundation::UInt64{0U} - second))
    {
        return false;
    }
    result = first + second;
    return true;
}

// The final-image residue proof's synthetic run-time memory reservation: the
// preferred page at a base aligned to it, between the bootstrap reservation
// and the image, as the boot allocates it from the final map.
constexpr Foundation::UInt64 FinalImageRuntimeMemoryBase{2U * RuntimeMemoryReservationPreferredBytes};

struct FinalImagePlanCounts
{
    Foundation::UInt16 DataRegisters;
    Foundation::UInt16 InstructionRegisters;
    Foundation::UInt16 RuntimeMemoryDataRegisters;
};

[[nodiscard]] bool BuildFinalImageTranslationPlan(
    const Ia64LoadedImageLayout& image, const Foundation::UInt16 dataRegisterCount, FinalImagePlanCounts& counts,
    Platform::Ia64::Memory::InitialTranslationResourcePlanFailure& failure,
    const Foundation::UInt64 runtimeMemoryBytes = RuntimeMemoryReservationPreferredBytes) noexcept
{
    const Foundation::UInt64 runtimeMemoryPages{runtimeMemoryBytes / PageSizeBytes};
    g_finalImageBootInfo.Signature = Boot::BootInfoSignature;
    g_finalImageBootInfo.Version = Boot::BootInfoVersion;
    g_finalImageBootInfo.SizeBytes = sizeof(BootInfo);
    g_finalImageBootInfo.MemoryRegionCount = 2U;
    g_finalImageBootInfo.FirmwareDescriptorVersion = 1U;
    g_finalImageBootInfo.FirmwareDescriptorSize = 40U;
    g_finalImageBootInfo.Reserved = 0U;
    g_finalImageBootInfo.FirmwareMemoryMapSizeBytes = 80U;
    g_finalImageBootInfo.MemoryRegions[0U] = PhysicalMemoryRegion{
        .PhysicalStart = FinalImageRuntimeMemoryBase,
        .VirtualStart = 0U,
        .PageCount = runtimeMemoryPages,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Kind = PhysicalMemoryKind::Conventional,
    };
    g_finalImageBootInfo.MemoryRegions[1U] = PhysicalMemoryRegion{
        .PhysicalStart = image.ImageBase,
        .VirtualStart = 0U,
        .PageCount = image.ImageSize / PageSizeBytes,
        .Attributes = 0x8U,
        .FirmwareType = 1U,
        .Kind = PhysicalMemoryKind::Loader,
    };
    const PhysicalPageRange reserved{
        .PhysicalStart = FinalImageRuntimeMemoryBase,
        .PageCount = BootstrapReservedPageCount,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Reserved = 0U,
    };
    const PhysicalPageRange runtimeMemory{
        .PhysicalStart = FinalImageRuntimeMemoryBase,
        .PageCount = runtimeMemoryPages,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Reserved = 0U,
    };

    g_finalImageMappingPlan.~InitialMappingPlan();
    new (&g_finalImageMappingPlan) InitialMappingPlan{};
    g_finalImageTranslationPlan.~InitialTranslationResourcePlan();
    new (&g_finalImageTranslationPlan) InitialTranslationResourcePlan{};
    const auto mappings = BootstrapMappingPlanBuilder::Build(image, g_finalImageBootInfo, reserved, runtimeMemory,
                                                             g_finalImageMappingPlan);
    if (!mappings.HasValue())
    {
        const auto mappingFailure = mappings.GetError();
        static_cast<void>(std::fprintf(stderr,
                                       "final IA-64 bootstrap mapping failed: error=%u plan=%u "
                                       "index=%zu\n",
                                       static_cast<unsigned int>(mappingFailure.Error),
                                       static_cast<unsigned int>(mappingFailure.MappingError),
                                       mappingFailure.MappingIndex));
        return false;
    }
    Foundation::Size runtimeMemoryMapping{g_finalImageMappingPlan.GetMappingCount()};
    for (Foundation::Size index{0U}; index < g_finalImageMappingPlan.GetMappingCount(); ++index)
    {
        const auto mapping = g_finalImageMappingPlan.GetMapping(index);
        if (mapping.HasValue() && (mapping.GetValue().PhysicalStart.GetValue() == FinalImageRuntimeMemoryBase))
        {
            runtimeMemoryMapping = index;
        }
    }

    PalVirtualMemoryCapabilities capabilities = FullCapabilities();
    capabilities.InstructionTranslationRegisterCount = 8U;
    capabilities.DataTranslationRegisterCount = dataRegisterCount;
    const auto translations = InitialTranslationResourcePlanner::Build(
        g_finalImageMappingPlan, capabilities, FirmwareInformation(), FinalImageRegions(), g_finalImageTranslationPlan);
    counts = {};
    if (!translations.HasValue())
    {
        failure = translations.GetError();
        return true;
    }
    counts.DataRegisters = translations.GetValue().DataRegisterCount;
    counts.InstructionRegisters = translations.GetValue().InstructionRegisterCount;
    for (Foundation::Size index{0U}; index < g_finalImageTranslationPlan.GetEntryCount(); ++index)
    {
        const auto entry = g_finalImageTranslationPlan.GetEntry(index);
        if (entry.HasValue() && (entry.GetValue().MappingIndex == runtimeMemoryMapping))
        {
            ++counts.RuntimeMemoryDataRegisters;
        }
    }
    failure = {};
    return true;
}

[[nodiscard]] bool TryDeriveFinalImageResiduePeriod(const Ia64LoadedImageLayout& image,
                                                    Foundation::UInt64& periodBytes) noexcept
{
    const Foundation::UInt64 imageEnd = image.ImageBase + image.ImageSize;
    const PalVirtualMemoryCapabilities capabilities = FullCapabilities();
    periodBytes = MinimumFinalImageResiduePeriodBytes;
    for (Foundation::Size mappingIndex{0U}; mappingIndex < g_finalImageMappingPlan.GetMappingCount(); ++mappingIndex)
    {
        const auto mappingResult = g_finalImageMappingPlan.GetMapping(mappingIndex);
        if (!mappingResult.HasValue())
        {
            return false;
        }
        const InitialMapping mapping = mappingResult.GetValue();
        const Foundation::UInt64 mappingStart = mapping.VirtualStart.GetValue();
        const Foundation::UInt64 mappingBytes = mapping.Pages.GetValue() * PageSizeBytes;
        const Foundation::UInt64 mappingEnd = mappingStart + mappingBytes;
        if ((mappingStart < image.ImageBase) || (mappingEnd > imageEnd))
        {
            continue;
        }
        for (Foundation::UInt8 pageShift{12U}; pageShift <= 28U; ++pageShift)
        {
            const Foundation::UInt64 pageSizeBit = Foundation::UInt64{1U} << pageShift;
            const Foundation::UInt64 pageSizeBytes = Foundation::UInt64{1U} << pageShift;
            if (((capabilities.Translation.InsertablePageSizeMask & pageSizeBit) != 0U) &&
                ((capabilities.PurgePageSizeMask & pageSizeBit) != 0U) && (pageSizeBytes <= mappingBytes) &&
                (pageSizeBytes > periodBytes))
            {
                periodBytes = pageSizeBytes;
            }
        }
    }
    return (periodBytes >= MinimumFinalImageResiduePeriodBytes) && ((periodBytes & (periodBytes - 1U)) == 0U);
}

[[nodiscard]] Ia64LoadedImageLayout RelocateFinalImage(const Ia64LoadedImageLayout& relativeImage,
                                                       const Foundation::UInt64 imageBase) noexcept
{
    Ia64LoadedImageLayout image = relativeImage;
    image.ImageBase = (imageBase + Platform::Ia64::Firmware::Ia64RelocatedImageAlignmentBytes - 1U) &
                      ~(Platform::Ia64::Firmware::Ia64RelocatedImageAlignmentBytes - 1U);
    image.Placement = Platform::Ia64::Firmware::Ia64LoadedImagePlacement::AlignedCopy;
    for (Foundation::Size index{0U}; index < 6U; ++index)
    {
        image.Sections[index].VirtualStart += image.ImageBase;
    }
    return image;
}

/// The proof the Verify property Ia64FinalImageTranslationResidues runs over
/// each firmware's kernel ELF: the production planners map the image at every
/// 64 KiB-aligned load-address residue, with the bootstrap reservation and the
/// run-time memory reservation (one register at every residue), after placing
/// the copy at 16 MiB alignment: exactly 11 DTRs and 4 ITRs. Ten DTRs fail. The output
/// names the least and the most registers over the residues.
[[nodiscard]] int RunFinalImageResidueProof(const int argumentCount, char* const arguments[]) noexcept
{
    constexpr int FixedArgumentCount{3};
    constexpr int SectionArgumentCount{12};
    constexpr int ExpectedArgumentCount{FixedArgumentCount + SectionArgumentCount};
    if ((argumentCount != ExpectedArgumentCount) || !TextIsEqual(arguments[1U], "--final-image-residues"))
    {
        static_cast<void>(std::fprintf(stderr,
                                       "usage: %s --final-image-residues image-size "
                                       "section-rva section-size ...\n",
                                       arguments[0U]));
        return 2;
    }

    Foundation::UInt64 imageSize{0U};
    if (!ParseHexadecimal(arguments[2U], imageSize) || (imageSize == 0U) || ((imageSize & (PageSizeBytes - 1U)) != 0U))
    {
        static_cast<void>(std::fprintf(stderr, "invalid final IA-64 image geometry\n"));
        return 1;
    }

    Ia64LoadedImageLayout relativeImage{
        .ImageBase = 0U,
        .ImageSize = imageSize,
        .Sections = {},
    };
    constexpr Ia64LoadedImageSectionPurpose purposes[6U]{
        Ia64LoadedImageSectionPurpose::Code,         Ia64LoadedImageSectionPurpose::ReadOnlyData,
        Ia64LoadedImageSectionPurpose::ReadOnlyData, Ia64LoadedImageSectionPurpose::ReadOnlyData,
        Ia64LoadedImageSectionPurpose::WritableData, Ia64LoadedImageSectionPurpose::WritableData,
    };
    for (Foundation::Size index{0U}; index < 6U; ++index)
    {
        Foundation::UInt64 sectionRva{0U};
        Foundation::UInt64 sectionSize{0U};
        if (!ParseHexadecimal(arguments[3U + (2U * index)], sectionRva) ||
            !ParseHexadecimal(arguments[4U + (2U * index)], sectionSize) || (sectionSize == 0U) ||
            ((sectionRva & (PageSizeBytes - 1U)) != 0U))
        {
            static_cast<void>(std::fprintf(stderr, "invalid final IA-64 section %zu geometry\n", index));
            return 1;
        }
        Foundation::UInt64 roundedSize{0U};
        if (!TryAddUnsigned(sectionSize, PageSizeBytes - 1U, roundedSize))
        {
            return 1;
        }
        roundedSize &= ~(PageSizeBytes - 1U);
        Foundation::UInt64 sectionEndRva{0U};
        if (!TryAddUnsigned(sectionRva, roundedSize, sectionEndRva) || (sectionEndRva > imageSize))
        {
            static_cast<void>(std::fprintf(stderr, "final IA-64 section %zu exceeds the loaded image\n", index));
            return 1;
        }
        relativeImage.Sections[index] = {
            .VirtualStart = sectionRva,
            .PageCount = roundedSize / PageSizeBytes,
            .Purpose = purposes[index],
            .Reserved = 0U,
        };
    }

    FinalImagePlanCounts periodCounts{};
    Platform::Ia64::Memory::InitialTranslationResourcePlanFailure periodFailure{};
    Foundation::UInt64 residuePeriodBytes{0U};
    if (!BuildFinalImageTranslationPlan(RelocateFinalImage(relativeImage, FinalImageSyntheticBase), 8U, periodCounts,
                                        periodFailure) ||
        (periodFailure.Error != InitialTranslationResourcePlanError::None) ||
        !TryDeriveFinalImageResiduePeriod(RelocateFinalImage(relativeImage, FinalImageSyntheticBase),
                                          residuePeriodBytes))
    {
        static_cast<void>(std::fprintf(stderr, "could not derive the final IA-64 relocation-residue period\n"));
        return 1;
    }
    const Foundation::UInt64 residueCount = residuePeriodBytes / Ia64LoadedImageBaseAlignmentBytes;

    Foundation::UInt16 maximumDataRegisters{0U};
    Foundation::UInt16 minimumDataRegisters{0xFFFFU};
    Foundation::UInt16 maximumInstructionRegisters{0U};
    Foundation::UInt64 worstResidue{0U};
    for (Foundation::UInt64 residue{0U}; residue < residueCount; ++residue)
    {
        FinalImagePlanCounts counts{};
        Platform::Ia64::Memory::InitialTranslationResourcePlanFailure failure{};
        if (!BuildFinalImageTranslationPlan(
                RelocateFinalImage(relativeImage,
                                   FinalImageSyntheticBase + (residue * Ia64LoadedImageBaseAlignmentBytes)),
                8U, counts, failure) ||
            (failure.Error != InitialTranslationResourcePlanError::None) || (counts.DataRegisters != 4U) ||
            (counts.InstructionRegisters != 2U))
        {
            static_cast<void>(std::fprintf(stderr,
                                           "final IA-64 TR plan rejected residue 0x%llx: "
                                           "error=%u encoding=%u mapping=%zu page=%llu\n",
                                           static_cast<unsigned long long>(residue * Ia64LoadedImageBaseAlignmentBytes),
                                           static_cast<unsigned int>(failure.Error),
                                           static_cast<unsigned int>(failure.EncodingError), failure.MappingIndex,
                                           static_cast<unsigned long long>(failure.MappingPageIndex)));
            return 1;
        }
        if (counts.RuntimeMemoryDataRegisters != 1U)
        {
            static_cast<void>(
                std::fprintf(stderr,
                             "the run-time memory reservation took %u DTRs at residue "
                             "0x%llx, expected one\n",
                             static_cast<unsigned int>(counts.RuntimeMemoryDataRegisters),
                             static_cast<unsigned long long>(residue * Ia64LoadedImageBaseAlignmentBytes)));
            return 1;
        }
        if (counts.DataRegisters > maximumDataRegisters)
        {
            maximumDataRegisters = counts.DataRegisters;
            worstResidue = residue;
        }
        if (counts.DataRegisters < minimumDataRegisters)
        {
            minimumDataRegisters = counts.DataRegisters;
        }
        if (counts.InstructionRegisters > maximumInstructionRegisters)
        {
            maximumInstructionRegisters = counts.InstructionRegisters;
        }
    }
    if ((minimumDataRegisters != 4U) || (maximumDataRegisters != 4U))
    {
        static_cast<void>(std::fprintf(stderr, "final IA-64 geometry unexpectedly requires %u DTRs at worst\n",
                                       static_cast<unsigned int>(maximumDataRegisters)));
        return 1;
    }

    FinalImagePlanCounts counts{};
    Platform::Ia64::Memory::InitialTranslationResourcePlanFailure failure{};
    if (!BuildFinalImageTranslationPlan(
            RelocateFinalImage(relativeImage,
                               FinalImageSyntheticBase + (worstResidue * Ia64LoadedImageBaseAlignmentBytes)),
            static_cast<Foundation::UInt16>(maximumDataRegisters - 1U), counts, failure) ||
        (failure.Error != InitialTranslationResourcePlanError::InvalidCapabilities))
    {
        static_cast<void>(std::fprintf(stderr, "the planner did not reject a below-minimum PAL report\n"));
        return 1;
    }
    static_cast<void>(std::fprintf(
        stdout,
        "PASS: aligned final IA-64 geometry needs %u to %u of 8 DTRs (one for the "
        "run-time memory reservation) and at most %u of 8 ITRs, and rejects "
        "a below-minimum PAL report of %u DTRs; checked %llu loader-aligned residues (period "
        "0x%llx)\n",
        static_cast<unsigned int>(minimumDataRegisters), static_cast<unsigned int>(maximumDataRegisters),
        static_cast<unsigned int>(maximumInstructionRegisters), static_cast<unsigned int>(maximumDataRegisters - 1U),
        static_cast<unsigned long long>(residueCount), static_cast<unsigned long long>(residuePeriodBytes)));
    return 0;
}

[[nodiscard]] InitialTranslationResourcePlan& FreshTransientOutput() noexcept
{
    return *::new (static_cast<void*>(g_transientOutputStorage)) InitialTranslationResourcePlan;
}

[[nodiscard]] bool PrepareExactPlan() noexcept
{
    return Add(g_plan, MakeMapping(Base, PhysicalBase, 32U, ReadExecute(), InitialMappingOwner::NucleusCode)) &&
           Add(g_plan, MakeMapping(Base + 0x20'000U, PhysicalBase + 0x20'000U, 1U, ReadWrite(),
                                   InitialMappingOwner::NucleusStack)) &&
           Add(g_plan, MakeMapping(Base + 0x21'000U, PhysicalBase + 0x21'000U, 1U, ReadWrite(),
                                   InitialMappingOwner::PageTable)) &&
           Add(g_plan, MakeMapping(Base + 0x28'000U, PhysicalBase + 0x28'000U, 8U, ReadOnly(),
                                   InitialMappingOwner::BootHandoff)) &&
           (g_plan.Seal() == InitialMappingPlanError::None);
}

void TestConsolidatedArchitectedPlan(TestContext& runner) noexcept
{
    const Foundation::UInt64 imageBase{FinalImageSyntheticBase};
    const Ia64LoadedImageLayout image{
        .ImageBase = imageBase,
        .ImageSize = 16U * 1024U * 1024U,
        .Placement = Platform::Ia64::Firmware::Ia64LoadedImagePlacement::AlignedCopy,
        .Sections =
            {
                {imageBase + 0x400000U, 1024U, Ia64LoadedImageSectionPurpose::Code, 0U},
                {imageBase + 0x800000U, 1022U, Ia64LoadedImageSectionPurpose::ReadOnlyData, 0U},
                {imageBase + 0xBFE000U, 1U, Ia64LoadedImageSectionPurpose::ReadOnlyData, 0U},
                {imageBase + 0xBFF000U, 1U, Ia64LoadedImageSectionPurpose::ReadOnlyData, 0U},
                {imageBase + 0xC00000U, 1023U, Ia64LoadedImageSectionPurpose::WritableData, 0U},
                {imageBase + 0xFFF000U, 1U, Ia64LoadedImageSectionPurpose::WritableData, 0U},
            },
    };
    FinalImagePlanCounts counts{};
    Platform::Ia64::Memory::InitialTranslationResourcePlanFailure failure{};
    ASTERIA_EXPECT(runner, "the production planners run with an architectural 8+8 PAL report",
                   BuildFinalImageTranslationPlan(image, 8U, counts, failure),
                   failure.Error == InitialTranslationResourcePlanError::None, counts.DataRegisters == 4U,
                   counts.InstructionRegisters == 2U, counts.RuntimeMemoryDataRegisters == 1U);
    ASTERIA_EXPECT(runner, "the consolidated fallback page keeps the same bound on an 8+8 CPU",
                   BuildFinalImageTranslationPlan(image, 8U, counts, failure, 4U * 1024U * 1024U),
                   failure.Error == InitialTranslationResourcePlanError::None, counts.DataRegisters == 4U,
                   counts.InstructionRegisters == 2U, counts.RuntimeMemoryDataRegisters == 1U);
}

void TestExactPlan(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "resource-planning mapping fixture seals", PrepareExactPlan());
    const PalVirtualMemoryCapabilities capabilities = FullCapabilities();
    const SalSystemTableInformation firmware = FirmwareInformation();
    const InitialTranslationRegionSet regions = PrivateRegions();
    const auto result = InitialTranslationResourcePlanner::Build(g_plan, capabilities, firmware, regions, g_output);
    ASTERIA_EXPECT(runner, "planner retains exact register counts and firmware purge operands", result.HasValue(),
                   result.GetValue().EntryCount == 6U, result.GetValue().DataRegisterCount == 6U,
                   result.GetValue().InstructionRegisterCount == 3U, result.GetValue().Reserved == 0U,
                   g_output.IsBuilt(), g_output.GetEntryCount() == 6U, g_output.GetDataRegisterCount() == 6U,
                   g_output.GetInstructionRegisterCount() == 3U, g_output.GetFirmwareTranslationVirtualAddress() == 0U,
                   g_output.GetFirmwareTranslationEncodedPageSize() == (22U << 2U));

    const auto firstResult = g_output.GetEntry(0U);
    const auto secondResult = g_output.GetEntry(1U);
    const auto stackResult = g_output.GetEntry(2U);
    const auto lastResult = g_output.GetEntry(5U);
    ASTERIA_EXPECT(runner, "first code extent receives DTR0 and ITR1 as a 64 KiB translation", firstResult.HasValue(),
                   firstResult.GetValue().MappingIndex == 0U, firstResult.GetValue().MappingPageIndex == 0U,
                   firstResult.GetValue().PageShift == 16U, firstResult.GetValue().DataRegisterIndex == 0U,
                   firstResult.GetValue().HasInstructionRegister, firstResult.GetValue().InstructionRegisterIndex == 1U,
                   firstResult.GetValue().Translation.FaultingAddress == Base);
    ASTERIA_EXPECT(runner, "the second aligned code extent consumes the next deterministic registers",
                   secondResult.HasValue(), secondResult.GetValue().MappingPageIndex == 16U,
                   secondResult.GetValue().PageShift == 16U, secondResult.GetValue().DataRegisterIndex == 1U,
                   secondResult.GetValue().InstructionRegisterIndex == 2U);
    ASTERIA_EXPECT(runner, "non-executable stack receives only a 4 KiB data translation", stackResult.HasValue(),
                   stackResult.GetValue().MappingIndex == 1U, stackResult.GetValue().PageShift == 12U,
                   !stackResult.GetValue().HasInstructionRegister,
                   stackResult.GetValue().InstructionRegisterIndex == 0U);
    ASTERIA_EXPECT(runner, "eight handoff pages greedily become two aligned 16 KiB translations", lastResult.HasValue(),
                   lastResult.GetValue().MappingIndex == 3U, lastResult.GetValue().MappingPageIndex == 4U,
                   lastResult.GetValue().PageShift == 14U, lastResult.GetValue().DataRegisterIndex == 5U);
    ASTERIA_EXPECT(runner, "entry lookup rejects the exact built extent", !g_output.GetEntry(6U).HasValue(),
                   g_output.GetEntry(6U).GetError() == InitialTranslationResourcePlanError::IndexOutOfBounds);

    bool exactRegionRegisters{true};
    for (Foundation::Size index{0U}; index < Platform::Ia64::Memory::InitialTranslationRegionCount; ++index)
    {
        const auto regionRegister = g_output.GetRegionRegister(index);
        exactRegionRegisters =
            exactRegionRegisters && regionRegister.HasValue() &&
            (regionRegister.GetValue() == ((static_cast<Foundation::UInt64>(0x2000U + index) << 8U) | (12U << 2U)));
    }
    ASTERIA_EXPECT(runner, "the sealed plan owns all eight exact RR words and bounds their lookup",
                   exactRegionRegisters,
                   !g_output.GetRegionRegister(Platform::Ia64::Memory::InitialTranslationRegionCount).HasValue(),
                   !g_failureOutput.GetRegionRegister(0U).HasValue());

    const auto deterministic =
        InitialTranslationResourcePlanner::Build(g_plan, capabilities, firmware, regions, g_deterministicOutput);
    bool identical = deterministic.HasValue();
    const auto* const firstBytes = reinterpret_cast<const Foundation::UInt8*>(&g_output);
    const auto* const secondBytes = reinterpret_cast<const Foundation::UInt8*>(&g_deterministicOutput);
    for (Foundation::Size index{0U}; index < OutputBytes; ++index)
    {
        identical = identical && (firstBytes[index] == secondBytes[index]);
    }
    ASTERIA_EXPECT(runner, "identical inputs produce byte-identical resource plans", identical);

    for (Foundation::Size index{0U}; index < OutputBytes; ++index)
    {
        g_patternedOutputStorageA[index] = 0xAAU;
        g_patternedOutputStorageB[index] = 0x55U;
    }
    auto* const patternedOutputA = ::new (static_cast<void*>(g_patternedOutputStorageA)) InitialTranslationResourcePlan;
    auto* const patternedOutputB = ::new (static_cast<void*>(g_patternedOutputStorageB)) InitialTranslationResourcePlan;
    const auto patternedResultA =
        InitialTranslationResourcePlanner::Build(g_plan, capabilities, firmware, regions, *patternedOutputA);
    const auto patternedResultB =
        InitialTranslationResourcePlanner::Build(g_plan, capabilities, firmware, regions, *patternedOutputB);
    bool canonical = patternedResultA.HasValue() && patternedResultB.HasValue();
    for (Foundation::Size index{0U}; index < OutputBytes; ++index)
    {
        canonical = canonical && (g_patternedOutputStorageA[index] == g_patternedOutputStorageB[index]);
    }
    ASTERIA_EXPECT(runner, "successful plans have one canonical representation in patterned storage", canonical);

    const auto rebuilt = InitialTranslationResourcePlanner::Build(g_plan, capabilities, firmware, regions, g_output);
    ASTERIA_EXPECT(runner, "a built immutable output cannot be replaced", !rebuilt.HasValue(),
                   rebuilt.GetError().Error == InitialTranslationResourcePlanError::OutputAlreadyBuilt);
}

[[nodiscard]] bool PrepareFirmwareIdentityPlan(InitialMappingPlan& plan, const Foundation::UInt8 virtualRegion) noexcept
{
    const Foundation::UInt64 regionBase = static_cast<Foundation::UInt64>(virtualRegion) << 61U;
    return Add(plan, MakeMapping(regionBase + 0x0010'0000U, 0x0200'0000U, 1U, ReadExecute(),
                                 InitialMappingOwner::NucleusCode)) &&
           Add(plan, MakeMapping(regionBase + 0x0050'0000U, 0x0201'0000U, 1U, ReadWrite(),
                                 InitialMappingOwner::NucleusStack)) &&
           Add(plan,
               MakeMapping(regionBase + 0x0050'1000U, 0x0201'1000U, 1U, ReadWrite(), InitialMappingOwner::PageTable)) &&
           (plan.Seal() == InitialMappingPlanError::None);
}

[[nodiscard]] bool PrepareRegionIdentifierConflictPlan() noexcept
{
    return Add(g_regionIdentifierConflictPlan,
               MakeMapping(0x0080'0000U, 0x0300'0000U, 1U, ReadExecute(), InitialMappingOwner::NucleusCode)) &&
           Add(g_regionIdentifierConflictPlan, MakeMapping(0x2000'0000'0080'0000ULL, 0x0301'0000U, 1U, ReadExecute(),
                                                           InitialMappingOwner::NucleusCode)) &&
           Add(g_regionIdentifierConflictPlan,
               MakeMapping(0x0090'0000U, 0x0302'0000U, 1U, ReadWrite(), InitialMappingOwner::NucleusStack)) &&
           Add(g_regionIdentifierConflictPlan,
               MakeMapping(0x0090'1000U, 0x0302'1000U, 1U, ReadWrite(), InitialMappingOwner::PageTable)) &&
           (g_regionIdentifierConflictPlan.Seal() == InitialMappingPlanError::None);
}

[[nodiscard]] bool PrepareCrossRegionFirmwareOverlapPlan() noexcept
{
    return Add(g_crossRegionFirmwareOverlapPlan, MakeMapping(0x2000'0000'0010'0000ULL, 0x0400'0000U, 1U, ReadExecute(),
                                                             InitialMappingOwner::NucleusCode)) &&
           Add(g_crossRegionFirmwareOverlapPlan, MakeMapping(0xC000'0000'0050'0000ULL, 0x0401'0000U, 1U, ReadWrite(),
                                                             InitialMappingOwner::NucleusStack)) &&
           Add(g_crossRegionFirmwareOverlapPlan,
               MakeMapping(0xC000'0000'0050'1000ULL, 0x0401'1000U, 1U, ReadWrite(), InitialMappingOwner::PageTable)) &&
           (g_crossRegionFirmwareOverlapPlan.Seal() == InitialMappingPlanError::None);
}

void TestInputFailures(TestContext& runner) noexcept
{
    const auto* const outputBytes = reinterpret_cast<const Foundation::UInt8*>(&g_failureOutput);
    for (Foundation::Size index{0U}; index < OutputBytes; ++index)
    {
        g_outputSnapshot[index] = outputBytes[index];
    }

    PalVirtualMemoryCapabilities capabilities = FullCapabilities();
    SalSystemTableInformation firmware = FirmwareInformation();
    InitialTranslationRegionSet regions = PrivateRegions();
    auto result =
        InitialTranslationResourcePlanner::Build(g_unsealedPlan, capabilities, firmware, regions, g_failureOutput);
    ASTERIA_EXPECT(runner, "an unsealed common plan is rejected", !result.HasValue(),
                   result.GetError().Error == InitialTranslationResourcePlanError::PlanNotSealed);

    capabilities.Reserved32 = 1U;
    result = InitialTranslationResourcePlanner::Build(g_plan, capabilities, firmware, regions, g_failureOutput);
    ASTERIA_EXPECT(runner, "forged PAL reserved state is rejected", !result.HasValue(),
                   result.GetError().Error == InitialTranslationResourcePlanError::InvalidCapabilities);

    capabilities = FullCapabilities();
    firmware.HasTranslationRegister = false;
    result = InitialTranslationResourcePlanner::Build(g_plan, capabilities, firmware, regions, g_failureOutput);
    ASTERIA_EXPECT(runner, "the handoff must describe firmware-owned ITR zero", !result.HasValue(),
                   result.GetError().Error ==
                       InitialTranslationResourcePlanError::MissingFirmwareInstructionTranslationRegister);

    firmware = FirmwareInformation();
    firmware.TranslationRegisterEncodedPageSize |= 1U;
    result = InitialTranslationResourcePlanner::Build(g_plan, capabilities, firmware, regions, g_failureOutput);
    ASTERIA_EXPECT(runner, "malformed firmware ITIR reserved bits are rejected", !result.HasValue(),
                   result.GetError().Error ==
                       InitialTranslationResourcePlanError::InvalidFirmwareInstructionTranslationRegister);

    firmware = FirmwareInformation();
    firmware.TranslationRegisterVirtualAddress = 0x0040'0000'0000'0000ULL;
    capabilities.Translation.ImplementedVirtualAddressMostSignificantBit = 53U;
    result = InitialTranslationResourcePlanner::Build(g_plan, capabilities, firmware, regions, g_failureOutput);
    ASTERIA_EXPECT(runner, "firmware ITR coverage must fit the current processor's implemented VA", !result.HasValue(),
                   result.GetError().Error ==
                       InitialTranslationResourcePlanError::UnimplementedFirmwareTranslationAddress);

    capabilities = FullCapabilities();
    bool unsupportedFirmwareRegionsRejected{true};
    for (Foundation::UInt8 virtualRegion{4U}; virtualRegion < 8U; ++virtualRegion)
    {
        firmware = FirmwareInformation();
        firmware.TranslationRegisterVirtualAddress = static_cast<Foundation::UInt64>(virtualRegion) << 61U;
        result = InitialTranslationResourcePlanner::Build(g_plan, capabilities, firmware, regions, g_failureOutput);
        unsupportedFirmwareRegionsRejected =
            unsupportedFirmwareRegionsRejected && !result.HasValue() &&
            (result.GetError().Error ==
             InitialTranslationResourcePlanError::InvalidFirmwareInstructionTranslationRegister);
    }
    ASTERIA_EXPECT(runner, "firmware ITR identity is limited to SAL virtual regions zero through three",
                   unsupportedFirmwareRegionsRejected);

    bool firmwareIdentityFixturesReady{true};
    for (Foundation::UInt8 virtualRegion{0U}; virtualRegion < 4U; ++virtualRegion)
    {
        firmwareIdentityFixturesReady =
            PrepareFirmwareIdentityPlan(g_firmwareIdentityPlans[virtualRegion], virtualRegion) &&
            firmwareIdentityFixturesReady;
    }
    ASTERIA_EXPECT(runner, "firmware-identity overlap fixtures seal", firmwareIdentityFixturesReady);

    bool firmwareRegionIdentitiesDerived{true};
    for (Foundation::UInt8 virtualRegion{0U}; virtualRegion < 4U; ++virtualRegion)
    {
        firmware = FirmwareInformation();
        firmware.TranslationRegisterVirtualAddress = static_cast<Foundation::UInt64>(virtualRegion) << 61U;
        InitialTranslationRegionSet matchingIdentity = PrivateRegions();
        matchingIdentity.Regions[virtualRegion].RegionIdentifier =
            static_cast<Foundation::UInt32>(0x1000U + virtualRegion);
        result = InitialTranslationResourcePlanner::Build(g_firmwareIdentityPlans[virtualRegion], capabilities,
                                                          firmware, matchingIdentity, g_failureOutput);
        firmwareRegionIdentitiesDerived =
            firmwareRegionIdentitiesDerived && !result.HasValue() &&
            (result.GetError().Error ==
             InitialTranslationResourcePlanError::FirmwareInstructionTranslationRegisterOverlap) &&
            (result.GetError().MappingIndex == 0U) && (result.GetError().MappingPageIndex == 0U);
    }
    ASTERIA_EXPECT(runner, "SAL virtual regions zero through three derive RIDs 0x1000 through 0x1003",
                   firmwareRegionIdentitiesDerived);

    firmware = FirmwareInformation();
    InitialTranslationResourcePlan& differentIdentityOutput = FreshTransientOutput();
    result = InitialTranslationResourcePlanner::Build(g_firmwareIdentityPlans[0U], capabilities, firmware,
                                                      PrivateRegions(), differentIdentityOutput);
    ASTERIA_EXPECT(runner, "equal low 61-bit intervals remain distinct when their RIDs differ", result.HasValue(),
                   differentIdentityOutput.IsBuilt());

    InitialTranslationRegionSet salIdentity = PrivateRegions();
    salIdentity.Regions[0U].RegionIdentifier = 0x1000U;
    bool adjacentIntervalsRemainDistinct{true};
    constexpr Foundation::UInt64 adjacentFirmwareStarts[]{
        0x000F'F000U,
        0x0010'1000U,
    };
    for (const Foundation::UInt64 firmwareStart : adjacentFirmwareStarts)
    {
        firmware = FirmwareInformation();
        firmware.TranslationRegisterVirtualAddress = firmwareStart;
        firmware.TranslationRegisterEncodedPageSize = 12U << 2U;
        result = InitialTranslationResourcePlanner::Build(g_firmwareIdentityPlans[0U], capabilities, firmware,
                                                          salIdentity, g_failureOutput);
        adjacentIntervalsRemainDistinct =
            adjacentIntervalsRemainDistinct && !result.HasValue() &&
            (result.GetError().Error == InitialTranslationResourcePlanError::ActiveRegionIdentifierReservedForFirmware);
    }
    ASTERIA_EXPECT(runner, "firmware-ITR overlap excludes exactly adjacent low 61-bit intervals",
                   adjacentIntervalsRemainDistinct);

    bool salRegionIdentifiersRejected{true};
    firmware = FirmwareInformation();
    firmware.TranslationRegisterVirtualAddress = 0x0040'0000U;
    for (Foundation::UInt32 regionIdentifier{0x1000U}; regionIdentifier <= 0x1003U; ++regionIdentifier)
    {
        InitialTranslationRegionSet reservedIdentity = PrivateRegions();
        reservedIdentity.Regions[0U].RegionIdentifier = regionIdentifier;
        result = InitialTranslationResourcePlanner::Build(g_firmwareIdentityPlans[0U], capabilities, firmware,
                                                          reservedIdentity, g_failureOutput);
        salRegionIdentifiersRejected =
            salRegionIdentifiersRejected && !result.HasValue() &&
            (result.GetError().Error ==
             InitialTranslationResourcePlanError::ActiveRegionIdentifierReservedForFirmware) &&
            (result.GetError().MappingIndex == 0U) && (result.GetError().MappingPageIndex == 0U);
    }
    ASTERIA_EXPECT(runner, "active native regions cannot reuse SAL handoff RIDs 0x1000 through 0x1003",
                   salRegionIdentifiersRejected);

    capabilities = FullCapabilities();
    capabilities.Translation.InsertablePageSizeMask |= Foundation::UInt64{1U} << 60U;
    capabilities.PurgePageSizeMask |= Foundation::UInt64{1U} << 60U;
    firmware = FirmwareInformation();
    firmware.TranslationRegisterVirtualAddress = Foundation::UInt64{1U} << 60U;
    firmware.TranslationRegisterEncodedPageSize = 60U << 2U;
    InitialTranslationResourcePlan& maximumFirmwareRangeOutput = FreshTransientOutput();
    result = InitialTranslationResourcePlanner::Build(g_plan, capabilities, firmware, PrivateRegions(),
                                                      maximumFirmwareRangeOutput);
    ASTERIA_EXPECT(runner, "a page-shift-60 firmware range may end at the low 61-bit boundary", result.HasValue(),
                   maximumFirmwareRangeOutput.GetFirmwareTranslationVirtualAddress() == (Foundation::UInt64{1U} << 60U),
                   maximumFirmwareRangeOutput.GetFirmwareTranslationEncodedPageSize() == (60U << 2U));

    ASTERIA_EXPECT(runner, "cross-region firmware-ITR overlap fixture seals", PrepareCrossRegionFirmwareOverlapPlan());
    firmware = FirmwareInformation();
    InitialTranslationRegionSet crossRegionFirmwareIdentity = PrivateRegions();
    crossRegionFirmwareIdentity.Regions[1U].RegionIdentifier = 0x1000U;
    result = InitialTranslationResourcePlanner::Build(g_crossRegionFirmwareOverlapPlan, FullCapabilities(), firmware,
                                                      crossRegionFirmwareIdentity, g_failureOutput);
    ASTERIA_EXPECT(runner, "firmware ITR overlap is detected by RID and low 61 address bits", !result.HasValue(),
                   result.GetError().Error ==
                       InitialTranslationResourcePlanError::FirmwareInstructionTranslationRegisterOverlap,
                   result.GetError().MappingIndex == 0U, result.GetError().MappingPageIndex == 0U);

    ASTERIA_EXPECT(runner, "cross-region RID-conflict fixture seals", PrepareRegionIdentifierConflictPlan());
    InitialTranslationRegionSet conflictingRegions = PrivateRegions();
    conflictingRegions.Regions[1U].RegionIdentifier = conflictingRegions.Regions[0U].RegionIdentifier;
    result = InitialTranslationResourcePlanner::Build(g_regionIdentifierConflictPlan, capabilities, firmware,
                                                      conflictingRegions, g_failureOutput);
    ASTERIA_EXPECT(runner, "active virtual regions cannot alias one native RID", !result.HasValue(),
                   result.GetError().Error == InitialTranslationResourcePlanError::ActiveRegionIdentifierConflict);

    capabilities = FullCapabilities();
    regions = PrivateRegions();
    regions.Regions[6U].ProtectionKey = 1U;
    result =
        InitialTranslationResourcePlanner::Build(g_plan, capabilities, FirmwareInformation(), regions, g_failureOutput);
    ASTERIA_EXPECT(runner, "bootstrap region policy rejects an inactive nonzero protection key", !result.HasValue(),
                   result.GetError().Error == InitialTranslationResourcePlanError::InvalidRegionConfiguration);

    regions = PrivateRegions();
    regions.Regions[7U].ProtectionKey = 0x00FF'FFFFU;
    result =
        InitialTranslationResourcePlanner::Build(g_plan, capabilities, FirmwareInformation(), regions, g_failureOutput);
    ASTERIA_EXPECT(runner, "bootstrap region policy rejects the maximum supported protection key", !result.HasValue(),
                   result.GetError().Error == InitialTranslationResourcePlanError::InvalidRegionConfiguration);

    regions = PrivateRegions();
    regions.Regions[6U].VhptWalkerEnabled = true;
    result =
        InitialTranslationResourcePlanner::Build(g_plan, capabilities, FirmwareInformation(), regions, g_failureOutput);
    ASTERIA_EXPECT(runner, "bootstrap region policy rejects a hardware-supported VHPT walker", !result.HasValue(),
                   result.GetError().Error == InitialTranslationResourcePlanError::InvalidRegionConfiguration);

    regions = PrivateRegions();
    regions.Regions[6U].Reserved16 = 1U;
    result =
        InitialTranslationResourcePlanner::Build(g_plan, capabilities, FirmwareInformation(), regions, g_failureOutput);
    ASTERIA_EXPECT(runner, "inactive native region configuration is validated before RR sealing", !result.HasValue(),
                   result.GetError().Error == InitialTranslationResourcePlanError::InvalidRegionConfiguration);

    regions = PrivateRegions();
    regions.Regions[6U].RegionIdentifier = regions.Regions[5U].RegionIdentifier;
    result =
        InitialTranslationResourcePlanner::Build(g_plan, capabilities, FirmwareInformation(), regions, g_failureOutput);
    ASTERIA_EXPECT(runner, "inactive native regions cannot seal duplicate RIDs", !result.HasValue(),
                   result.GetError().Error == InitialTranslationResourcePlanError::InvalidRegionConfiguration);

    regions = PrivateRegions();
    regions.Regions[7U].Reserved16 = 1U;
    result =
        InitialTranslationResourcePlanner::Build(g_plan, capabilities, FirmwareInformation(), regions, g_failureOutput);
    ASTERIA_EXPECT(runner, "native encoding failure preserves its exact mapping and typed cause", !result.HasValue(),
                   result.GetError().Error == InitialTranslationResourcePlanError::TranslationEncodingFailed,
                   result.GetError().EncodingError == InitialTranslationEncodingError::InvalidRegionConfiguration,
                   result.GetError().MappingIndex == 0U, result.GetError().MappingPageIndex == 0U);

    bool unchanged = !g_failureOutput.IsBuilt() && (g_failureOutput.GetEntryCount() == 0U) &&
                     !g_failureOutput.GetEntry(0U).HasValue();
    for (Foundation::Size index{0U}; index < OutputBytes; ++index)
    {
        unchanged = unchanged && (outputBytes[index] == g_outputSnapshot[index]);
    }
    ASTERIA_EXPECT(runner, "every failed build leaves the complete output byte-unchanged", unchanged);
}

[[nodiscard]] bool PrepareCapacityPlan(InitialMappingPlan& plan, const bool executable) noexcept
{
    bool ready{true};
    for (Foundation::UInt64 index{0U}; index < 9U; ++index)
    {
        const InitialMappingOwner owner =
            executable ? InitialMappingOwner::NucleusCode
                       : (index == 0U ? InitialMappingOwner::NucleusCode : InitialMappingOwner::BootHandoff);
        const MappingPermissions permissions = (executable || (index == 0U)) ? ReadExecute() : ReadOnly();
        ready = ready && Add(plan, MakeMapping(Base + (index * 0x2000U), PhysicalBase + (index * 0x2000U), 1U,
                                               permissions, owner));
    }
    ready = ready && Add(plan, MakeMapping(Base + 0x20'000U, PhysicalBase + 0x20'000U, 1U, ReadWrite(),
                                           InitialMappingOwner::NucleusStack));
    ready = ready && Add(plan, MakeMapping(Base + 0x22'000U, PhysicalBase + 0x22'000U, 1U, ReadWrite(),
                                           InitialMappingOwner::PageTable));
    return ready && (plan.Seal() == InitialMappingPlanError::None);
}

void TestRegisterCapacity(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "data-register capacity fixture seals", PrepareCapacityPlan(g_dataCapacityPlan, false));
    PalVirtualMemoryCapabilities capabilities = FullCapabilities();
    capabilities.InstructionTranslationRegisterCount = 32U;
    auto result = InitialTranslationResourcePlanner::Build(g_dataCapacityPlan, capabilities, FirmwareInformation(),
                                                           PrivateRegions(), g_dataCapacityOutput);
    ASTERIA_EXPECT(runner, "every planned range requires an available data translation register", !result.HasValue(),
                   result.GetError().Error ==
                       InitialTranslationResourcePlanError::DataTranslationRegisterCapacityExceeded,
                   result.GetError().MappingIndex == 8U, result.GetError().MappingPageIndex == 0U);

    ASTERIA_EXPECT(runner, "instruction-register capacity fixture seals",
                   PrepareCapacityPlan(g_instructionCapacityPlan, true));
    capabilities = FullCapabilities();
    capabilities.DataTranslationRegisterCount = 32U;
    result = InitialTranslationResourcePlanner::Build(g_instructionCapacityPlan, capabilities, FirmwareInformation(),
                                                      PrivateRegions(), g_instructionCapacityOutput);
    ASTERIA_EXPECT(
        runner, "firmware ITR zero leaves exactly seven minimum-profile instruction registers", !result.HasValue(),
        result.GetError().Error == InitialTranslationResourcePlanError::InstructionTranslationRegisterCapacityExceeded,
        result.GetError().MappingIndex == 7U, result.GetError().MappingPageIndex == 0U);
}

/// The DTR and ITR counts of the hosted fixtures' boot geometry with its IVT
/// at table and its arena of arenaBytes at arena; zero counts when the
/// geometry, the mapping plan or the translation plan is rejected.
[[nodiscard]] Platform::Ia64::Memory::InitialTranslationResourcePlanSummary
PlanHostedGeometry(const Foundation::UInt64 table, const Foundation::UInt64 arena,
                   const Foundation::UInt64 arenaBytes) noexcept
{
    const PhysicalPageRange reserved{
        .PhysicalStart = PhysicalBase,
        .PageCount = BootstrapReservedPageCount,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Reserved = 0U,
    };
    TestSupport::Ia64HostedBootGeometry geometry{};
    if (!TestSupport::MakeIa64HostedBootGeometry(table, arena, arenaBytes, reserved, geometry))
    {
        return {};
    }
    g_hostedBootInfo = BootInfo{
        .Signature = Boot::BootInfoSignature,
        .Version = Boot::BootInfoVersion,
        .SizeBytes = sizeof(BootInfo),
        .MemoryRegionCount = 3U,
        .FirmwareDescriptorVersion = 1U,
        .FirmwareDescriptorSize = 40U,
        .Reserved = 0U,
        .FirmwareMemoryMapSizeBytes = 0U,
        .MemoryRegions = {},
        .Acpi = {},
        .Entropy = {},
        .Display = {},
        .Inventory = {},
        .Files = {},
        .Crc32c = 0U,
        .ReservedAbi = 0U,
        .Kernel = {},
        .InventoryRecord = {},
        .DebugPort = {},
        .Trust = {},
        .DmaExclusions = {},
        .LowMemoryBlocks = 0U,
        .WatchdogSeconds = Asteria::Abi::WatchdogDefaultSeconds,
        .Power = {},
        .SessionMode = 0U,
        .LoginEnabled = 0U,
    };
    g_hostedBootInfo.MemoryRegions[0U] = PhysicalMemoryRegion{
        .PhysicalStart = PhysicalBase,
        .VirtualStart = 0U,
        .PageCount = BootstrapReservedPageCount,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Kind = PhysicalMemoryKind::Conventional,
    };
    g_hostedBootInfo.MemoryRegions[1U] = geometry.ImageRegion;
    g_hostedBootInfo.MemoryRegions[2U] = geometry.RuntimeMemoryRegion;
    g_hostedMappingPlan.~InitialMappingPlan();
    new (&g_hostedMappingPlan) InitialMappingPlan{};
    g_hostedTranslationPlan.~InitialTranslationResourcePlan();
    new (&g_hostedTranslationPlan) InitialTranslationResourcePlan{};
    if (!TestSupport::SortIa64HostedBootRegions(g_hostedBootInfo) ||
        !BootstrapMappingPlanBuilder::Build(geometry.Image, g_hostedBootInfo, reserved, geometry.RuntimeMemoryPages,
                                            g_hostedMappingPlan)
             .HasValue())
    {
        return {};
    }
    PalVirtualMemoryCapabilities capabilities = FullCapabilities();
    capabilities.InstructionTranslationRegisterCount = 32U;
    capabilities.DataTranslationRegisterCount = 32U;
    const auto translations = InitialTranslationResourcePlanner::Build(
        g_hostedMappingPlan, capabilities, FirmwareInformation(), FinalImageRegions(), g_hostedTranslationPlan);
    return translations.HasValue() ? translations.GetValue()
                                   : Platform::Ia64::Memory::InitialTranslationResourcePlanSummary{};
}

// The hosted fixtures' geometry (Tests/Support/Ia64HostedBootGeometry.hpp)
// needs the same registers wherever the host places the IVT (any 32 KiB
// residue of a 16 MiB period) and the arena (a page of its size before or
// after the image), for arenas that take 1, 4 and 16 MiB pages.
void TestHostedFixtureGeometry(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 HostBase{0x0000'5555'0000'0000U};
    constexpr Foundation::UInt64 Period{16U * 1024U * 1024U};
    constexpr Foundation::UInt64 TableAlignment{32U * 1024U};
    constexpr Foundation::UInt64 ArenaBytes[]{0xED000U, 0x2C7000U, 0x986000U};
    Foundation::Size placements{0U};
    Foundation::Size constantPlacements{0U};
    for (const Foundation::UInt64 arenaBytes : ArenaBytes)
    {
        const Foundation::UInt64 page = TestSupport::Ia64HostedRuntimeMemoryPageBytes(arenaBytes);
        for (Foundation::UInt64 residue{0U}; residue < Period; residue += TableAlignment)
        {
            const Foundation::UInt64 table = HostBase + Period + residue;
            const Foundation::UInt64 after = (table + Period + page - 1U) & ~(page - 1U);
            const Foundation::UInt64 before = ((table - page) & ~(page - 1U)) - page;
            const Foundation::UInt64 arenas[2U]{after, before};
            for (const Foundation::UInt64 arena : arenas)
            {
                const auto summary = PlanHostedGeometry(table, arena, arenaBytes);
                ++placements;
                if ((summary.DataRegisterCount == TestSupport::Ia64HostedDataTranslationRegisters) &&
                    (summary.InstructionRegisterCount == TestSupport::Ia64HostedInstructionTranslationRegisters))
                {
                    ++constantPlacements;
                }
            }
        }
    }
    const auto misalignedTable = PlanHostedGeometry(HostBase + 0x4000U, HostBase + Period, 0xED000U);
    const auto misalignedArena = PlanHostedGeometry(HostBase, HostBase + Period + 0x8'0000U, 0xED000U);
    const auto overlappingArena = PlanHostedGeometry(HostBase + Period, HostBase + Period, 0xED000U);
    ASTERIA_EXPECT(runner,
                   "the hosted fixture geometry needs 14 DTRs and 5 ITRs wherever the host places its IVT and arena",
                   placements == (3U * 2U * (Period / TableAlignment)), constantPlacements == placements,
                   misalignedTable.DataRegisterCount == 0U, misalignedArena.DataRegisterCount == 0U,
                   overlappingArena.DataRegisterCount == 0U);
}
} // namespace
} // namespace Asteria::Test

int main(const int argumentCount, char* const arguments[])
{
    if (argumentCount != 1)
    {
        return Asteria::Test::RunFinalImageResidueProof(argumentCount, arguments);
    }
    TestContext runner{"InitialTranslationResourcePlanTests"};
    Asteria::Test::TestConsolidatedArchitectedPlan(runner);
    Asteria::Test::TestExactPlan(runner);
    Asteria::Test::TestInputFailures(runner);
    Asteria::Test::TestRegisterCapacity(runner);
    Asteria::Test::TestHostedFixtureGeometry(runner);
    return runner.Finish();
}
