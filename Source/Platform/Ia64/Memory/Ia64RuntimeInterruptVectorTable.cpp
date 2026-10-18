// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64RuntimeInterruptVectorTable.hpp"

#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Ia64/Diagnostics/Ia64Stop.hpp"
#include "Platform/Ia64/Interrupt/Ia64ExternalInterruptEntry.hpp"
#include "Platform/Ia64/Interrupt/Ia64PageFaultEntryContract.hpp"
#include "Platform/Ia64/Interrupt/Ia64UserFaultEntry.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Ia64/Memory/InitialTranslationResourcePlan.hpp"
#include "Platform/Ia64/System/Ia64SystemCallEntry.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

extern "C" Asteria::Foundation::UInt64
AsteriaIa64InstallRuntimeInterruptVectorTable(Asteria::Foundation::UInt64 contextAddress,
                                              Asteria::Foundation::UInt64 tableAddress) noexcept;

extern "C" [[noreturn]] void AsteriaIa64FailStopRuntimeInterruptVectorTableInstall() noexcept;

extern "C" Asteria::Foundation::UInt64 AsteriaIa64GetRuntimeInterruptVectorTableAddress() noexcept;

// The target entry points and the runtime IVT image are in
// Ia64RuntimeInterruptVectorTable.S.

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 RegionZeroLimit{Foundation::UInt64{1U} << 61U};
constexpr Foundation::UInt64 PageSizeBytes{4096U};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};
constexpr Foundation::UInt64 PhysicalPageNumberMask{0x0003'FFFF'FFFF'F000ULL};
constexpr Foundation::UInt64 CurrentProcessorStatusForbiddenMask{
    (Foundation::UInt64{1U} << 14U) | (Foundation::UInt64{1U} << 13U) | (Foundation::UInt64{1U} << 15U)};
constexpr Foundation::UInt64 InterruptedProcessorStatusRequiredMask{
    (Foundation::UInt64{1U} << 44U) | (Foundation::UInt64{1U} << 36U) | (Foundation::UInt64{1U} << 13U)};
constexpr Foundation::UInt64 InterruptedProcessorStatusForbiddenMask{
    (Foundation::UInt64{1U} << 46U) | (Foundation::UInt64{1U} << 34U) | (Foundation::UInt64{1U} << 15U)};
constexpr Foundation::UInt64 TranslationStateMask{(Foundation::UInt64{1U} << 36U) | (Foundation::UInt64{1U} << 27U) |
                                                  (Foundation::UInt64{1U} << 17U)};
constexpr Foundation::UInt64 InterruptionStatusRseMask{Foundation::UInt64{1U} << 37U};
constexpr Foundation::UInt64 InterruptionStatusIncompleteFrameMask{Foundation::UInt64{1U} << 38U};
constexpr Foundation::UInt64 InterruptionStatusNestedMask{Foundation::UInt64{1U} << 39U};
constexpr Foundation::UInt64 PrivilegeMask{Foundation::UInt64{3U} << 32U};
constexpr Foundation::UInt64 UserPrivilege{PrivilegeMask};
constexpr Foundation::UInt64 FailStopBundleLow{0x0000'0001'0000'0011ULL};
constexpr Foundation::UInt64 FailStopBundleHigh{0x4000'0000'0000'0200ULL};

using InstallResult =
    Foundation::Result<Ia64RuntimeInterruptVectorTableInstallSummary, Ia64RuntimeInterruptVectorTableInstallError>;
using RefillResult = Foundation::Result<Ia64RuntimeRefillAction, Ia64RuntimeRefillError>;

static_assert(sizeof(System::Ia64SystemCallEntryContext) == 64U);
static_assert(alignof(System::Ia64SystemCallEntryContext) == 16U);
static_assert(__builtin_offsetof(System::Ia64SystemCallEntryContext, ProcessorIndex) == 48U);
static_assert(__builtin_offsetof(System::Ia64SystemCallEntryContext, Reserved) == 56U);
static_assert(Ia64RuntimePageNotPresentOffset == Interrupt::Ia64PageFaultPageNotPresentVectorOffset);
static_assert(Ia64RuntimeKeyPermissionOffset == Interrupt::Ia64PageFaultKeyPermissionVectorOffset);
static_assert(Ia64RuntimeInstructionAccessRightsOffset == Interrupt::Ia64PageFaultInstructionAccessRightsVectorOffset);
static_assert(Ia64RuntimeDataAccessRightsOffset == Interrupt::Ia64PageFaultDataAccessRightsVectorOffset);
static_assert(Interrupt::Ia64PageFaultMaximumProcessorCount == Kernel::Scheduler::MaximumProcessorCount);

[[nodiscard]] constexpr Ia64RuntimeInterruptVectorTableImage MakeHostedTableImage() noexcept
{
    Ia64RuntimeInterruptVectorTableImage image{};
    for (Foundation::Size offset{0U}; offset < Ia64RuntimeInterruptVectorTableSizeBytes; offset += 16U)
    {
        for (Foundation::Size byte{0U}; byte < 8U; ++byte)
        {
            image.Bytes[offset + byte] = static_cast<Foundation::UInt8>(FailStopBundleLow >> (byte * 8U));
            image.Bytes[offset + 8U + byte] = static_cast<Foundation::UInt8>(FailStopBundleHigh >> (byte * 8U));
        }
    }

    // Hosted builds cannot execute or validate IA-64 entry code. These eight
    // immutable marker bundles make the structural slots observable without
    // claiming target-handler success; target builds use the real assembly in
    // Ia64RuntimeInterruptVectorTable.S.
    constexpr Foundation::UInt8 instructionMarker[16U]{
        0x49U, 0x41U, 0x36U, 0x34U, 0x2DU, 0x49U, 0x54U, 0x4CU, 0x42U, 0x2DU, 0x48U, 0x4FU, 0x53U, 0x54U, 0x00U, 0x01U,
    };
    constexpr Foundation::UInt8 dataMarker[16U]{
        0x49U, 0x41U, 0x36U, 0x34U, 0x2DU, 0x44U, 0x54U, 0x4CU, 0x42U, 0x2DU, 0x48U, 0x4FU, 0x53U, 0x54U, 0x00U, 0x02U,
    };
    constexpr Foundation::UInt8 breakMarker[16U]{
        0x49U, 0x41U, 0x36U, 0x34U, 0x2DU, 0x42U, 0x52U, 0x4BU, 0x2DU, 0x48U, 0x4FU, 0x53U, 0x54U, 0x00U, 0x00U, 0x03U,
    };
    constexpr Foundation::UInt8 externalInterruptMarker[16U]{
        0x49U, 0x41U, 0x36U, 0x34U, 0x2DU, 0x45U, 0x58U, 0x54U, 0x2DU, 0x48U, 0x4FU, 0x53U, 0x54U, 0x00U, 0x00U, 0x04U,
    };
    constexpr Foundation::UInt8 pageNotPresentMarker[16U]{
        0x49U, 0x41U, 0x36U, 0x34U, 0x2DU, 0x50U, 0x46U, 0x30U, 0x2DU, 0x48U, 0x4FU, 0x53U, 0x54U, 0x00U, 0x00U, 0x05U,
    };
    constexpr Foundation::UInt8 keyPermissionMarker[16U]{
        0x49U, 0x41U, 0x36U, 0x34U, 0x2DU, 0x50U, 0x46U, 0x31U, 0x2DU, 0x48U, 0x4FU, 0x53U, 0x54U, 0x00U, 0x00U, 0x06U,
    };
    constexpr Foundation::UInt8 instructionAccessRightsMarker[16U]{
        0x49U, 0x41U, 0x36U, 0x34U, 0x2DU, 0x50U, 0x46U, 0x32U, 0x2DU, 0x48U, 0x4FU, 0x53U, 0x54U, 0x00U, 0x00U, 0x07U,
    };
    constexpr Foundation::UInt8 dataAccessRightsMarker[16U]{
        0x49U, 0x41U, 0x36U, 0x34U, 0x2DU, 0x50U, 0x46U, 0x33U, 0x2DU, 0x48U, 0x4FU, 0x53U, 0x54U, 0x00U, 0x00U, 0x08U,
    };
    for (Foundation::Size byte{0U}; byte < 16U; ++byte)
    {
        image.Bytes[Ia64RuntimeAlternateInstructionTlbOffset + byte] = instructionMarker[byte];
        image.Bytes[Ia64RuntimeAlternateDataTlbOffset + byte] = dataMarker[byte];
        image.Bytes[Ia64RuntimeBreakOffset + byte] = breakMarker[byte];
        image.Bytes[Ia64RuntimeExternalInterruptOffset + byte] = externalInterruptMarker[byte];
        image.Bytes[Ia64RuntimePageNotPresentOffset + byte] = pageNotPresentMarker[byte];
        image.Bytes[Ia64RuntimeKeyPermissionOffset + byte] = keyPermissionMarker[byte];
        image.Bytes[Ia64RuntimeInstructionAccessRightsOffset + byte] = instructionAccessRightsMarker[byte];
        image.Bytes[Ia64RuntimeDataAccessRightsOffset + byte] = dataAccessRightsMarker[byte];
    }
    return image;
}

[[nodiscard]] InstallResult FailInstall(const Ia64RuntimeInterruptVectorTableInstallError error) noexcept
{
    return InstallResult::Failure(error);
}

[[nodiscard]] RefillResult FailRefill(const Ia64RuntimeRefillError error) noexcept
{
    return RefillResult::Failure(error);
}

[[nodiscard]] constexpr bool TryCalculateEnd(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes,
                                             Foundation::UInt64& end) noexcept
{
    if ((sizeBytes == 0U) || (start > (MaximumUInt64 - sizeBytes)))
    {
        return false;
    }
    end = start + sizeBytes;
    return true;
}

[[nodiscard]] constexpr bool IsRangeInside(const Foundation::UInt64 outerStart, const Foundation::UInt64 outerEnd,
                                           const Foundation::UInt64 innerStart,
                                           const Foundation::UInt64 innerSize) noexcept
{
    return (outerStart < outerEnd) && (innerSize != 0U) && (innerStart >= outerStart) && (innerStart < outerEnd) &&
           (innerSize <= (outerEnd - innerStart));
}

[[nodiscard]] constexpr bool IsExactSizedRangeInside(const Foundation::UInt64 outerStart,
                                                     const Foundation::UInt64 outerEnd,
                                                     const Foundation::UInt64 innerStart,
                                                     const Foundation::UInt64 innerEnd,
                                                     const Foundation::UInt64 expectedSize) noexcept
{
    Foundation::UInt64 calculatedEnd{0U};
    return TryCalculateEnd(innerStart, expectedSize, calculatedEnd) && (calculatedEnd == innerEnd) &&
           IsRangeInside(outerStart, outerEnd, innerStart, expectedSize);
}

[[nodiscard]] constexpr bool HasExactSize(const Foundation::UInt64 start, const Foundation::UInt64 end,
                                          const Foundation::UInt64 expectedSize) noexcept
{
    return (start <= end) && ((end - start) == expectedSize);
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] Foundation::UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
}

[[nodiscard]] bool MappingHasPermissions(const Kernel::Memory::InitialMapping& mapping, const bool requireWrite,
                                         const bool requireExecute) noexcept
{
    return mapping.Permissions.Contains(Kernel::Memory::MappingPermission::Read) &&
           mapping.Permissions.Contains(Kernel::Memory::MappingPermission::Global) &&
           (!requireWrite || mapping.Permissions.Contains(Kernel::Memory::MappingPermission::Write)) &&
           (!requireExecute || mapping.Permissions.Contains(Kernel::Memory::MappingPermission::Execute));
}

[[nodiscard]] bool HasIdentityGlobalMappingCoverage(const Kernel::Memory::InitialMappingPlan& plan,
                                                    const Foundation::UInt64 rangeStart,
                                                    const Foundation::UInt64 rangeEnd, const bool requireWrite,
                                                    const bool requireExecute, const bool requireOwner,
                                                    const Kernel::Memory::InitialMappingOwner owner) noexcept
{
    Foundation::UInt64 cursor = rangeStart;
    while (cursor < rangeEnd)
    {
        bool found{false};
        Foundation::UInt64 coveredEnd{cursor};
        for (Foundation::Size index{0U}; index < plan.GetMappingCount(); ++index)
        {
            const auto mappingResult = plan.GetMapping(index);
            if (!mappingResult.HasValue())
            {
                return false;
            }
            const Kernel::Memory::InitialMapping mapping = mappingResult.GetValue();
            const Foundation::UInt64 mappingStart = mapping.VirtualStart.GetValue();
            const Foundation::UInt64 mappingPages = mapping.Pages.GetValue();
            if ((mappingPages == 0U) || (mappingPages > (MaximumUInt64 >> 12U)))
            {
                return false;
            }
            const Foundation::UInt64 mappingSize = mappingPages << 12U;
            Foundation::UInt64 mappingEnd{0U};
            if (!TryCalculateEnd(mappingStart, mappingSize, mappingEnd))
            {
                return false;
            }
            if ((cursor < mappingStart) || (cursor >= mappingEnd) ||
                (mapping.PhysicalStart.GetValue() != mappingStart) || (mapping.Reserved != 0U) ||
                !MappingHasPermissions(mapping, requireWrite, requireExecute) ||
                (requireOwner && (mapping.Owner != owner)))
            {
                continue;
            }
            found = true;
            coveredEnd = mappingEnd < rangeEnd ? mappingEnd : rangeEnd;
            break;
        }
        if (!found || (coveredEnd <= cursor))
        {
            return false;
        }
        cursor = coveredEnd;
    }
    return cursor == rangeEnd;
}

[[nodiscard]] bool TranslationEntryCovers(const InitialTranslationResourceEntry& entry, const Foundation::UInt64 cursor,
                                          const bool instruction, Foundation::UInt64& coveredEnd) noexcept
{
    if ((entry.PageShift < 12U) || (entry.PageShift > 60U) || (instruction && !entry.HasInstructionRegister))
    {
        return false;
    }
    const Foundation::UInt64 entryStart = entry.Translation.FaultingAddress;
    const Foundation::UInt64 entrySize = Foundation::UInt64{1U} << entry.PageShift;
    Foundation::UInt64 entryEnd{0U};
    if (!TryCalculateEnd(entryStart, entrySize, entryEnd) || (cursor < entryStart) || (cursor >= entryEnd) ||
        (entry.Translation.TranslationInsertion != (static_cast<Foundation::UInt64>(entry.PageShift) << 2U)) ||
        ((entry.Translation.TranslationWord & PhysicalPageNumberMask) != entryStart))
    {
        return false;
    }
    coveredEnd = entryEnd;
    return true;
}

[[nodiscard]] bool HasTranslationRegisterCoverage(const InitialTranslationResourcePlan& plan,
                                                  const Foundation::UInt64 rangeStart,
                                                  const Foundation::UInt64 rangeEnd, const bool instruction) noexcept
{
    Foundation::UInt64 cursor = rangeStart;
    while (cursor < rangeEnd)
    {
        bool found{false};
        Foundation::UInt64 coveredEnd{cursor};
        for (Foundation::Size index{0U}; index < plan.GetEntryCount(); ++index)
        {
            const auto entryResult = plan.GetEntry(index);
            if (!entryResult.HasValue())
            {
                return false;
            }
            Foundation::UInt64 entryEnd{0U};
            if (TranslationEntryCovers(entryResult.GetValue(), cursor, instruction, entryEnd))
            {
                found = true;
                coveredEnd = entryEnd < rangeEnd ? entryEnd : rangeEnd;
                break;
            }
        }
        if (!found || (coveredEnd <= cursor))
        {
            return false;
        }
        cursor = coveredEnd;
    }
    return cursor == rangeEnd;
}

[[nodiscard]] constexpr Foundation::UInt8 ProcessorBit(const Kernel::Scheduler::CpuId processor) noexcept
{
    return static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << processor.GetValue());
}

[[nodiscard]] constexpr bool IsContextZero(const Ia64RuntimeInterruptVectorTableContext& context) noexcept
{
    return (context.BindingsWireAddress == 0U) && (context.ProcessorIndex == 0U) &&
           (context.SystemCallEntryContextAddress == 0U) && (context.ExternalInterruptEntryContextAddress == 0U) &&
           (context.PageFaultEntryContextAddress == 0U) && (context.UserFaultEntryContextAddress == 0U) &&
           (context.StopStorageAddress == 0U);
}

[[nodiscard]] constexpr bool IsContextExact(const Ia64RuntimeInterruptVectorTableContext& left,
                                            const Ia64RuntimeInterruptVectorTableContext& right) noexcept
{
    return (left.BindingsWireAddress == right.BindingsWireAddress) && (left.ProcessorIndex == right.ProcessorIndex) &&
           (left.SystemCallEntryContextAddress == right.SystemCallEntryContextAddress) &&
           (left.ExternalInterruptEntryContextAddress == right.ExternalInterruptEntryContextAddress) &&
           (left.PageFaultEntryContextAddress == right.PageFaultEntryContextAddress) &&
           (left.UserFaultEntryContextAddress == right.UserFaultEntryContextAddress) &&
           (left.StopStorageAddress == right.StopStorageAddress);
}

} // namespace

