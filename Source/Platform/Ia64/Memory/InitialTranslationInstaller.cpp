// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/InitialTranslationInstaller.hpp"

extern "C" Asteria::Foundation::UInt64 AsteriaIa64BeginInitialTranslationInstall() noexcept;

extern "C" void AsteriaIa64PurgeLocalTranslationCache(Asteria::Foundation::UInt64 address) noexcept;

extern "C" void AsteriaIa64SerializeDataAndInstruction() noexcept;

extern "C" void AsteriaIa64PurgeInstructionTranslation(Asteria::Foundation::UInt64 virtualAddress,
                                                       Asteria::Foundation::UInt64 encodedPageSize) noexcept;

extern "C" void AsteriaIa64SerializeInstruction() noexcept;

extern "C" void AsteriaIa64WriteRegionRegister(Asteria::Foundation::UInt64 virtualAddress,
                                               Asteria::Foundation::UInt64 regionRegister) noexcept;

extern "C" Asteria::Foundation::UInt64
AsteriaIa64ReadRegionRegister(Asteria::Foundation::UInt64 virtualAddress) noexcept;

extern "C" [[noreturn]] void AsteriaIa64FailStopInitialTranslationInstall() noexcept;

extern "C" void AsteriaIa64InsertDataTranslation(Asteria::Foundation::UInt64 registerIndex,
                                                 Asteria::Foundation::UInt64 translationWord,
                                                 Asteria::Foundation::UInt64 faultingAddress,
                                                 Asteria::Foundation::UInt64 translationInsertion) noexcept;

extern "C" void AsteriaIa64InsertInstructionTranslation(Asteria::Foundation::UInt64 registerIndex,
                                                        Asteria::Foundation::UInt64 translationWord,
                                                        Asteria::Foundation::UInt64 faultingAddress,
                                                        Asteria::Foundation::UInt64 translationInsertion) noexcept;

extern "C" void AsteriaIa64EndInitialTranslationInstall() noexcept;

