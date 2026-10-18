// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/InitialTranslationResourcePlan.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
using Kernel::Memory::InitialMapping;
using Kernel::Memory::MappingPermission;

constexpr Foundation::UInt8 MinimumPageShift{12U};
constexpr Foundation::UInt8 MaximumPageShift{60U};
constexpr Foundation::UInt16 MaximumRegisterCount{256U};
constexpr Foundation::UInt8 RequiredMemoryAttributeMask{(Foundation::UInt8{1U} << 0U) | (Foundation::UInt8{1U} << 4U)};
constexpr Foundation::UInt8 ArchitectedMemoryAttributeMask{RequiredMemoryAttributeMask | (Foundation::UInt8{1U} << 5U) |
                                                           (Foundation::UInt8{1U} << 6U) |
                                                           (Foundation::UInt8{1U} << 7U)};
constexpr Foundation::UInt64 BelowMinimumPageSizeMask{(Foundation::UInt64{1U} << MinimumPageShift) - 1U};
constexpr Foundation::UInt64 VirtualRegionOffsetMask{(Foundation::UInt64{1U} << 61U) - 1U};
constexpr Foundation::UInt32 SalRegionIdentifierBase{0x1000U};
constexpr Foundation::UInt8 MaximumSalVirtualRegion{3U};
constexpr Foundation::UInt32 MaximumSalRegionIdentifier{SalRegionIdentifierBase +
                                                        static_cast<Foundation::UInt32>(MaximumSalVirtualRegion)};
constexpr Foundation::Size NoMappingIndex{~Foundation::Size{0U}};

[[nodiscard]] bool FitsUnsignedBits(const Foundation::UInt32 value, const Foundation::UInt8 bitCount) noexcept
{
    return static_cast<Foundation::UInt64>(value) < (Foundation::UInt64{1U} << bitCount);
}

[[nodiscard]] InitialTranslationResourcePlanFailure
Failure(const InitialTranslationResourcePlanError error,
        const InitialTranslationEncodingError encodingError = InitialTranslationEncodingError::None,
        const Foundation::Size mappingIndex = NoMappingIndex, const Foundation::UInt64 mappingPageIndex = 0U) noexcept
{
    return InitialTranslationResourcePlanFailure{
        .Error = error,
        .EncodingError = encodingError,
        .MappingIndex = mappingIndex,
        .MappingPageIndex = mappingPageIndex,
    };
}

[[nodiscard]] bool IsTranslationCapabilitySetValid(const TranslationCapabilities& capabilities) noexcept
{
    return (capabilities.PhysicalAddressBits >= 32U) && (capabilities.PhysicalAddressBits <= 63U) &&
           (capabilities.ImplementedVirtualAddressMostSignificantBit >= 50U) &&
           (capabilities.ImplementedVirtualAddressMostSignificantBit <= 60U) &&
           (capabilities.RegionIdentifierBits >= 18U) && (capabilities.RegionIdentifierBits <= 24U) &&
           (capabilities.ProtectionKeyBits >= capabilities.RegionIdentifierBits) &&
           (capabilities.ProtectionKeyBits <= 24U) &&
           ((capabilities.MemoryAttributeMask & static_cast<Foundation::UInt8>(~ArchitectedMemoryAttributeMask)) ==
            0U) &&
           ((capabilities.MemoryAttributeMask & RequiredMemoryAttributeMask) == RequiredMemoryAttributeMask) &&
           (capabilities.Reserved16 == 0U) && (capabilities.Reserved64 == 0U);
}