#if !defined(ASTERIA_TARGET_IA64) || !defined(__ia64__)
extern "C" alignas(Ia64RuntimeInterruptVectorTableSizeBytes) [[gnu::used]] constinit const
    Ia64RuntimeInterruptVectorTableImage AsteriaIa64RuntimeInterruptVectorTableImage = MakeHostedTableImage();
#endif

Foundation::Result<Ia64RuntimeInterruptVectorTableInstallSummary, Ia64RuntimeInterruptVectorTableInstallError>
Ia64RuntimeInterruptVectorTable::ValidateInstall(const Kernel::Scheduler::CpuId processor,
                                                 const InitialAddressSpacePreparation& preparation,
                                                 Ia64ActiveRuntimeTranslationBindings& bindings,
                                                 const Ia64RuntimeObjectRange runtimeObjects) const noexcept
{
    if (!processor.IsValid())
    {
        return FailInstall(Ia64RuntimeInterruptVectorTableInstallError::InvalidProcessor);
    }
    const Foundation::UInt8 processorBit = ProcessorBit(processor);
    if ((m_installedProcessorMask & processorBit) != 0U)
    {
        return FailInstall(Ia64RuntimeInterruptVectorTableInstallError::ProcessorAlreadyInstalled);
    }

    const auto mappingPlanResult = preparation.TryGetMappingPlan();
    const auto translationPlanResult = preparation.TryGetTranslationResourcePlan();
    if (!preparation.IsPrepared() || !mappingPlanResult.HasValue() || !translationPlanResult.HasValue() ||
        (mappingPlanResult.GetValue() == nullptr) || (translationPlanResult.GetValue() == nullptr) ||
        !translationPlanResult.GetValue()->IsBuilt())
    {
        return FailInstall(Ia64RuntimeInterruptVectorTableInstallError::PreparationNotReady);
    }

#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    const Foundation::UInt64 tableAddress = AsteriaIa64GetRuntimeInterruptVectorTableAddress();
#else
    const Foundation::UInt64 tableAddress = AddressOf(&AsteriaIa64RuntimeInterruptVectorTableImage);
#endif
    Foundation::UInt64 tableEnd{0U};
    if ((tableAddress == 0U) || ((tableAddress & (Ia64RuntimeInterruptVectorTableSizeBytes - 1U)) != 0U) ||
        !TryCalculateEnd(tableAddress, Ia64RuntimeInterruptVectorTableSizeBytes, tableEnd) ||
        (tableEnd > RegionZeroLimit))
    {
        return FailInstall(Ia64RuntimeInterruptVectorTableInstallError::InvalidTableAddress);
    }
    if ((Ia64RuntimeAlternateInstructionTlbOffset + Ia64RuntimeInterruptVectorSlotSizeBytes) >
            Ia64RuntimeAlternateDataTlbOffset ||
        (Ia64RuntimeAlternateDataTlbOffset + Ia64RuntimeInterruptVectorSlotSizeBytes) > Ia64RuntimeBreakOffset ||
        (Ia64RuntimeBreakOffset + Ia64RuntimeInterruptVectorSlotSizeBytes) > Ia64RuntimeExternalInterruptOffset ||
        (Ia64RuntimeExternalInterruptOffset + Ia64RuntimeInterruptVectorSlotSizeBytes) >
            Ia64RuntimePageNotPresentOffset ||
        (Ia64RuntimePageNotPresentOffset + Ia64RuntimeHeavyweightInterruptVectorSlotSizeBytes) >
            Ia64RuntimeKeyPermissionOffset ||
        (Ia64RuntimeKeyPermissionOffset + Ia64RuntimeHeavyweightInterruptVectorSlotSizeBytes) >
            Ia64RuntimeInstructionAccessRightsOffset ||
        (Ia64RuntimeInstructionAccessRightsOffset + Ia64RuntimeHeavyweightInterruptVectorSlotSizeBytes) >
            Ia64RuntimeDataAccessRightsOffset ||
        (Ia64RuntimeDataAccessRightsOffset + Ia64RuntimeHeavyweightInterruptVectorSlotSizeBytes) >
            Ia64RuntimeInterruptVectorTableSizeBytes)
    {
        return FailInstall(Ia64RuntimeInterruptVectorTableInstallError::InvalidTableLayout);
    }

    Foundation::UInt64 runtimeObjectEnd{0U};
    if ((runtimeObjects.VirtualStart & PageOffsetMask) != 0U || (runtimeObjects.SizeBytes & PageOffsetMask) != 0U ||
        !TryCalculateEnd(runtimeObjects.VirtualStart, runtimeObjects.SizeBytes, runtimeObjectEnd) ||
        (runtimeObjectEnd > RegionZeroLimit))
    {
        return FailInstall(Ia64RuntimeInterruptVectorTableInstallError::InvalidRuntimeObjectRange);
    }
    if ((m_runtimeObjectEnd != 0U) &&
        ((runtimeObjects.VirtualStart != m_runtimeObjectStart) || (runtimeObjectEnd != m_runtimeObjectEnd)))
    {
        return FailInstall(Ia64RuntimeInterruptVectorTableInstallError::RuntimeObjectRangeMismatch);
    }
    if (!IsRangeInside(runtimeObjects.VirtualStart, runtimeObjectEnd, AddressOf(this), sizeof(*this)))
    {
        return FailInstall(Ia64RuntimeInterruptVectorTableInstallError::OwnerOutsideRuntimeObjectRange);
    }
    if (!IsRangeInside(runtimeObjects.VirtualStart, runtimeObjectEnd, AddressOf(&bindings), sizeof(bindings)))
    {
        return FailInstall(Ia64RuntimeInterruptVectorTableInstallError::BindingsOutsideRuntimeObjectRange);
    }

    if (m_stopStorageAddress != 0U && (((m_stopStorageAddress & (alignof(Diagnostics::Ia64StopStorage) - 1U)) != 0U) ||
                                       !IsRangeInside(runtimeObjects.VirtualStart, runtimeObjectEnd,
                                                      m_stopStorageAddress, sizeof(Diagnostics::Ia64StopStorage))))
    {
        return FailInstall(Ia64RuntimeInterruptVectorTableInstallError::StopStorageOutsideRuntimeObjectRange);
    }

    const Kernel::Memory::InitialMappingPlan& mappingPlan = *mappingPlanResult.GetValue();
    const InitialTranslationResourcePlan& translationPlan = *translationPlanResult.GetValue();
    if (!HasIdentityGlobalMappingCoverage(mappingPlan, tableAddress, tableEnd, false, true, true,
                                          Kernel::Memory::InitialMappingOwner::NucleusCode))
    {
        return FailInstall(Ia64RuntimeInterruptVectorTableInstallError::TableMappingMissing);
    }
    if (!HasTranslationRegisterCoverage(translationPlan, tableAddress, tableEnd, true))
    {
        return FailInstall(Ia64RuntimeInterruptVectorTableInstallError::TableInstructionCoverageMissing);
    }
    if (!HasIdentityGlobalMappingCoverage(mappingPlan, runtimeObjects.VirtualStart, runtimeObjectEnd, true, false,
                                          false, Kernel::Memory::InitialMappingOwner::NucleusWritableData))
    {
        return FailInstall(Ia64RuntimeInterruptVectorTableInstallError::RuntimeObjectMappingMissing);
    }
    if (!HasTranslationRegisterCoverage(translationPlan, runtimeObjects.VirtualStart, runtimeObjectEnd, false))
    {
        return FailInstall(Ia64RuntimeInterruptVectorTableInstallError::RuntimeObjectDataCoverageMissing);
    }

    const Ia64ActiveRuntimeTranslationBindingError admission =
        bindings.ValidateAdmission(processor, runtimeObjects.VirtualStart, runtimeObjectEnd, *this);
    if (admission != Ia64ActiveRuntimeTranslationBindingError::None)
    {
        return FailInstall(Ia64RuntimeInterruptVectorTableInstallError::BindingAdmissionRejected);
    }

    const Ia64RuntimeInterruptVectorTableContext expectedContext{
        .BindingsWireAddress = AddressOf(&bindings.m_wire),
        .ProcessorIndex = processor.GetValue(),
        .SystemCallEntryContextAddress = 0U,
        .ExternalInterruptEntryContextAddress = 0U,
        .PageFaultEntryContextAddress = 0U,
        .UserFaultEntryContextAddress = 0U,
        .StopStorageAddress = m_stopStorageAddress,
    };
    const Ia64RuntimeInterruptVectorTableContext& context = m_contexts[processor.GetValue()];
    if (!IsContextZero(context) && !IsContextExact(context, expectedContext))
    {
        return FailInstall(Ia64RuntimeInterruptVectorTableInstallError::ContextAlreadyConfigured);
    }

    return InstallResult::Success(Ia64RuntimeInterruptVectorTableInstallSummary{
        .TableAddress = tableAddress,
        .ContextAddress = AddressOf(&context),
        .SizeBytes = static_cast<Foundation::UInt32>(Ia64RuntimeInterruptVectorTableSizeBytes),
        .ProcessorIndex = processor.GetValue(),
        .InstalledProcessorMask = static_cast<Foundation::UInt8>(m_installedProcessorMask | processorBit),
        .Reserved = 0U,
    });
}