// The target entry points are in InitialTranslationInstaller.S.

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt8 MinimumPageShift{12U};
constexpr Foundation::UInt8 MaximumPageShift{60U};
constexpr Foundation::UInt16 MaximumRegisterCount{256U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 VirtualRegionOffsetMask{(Foundation::UInt64{1U} << 61U) - 1U};
constexpr Foundation::UInt64 PhysicalPageNumberMask{0x0003'FFFF'FFFF'F000ULL};
constexpr Foundation::UInt64 PresentBit{Foundation::UInt64{1U} << 0U};
constexpr Foundation::UInt64 MemoryAttributeMask{Foundation::UInt64{7U} << 2U};
constexpr Foundation::UInt64 AccessedBit{Foundation::UInt64{1U} << 5U};
constexpr Foundation::UInt64 DirtyBit{Foundation::UInt64{1U} << 6U};
constexpr Foundation::UInt64 AccessRightsMask{Foundation::UInt64{7U} << 9U};
constexpr Foundation::UInt64 ExceptionDeferralBit{Foundation::UInt64{1U} << 52U};
constexpr Foundation::UInt64 AllowedTranslationWordMask{PresentBit | MemoryAttributeMask | AccessedBit | DirtyBit |
                                                        AccessRightsMask | PhysicalPageNumberMask |
                                                        ExceptionDeferralBit};
constexpr Foundation::UInt64 AllowedTranslationInsertionMask{0x0000'0000'FFFF'FFFCULL};
constexpr Foundation::UInt64 AllowedRegionRegisterMask{0x0000'0000'FFFF'FFFDULL};
constexpr Foundation::UInt64 BelowMinimumPageSizeMask{(Foundation::UInt64{1U} << MinimumPageShift) - 1U};
constexpr Foundation::UInt8 RequiredMemoryAttributeMask{(Foundation::UInt8{1U} << 0U) | (Foundation::UInt8{1U} << 4U)};
constexpr Foundation::UInt8 ArchitectedMemoryAttributeMask{RequiredMemoryAttributeMask | (Foundation::UInt8{1U} << 5U) |
                                                           (Foundation::UInt8{1U} << 6U) |
                                                           (Foundation::UInt8{1U} << 7U)};
constexpr Foundation::UInt32 SalRegionIdentifierBase{0x1000U};
constexpr Foundation::UInt32 MaximumSalRegionIdentifier{0x1003U};
constexpr Foundation::Size NoEntryIndex{~Foundation::Size{0U}};

[[nodiscard]] Foundation::Result<InitialTranslationInstallSummary, InitialTranslationInstallFailure>
Fail(const InitialTranslationInstallError error, const Foundation::Size entryIndex = NoEntryIndex) noexcept
{
    return Foundation::Result<InitialTranslationInstallSummary, InitialTranslationInstallFailure>::Failure(
        InitialTranslationInstallFailure{
            .Error = error,
            .Reserved = 0U,
            .EntryIndex = entryIndex,
        });
}

[[nodiscard]] bool TryAdd(const Foundation::UInt64 left, const Foundation::UInt64 right,
                          Foundation::UInt64& sum) noexcept
{
    if (left > (MaximumUInt64 - right))
    {
        return false;
    }
    sum = left + right;
    return true;
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

[[nodiscard]] bool FitsUnsignedBits(const Foundation::UInt32 value, const Foundation::UInt8 bitCount) noexcept
{
    return static_cast<Foundation::UInt64>(value) < (Foundation::UInt64{1U} << bitCount);
}

[[nodiscard]] bool IsPurgePlanValid(const PalTranslationCachePurgePlan& plan) noexcept
{
    if ((plan.OuterCount == 0U) || (plan.InnerCount == 0U))
    {
        return false;
    }
    const Foundation::UInt64 operationCount =
        static_cast<Foundation::UInt64>(plan.OuterCount) * static_cast<Foundation::UInt64>(plan.InnerCount);
    if ((operationCount == 0U) || (operationCount > MaximumTranslationCachePurgeOperations) ||
        (plan.OperationCount != operationCount))
    {
        return false;
    }
    const Foundation::UInt64 innerOffset = (operationCount - 1U) * static_cast<Foundation::UInt64>(plan.InnerStride);
    const Foundation::UInt64 outerOffset =
        (static_cast<Foundation::UInt64>(plan.OuterCount) - 1U) * static_cast<Foundation::UInt64>(plan.OuterStride);
    Foundation::UInt64 finalOffset{0U};
    Foundation::UInt64 lastAddress{0U};
    return TryAdd(innerOffset, outerOffset, finalOffset) && TryAdd(plan.BaseAddress, finalOffset, lastAddress) &&
           (lastAddress == plan.LastPurgeAddress);
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
    if (address > (MaximumUInt64 - (pageSize - 1U)))
    {
        return false;
    }
    const Foundation::UInt64 endAddress = address + pageSize - 1U;
    return ((address >> 61U) == (endAddress >> 61U)) &&
           IsVirtualAddressImplemented(address, implementedMostSignificantBit) &&
           IsVirtualAddressImplemented(endAddress, implementedMostSignificantBit);
}

[[nodiscard]] bool IsPhysicalRangeValid(const Foundation::UInt64 physicalAddress, const Foundation::UInt8 pageShift,
                                        const Foundation::UInt8 physicalAddressBits) noexcept
{
    const Foundation::UInt8 encodableBits = physicalAddressBits < 50U ? physicalAddressBits : Foundation::UInt8{50U};
    if (pageShift > encodableBits)
    {
        return false;
    }
    const Foundation::UInt64 pageSize = Foundation::UInt64{1U} << pageShift;
    const Foundation::UInt64 limit = Foundation::UInt64{1U} << encodableBits;
    return ((physicalAddress & (pageSize - 1U)) == 0U) && ((physicalAddress & ~PhysicalPageNumberMask) == 0U) &&
           (physicalAddress <= (limit - pageSize));
}

[[nodiscard]] bool
ValidateRegionRegisters(const InitialTranslationResourcePlan& plan, const TranslationCapabilities& capabilities,
                        Foundation::UInt64 (&regionRegisters)[InitialTranslationRegionCount]) noexcept
{
    for (Foundation::Size index{0U}; index < InitialTranslationRegionCount; ++index)
    {
        const auto regionRegisterResult = plan.GetRegionRegister(index);
        if (!regionRegisterResult.HasValue())
        {
            return false;
        }
        const Foundation::UInt64 regionRegister = regionRegisterResult.GetValue();
        const Foundation::UInt32 regionIdentifier = static_cast<Foundation::UInt32>(regionRegister >> 8U);
        const Foundation::UInt8 pageShift = static_cast<Foundation::UInt8>((regionRegister >> 2U) & 0x3FU);
        if (((regionRegister & ~AllowedRegionRegisterMask) != 0U) ||
            ((regionRegister & Foundation::UInt64{1U}) != 0U) || (pageShift != MinimumPageShift) ||
            !FitsUnsignedBits(regionIdentifier, capabilities.RegionIdentifierBits) ||
            ((regionIdentifier >= SalRegionIdentifierBase) && (regionIdentifier <= MaximumSalRegionIdentifier)))
        {
            return false;
        }
        for (Foundation::Size previous{0U}; previous < index; ++previous)
        {
            if (static_cast<Foundation::UInt32>(regionRegisters[previous] >> 8U) == regionIdentifier)
            {
                return false;
            }
        }
        regionRegisters[index] = regionRegister;
    }
    return true;
}

[[nodiscard]] bool IsFirmwareTranslationValid(const InitialTranslationResourcePlan& plan,
                                              const PalVirtualMemoryCapabilities& capabilities) noexcept
{
    const Foundation::UInt64 encodedPageSize = plan.GetFirmwareTranslationEncodedPageSize();
    if ((encodedPageSize & ~Foundation::UInt64{0xFCU}) != 0U)
    {
        return false;
    }
    const Foundation::UInt8 pageShift = static_cast<Foundation::UInt8>((encodedPageSize >> 2U) & 0x3FU);
    if ((pageShift < MinimumPageShift) || (pageShift > MaximumPageShift) ||
        ((capabilities.Translation.InsertablePageSizeMask & (Foundation::UInt64{1U} << pageShift)) == 0U) ||
        ((capabilities.PurgePageSizeMask & (Foundation::UInt64{1U} << pageShift)) == 0U))
    {
        return false;
    }
    const Foundation::UInt64 virtualAddress = plan.GetFirmwareTranslationVirtualAddress();
    return ((virtualAddress >> 61U) <= 3U) && ((virtualAddress & ((Foundation::UInt64{1U} << pageShift) - 1U)) == 0U) &&
           IsVirtualRangeImplemented(virtualAddress, pageShift,
                                     capabilities.Translation.ImplementedVirtualAddressMostSignificantBit);
}

[[nodiscard]] bool ValidateEntry(const InitialTranslationResourceEntry& entry, const Foundation::Size expectedDataIndex,
                                 const Foundation::UInt16 expectedInstructionIndex,
                                 const PalVirtualMemoryCapabilities& capabilities,
                                 const Foundation::UInt64 (&regionRegisters)[InitialTranslationRegionCount]) noexcept
{
    const EncodedInitialTranslation translation = entry.Translation;
    if ((entry.PageShift < MinimumPageShift) || (entry.PageShift > MaximumPageShift) ||
        ((capabilities.Translation.InsertablePageSizeMask & (Foundation::UInt64{1U} << entry.PageShift)) == 0U) ||
        (entry.DataRegisterIndex != expectedDataIndex) ||
        (entry.InstructionRegisterIndex !=
         (entry.HasInstructionRegister ? expectedInstructionIndex : Foundation::UInt16{0U})) ||
        ((translation.TranslationInsertion & ~AllowedTranslationInsertionMask) != 0U) ||
        (static_cast<Foundation::UInt8>((translation.TranslationInsertion >> 2U) & 0x3FU) != entry.PageShift) ||
        ((translation.RegionRegister & ~AllowedRegionRegisterMask) != 0U))
    {
        return false;
    }
    const Foundation::UInt8 regionIndex = static_cast<Foundation::UInt8>(translation.FaultingAddress >> 61U);
    if ((regionIndex >= InitialTranslationRegionCount) ||
        (translation.RegionRegister != regionRegisters[regionIndex]) ||
        ((translation.TranslationInsertion >> 8U) != static_cast<Foundation::UInt64>(0U)))
    {
        return false;
    }
    const Foundation::UInt64 pageSize = Foundation::UInt64{1U} << entry.PageShift;
    const Foundation::UInt64 physicalAddress = translation.TranslationWord & PhysicalPageNumberMask;
    const Foundation::UInt8 memoryAttribute = static_cast<Foundation::UInt8>((translation.TranslationWord >> 2U) & 7U);
    const Foundation::UInt8 accessRights = static_cast<Foundation::UInt8>((translation.TranslationWord >> 9U) & 7U);
    const bool dirty = (translation.TranslationWord & DirtyBit) != 0U;
    return ((translation.FaultingAddress & (pageSize - 1U)) == 0U) &&
           IsVirtualRangeImplemented(translation.FaultingAddress, entry.PageShift,
                                     capabilities.Translation.ImplementedVirtualAddressMostSignificantBit) &&
           ((translation.TranslationWord & ~AllowedTranslationWordMask) == 0U) &&
           ((translation.TranslationWord & (PresentBit | AccessedBit | ExceptionDeferralBit)) ==
            (PresentBit | AccessedBit | ExceptionDeferralBit)) &&
           ((memoryAttribute == 0U) || (memoryAttribute == 4U) || (memoryAttribute == 6U)) &&
           ((capabilities.Translation.MemoryAttributeMask &
             static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << memoryAttribute)) != 0U) &&
           ((accessRights == 0U) || (accessRights == 1U) || (accessRights == 2U)) && (dirty == (accessRights == 2U)) &&
           IsPhysicalRangeValid(physicalAddress, entry.PageShift, capabilities.Translation.PhysicalAddressBits);
}

[[nodiscard]] bool EntriesOverlap(const InitialTranslationResourceEntry& first,
                                  const InitialTranslationResourceEntry& second) noexcept
{
    const Foundation::UInt32 firstRegionIdentifier =
        static_cast<Foundation::UInt32>(first.Translation.RegionRegister >> 8U);
    const Foundation::UInt32 secondRegionIdentifier =
        static_cast<Foundation::UInt32>(second.Translation.RegionRegister >> 8U);
    if (firstRegionIdentifier != secondRegionIdentifier)
    {
        return false;
    }
    const Foundation::UInt64 firstStart = first.Translation.FaultingAddress & VirtualRegionOffsetMask;
    const Foundation::UInt64 secondStart = second.Translation.FaultingAddress & VirtualRegionOffsetMask;
    const Foundation::UInt64 firstEnd = firstStart + (Foundation::UInt64{1U} << first.PageShift) - 1U;
    const Foundation::UInt64 secondEnd = secondStart + (Foundation::UInt64{1U} << second.PageShift) - 1U;
    return (firstStart <= secondEnd) && (secondStart <= firstEnd);
}
} // namespace

Foundation::Result<InitialTranslationInstallSummary, InitialTranslationInstallFailure>
InitialTranslationInstaller::Install(const InitialTranslationResourcePlan& plan,
                                     const PalVirtualMemoryCapabilities& capabilities,
                                     const PalTranslationCachePurgePlan& purgePlan) noexcept
{
    const PalTranslationCachePurgePlan purgeSnapshot{purgePlan};
    if (!IsPurgePlanValid(purgeSnapshot))
    {
        return Fail(InitialTranslationInstallError::InvalidPurgePlan);
    }
    if (!plan.IsBuilt())
    {
        return Fail(InitialTranslationInstallError::ResourcePlanNotBuilt);
    }
    if (!AreCapabilitiesValid(capabilities))
    {
        return Fail(InitialTranslationInstallError::InvalidCapabilities);
    }
    Foundation::UInt64 regionRegisters[InitialTranslationRegionCount]{};
    if (!ValidateRegionRegisters(plan, capabilities.Translation, regionRegisters))
    {
        return Fail(InitialTranslationInstallError::InvalidRegionConfiguration);
    }
    if (!IsFirmwareTranslationValid(plan, capabilities))
    {
        return Fail(InitialTranslationInstallError::InvalidFirmwareTranslation);
    }

    const Foundation::Size entryCount = plan.GetEntryCount();
    const Foundation::UInt16 dataRegisterCount = plan.GetDataRegisterCount();
    const Foundation::UInt16 instructionRegisterCount = plan.GetInstructionRegisterCount();
    if ((entryCount == 0U) || (entryCount > MaximumInitialTranslationEntries) || (dataRegisterCount != entryCount) ||
        (dataRegisterCount > capabilities.DataTranslationRegisterCount) || (instructionRegisterCount == 0U) ||
        (instructionRegisterCount > capabilities.InstructionTranslationRegisterCount))
    {
        return Fail(InitialTranslationInstallError::InvalidResourceEntry);
    }

    Foundation::UInt16 nextInstructionIndex{1U};
    for (Foundation::Size index{0U}; index < entryCount; ++index)
    {
        const auto entryResult = plan.GetEntry(index);
        if (!entryResult.HasValue() ||
            !ValidateEntry(entryResult.GetValue(), index, nextInstructionIndex, capabilities, regionRegisters))
        {
            return Fail(InitialTranslationInstallError::InvalidResourceEntry, index);
        }
        const InitialTranslationResourceEntry entry = entryResult.GetValue();
        for (Foundation::Size previousIndex{0U}; previousIndex < index; ++previousIndex)
        {
            const auto previousResult = plan.GetEntry(previousIndex);
            if (!previousResult.HasValue())
            {
                return Fail(InitialTranslationInstallError::InvalidResourceEntry, previousIndex);
            }
            const InitialTranslationResourceEntry previous = previousResult.GetValue();
            if (EntriesOverlap(previous, entry))
            {
                return Fail(InitialTranslationInstallError::OverlappingResourceEntries, index);
            }
        }
        if (entry.HasInstructionRegister)
        {
            ++nextInstructionIndex;
        }
    }
    if (nextInstructionIndex != instructionRegisterCount)
    {
        return Fail(InitialTranslationInstallError::InvalidResourceEntry);
    }

    if (AsteriaIa64BeginInitialTranslationInstall() != 0U)
    {
        return Fail(InitialTranslationInstallError::UnsupportedProcessorState);
    }

    Foundation::UInt64 purgeAddress = purgeSnapshot.BaseAddress;
    for (Foundation::UInt32 outer{0U}; outer < purgeSnapshot.OuterCount; ++outer)
    {
        for (Foundation::UInt32 inner{0U}; inner < purgeSnapshot.InnerCount; ++inner)
        {
            AsteriaIa64PurgeLocalTranslationCache(purgeAddress);
            purgeAddress += static_cast<Foundation::UInt64>(purgeSnapshot.InnerStride);
        }
        purgeAddress += static_cast<Foundation::UInt64>(purgeSnapshot.OuterStride);
    }
    AsteriaIa64SerializeDataAndInstruction();
    AsteriaIa64PurgeInstructionTranslation(plan.GetFirmwareTranslationVirtualAddress(),
                                           plan.GetFirmwareTranslationEncodedPageSize());
    AsteriaIa64SerializeInstruction();

    for (Foundation::Size index{0U}; index < InitialTranslationRegionCount; ++index)
    {
        AsteriaIa64WriteRegionRegister(static_cast<Foundation::UInt64>(index) << 61U, regionRegisters[index]);
    }
    AsteriaIa64SerializeDataAndInstruction();
    for (Foundation::Size index{0U}; index < InitialTranslationRegionCount; ++index)
    {
        if (AsteriaIa64ReadRegionRegister(static_cast<Foundation::UInt64>(index) << 61U) != regionRegisters[index])
        {
            AsteriaIa64FailStopInitialTranslationInstall();
        }
    }

    for (Foundation::Size index{0U}; index < entryCount; ++index)
    {
        const InitialTranslationResourceEntry entry = plan.GetEntry(index).GetValue();
        AsteriaIa64InsertDataTranslation(entry.DataRegisterIndex, entry.Translation.TranslationWord,
                                         entry.Translation.FaultingAddress, entry.Translation.TranslationInsertion);
        if (entry.HasInstructionRegister)
        {
            AsteriaIa64InsertInstructionTranslation(entry.InstructionRegisterIndex, entry.Translation.TranslationWord,
                                                    entry.Translation.FaultingAddress,
                                                    entry.Translation.TranslationInsertion);
        }
    }
    AsteriaIa64SerializeDataAndInstruction();
    AsteriaIa64EndInitialTranslationInstall();

    return Foundation::Result<InitialTranslationInstallSummary, InitialTranslationInstallFailure>::Success(
        InitialTranslationInstallSummary{
            .PurgeOperationCount = purgeSnapshot.OperationCount,
            .EntryCount = static_cast<Foundation::UInt16>(entryCount),
            .DataRegisterCount = dataRegisterCount,
            .InstructionRegisterCount = instructionRegisterCount,
            .Reserved = 0U,
        });
}
} // namespace Asteria::Platform::Ia64::Memory