[[nodiscard]] bool AreCapabilitiesValid(const PalVirtualMemoryCapabilities& capabilities) noexcept
{
    const Foundation::UInt16 maximumUniqueTranslationCaches =
        static_cast<Foundation::UInt16>(capabilities.TranslationCacheLevelCount) * 2U;
    return IsTranslationCapabilitySetValid(capabilities.Translation) &&
           (capabilities.InstructionTranslationRegisterCount >= 8U) &&
           (capabilities.InstructionTranslationRegisterCount <= MaximumRegisterCount) &&
           (capabilities.DataTranslationRegisterCount >= 8U) &&
           (capabilities.DataTranslationRegisterCount <= MaximumRegisterCount) &&
           (capabilities.ProtectionKeyRegisterCount >= 16U) &&
           (capabilities.ProtectionKeyRegisterCount <= MaximumRegisterCount) &&
           (capabilities.TranslationCacheLevelCount != 0U) && (capabilities.UniqueTranslationCacheCount != 0U) &&
           (static_cast<Foundation::UInt16>(capabilities.UniqueTranslationCacheCount) <=
            maximumUniqueTranslationCaches) &&
           ((capabilities.Translation.InsertablePageSizeMask & ArchitectedMinimumInsertablePageSizeMask) ==
            ArchitectedMinimumInsertablePageSizeMask) &&
           ((capabilities.PurgePageSizeMask & ArchitectedMinimumPurgePageSizeMask) ==
            ArchitectedMinimumPurgePageSizeMask) &&
           (((capabilities.Translation.InsertablePageSizeMask | capabilities.PurgePageSizeMask) &
             BelowMinimumPageSizeMask) == 0U) &&
           ((capabilities.Translation.InsertablePageSizeMask & ~capabilities.PurgePageSizeMask) == 0U) &&
           (capabilities.Reserved8 == 0U) && (capabilities.Reserved32 == 0U);
}

[[nodiscard]] bool ValidateRegionSet(const TranslationCapabilities& capabilities,
                                     const InitialTranslationRegionSet& regions,
                                     Foundation::UInt64 (&regionRegisters)[InitialTranslationRegionCount]) noexcept
{
    for (Foundation::Size index{0U}; index < InitialTranslationRegionCount; ++index)
    {
        const RegionConfiguration region = regions.Regions[index];
        if ((region.Reserved16 != 0U) || (region.Reserved32 != 0U) || (region.ProtectionKey != 0U) ||
            region.VhptWalkerEnabled || !FitsUnsignedBits(region.RegionIdentifier, capabilities.RegionIdentifierBits) ||
            !FitsUnsignedBits(region.ProtectionKey, capabilities.ProtectionKeyBits) ||
            ((region.RegionIdentifier >= SalRegionIdentifierBase) &&
             (region.RegionIdentifier <= MaximumSalRegionIdentifier)))
        {
            return false;
        }
        for (Foundation::Size previous{0U}; previous < index; ++previous)
        {
            if (regions.Regions[previous].RegionIdentifier == region.RegionIdentifier)
            {
                return false;
            }
        }
        regionRegisters[index] = (static_cast<Foundation::UInt64>(region.RegionIdentifier) << 8U) |
                                 (static_cast<Foundation::UInt64>(MinimumPageShift) << 2U) |
                                 (region.VhptWalkerEnabled ? Foundation::UInt64{1U} : Foundation::UInt64{0U});
    }
    return true;
}

[[nodiscard]] bool IsVirtualAddressImplemented(const Foundation::UInt64 address,
                                               const Foundation::UInt8 implementedMostSignificantBit) noexcept
{
    if (implementedMostSignificantBit == 60U)
    {
        return true;
    }
    const Foundation::UInt8 unimplementedBitCount = static_cast<Foundation::UInt8>(60U - implementedMostSignificantBit);
    const Foundation::UInt64 mask = (Foundation::UInt64{1U} << unimplementedBitCount) - 1U;
    const Foundation::UInt64 actual = (address >> (implementedMostSignificantBit + 1U)) & mask;
    const Foundation::UInt64 expected =
        ((address & (Foundation::UInt64{1U} << implementedMostSignificantBit)) != 0U) ? mask : 0U;
    return actual == expected;
}

[[nodiscard]] bool IsVirtualRangeImplemented(const Foundation::UInt64 address, const Foundation::UInt8 pageShift,
                                             const Foundation::UInt8 implementedMostSignificantBit) noexcept
{
    if (pageShift > implementedMostSignificantBit)
    {
        return false;
    }
    const Foundation::UInt64 pageSize = Foundation::UInt64{1U} << pageShift;
    const Foundation::UInt64 maximumAddress = ~Foundation::UInt64{0U};
    if (address > (maximumAddress - (pageSize - 1U)))
    {
        return false;
    }
    const Foundation::UInt64 endAddress = address + pageSize - 1U;
    return ((address >> 61U) == (endAddress >> 61U)) &&
           IsVirtualAddressImplemented(address, implementedMostSignificantBit) &&
           IsVirtualAddressImplemented(endAddress, implementedMostSignificantBit);
}