Foundation::Result<Ia64RuntimeInterruptVectorTableInstallSummary, Ia64RuntimeInterruptVectorTableInstallError>
Ia64RuntimeInterruptVectorTable::InstallAndAdmit(const Kernel::Scheduler::CpuId processor,
                                                 const InitialAddressSpacePreparation& preparation,
                                                 Ia64ActiveRuntimeTranslationBindings& bindings,
                                                 const Ia64RuntimeObjectRange runtimeObjects) noexcept
{
    const InstallResult validation = ValidateInstall(processor, preparation, bindings, runtimeObjects);
    if (!validation.HasValue())
    {
        return validation;
    }

    const Foundation::UInt64 runtimeObjectEnd = runtimeObjects.VirtualStart + runtimeObjects.SizeBytes;
    Ia64RuntimeInterruptVectorTableContext& context = m_contexts[processor.GetValue()];
    const Ia64RuntimeInterruptVectorTableContext expectedContext{
        .BindingsWireAddress = AddressOf(&bindings.m_wire),
        .ProcessorIndex = processor.GetValue(),
        .SystemCallEntryContextAddress = 0U,
        .ExternalInterruptEntryContextAddress = 0U,
        .PageFaultEntryContextAddress = 0U,
        .UserFaultEntryContextAddress = 0U,
        .StopStorageAddress = m_stopStorageAddress,
    };
    const bool contextConfiguredByAttempt = IsContextZero(context);
    if (contextConfiguredByAttempt)
    {
        context = expectedContext;
    }

    if (AsteriaIa64InstallRuntimeInterruptVectorTable(AddressOf(&context), validation.GetValue().TableAddress) != 0U)
    {
        if (contextConfiguredByAttempt)
        {
            context = Ia64RuntimeInterruptVectorTableContext{};
        }
        return FailInstall(Ia64RuntimeInterruptVectorTableInstallError::UnsupportedProcessorState);
    }

    if (bindings.AdmitProcessor(processor, runtimeObjects.VirtualStart, runtimeObjectEnd, *this) !=
        Ia64ActiveRuntimeTranslationBindingError::None)
    {
        AsteriaIa64FailStopRuntimeInterruptVectorTableInstall();
    }
    if (m_runtimeObjectEnd == 0U)
    {
        m_runtimeObjectStart = runtimeObjects.VirtualStart;
        m_runtimeObjectEnd = runtimeObjectEnd;
    }
    m_installedProcessorMask |= ProcessorBit(processor);
    return validation;
}

Ia64RuntimeInterruptVectorTablePublicationError
Ia64RuntimeInterruptVectorTable::ValidateEntryContextPlacement(const Kernel::Scheduler::CpuId processor,
                                                               const Foundation::UInt64 contextAddress,
                                                               const Foundation::UInt64 contextSizeBytes) const noexcept
{
    if (!processor.IsValid())
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::InvalidProcessor;
    }
    if ((m_installedProcessorMask & ProcessorBit(processor)) == 0U)
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::ProcessorNotInstalled;
    }
    if (contextAddress == 0U)
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::NullContext;
    }
    if ((contextAddress & 15U) != 0U)
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::MisalignedContext;
    }
    Foundation::UInt64 contextEnd{0U};
    if (!TryCalculateEnd(contextAddress, contextSizeBytes, contextEnd) || (contextEnd > RegionZeroLimit))
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::ContextOutsideRegionZero;
    }
    if ((m_runtimeObjectStart >= m_runtimeObjectEnd) ||
        !IsRangeInside(m_runtimeObjectStart, m_runtimeObjectEnd, contextAddress, contextSizeBytes))
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::ContextOutsideRuntimeObjectRange;
    }
    return Ia64RuntimeInterruptVectorTablePublicationError::None;
}

Ia64RuntimeInterruptVectorTablePublicationError Ia64RuntimeInterruptVectorTable::ValidateEntryStorage(
    const Foundation::UInt64 contextAddress, const Foundation::UInt64 contextSizeBytes,
    const Foundation::UInt64 frameAddress, const Foundation::UInt64 frameSizeBytes,
    const Foundation::UInt64 memoryStackStart, const Foundation::UInt64 memoryStackEnd,
    const Foundation::UInt64 rseBackingStoreStart, const Foundation::UInt64 rseBackingStoreEnd) const noexcept
{
    Foundation::UInt64 frameEnd{0U};
    const bool frameInside = TryCalculateEnd(frameAddress, frameSizeBytes, frameEnd) &&
                             IsRangeInside(m_runtimeObjectStart, m_runtimeObjectEnd, frameAddress, frameSizeBytes);
    const bool memoryStackInside = IsExactSizedRangeInside(m_runtimeObjectStart, m_runtimeObjectEnd, memoryStackStart,
                                                           memoryStackEnd, System::Ia64SystemCallMemoryStackSizeBytes);
    const bool rseBackingStoreInside =
        IsExactSizedRangeInside(m_runtimeObjectStart, m_runtimeObjectEnd, rseBackingStoreStart, rseBackingStoreEnd,
                                System::Ia64SystemCallRseBackingStoreSizeBytes);
    if (!frameInside || !memoryStackInside || !rseBackingStoreInside)
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::ReferencedStorageOutsideRuntimeObjectRange;
    }
    const Foundation::UInt64 contextEnd = contextAddress + contextSizeBytes;
    if (RangesOverlap(contextAddress, contextEnd, frameAddress, frameEnd) ||
        RangesOverlap(contextAddress, contextEnd, memoryStackStart, memoryStackEnd) ||
        RangesOverlap(contextAddress, contextEnd, rseBackingStoreStart, rseBackingStoreEnd))
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::ReferencedStorageOverlapsContext;
    }
    return Ia64RuntimeInterruptVectorTablePublicationError::None;
}