[[nodiscard]] bool InclusiveRangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                          const Foundation::UInt64 secondStart,
                                          const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart <= secondEnd) && (secondStart <= firstEnd);
}

[[nodiscard]] bool IsFirmwareTranslationValid(const PalVirtualMemoryCapabilities& capabilities,
                                              const Firmware::SalSystemTableInformation& firmware,
                                              Foundation::UInt8& pageShift,
                                              Foundation::UInt32& regionIdentifier) noexcept
{
    if (!firmware.HasTranslationRegister || (firmware.Reserved8 != 0U) ||
        ((firmware.TranslationRegisterEncodedPageSize & ~Foundation::UInt64{0xFCU}) != 0U))
    {
        return false;
    }
    pageShift = static_cast<Foundation::UInt8>((firmware.TranslationRegisterEncodedPageSize >> 2U) & 0x3FU);
    if ((pageShift < MinimumPageShift) || (pageShift > MaximumPageShift))
    {
        return false;
    }
    const Foundation::UInt8 virtualRegion =
        static_cast<Foundation::UInt8>(firmware.TranslationRegisterVirtualAddress >> 61U);
    if (virtualRegion > MaximumSalVirtualRegion)
    {
        return false;
    }
    const Foundation::UInt64 pageSizeBit = Foundation::UInt64{1U} << pageShift;
    const Foundation::UInt64 pageSize = Foundation::UInt64{1U} << pageShift;
    if (((capabilities.Translation.InsertablePageSizeMask & pageSizeBit) == 0U) ||
        ((capabilities.PurgePageSizeMask & pageSizeBit) == 0U) ||
        ((firmware.TranslationRegisterVirtualAddress & (pageSize - 1U)) != 0U))
    {
        return false;
    }
    regionIdentifier = SalRegionIdentifierBase + static_cast<Foundation::UInt32>(virtualRegion);
    return true;
}

[[nodiscard]] Foundation::UInt8 SelectPageShift(const InitialMapping& mapping, const Foundation::UInt64 firstPageIndex,
                                                const TranslationCapabilities& capabilities) noexcept
{
    const Foundation::UInt64 remainingPages = mapping.Pages.GetValue() - firstPageIndex;
    const Foundation::UInt64 byteOffset = firstPageIndex << MinimumPageShift;
    const Foundation::UInt64 virtualAddress = mapping.VirtualStart.GetValue() + byteOffset;
    const Foundation::UInt64 physicalAddress = mapping.PhysicalStart.GetValue() + byteOffset;
    Foundation::UInt8 maximumEncodableShift = capabilities.ImplementedVirtualAddressMostSignificantBit;
    if (maximumEncodableShift > capabilities.PhysicalAddressBits)
    {
        maximumEncodableShift = capabilities.PhysicalAddressBits;
    }
    if (maximumEncodableShift > 50U)
    {
        maximumEncodableShift = 50U;
    }
    Foundation::UInt8 pageShift = maximumEncodableShift;
    while (true)
    {
        const Foundation::UInt64 pageSizeBit = Foundation::UInt64{1U} << pageShift;
        const Foundation::UInt64 coveredPages = Foundation::UInt64{1U} << (pageShift - MinimumPageShift);
        const Foundation::UInt64 pageSize = Foundation::UInt64{1U} << pageShift;
        if (((capabilities.InsertablePageSizeMask & pageSizeBit) != 0U) && (coveredPages <= remainingPages) &&
            ((virtualAddress & (pageSize - 1U)) == 0U) && ((physicalAddress & (pageSize - 1U)) == 0U))
        {
            return pageShift;
        }
        if (pageShift == MinimumPageShift)
        {
            return 0U;
        }
        --pageShift;
    }
}