Ia64RuntimeInterruptVectorTablePublicationError Ia64RuntimeInterruptVectorTable::ValidateSystemCallEntryContext(
    const Kernel::Scheduler::CpuId processor, const System::Ia64SystemCallEntryContext* const context) const noexcept
{
    const Ia64RuntimeInterruptVectorTablePublicationError placement =
        ValidateEntryContextPlacement(processor, AddressOf(context), sizeof(System::Ia64SystemCallEntryContext));
    if (placement != Ia64RuntimeInterruptVectorTablePublicationError::None)
    {
        return placement;
    }
    if (context->ProcessorIndex != processor.GetValue())
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::ProcessorMismatch;
    }
    if (context->Reserved != 0U)
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::ReservedNotZero;
    }
    if (System::ValidateIa64SystemCallTrustedContext(*context) != System::Ia64SystemCallContextValidationError::None)
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::InvalidSystemCallContext;
    }
    return ValidateEntryStorage(AddressOf(context), sizeof(*context), context->FrameAddress,
                                sizeof(System::Ia64SystemCallEntryFrame), context->MemoryStackStart,
                                context->MemoryStackEnd, context->RseBackingStoreStart, context->RseBackingStoreEnd);
}

Ia64RuntimeInterruptVectorTablePublicationError Ia64RuntimeInterruptVectorTable::ValidateUserFaultEntryContext(
    const Kernel::Scheduler::CpuId processor, const Interrupt::Ia64UserFaultEntryContext* const context) const noexcept
{
    const Ia64RuntimeInterruptVectorTablePublicationError placement =
        ValidateEntryContextPlacement(processor, AddressOf(context), sizeof(Interrupt::Ia64UserFaultEntryContext));
    if (placement != Ia64RuntimeInterruptVectorTablePublicationError::None)
    {
        return placement;
    }
    if (context->ProcessorIndex != processor.GetValue())
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::ProcessorMismatch;
    }
    if (context->Reserved != 0U)
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::ReservedNotZero;
    }
    if (Interrupt::ValidateIa64UserFaultTrustedContext(*context) !=
        Interrupt::Ia64UserFaultContextValidationError::None)
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::InvalidUserFaultContext;
    }
    return ValidateEntryStorage(AddressOf(context), sizeof(*context), context->FrameAddress,
                                sizeof(Interrupt::Ia64PageFaultFrame), context->MemoryStackStart,
                                context->MemoryStackEnd, context->RseBackingStoreStart, context->RseBackingStoreEnd);
}

Ia64RuntimeInterruptVectorTablePublicationError
Ia64RuntimeInterruptVectorTable::Admit(Foundation::UInt64 (&admitted)[MaximumSelectableEntryContexts],
                                       const Foundation::UInt64 contextAddress) noexcept
{
    for (Foundation::UInt64& slot : admitted)
    {
        if ((slot == 0U) || (slot == contextAddress))
        {
            slot = contextAddress;
            return Ia64RuntimeInterruptVectorTablePublicationError::None;
        }
    }
    return Ia64RuntimeInterruptVectorTablePublicationError::AdmissionCapacityExceeded;
}

bool Ia64RuntimeInterruptVectorTable::IsAdmitted(const Foundation::UInt64 (&admitted)[MaximumSelectableEntryContexts],
                                                 const Foundation::UInt64 contextAddress) noexcept
{
    for (const Foundation::UInt64 slot : admitted)
    {
        if ((slot != 0U) && (slot == contextAddress))
        {
            return true;
        }
    }
    return false;
}

Ia64RuntimeInterruptVectorTablePublicationError Ia64RuntimeInterruptVectorTable::PublishSystemCallEntryContext(
    const Kernel::Scheduler::CpuId processor, const System::Ia64SystemCallEntryContext* const context) noexcept
{
    const Ia64RuntimeInterruptVectorTablePublicationError validation =
        ValidateSystemCallEntryContext(processor, context);
    if (validation != Ia64RuntimeInterruptVectorTablePublicationError::None)
    {
        return validation;
    }

    Ia64RuntimeInterruptVectorTableContext& target = m_contexts[processor.GetValue()];
    Synchronization::AtomicUInt64Reference publication{target.SystemCallEntryContextAddress};
    if (publication.LoadAcquire() != 0U)
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::AlreadyPublished;
    }
    const Ia64RuntimeInterruptVectorTablePublicationError admitted =
        Admit(m_admittedSystemCallContexts[processor.GetValue()], AddressOf(context));
    if (admitted != Ia64RuntimeInterruptVectorTablePublicationError::None)
    {
        return admitted;
    }

    publication.StoreRelease(AddressOf(context));
    return Ia64RuntimeInterruptVectorTablePublicationError::None;
}

Ia64RuntimeInterruptVectorTablePublicationError Ia64RuntimeInterruptVectorTable::AdmitSystemCallEntryContext(
    const Kernel::Scheduler::CpuId processor, const System::Ia64SystemCallEntryContext* const context) noexcept
{
    const Ia64RuntimeInterruptVectorTablePublicationError validation =
        ValidateSystemCallEntryContext(processor, context);
    if (validation != Ia64RuntimeInterruptVectorTablePublicationError::None)
    {
        return validation;
    }
    return Admit(m_admittedSystemCallContexts[processor.GetValue()], AddressOf(context));
}

Ia64RuntimeInterruptVectorTablePublicationError Ia64RuntimeInterruptVectorTable::SelectSystemCallEntryContext(
    const Kernel::Scheduler::CpuId processor, const System::Ia64SystemCallEntryContext* const context) noexcept
{
    if (!processor.IsValid())
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::InvalidProcessor;
    }
    if ((m_installedProcessorMask & ProcessorBit(processor)) == 0U)
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::ProcessorNotInstalled;
    }
    if (context == nullptr)
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::NullContext;
    }
    if (!IsAdmitted(m_admittedSystemCallContexts[processor.GetValue()], AddressOf(context)))
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::NotAdmitted;
    }
    Synchronization::AtomicUInt64Reference{m_contexts[processor.GetValue()].SystemCallEntryContextAddress}.StoreRelease(
        AddressOf(context));
    return Ia64RuntimeInterruptVectorTablePublicationError::None;
}

Ia64RuntimeInterruptVectorTablePublicationError Ia64RuntimeInterruptVectorTable::AdmitUserFaultEntryContext(
    const Kernel::Scheduler::CpuId processor, const Interrupt::Ia64UserFaultEntryContext* const context) noexcept
{
    const Ia64RuntimeInterruptVectorTablePublicationError validation =
        ValidateUserFaultEntryContext(processor, context);
    if (validation != Ia64RuntimeInterruptVectorTablePublicationError::None)
    {
        return validation;
    }
    return Admit(m_admittedUserFaultContexts[processor.GetValue()], AddressOf(context));
}

Ia64RuntimeInterruptVectorTablePublicationError Ia64RuntimeInterruptVectorTable::SelectUserFaultEntryContext(
    const Kernel::Scheduler::CpuId processor, const Interrupt::Ia64UserFaultEntryContext* const context) noexcept
{
    if (!processor.IsValid())
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::InvalidProcessor;
    }
    if ((m_installedProcessorMask & ProcessorBit(processor)) == 0U)
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::ProcessorNotInstalled;
    }
    if (context == nullptr)
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::NullContext;
    }
    if (!IsAdmitted(m_admittedUserFaultContexts[processor.GetValue()], AddressOf(context)))
    {
        return Ia64RuntimeInterruptVectorTablePublicationError::NotAdmitted;
    }
    Synchronization::AtomicUInt64Reference{m_contexts[processor.GetValue()].UserFaultEntryContextAddress}.StoreRelease(
        AddressOf(context));
    return Ia64RuntimeInterruptVectorTablePublicationError::None;
}

Ia64RuntimeExternalInterruptPublicationError Ia64RuntimeInterruptVectorTable::PublishExternalInterruptEntryContext(
    const Kernel::Scheduler::CpuId processor,
    const Interrupt::Ia64ExternalInterruptEntryContext* const context) noexcept
{
    if (!processor.IsValid())
    {
        return Ia64RuntimeExternalInterruptPublicationError::InvalidProcessor;
    }
    if ((m_installedProcessorMask & ProcessorBit(processor)) == 0U)
    {
        return Ia64RuntimeExternalInterruptPublicationError::ProcessorNotInstalled;
    }
    if (context == nullptr)
    {
        return Ia64RuntimeExternalInterruptPublicationError::NullContext;
    }

    const Foundation::UInt64 contextAddress = AddressOf(context);
    if ((contextAddress & (alignof(Interrupt::Ia64ExternalInterruptEntryContext) - 1U)) != 0U)
    {
        return Ia64RuntimeExternalInterruptPublicationError::MisalignedContext;
    }
    Foundation::UInt64 contextEnd{0U};
    if (!TryCalculateEnd(contextAddress, sizeof(*context), contextEnd) || (contextEnd > RegionZeroLimit))
    {
        return Ia64RuntimeExternalInterruptPublicationError::ContextOutsideRegionZero;
    }
    if ((m_runtimeObjectStart >= m_runtimeObjectEnd) ||
        !IsRangeInside(m_runtimeObjectStart, m_runtimeObjectEnd, contextAddress, sizeof(*context)))
    {
        return Ia64RuntimeExternalInterruptPublicationError::ContextOutsideRuntimeObjectRange;
    }
    if (context->ProcessorIndex != processor.GetValue())
    {
        return Ia64RuntimeExternalInterruptPublicationError::ProcessorMismatch;
    }
    if (context->TimerVector != Interrupt::Ia64ExternalInterruptTimerVector)
    {
        return Ia64RuntimeExternalInterruptPublicationError::InvalidVector;
    }
    if (context->ExternalPreemptionControlAddress != 0U)
    {
        return Ia64RuntimeExternalInterruptPublicationError::ReservedNotZero;
    }

    if ((context->EvidenceAddress == 0U) ||
        ((context->EvidenceAddress & (alignof(Interrupt::Ia64LocalTimerInterruptEvidence) - 1U)) != 0U))
    {
        return Ia64RuntimeExternalInterruptPublicationError::InvalidEvidenceAddress;
    }
    Foundation::UInt64 evidenceEnd{0U};
    if (!TryCalculateEnd(context->EvidenceAddress, sizeof(Interrupt::Ia64LocalTimerInterruptEvidence), evidenceEnd) ||
        (evidenceEnd > RegionZeroLimit))
    {
        return Ia64RuntimeExternalInterruptPublicationError::EvidenceOutsideRegionZero;
    }
    if (!IsRangeInside(m_runtimeObjectStart, m_runtimeObjectEnd, context->EvidenceAddress,
                       sizeof(Interrupt::Ia64LocalTimerInterruptEvidence)))
    {
        return Ia64RuntimeExternalInterruptPublicationError::EvidenceOutsideRuntimeObjectRange;
    }
    if (RangesOverlap(contextAddress, contextEnd, context->EvidenceAddress, evidenceEnd))
    {
        return Ia64RuntimeExternalInterruptPublicationError::EvidenceOverlapsContext;
    }

    const auto* const evidenceBytes =
        reinterpret_cast<const Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(context->EvidenceAddress));
    for (Foundation::Size index{0U}; index < sizeof(Interrupt::Ia64LocalTimerInterruptEvidence); ++index)
    {
        if (evidenceBytes[index] != 0U)
        {
            return Ia64RuntimeExternalInterruptPublicationError::EvidenceNotZero;
        }
    }

    Ia64RuntimeInterruptVectorTableContext& target = m_contexts[processor.GetValue()];
    Synchronization::AtomicUInt64Reference publication{target.ExternalInterruptEntryContextAddress};
    if (publication.LoadAcquire() != 0U)
    {
        return Ia64RuntimeExternalInterruptPublicationError::AlreadyPublished;
    }

    // The boot coordinator keeps PSR.i clear until this release store. The
    // context and the complete zero evidence range are already retained in the
    // locked DTR corridor, so no interrupt can observe a partial publication.
    publication.StoreRelease(contextAddress);
    return Ia64RuntimeExternalInterruptPublicationError::None;
}