[[nodiscard]] Foundation::Result<InitialTranslationResourcePlanSummary, InitialTranslationResourcePlanFailure>
Fail(const InitialTranslationResourcePlanFailure& failure) noexcept
{
    return Foundation::Result<InitialTranslationResourcePlanSummary, InitialTranslationResourcePlanFailure>::Failure(
        failure);
}
} // namespace

Foundation::Result<InitialTranslationResourceEntry, InitialTranslationResourcePlanError>
InitialTranslationResourcePlan::GetEntry(const Foundation::Size index) const noexcept
{
    if (!m_built || (index >= m_entryCount))
    {
        return Foundation::Result<InitialTranslationResourceEntry, InitialTranslationResourcePlanError>::Failure(
            InitialTranslationResourcePlanError::IndexOutOfBounds);
    }
    return Foundation::Result<InitialTranslationResourceEntry, InitialTranslationResourcePlanError>::Success(
        m_entries[index]);
}

Foundation::Size InitialTranslationResourcePlan::GetEntryCount() const noexcept
{
    return m_built ? m_entryCount : 0U;
}

Foundation::UInt16 InitialTranslationResourcePlan::GetDataRegisterCount() const noexcept
{
    return m_built ? m_dataRegisterCount : Foundation::UInt16{0U};
}

Foundation::UInt16 InitialTranslationResourcePlan::GetInstructionRegisterCount() const noexcept
{
    return m_built ? m_instructionRegisterCount : Foundation::UInt16{0U};
}

Foundation::UInt64 InitialTranslationResourcePlan::GetFirmwareTranslationVirtualAddress() const noexcept
{
    return m_built ? m_firmwareTranslationVirtualAddress : Foundation::UInt64{0U};
}

Foundation::UInt64 InitialTranslationResourcePlan::GetFirmwareTranslationEncodedPageSize() const noexcept
{
    return m_built ? m_firmwareTranslationEncodedPageSize : Foundation::UInt64{0U};
}

Foundation::Result<Foundation::UInt64, InitialTranslationResourcePlanError>
InitialTranslationResourcePlan::GetRegionRegister(const Foundation::Size regionIndex) const noexcept
{
    if (!m_built || (regionIndex >= InitialTranslationRegionCount))
    {
        return Foundation::Result<Foundation::UInt64, InitialTranslationResourcePlanError>::Failure(
            InitialTranslationResourcePlanError::IndexOutOfBounds);
    }
    return Foundation::Result<Foundation::UInt64, InitialTranslationResourcePlanError>::Success(
        m_regionRegisters[regionIndex]);
}

bool InitialTranslationResourcePlan::IsBuilt() const noexcept
{
    return m_built;
}