Ia64RuntimePageFaultPublicationError Ia64RuntimeInterruptVectorTable::PublishPageFaultEntryContext(
    const Kernel::Scheduler::CpuId processor, const Interrupt::Ia64PageFaultEntryContext* const context) noexcept
{
    if (!processor.IsValid())
    {
        return Ia64RuntimePageFaultPublicationError::InvalidProcessor;
    }
    if ((m_installedProcessorMask & ProcessorBit(processor)) == 0U)
    {
        return Ia64RuntimePageFaultPublicationError::ProcessorNotInstalled;
    }
    if (context == nullptr)
    {
        return Ia64RuntimePageFaultPublicationError::NullContext;
    }

    const Foundation::UInt64 contextAddress = AddressOf(context);
    if ((contextAddress & (alignof(Interrupt::Ia64PageFaultEntryContext) - 1U)) != 0U)
    {
        return Ia64RuntimePageFaultPublicationError::MisalignedContext;
    }
    Foundation::UInt64 contextEnd{0U};
    if (!TryCalculateEnd(contextAddress, sizeof(*context), contextEnd) || (contextEnd > RegionZeroLimit))
    {
        return Ia64RuntimePageFaultPublicationError::ContextOutsideRegionZero;
    }
    if ((m_runtimeObjectStart >= m_runtimeObjectEnd) ||
        !IsRangeInside(m_runtimeObjectStart, m_runtimeObjectEnd, contextAddress, sizeof(*context)))
    {
        return Ia64RuntimePageFaultPublicationError::ContextOutsideRuntimeObjectRange;
    }
    if (context->ProcessorIndex != processor.GetValue())
    {
        return Ia64RuntimePageFaultPublicationError::ProcessorMismatch;
    }
    if ((context->PageNotPresentVectorOffset != Ia64RuntimePageNotPresentOffset) ||
        (context->KeyPermissionVectorOffset != Ia64RuntimeKeyPermissionOffset) ||
        (context->InstructionAccessRightsVectorOffset != Ia64RuntimeInstructionAccessRightsOffset) ||
        (context->DataAccessRightsVectorOffset != Ia64RuntimeDataAccessRightsOffset))
    {
        return Ia64RuntimePageFaultPublicationError::InvalidVectorIdentity;
    }
    if ((context->Reserved[0U] != 0U) || (context->Reserved[1U] != 0U))
    {
        return Ia64RuntimePageFaultPublicationError::ReservedNotZero;
    }

    const Foundation::UInt64 controlAddress = context->ControlAddress;
    Foundation::UInt64 controlEnd{0U};
    if ((controlAddress == 0U) || ((controlAddress & (alignof(Interrupt::Ia64PageFaultEntryControl) - 1U)) != 0U) ||
        !TryCalculateEnd(controlAddress, sizeof(Interrupt::Ia64PageFaultEntryControl), controlEnd) ||
        (controlEnd > RegionZeroLimit))
    {
        return Ia64RuntimePageFaultPublicationError::InvalidControlAddress;
    }
    if (!IsRangeInside(m_runtimeObjectStart, m_runtimeObjectEnd, controlAddress,
                       sizeof(Interrupt::Ia64PageFaultEntryControl)))
    {
        return Ia64RuntimePageFaultPublicationError::ControlOutsideRuntimeObjectRange;
    }
    const auto* const control = reinterpret_cast<const Interrupt::Ia64PageFaultEntryControl*>(
        static_cast<Foundation::UIntPointer>(controlAddress));
    if (Interrupt::ValidateConfiguredIa64PageFaultEntryControl(*control) !=
        Interrupt::Ia64PageFaultEntryControlError::None)
    {
        return Ia64RuntimePageFaultPublicationError::ControlNotConfigured;
    }
    if (control->ProcessorIndex != processor.GetValue())
    {
        return Ia64RuntimePageFaultPublicationError::ProcessorMismatch;
    }

    Foundation::UInt64 frameEnd{0U};
    if ((control->FrameAddress == 0U) ||
        ((control->FrameAddress & (alignof(Interrupt::Ia64PageFaultFrame) - 1U)) != 0U) ||
        !TryCalculateEnd(control->FrameAddress, sizeof(Interrupt::Ia64PageFaultFrame), frameEnd) ||
        (frameEnd > RegionZeroLimit))
    {
        return Ia64RuntimePageFaultPublicationError::InvalidFrameAddress;
    }
    if (!IsRangeInside(m_runtimeObjectStart, m_runtimeObjectEnd, control->FrameAddress,
                       sizeof(Interrupt::Ia64PageFaultFrame)))
    {
        return Ia64RuntimePageFaultPublicationError::FrameOutsideRuntimeObjectRange;
    }
    const auto* const frame = reinterpret_cast<const Interrupt::Ia64PageFaultFrame*>(
        static_cast<Foundation::UIntPointer>(control->FrameAddress));
    if (!Interrupt::Ia64PageFaultFrameIsPristine(*frame))
    {
        return Ia64RuntimePageFaultPublicationError::FrameNotPristine;
    }

    if ((control->CoordinatorMemoryStackStart & (Interrupt::Ia64PageFaultStoragePageSizeBytes - 1U)) != 0U ||
        !HasExactSize(control->CoordinatorMemoryStackStart, control->CoordinatorMemoryStackEnd,
                      Interrupt::Ia64PageFaultCoordinatorStackSizeBytes) ||
        (control->CoordinatorMemoryStackEnd > RegionZeroLimit))
    {
        return Ia64RuntimePageFaultPublicationError::InvalidCoordinatorMemoryStack;
    }
    if (!IsExactSizedRangeInside(m_runtimeObjectStart, m_runtimeObjectEnd, control->CoordinatorMemoryStackStart,
                                 control->CoordinatorMemoryStackEnd, Interrupt::Ia64PageFaultCoordinatorStackSizeBytes))
    {
        return Ia64RuntimePageFaultPublicationError::CoordinatorMemoryStackOutsideRuntimeObjectRange;
    }
    if ((control->CoordinatorRseBackingStoreStart & (Interrupt::Ia64PageFaultStoragePageSizeBytes - 1U)) != 0U ||
        !HasExactSize(control->CoordinatorRseBackingStoreStart, control->CoordinatorRseBackingStoreEnd,
                      Interrupt::Ia64PageFaultCoordinatorRseBackingStoreSizeBytes) ||
        (control->CoordinatorRseBackingStoreEnd > RegionZeroLimit))
    {
        return Ia64RuntimePageFaultPublicationError::InvalidCoordinatorRseBackingStore;
    }
    if (!IsExactSizedRangeInside(m_runtimeObjectStart, m_runtimeObjectEnd, control->CoordinatorRseBackingStoreStart,
                                 control->CoordinatorRseBackingStoreEnd,
                                 Interrupt::Ia64PageFaultCoordinatorRseBackingStoreSizeBytes))
    {
        return Ia64RuntimePageFaultPublicationError::CoordinatorRseBackingStoreOutsideRuntimeObjectRange;
    }
    if (RangesOverlap(contextAddress, contextEnd, control->InterruptedRseBackingStoreStart,
                      control->InterruptedRseBackingStoreEnd))
    {
        return Ia64RuntimePageFaultPublicationError::ReferencedStorageOverlaps;
    }

    const Foundation::UInt64 starts[5U]{
        contextAddress,
        controlAddress,
        control->FrameAddress,
        control->CoordinatorMemoryStackStart,
        control->CoordinatorRseBackingStoreStart,
    };
    const Foundation::UInt64 ends[5U]{
        contextEnd, controlEnd, frameEnd, control->CoordinatorMemoryStackEnd, control->CoordinatorRseBackingStoreEnd,
    };
    for (Foundation::Size first{0U}; first < 5U; ++first)
    {
        for (Foundation::Size second{first + 1U}; second < 5U; ++second)
        {
            if (RangesOverlap(starts[first], ends[first], starts[second], ends[second]))
            {
                return Ia64RuntimePageFaultPublicationError::ReferencedStorageOverlaps;
            }
        }
    }

    Ia64RuntimeInterruptVectorTableContext& target = m_contexts[processor.GetValue()];
    Synchronization::AtomicUInt64Reference publication{target.PageFaultEntryContextAddress};
    if (publication.LoadAcquire() != 0U)
    {
        return Ia64RuntimePageFaultPublicationError::AlreadyPublished;
    }

    publication.StoreRelease(contextAddress);
    return Ia64RuntimePageFaultPublicationError::None;
}