bool InitialTranslationResourcePlanner::ValidateAndMeasure(
    const Kernel::Memory::InitialMappingPlan& mappings, const PalVirtualMemoryCapabilities& capabilities,
    const InitialTranslationRegionSet& regions, const Foundation::UInt64 firmwareTranslationLowStart,
    const Foundation::UInt64 firmwareTranslationLowEnd, const Foundation::UInt32 firmwareRegionIdentifier,
    Foundation::Size& entryCount, Foundation::UInt16& instructionRegisterCount,
    InitialTranslationResourcePlanFailure& failure) noexcept
{
    entryCount = 0U;
    instructionRegisterCount = 1U;
    Foundation::UInt16 activeRegionMask{0U};
    for (Foundation::Size mappingIndex{0U}; mappingIndex < mappings.GetMappingCount(); ++mappingIndex)
    {
        const auto mappingResult = mappings.GetMapping(mappingIndex);
        if (!mappingResult.HasValue())
        {
            failure = Failure(InitialTranslationResourcePlanError::InternalInvariantViolation,
                              InitialTranslationEncodingError::InternalInvariantViolation, mappingIndex);
            return false;
        }
        const InitialMapping mapping = mappingResult.GetValue();
        const bool executable = mapping.Permissions.Contains(MappingPermission::Execute);
        Foundation::UInt64 firstPageIndex{0U};
        while (firstPageIndex < mapping.Pages.GetValue())
        {
            const Foundation::UInt8 pageShift = SelectPageShift(mapping, firstPageIndex, capabilities.Translation);
            if (pageShift == 0U)
            {
                failure = Failure(InitialTranslationResourcePlanError::InvalidCapabilities,
                                  InitialTranslationEncodingError::UnsupportedPageSize, mappingIndex, firstPageIndex);
                return false;
            }
            if ((entryCount >= MaximumInitialTranslationEntries) ||
                (entryCount >= capabilities.DataTranslationRegisterCount))
            {
                failure = Failure(InitialTranslationResourcePlanError::DataTranslationRegisterCapacityExceeded,
                                  InitialTranslationEncodingError::None, mappingIndex, firstPageIndex);
                return false;
            }
            if (executable)
            {
                if (instructionRegisterCount >= capabilities.InstructionTranslationRegisterCount)
                {
                    failure =
                        Failure(InitialTranslationResourcePlanError::InstructionTranslationRegisterCapacityExceeded,
                                InitialTranslationEncodingError::None, mappingIndex, firstPageIndex);
                    return false;
                }
                ++instructionRegisterCount;
            }
            const Foundation::UInt8 virtualRegion = static_cast<Foundation::UInt8>(
                (mapping.VirtualStart.GetValue() + (firstPageIndex << MinimumPageShift)) >> 61U);
            const auto encoding =
                InitialTranslationEncoder::EncodeRange(mappings, mappingIndex, firstPageIndex, pageShift,
                                                       capabilities.Translation, regions.Regions[virtualRegion]);
            if (!encoding.HasValue())
            {
                failure = Failure(InitialTranslationResourcePlanError::TranslationEncodingFailed, encoding.GetError(),
                                  mappingIndex, firstPageIndex);
                return false;
            }
            const EncodedInitialTranslation encoded = encoding.GetValue();
            const Foundation::UInt32 nativeRegionIdentifier =
                static_cast<Foundation::UInt32>(encoded.RegionRegister >> 8U);
            const Foundation::UInt64 nativeLowStart = encoded.FaultingAddress & VirtualRegionOffsetMask;
            const Foundation::UInt64 nativeLowEnd = nativeLowStart + (Foundation::UInt64{1U} << pageShift) - 1U;
            if (executable && (nativeRegionIdentifier == firmwareRegionIdentifier) &&
                InclusiveRangesOverlap(nativeLowStart, nativeLowEnd, firmwareTranslationLowStart,
                                       firmwareTranslationLowEnd))
            {
                failure = Failure(InitialTranslationResourcePlanError::FirmwareInstructionTranslationRegisterOverlap,
                                  InitialTranslationEncodingError::None, mappingIndex, firstPageIndex);
                return false;
            }
            if ((nativeRegionIdentifier >= SalRegionIdentifierBase) &&
                (nativeRegionIdentifier <= MaximumSalRegionIdentifier))
            {
                failure = Failure(InitialTranslationResourcePlanError::ActiveRegionIdentifierReservedForFirmware,
                                  InitialTranslationEncodingError::None, mappingIndex, firstPageIndex);
                return false;
            }
            const Foundation::UInt16 regionBit =
                static_cast<Foundation::UInt16>(Foundation::UInt16{1U} << virtualRegion);
            activeRegionMask = static_cast<Foundation::UInt16>(activeRegionMask | regionBit);
            ++entryCount;
            firstPageIndex += Foundation::UInt64{1U} << (pageShift - MinimumPageShift);
        }
    }

    for (Foundation::Size firstRegion{0U}; firstRegion < InitialTranslationRegionCount; ++firstRegion)
    {
        const Foundation::UInt16 firstBit = static_cast<Foundation::UInt16>(Foundation::UInt16{1U} << firstRegion);
        if ((activeRegionMask & firstBit) == 0U)
        {
            continue;
        }
        for (Foundation::Size secondRegion{firstRegion + 1U}; secondRegion < InitialTranslationRegionCount;
             ++secondRegion)
        {
            const Foundation::UInt16 secondBit =
                static_cast<Foundation::UInt16>(Foundation::UInt16{1U} << secondRegion);
            if (((activeRegionMask & secondBit) != 0U) &&
                (regions.Regions[firstRegion].RegionIdentifier == regions.Regions[secondRegion].RegionIdentifier))
            {
                failure = Failure(InitialTranslationResourcePlanError::ActiveRegionIdentifierConflict);
                return false;
            }
        }
    }
    return true;
}

bool InitialTranslationResourcePlanner::Populate(const Kernel::Memory::InitialMappingPlan& mappings,
                                                 const PalVirtualMemoryCapabilities& capabilities,
                                                 const InitialTranslationRegionSet& regions,
                                                 const Foundation::Size entryCount,
                                                 const Foundation::UInt16 instructionRegisterCount,
                                                 InitialTranslationResourcePlan& output,
                                                 InitialTranslationResourcePlanFailure& failure) noexcept
{
    Foundation::Size destination{0U};
    Foundation::UInt16 nextInstructionRegister{1U};
    for (Foundation::Size mappingIndex{0U}; mappingIndex < mappings.GetMappingCount(); ++mappingIndex)
    {
        const auto mappingResult = mappings.GetMapping(mappingIndex);
        if (!mappingResult.HasValue())
        {
            for (Foundation::Size index{0U}; index < destination; ++index)
            {
                output.m_entries[index] = InitialTranslationResourceEntry{};
            }
            failure = Failure(InitialTranslationResourcePlanError::InternalInvariantViolation,
                              InitialTranslationEncodingError::InternalInvariantViolation, mappingIndex);
            return false;
        }
        const InitialMapping mapping = mappingResult.GetValue();
        Foundation::UInt64 firstPageIndex{0U};
        while (firstPageIndex < mapping.Pages.GetValue())
        {
            const Foundation::UInt8 pageShift = SelectPageShift(mapping, firstPageIndex, capabilities.Translation);
            const Foundation::UInt8 virtualRegion = static_cast<Foundation::UInt8>(
                (mapping.VirtualStart.GetValue() + (firstPageIndex << MinimumPageShift)) >> 61U);
            const auto encoding =
                InitialTranslationEncoder::EncodeRange(mappings, mappingIndex, firstPageIndex, pageShift,
                                                       capabilities.Translation, regions.Regions[virtualRegion]);
            if (!encoding.HasValue() || (destination >= entryCount))
            {
                for (Foundation::Size index{0U}; index < destination; ++index)
                {
                    output.m_entries[index] = InitialTranslationResourceEntry{};
                }
                failure = Failure(InitialTranslationResourcePlanError::InternalInvariantViolation,
                                  encoding.HasValue() ? InitialTranslationEncodingError::InternalInvariantViolation
                                                      : encoding.GetError(),
                                  mappingIndex, firstPageIndex);
                return false;
            }
            const bool executable = mapping.Permissions.Contains(MappingPermission::Execute);
            output.m_entries[destination] = InitialTranslationResourceEntry{
                .Translation = encoding.GetValue(),
                .MappingPageIndex = firstPageIndex,
                .MappingIndex = static_cast<Foundation::UInt16>(mappingIndex),
                .DataRegisterIndex = static_cast<Foundation::UInt16>(destination),
                .InstructionRegisterIndex = executable ? nextInstructionRegister : Foundation::UInt16{0U},
                .PageShift = pageShift,
                .HasInstructionRegister = executable,
            };
            if (executable)
            {
                ++nextInstructionRegister;
            }
            ++destination;
            firstPageIndex += Foundation::UInt64{1U} << (pageShift - MinimumPageShift);
        }
    }

    if ((destination == entryCount) && (nextInstructionRegister == instructionRegisterCount))
    {
        return true;
    }
    for (Foundation::Size index{0U}; index < destination; ++index)
    {
        output.m_entries[index] = InitialTranslationResourceEntry{};
    }
    failure = Failure(InitialTranslationResourcePlanError::InternalInvariantViolation,
                      InitialTranslationEncodingError::InternalInvariantViolation);
    return false;
}