Foundation::Result<Ia64RuntimeRefillAction, Ia64RuntimeRefillError>
Ia64RuntimeInterruptVectorTable::ModelRefill(const Ia64RuntimeInterruptVectorTableContext& context,
                                             const Foundation::UInt64 faultAddress, const Ia64RuntimeRefillKind kind,
                                             const Ia64RuntimeRefillProcessorState processorState) noexcept
{
    if ((processorState.CurrentProcessorStatus & CurrentProcessorStatusForbiddenMask) != 0U ||
        ((processorState.CurrentProcessorStatus ^ processorState.InterruptedProcessorStatus) & TranslationStateMask) !=
            0U ||
        (processorState.CurrentProcessorStatus & TranslationStateMask) != TranslationStateMask)
    {
        return FailRefill(Ia64RuntimeRefillError::IncompatibleCurrentState);
    }
    if ((processorState.InterruptedProcessorStatus & InterruptedProcessorStatusRequiredMask) !=
            InterruptedProcessorStatusRequiredMask ||
        (processorState.InterruptedProcessorStatus & InterruptedProcessorStatusForbiddenMask) != 0U)
    {
        return FailRefill(Ia64RuntimeRefillError::IncompatibleInterruptedState);
    }
    if ((processorState.InterruptionStatus & InterruptionStatusNestedMask) != 0U)
    {
        return FailRefill(Ia64RuntimeRefillError::NestedInterruption);
    }
    const bool hasIncompleteRegisterFrame =
        (processorState.InterruptionStatus & InterruptionStatusIncompleteFrameMask) != 0U;
    const bool hasRseReference = (processorState.InterruptionStatus & InterruptionStatusRseMask) != 0U;
    if (hasIncompleteRegisterFrame && ((kind != Ia64RuntimeRefillKind::Data) || !hasRseReference))
    {
        return FailRefill(Ia64RuntimeRefillError::IncompleteRegisterFrame);
    }
    if (hasRseReference && (kind != Ia64RuntimeRefillKind::Data))
    {
        return FailRefill(Ia64RuntimeRefillError::RseReference);
    }
    if ((kind != Ia64RuntimeRefillKind::Instruction) && (kind != Ia64RuntimeRefillKind::Data))
    {
        return FailRefill(Ia64RuntimeRefillError::InvalidKind);
    }

    const Foundation::UInt64 contextAddress = AddressOf(&context);
    if ((contextAddress & 15U) != 0U || (contextAddress >= RegionZeroLimit))
    {
        return FailRefill(Ia64RuntimeRefillError::InvalidContext);
    }
    if (context.ProcessorIndex >= Kernel::Scheduler::MaximumProcessorCount)
    {
        return FailRefill(Ia64RuntimeRefillError::InvalidProcessor);
    }
    if ((context.BindingsWireAddress == 0U) || ((context.BindingsWireAddress & 7U) != 0U) ||
        (context.BindingsWireAddress >= RegionZeroLimit))
    {
        return FailRefill(Ia64RuntimeRefillError::InvalidBindingsPointer);
    }

    const auto* const bindings = reinterpret_cast<const Ia64ActiveRuntimeTranslationBindingsWireStorage*>(
        static_cast<Foundation::UIntPointer>(context.BindingsWireAddress));
    const Foundation::UInt64 ledgerAddress = bindings->Slots[context.ProcessorIndex].LoadAcquire();
    if (ledgerAddress == 0U)
    {
        return FailRefill(Ia64RuntimeRefillError::InvalidBindingSlot);
    }
    if (ledgerAddress == 1U)
    {
        return FailRefill(Ia64RuntimeRefillError::NoActiveLedger);
    }
    if (((ledgerAddress & 7U) != 0U) || (ledgerAddress >= RegionZeroLimit))
    {
        return FailRefill(Ia64RuntimeRefillError::InvalidLedgerPointer);
    }

    const auto* const ledger = reinterpret_cast<const Ia64RuntimeTranslationLedgerWireStorage*>(
        static_cast<Foundation::UIntPointer>(ledgerAddress));
    const Foundation::UInt64 publication = ledger->PublicationWord.LoadAcquire();
    if ((publication == 0U) || (publication > (Kernel::Memory::MaximumRuntimeAddressSpaceMappings + 1U)))
    {
        return FailRefill(Ia64RuntimeRefillError::InvalidPublication);
    }
    const Foundation::UInt64 recordCount = publication - 1U;
    const Foundation::UInt64 pageAddress = faultAddress & ~PageOffsetMask;
    for (Foundation::UInt64 index{0U}; index < recordCount; ++index)
    {
        const Ia64RuntimeTranslationLedgerWireRecord& record = ledger->Records[index];
        if (pageAddress < record.VirtualStart)
        {
            continue;
        }
        const Foundation::UInt64 pageIndex = (pageAddress - record.VirtualStart) >> 12U;
        if (pageIndex >= record.PageCount)
        {
            continue;
        }
        const Foundation::UInt64 pageOffset = pageIndex << 12U;
        const Foundation::UInt64 physicalPage = (record.BaseTranslationWord & PhysicalPageNumberMask) + pageOffset;
        const Foundation::UInt64 invariantAttributes = record.BaseTranslationWord & ~PhysicalPageNumberMask;
        return RefillResult::Success(Ia64RuntimeRefillAction{
            .TranslationWord = physicalPage | invariantAttributes,
            .TranslationInsertion = Ia64RuntimeTranslationInsertion,
            .FaultingAddress = pageAddress,
            .Kind = kind,
            .Outcome = Ia64RuntimeRefillOutcome::Insert,
            .Reserved = {},
        });
    }
    if ((processorState.InterruptedProcessorStatus & PrivilegeMask) == UserPrivilege)
    {
        return RefillResult::Success(Ia64RuntimeRefillAction{
            .TranslationWord = 0U,
            .TranslationInsertion = 0U,
            .FaultingAddress = faultAddress,
            .Kind = kind,
            .Outcome = Ia64RuntimeRefillOutcome::UserFault,
            .Reserved = {},
        });
    }
    return FailRefill(Ia64RuntimeRefillError::MappingNotFound);
}
} // namespace Asteria::Platform::Ia64::Memory