Foundation::Result<InitialTranslationResourcePlanSummary, InitialTranslationResourcePlanFailure>
InitialTranslationResourcePlanner::Build(const Kernel::Memory::InitialMappingPlan& mappings,
                                         const PalVirtualMemoryCapabilities& capabilities,
                                         const Firmware::SalSystemTableInformation& firmware,
                                         const InitialTranslationRegionSet& regions,
                                         InitialTranslationResourcePlan& output) noexcept
{
    if (output.m_built)
    {
        return Fail(Failure(InitialTranslationResourcePlanError::OutputAlreadyBuilt));
    }
    if (!mappings.IsSealed())
    {
        return Fail(Failure(InitialTranslationResourcePlanError::PlanNotSealed));
    }
    if (!AreCapabilitiesValid(capabilities))
    {
        return Fail(Failure(InitialTranslationResourcePlanError::InvalidCapabilities));
    }
    if (!firmware.HasTranslationRegister)
    {
        return Fail(Failure(InitialTranslationResourcePlanError::MissingFirmwareInstructionTranslationRegister));
    }
    Foundation::UInt8 firmwarePageShift{0U};
    Foundation::UInt32 firmwareRegionIdentifier{0U};
    if (!IsFirmwareTranslationValid(capabilities, firmware, firmwarePageShift, firmwareRegionIdentifier))
    {
        return Fail(Failure(InitialTranslationResourcePlanError::InvalidFirmwareInstructionTranslationRegister));
    }
    if (!IsVirtualRangeImplemented(firmware.TranslationRegisterVirtualAddress, firmwarePageShift,
                                   capabilities.Translation.ImplementedVirtualAddressMostSignificantBit))
    {
        return Fail(Failure(InitialTranslationResourcePlanError::UnimplementedFirmwareTranslationAddress));
    }
    const Foundation::UInt64 firmwareTranslationEnd =
        firmware.TranslationRegisterVirtualAddress + (Foundation::UInt64{1U} << firmwarePageShift) - 1U;
    const Foundation::UInt64 firmwareTranslationLowStart =
        firmware.TranslationRegisterVirtualAddress & VirtualRegionOffsetMask;
    const Foundation::UInt64 firmwareTranslationLowEnd = firmwareTranslationEnd & VirtualRegionOffsetMask;
    Foundation::Size entryCount{0U};
    Foundation::UInt16 instructionRegisterCount{1U};
    InitialTranslationResourcePlanFailure failure{};
    if (!ValidateAndMeasure(mappings, capabilities, regions, firmwareTranslationLowStart, firmwareTranslationLowEnd,
                            firmwareRegionIdentifier, entryCount, instructionRegisterCount, failure))
    {
        return Fail(failure);
    }
    Foundation::UInt64 regionRegisters[InitialTranslationRegionCount]{};
    if (!ValidateRegionSet(capabilities.Translation, regions, regionRegisters))
    {
        return Fail(Failure(InitialTranslationResourcePlanError::InvalidRegionConfiguration));
    }
    if (!Populate(mappings, capabilities, regions, entryCount, instructionRegisterCount, output, failure))
    {
        return Fail(failure);
    }

    output.m_firmwareTranslationVirtualAddress = firmware.TranslationRegisterVirtualAddress;
    output.m_firmwareTranslationEncodedPageSize = firmware.TranslationRegisterEncodedPageSize;
    for (Foundation::Size index{0U}; index < InitialTranslationRegionCount; ++index)
    {
        output.m_regionRegisters[index] = regionRegisters[index];
    }
    output.m_entryCount = entryCount;
    output.m_dataRegisterCount = static_cast<Foundation::UInt16>(entryCount);
    output.m_instructionRegisterCount = instructionRegisterCount;
    output.m_built = true;

    const InitialTranslationResourcePlanSummary summary{
        .EntryCount = static_cast<Foundation::UInt16>(entryCount),
        .DataRegisterCount = static_cast<Foundation::UInt16>(entryCount),
        .InstructionRegisterCount = instructionRegisterCount,
        .Reserved = 0U,
    };
    return Foundation::Result<InitialTranslationResourcePlanSummary, InitialTranslationResourcePlanFailure>::Success(
        summary);
}
} // namespace Asteria::Platform::Ia64::Memory
