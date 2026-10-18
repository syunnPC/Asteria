// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Memory/InitialAddressSpacePreparation.hpp"

#include "Platform/Amd64/Memory/Amd64ControlRegister4.hpp"

#include "Runtime/Memory.hpp"

extern "C" Asteria::Foundation::UInt64 AsteriaAmd64ActivateInitialAddressSpace(
    Asteria::Foundation::UInt64 rootPhysicalAddress, Asteria::Foundation::UInt64 initialStackPointer,
    Asteria::Foundation::UInt64 globalDescriptorRegisterAddress,
    Asteria::Foundation::UInt64 interruptDescriptorRegisterAddress,
    Asteria::Foundation::UInt64 currentRootPhysicalAddress, Asteria::Foundation::UInt64 packedSelectors) noexcept;

extern "C" [[noreturn]] void AsteriaAmd64FailStopInterrupt() noexcept;

#if defined(ASTERIA_TARGET_AMD64) && defined(__x86_64__)
extern "C" const Asteria::Foundation::UInt8 AsteriaAmd64ActivationTextStart[];
extern "C" const Asteria::Foundation::UInt8 AsteriaAmd64ActivationTextEnd[];
extern "C" const Asteria::Foundation::UInt8 AsteriaAmd64ActivationReadOnlyStart[];
extern "C" const Asteria::Foundation::UInt8 AsteriaAmd64ActivationReadOnlyEnd[];
#endif

namespace Asteria::Platform::Amd64::Memory
{
namespace
{
using Kernel::Memory::InitialMapping;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MappingPermissions;
using Kernel::Memory::MemoryCachePolicy;

constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 PageSizeBytes{4096U};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};
constexpr Foundation::UInt64 Cr0ProtectedModeEnableBit{1U << 0U};
constexpr Foundation::UInt64 Cr0WriteProtectBit{1U << 16U};
constexpr Foundation::UInt64 Cr0NotWriteThroughBit{1U << 29U};
constexpr Foundation::UInt64 Cr0CacheDisableBit{1U << 30U};
constexpr Foundation::UInt64 Cr0PagingBit{1U << 31U};
constexpr Foundation::UInt64 Cr4PhysicalAddressExtensionBit{1U << 5U};
constexpr Foundation::UInt64 Cr4GlobalPagesBit{1U << 7U};
constexpr Foundation::UInt64 Cr4FiveLevelPagingBit{1U << 12U};
constexpr Foundation::UInt64 Cr4ProcessContextIdentifierBit{1U << 17U};
constexpr Foundation::UInt64 Cr4ControlFlowEnforcementBit{1U << 23U};
constexpr Foundation::UInt64 Cr4ProtectionKeysSupervisorBit{1U << 24U};
constexpr Foundation::UInt64 Cr4LinearAddressSpaceSeparationBit{1U << 27U};
constexpr Foundation::UInt64 Cr4SupervisorLinearAddressMaskingBit{1U << 28U};
// The control-register policy the activation requires of the state it keeps
// once it has established write protection and execute-disable and cleared
// global pages and PCIDs (EstablishInitialControlState): protected mode,
// write protection and paging on; global pages, five-level paging, PCIDs,
// CET, supervisor protection keys, LASS and supervisor LAM off. Nothing
// after the activation sets a forbidden bit.
constexpr Foundation::UInt64 RequiredControlRegister0Bits{Cr0ProtectedModeEnableBit | Cr0WriteProtectBit |
                                                          Cr0PagingBit};
constexpr Foundation::UInt64 ForbiddenControlRegister4Bits{
    (Cr4GlobalPagesBit | Cr4FiveLevelPagingBit | Cr4ProcessContextIdentifierBit | Cr4ControlFlowEnforcementBit |
     Cr4ProtectionKeysSupervisorBit | Cr4LinearAddressSpaceSeparationBit | Cr4SupervisorLinearAddressMaskingBit)};
constexpr Foundation::UInt64 EferLongModeEnableBit{1U << 8U};
constexpr Foundation::UInt64 EferLongModeActiveBit{1U << 10U};
constexpr Foundation::UInt64 EferExecuteDisableBit{1U << 11U};
constexpr Foundation::UInt64 RflagsReservedOneBit{1U << 1U};
constexpr Foundation::UInt64 RflagsTrapBit{1U << 8U};
constexpr Foundation::UInt64 RflagsDirectionBit{1U << 10U};
constexpr Foundation::UInt64 RflagsNestedTaskBit{1U << 14U};
constexpr Foundation::UInt64 RflagsVirtual8086Bit{1U << 17U};
constexpr Foundation::UInt64 MtrrEnableBit{1U << 11U};
constexpr Foundation::UInt64 FixedMtrrEnableBit{1U << 10U};
constexpr Foundation::UInt64 MtrrValidBit{1U << 11U};
constexpr Foundation::UInt64 MtrrVariableCountMask{0xFFU};
constexpr Foundation::UInt64 MtrrFixedSupportedBit{1U << 8U};
constexpr Foundation::UInt64 UncachedMemoryType{0U};
constexpr Foundation::UInt64 WriteBackMemoryType{6U};
#if defined(ASTERIA_TARGET_AMD64) && defined(__x86_64__)
constexpr Foundation::UInt64 ModelSpecificRegisterMtrrCapability{0xFEU};
constexpr Foundation::UInt64 ModelSpecificRegisterMtrrDefaultType{0x2FFU};
constexpr Foundation::UInt64 ModelSpecificRegisterPat{0x277U};
constexpr Foundation::UInt64 ModelSpecificRegisterEfer{0xC000'0080U};
constexpr Foundation::UInt32 CpuIdBasicFeatureLeaf{1U};
constexpr Foundation::UInt32 CpuIdMtrrBit{1U << 12U};
#endif
constexpr Foundation::UInt64 RequiredStackPermissions{static_cast<Foundation::UInt64>(MappingPermission::Read) |
                                                      static_cast<Foundation::UInt64>(MappingPermission::Write)};
constexpr Foundation::Size GlobalDescriptorTableSizeBytes{InitialGlobalDescriptorTableEntryCount *
                                                          sizeof(Foundation::UInt64)};
constexpr Foundation::Size InterruptDescriptorTableSizeBytes{InitialInterruptDescriptorTableWordCount *
                                                             sizeof(Foundation::UInt64)};
static_assert(GlobalDescriptorTableSizeBytes + InterruptDescriptorTableSizeBytes ==
              InitialDescriptorTableStorageSizeBytes);
constexpr Foundation::UInt64 CodeDescriptor{0x00AF'9A00'0000'FFFFULL};
constexpr Foundation::UInt64 DataDescriptor{0x00CF'9200'0000'FFFFULL};
constexpr Foundation::UInt8 InterruptGateAttributes{0x8EU};
#if defined(ASTERIA_TARGET_AMD64) && defined(__x86_64__)
constexpr Foundation::UInt64 FixedMtrrMsrs[InitialFixedMtrrRegisterCount]{
    0x250U, 0x258U, 0x259U, 0x268U, 0x269U, 0x26AU, 0x26BU, 0x26CU, 0x26DU, 0x26EU, 0x26FU,
};
#endif

[[nodiscard]] constexpr InitialAddressSpacePreparationFailure MakeFailure(
    const InitialAddressSpacePreparationError error,
    const InitialProcessorStateCaptureError captureError = InitialProcessorStateCaptureError::None,
    const InitialPageTablePreparationError pageTableError = InitialPageTablePreparationError::None,
    const InitialPageTableBuildError pageTableBuildError = InitialPageTableBuildError::None,
    const InitialPageTableVerificationError pageTableVerificationError = InitialPageTableVerificationError::None,
    const Kernel::Memory::InitialMappingPlanError mappingError = Kernel::Memory::InitialMappingPlanError::None,
    const Foundation::Size mappingIndex = 0U, const Foundation::UInt32 adapterStatus = 0U) noexcept
{
    return InitialAddressSpacePreparationFailure{
        .Error = error,
        .CaptureError = captureError,
        .PageTableError = pageTableError,
        .PageTableBuildError = pageTableBuildError,
        .PageTableVerificationError = pageTableVerificationError,
        .MappingError = mappingError,
        .AdapterStatus = adapterStatus,
        .Reserved = 0U,
        .MappingIndex = mappingIndex,
    };
}

[[nodiscard]] bool TryGetRangeEnd(const Foundation::UInt64 start, const Foundation::UInt64 bytes,
                                  Foundation::UInt64& end) noexcept
{
    if ((bytes == 0U) || (start > (MaximumUInt64 - bytes)))
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

template <typename TObject>
[[nodiscard]] bool RangeOverlapsObject(const Foundation::UInt64 start, const Foundation::UInt64 end,
                                       const TObject& object) noexcept
{
    const Foundation::UInt64 objectStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&object));
    Foundation::UInt64 objectEnd{0U};
    return !TryGetRangeEnd(objectStart, sizeof(object), objectEnd) || RangesOverlap(start, end, objectStart, objectEnd);
}

[[nodiscard]] bool IsZeroBytes(const Foundation::UInt8* const bytes, const Foundation::Size sizeBytes) noexcept
{
    const volatile Foundation::UInt8* const observed = bytes;
    for (Foundation::Size offset{0U}; offset < sizeBytes; ++offset)
    {
        if (observed[offset] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool PagingCapabilitiesAreValid(const PagingCapabilities& capabilities) noexcept
{
    return (capabilities.PhysicalAddressBits >= 32U) && (capabilities.PhysicalAddressBits <= 52U) &&
           capabilities.FourLevelPagingEnabled && capabilities.ExecuteDisableEnabled &&
           capabilities.SupervisorWriteProtectEnabled && capabilities.GlobalPagesEnabled &&
           capabilities.PatLayoutConfigured && (capabilities.Reserved16 == 0U) && (capabilities.Reserved == 0U);
}

[[nodiscard]] bool MappingSummaryIsValid(const BootstrapMappingPlanSummary& summary) noexcept
{
    Foundation::UInt64 reservedEnd{0U};
    const Foundation::UInt64 reservedBytes = BootstrapReservedPageCount << 12U;
    return (summary.ReservedPhysicalStart != 0U) && ((summary.ReservedPhysicalStart & PageOffsetMask) == 0U) &&
           TryGetRangeEnd(summary.ReservedPhysicalStart, reservedBytes, reservedEnd) &&
           (reservedEnd > summary.ReservedPhysicalStart) &&
           (summary.StackPhysicalStart == (summary.ReservedPhysicalStart + (BootstrapStackPageOffset << 12U))) &&
           (summary.PageTableStoragePhysicalStart ==
            (summary.ReservedPhysicalStart + (BootstrapPageTableStoragePageOffset << 12U))) &&
           (summary.DescriptorTableStoragePhysicalStart ==
            (summary.ReservedPhysicalStart + (BootstrapDescriptorTableStoragePageOffset << 12U))) &&
           (summary.MappingCount == BootstrapInitialMappingCount) && (summary.Reserved16 == 0U) &&
           (summary.Reserved32 == 0U);
}

[[nodiscard]] bool BootstrapMappingProfileIsValid(const Kernel::Memory::InitialMappingPlan& mappings,
                                                  const BootstrapMappingPlanSummary& mappingSummary) noexcept
{
    constexpr InitialMappingOwner ImageOwners[Firmware::Amd64LoadedImageSectionCount]{
        InitialMappingOwner::NucleusCode,         InitialMappingOwner::NucleusReadOnlyData,
        InitialMappingOwner::NucleusReadOnlyData, InitialMappingOwner::NucleusWritableData,
        InitialMappingOwner::NucleusReadOnlyData, InitialMappingOwner::NucleusReadOnlyData,
        InitialMappingOwner::NucleusWritableData,
    };
    const MappingPermissions readOnly{MappingPermission::Read};
    const MappingPermissions executable = readOnly.With(MappingPermission::Execute);
    const MappingPermissions writable = readOnly.With(MappingPermission::Write);
    constexpr Foundation::UInt64 MaximumImagePages{Firmware::MaximumAmd64LoadedImageBytes >> 12U};
    Foundation::UInt64 reservedEnd{0U};

    if (!mappings.IsSealed() || (mappings.GetMappingCount() != BootstrapInitialMappingCount) ||
        !TryGetRangeEnd(mappingSummary.ReservedPhysicalStart, BootstrapReservedPageCount << 12U, reservedEnd))
    {
        return false;
    }
    Foundation::Size imageIndex{0U};
    Foundation::Size stackCount{0U};
    Foundation::Size pageTableCount{0U};
    Foundation::Size descriptorTableCount{0U};
    Foundation::UInt64 imagePages{0U};
    Foundation::UInt64 expectedImageStart{0U};
    for (Foundation::Size index{0U}; index < mappings.GetMappingCount(); ++index)
    {
        const auto mappingResult = mappings.GetMapping(index);
        if (!mappingResult.HasValue())
        {
            return false;
        }
        const InitialMapping mapping = mappingResult.GetValue();
        Foundation::UInt64 mappingEnd{0U};
        if ((mapping.Reserved != 0U) || mapping.Permissions.Contains(MappingPermission::Global) ||
            (mapping.CachePolicy != MemoryCachePolicy::WriteBack) ||
            (mapping.VirtualStart.GetValue() != mapping.PhysicalStart.GetValue()) ||
            !TryGetRangeEnd(mapping.VirtualStart.GetValue(), mapping.Pages.GetValue() << 12U, mappingEnd))
        {
            return false;
        }

        const bool isImageMapping = (mapping.Owner == InitialMappingOwner::NucleusCode) ||
                                    (mapping.Owner == InitialMappingOwner::NucleusReadOnlyData) ||
                                    (mapping.Owner == InitialMappingOwner::NucleusWritableData);
        if (isImageMapping)
        {
            if ((imageIndex >= Firmware::Amd64LoadedImageSectionCount) || (mapping.Owner != ImageOwners[imageIndex]) ||
                ((imageIndex != 0U) && (mapping.VirtualStart.GetValue() != expectedImageStart)) ||
                (mapping.Pages.GetValue() > (MaximumImagePages - imagePages)) ||
                RangesOverlap(mapping.PhysicalStart.GetValue(), mappingEnd, mappingSummary.ReservedPhysicalStart,
                              reservedEnd))
            {
                return false;
            }
            const MappingPermissions expectedPermissions =
                mapping.Owner == InitialMappingOwner::NucleusCode
                    ? executable
                    : (mapping.Owner == InitialMappingOwner::NucleusWritableData ? writable : readOnly);
            if (mapping.Permissions.GetBits() != expectedPermissions.GetBits())
            {
                return false;
            }
            imagePages += mapping.Pages.GetValue();
            expectedImageStart = mappingEnd;
            ++imageIndex;
            continue;
        }

        if (mapping.Permissions.GetBits() != writable.GetBits())
        {
            return false;
        }
        switch (mapping.Owner)
        {
        case InitialMappingOwner::NucleusStack:
            ++stackCount;
            if ((mapping.PhysicalStart.GetValue() != mappingSummary.StackPhysicalStart) ||
                (mapping.Pages.GetValue() != BootstrapStackPageCount))
            {
                return false;
            }
            break;
        case InitialMappingOwner::PageTable:
            ++pageTableCount;
            if ((mapping.PhysicalStart.GetValue() != mappingSummary.PageTableStoragePhysicalStart) ||
                (mapping.Pages.GetValue() != BootstrapPageTableStoragePageCount))
            {
                return false;
            }
            break;
        case InitialMappingOwner::NucleusDescriptorTables:
            ++descriptorTableCount;
            if ((mapping.PhysicalStart.GetValue() != mappingSummary.DescriptorTableStoragePhysicalStart) ||
                (mapping.Pages.GetValue() != BootstrapDescriptorTableStoragePageCount))
            {
                return false;
            }
            break;
        default:
            return false;
        }
    }
    return (imageIndex == Firmware::Amd64LoadedImageSectionCount) && (imagePages != 0U) && (stackCount == 1U) &&
           (pageTableCount == 1U) && (descriptorTableCount == 1U);
}

#if defined(ASTERIA_TARGET_AMD64) && defined(__x86_64__)
[[nodiscard]] bool HasIdentityMappingForRange(const Kernel::Memory::InitialMappingPlan& mappings,
                                              const Foundation::UInt64 rangeStart, const Foundation::UInt64 rangeEnd,
                                              const MappingPermissions permissions,
                                              const InitialMappingOwner owner) noexcept
{
    if ((rangeStart >= rangeEnd) || (rangeEnd > 0x0000'8000'0000'0000ULL))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < mappings.GetMappingCount(); ++index)
    {
        const auto mappingResult = mappings.GetMapping(index);
        if (!mappingResult.HasValue())
        {
            return false;
        }
        const InitialMapping mapping = mappingResult.GetValue();
        Foundation::UInt64 mappingEnd{0U};
        if (!TryGetRangeEnd(mapping.VirtualStart.GetValue(), mapping.Pages.GetValue() << 12U, mappingEnd))
        {
            return false;
        }
        if ((rangeStart >= mapping.VirtualStart.GetValue()) && (rangeEnd <= mappingEnd))
        {
            return (mapping.VirtualStart.GetValue() == mapping.PhysicalStart.GetValue()) &&
                   (mapping.Permissions.GetBits() == permissions.GetBits()) &&
                   (mapping.CachePolicy == MemoryCachePolicy::WriteBack) && (mapping.Owner == owner) &&
                   (mapping.Reserved == 0U);
        }
    }
    return false;
}
#endif

[[nodiscard]] bool ContainsGlobalMapping(const Kernel::Memory::InitialMappingPlan& mappings) noexcept
{
    if (!mappings.IsSealed() || (mappings.GetMappingCount() != BootstrapInitialMappingCount))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < mappings.GetMappingCount(); ++index)
    {
        const auto mappingResult = mappings.GetMapping(index);
        if (mappingResult.HasValue() && mappingResult.GetValue().Permissions.Contains(MappingPermission::Global))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool ActivationMappingsAreValid(const Kernel::Memory::InitialMappingPlan& mappings,
                                              const BootstrapMappingPlanSummary& mappingSummary) noexcept
{
    if (!MappingSummaryIsValid(mappingSummary) || !BootstrapMappingProfileIsValid(mappings, mappingSummary))
    {
        return false;
    }
#if defined(ASTERIA_TARGET_AMD64) && defined(__x86_64__)
    const Foundation::UInt64 textStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(AsteriaAmd64ActivationTextStart));
    const Foundation::UInt64 textEnd =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(AsteriaAmd64ActivationTextEnd));
    const Foundation::UInt64 readOnlyStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(AsteriaAmd64ActivationReadOnlyStart));
    const Foundation::UInt64 readOnlyEnd =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(AsteriaAmd64ActivationReadOnlyEnd));
    const MappingPermissions executable = MappingPermissions{MappingPermission::Read}.With(MappingPermission::Execute);
    const MappingPermissions readOnly{MappingPermission::Read};
    return HasIdentityMappingForRange(mappings, textStart, textEnd, executable, InitialMappingOwner::NucleusCode) &&
           HasIdentityMappingForRange(mappings, readOnlyStart, readOnlyEnd, readOnly,
                                      InitialMappingOwner::NucleusReadOnlyData);
#else
    return true;
#endif
}

#if defined(ASTERIA_TARGET_AMD64) && defined(__x86_64__)
void ExecuteCpuId(const Foundation::UInt32 leaf, Foundation::UInt32& eax, Foundation::UInt32& ebx,
                  Foundation::UInt32& ecx, Foundation::UInt32& edx) noexcept
{
    constexpr Foundation::UInt32 Subleaf{0U};
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf), "c"(Subleaf) : "memory");
}

[[nodiscard]] Foundation::UInt64 ReadModelSpecificRegister(const Foundation::UInt32 index) noexcept
{
    Foundation::UInt32 low{0U};
    Foundation::UInt32 high{0U};
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(index) : "memory");
    return static_cast<Foundation::UInt64>(low) | (static_cast<Foundation::UInt64>(high) << 32U);
}

// Establishes EstablishInitialControlState on the executing processor,
// writing only the registers that change: EFER first, then CR4, whose
// cleared PGE (or PCIDE) invalidates every TLB entry, global ones included,
// so no firmware translation survives into the owned root; then CR0. Runs
// after ExitBootServices under the firmware's tables, whose leaves have
// execute-disable clear (reserved while EFER.NXE was clear), so nothing they
// map becomes non-executable.
void EstablishCurrentControlState() noexcept
{
    Foundation::UInt64 cr0{0U};
    Foundation::UInt64 cr4{0U};
    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0) : : "memory");
    __asm__ volatile("movq %%cr4, %0" : "=r"(cr4) : : "memory");
    const Foundation::UInt64 efer =
        ReadModelSpecificRegister(static_cast<Foundation::UInt32>(ModelSpecificRegisterEfer));
    const InitialControlState established =
        EstablishInitialControlState(InitialControlState{.Cr0 = cr0, .Cr4 = cr4, .Efer = efer});
    if (established.Efer != efer)
    {
        __asm__ volatile("wrmsr"
                         :
                         : "c"(static_cast<Foundation::UInt32>(ModelSpecificRegisterEfer)),
                           "a"(static_cast<Foundation::UInt32>(established.Efer)),
                           "d"(static_cast<Foundation::UInt32>(established.Efer >> 32U))
                         : "memory");
    }
    if (established.Cr4 != cr4)
    {
        Foundation::UInt64 control = established.Cr4;
        __asm__ volatile("andq %1,%0; movq %0,%%cr4"
                         : "+r"(control)
                         : "i"(~DisabledTranslationFeatures)
                         : "memory", "cc");
    }
    if (established.Cr0 != cr0)
    {
        __asm__ volatile("movq %0, %%cr0" : : "r"(established.Cr0) : "memory");
    }
}
#endif

[[nodiscard]] bool ProcessorStateMatchesCurrent(const PagingCapabilities& capabilities,
                                                const InitialProcessorStateReport& expected) noexcept
{
    if (!PagingCapabilitiesAreValid(capabilities) || (expected.VariableMtrrCount > MaximumInitialVariableMtrrCount) ||
        ((expected.MtrrCapability & MtrrVariableCountMask) != expected.VariableMtrrCount) ||
        (((expected.MtrrCapability & MtrrFixedSupportedBit) != 0U) != expected.FixedMtrrsSupported))
    {
        return false;
    }
#if defined(ASTERIA_TARGET_AMD64) && defined(__x86_64__)
    Foundation::UInt32 eax{0U};
    Foundation::UInt32 ebx{0U};
    Foundation::UInt32 ecx{0U};
    Foundation::UInt32 edx{0U};
    ExecuteCpuId(CpuIdBasicFeatureLeaf, eax, ebx, ecx, edx);
    if ((edx & CpuIdMtrrBit) == 0U)
    {
        return false;
    }

    Foundation::UInt64 cr0{0U};
    Foundation::UInt64 cr3{0U};
    Foundation::UInt64 cr4{0U};
    Foundation::UInt64 rflags{0U};
    Foundation::UInt16 codeSelector{0U};
    Foundation::UInt16 stackSelector{0U};
    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0) : : "memory");
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3) : : "memory");
    __asm__ volatile("movq %%cr4, %0" : "=r"(cr4) : : "memory");
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags) : : "memory");
    __asm__ volatile("movw %%cs, %0" : "=r"(codeSelector));
    __asm__ volatile("movw %%ss, %0" : "=r"(stackSelector));
    // Before the activation establishes it, the current state is the
    // firmware's; the report holds it as established.
    const InitialControlState current = EstablishInitialControlState(InitialControlState{
        .Cr0 = cr0,
        .Cr4 = cr4,
        .Efer = ReadModelSpecificRegister(static_cast<Foundation::UInt32>(ModelSpecificRegisterEfer)),
    });
    if ((current.Cr0 != expected.Cr0) || (cr3 != expected.Cr3) || (current.Cr4 != expected.Cr4) ||
        ((rflags & RflagsReservedOneBit) == 0U) ||
        ((rflags & (RflagsTrapBit | RflagsDirectionBit | RflagsNestedTaskBit | RflagsVirtual8086Bit)) != 0U) ||
        (codeSelector != expected.CodeSelector) || (stackSelector != expected.StackSelector) ||
        (current.Efer != expected.Efer) ||
        (ReadModelSpecificRegister(static_cast<Foundation::UInt32>(ModelSpecificRegisterPat)) != expected.Pat) ||
        (ReadModelSpecificRegister(static_cast<Foundation::UInt32>(ModelSpecificRegisterMtrrCapability)) !=
         expected.MtrrCapability) ||
        (ReadModelSpecificRegister(static_cast<Foundation::UInt32>(ModelSpecificRegisterMtrrDefaultType)) !=
         expected.MtrrDefaultType))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < expected.VariableMtrrCount; ++index)
    {
        const Foundation::UInt32 baseIndex = static_cast<Foundation::UInt32>(0x200U + (index * 2U));
        if ((ReadModelSpecificRegister(baseIndex) != expected.VariableMtrrBase[index]) ||
            (ReadModelSpecificRegister(baseIndex + 1U) != expected.VariableMtrrMask[index]))
        {
            return false;
        }
    }
    if (expected.FixedMtrrsSupported)
    {
        for (Foundation::Size index{0U}; index < InitialFixedMtrrRegisterCount; ++index)
        {
            if (ReadModelSpecificRegister(static_cast<Foundation::UInt32>(FixedMtrrMsrs[index])) !=
                expected.FixedMtrr[index])
            {
                return false;
            }
        }
    }
    return true;
#else
    if ((expected.Cr0 != 0x8001'0033U) || (expected.Cr3 != 0x0010'0000U) || (expected.Cr4 != 0x0000'0668U) ||
        (expected.Efer != 0x0000'0D00U) || (expected.Pat != ExpectedInitialPatValue) ||
        (expected.MtrrCapability != 0U) || (expected.MtrrDefaultType != (MtrrEnableBit | WriteBackMemoryType)) ||
        ((expected.Rflags & RflagsReservedOneBit) == 0U) ||
        ((expected.Rflags & (RflagsTrapBit | RflagsDirectionBit | RflagsNestedTaskBit | RflagsVirtual8086Bit)) != 0U) ||
        (expected.CodeSelector != 0x38U) || (expected.StackSelector != 0x30U) || (expected.VariableMtrrCount != 0U) ||
        expected.FixedMtrrsSupported)
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < MaximumInitialVariableMtrrCount; ++index)
    {
        if ((expected.VariableMtrrBase[index] != 0U) || (expected.VariableMtrrMask[index] != 0U))
        {
            return false;
        }
    }
    for (Foundation::Size index{0U}; index < InitialFixedMtrrRegisterCount; ++index)
    {
        if (expected.FixedMtrr[index] != 0U)
        {
            return false;
        }
    }
    return true;
#endif
}

[[nodiscard]] Foundation::UInt8 GetFixedMemoryType(const InitialProcessorStateReport& report,
                                                   const Foundation::UInt64 physicalAddress) noexcept
{
    Foundation::Size registerIndex{0U};
    Foundation::Size byteIndex{0U};
    if (physicalAddress < 0x80000U)
    {
        registerIndex = 0U;
        byteIndex = static_cast<Foundation::Size>(physicalAddress >> 16U);
    }
    else if (physicalAddress < 0xA0000U)
    {
        registerIndex = 1U;
        byteIndex = static_cast<Foundation::Size>((physicalAddress - 0x80000U) >> 14U);
    }
    else if (physicalAddress < 0xC0000U)
    {
        registerIndex = 2U;
        byteIndex = static_cast<Foundation::Size>((physicalAddress - 0xA0000U) >> 14U);
    }
    else
    {
        const Foundation::UInt64 offset = physicalAddress - 0xC0000U;
        registerIndex = 3U + static_cast<Foundation::Size>(offset >> 15U);
        byteIndex = static_cast<Foundation::Size>((offset >> 12U) & 0x7U);
    }
    return static_cast<Foundation::UInt8>(report.FixedMtrr[registerIndex] >> (byteIndex * 8U));
}

[[nodiscard]] bool TryDecodeVariableMtrr(const InitialProcessorStateReport& report, const Foundation::Size index,
                                         const Foundation::UInt8 physicalAddressBits, Foundation::UInt64& rangeStart,
                                         Foundation::UInt64& rangeEnd, Foundation::UInt8& memoryType,
                                         bool& valid) noexcept
{
    const Foundation::UInt64 physicalLimit = Foundation::UInt64{1U} << physicalAddressBits;
    const Foundation::UInt64 physicalMask = (physicalLimit - 1U) & ~PageOffsetMask;
    // Intel SDM Vol. 3A section 12.11.2.3, figure 12-7: the PhysBase and
    // PhysMask address fields end at MAXPHYADDR-1; the bits from MAXPHYADDR
    // up are reserved. Hyper-V generation 2 firmware can set PhysMask bits
    // at and above the physical-address width advertised by CPUID.
    // Ignore those high bits for compatibility; address matching uses only
    // the architecturally defined address fields. The low
    // reserved bits (PhysBase 11:8, PhysMask 10:0) must still be clear.
    const Foundation::UInt64 base = report.VariableMtrrBase[index];
    const Foundation::UInt64 mask = report.VariableMtrrMask[index];
    if (((base & (PageOffsetMask & ~Foundation::UInt64{0xFFU})) != 0U) ||
        ((mask & (PageOffsetMask & ~MtrrValidBit)) != 0U))
    {
        return false;
    }
    memoryType = static_cast<Foundation::UInt8>(base & 0xFFU);
    valid = (mask & MtrrValidBit) != 0U;
    if (!valid)
    {
        rangeStart = 0U;
        rangeEnd = 0U;
        return true;
    }

    // An address matches when it equals the base under the mask, so base bits the mask leaves out
    // do not count (SDM section 12.11.2.3). Firmware can describe a 0-64 GiB range with a base of 4 GiB.
    const Foundation::UInt64 maskAddress = mask & physicalMask;
    const Foundation::UInt64 rangeBytes = ((~maskAddress) & physicalMask) + PageSizeBytes;
    rangeStart = base & maskAddress;
    if ((rangeBytes < PageSizeBytes) || ((rangeBytes & (rangeBytes - 1U)) != 0U) ||
        (maskAddress != (physicalMask & ~(rangeBytes - 1U))) || (rangeStart > (physicalLimit - rangeBytes)))
    {
        return false;
    }
    rangeEnd = rangeStart + rangeBytes;
    return true;
}

// Whether the MTRRs keep a kernel page strongly ordered and coherent: its
// effective type is write-back, or uncached (UC wins any overlap, and
// MTRRs that are disabled make every page UC). Uncached is slow but correct,
// and under a hypervisor with nested paging the guest's MTRRs do not decide
// the type at all; Hyper-V presents them disabled. Write-combining,
// write-through and write-protect are refused: combined with PAT write-back
// they would weaken the ordering or the caching the kernel relies on.
[[nodiscard]] bool IsOrderedMemoryTypePage(const InitialProcessorStateReport& report,
                                           const Foundation::UInt8 physicalAddressBits,
                                           const Foundation::UInt64 physicalAddress) noexcept
{
    if ((report.MtrrDefaultType & ~(MtrrEnableBit | FixedMtrrEnableBit | 0xFFU)) != 0U)
    {
        return false;
    }
    if ((report.MtrrDefaultType & MtrrEnableBit) == 0U)
    {
        return true;
    }
    if ((physicalAddress < 0x100000U) && ((report.MtrrDefaultType & FixedMtrrEnableBit) != 0U))
    {
        const Foundation::UInt8 fixed =
            report.FixedMtrrsSupported ? GetFixedMemoryType(report, physicalAddress) : 0xFFU;
        return (fixed == WriteBackMemoryType) || (fixed == UncachedMemoryType);
    }

    bool matched{false};
    bool uncached{false};
    bool writeBackOnly{true};
    for (Foundation::Size index{0U}; index < report.VariableMtrrCount; ++index)
    {
        Foundation::UInt64 rangeStart{0U};
        Foundation::UInt64 rangeEnd{0U};
        Foundation::UInt8 memoryType{0U};
        bool valid{false};
        if (!TryDecodeVariableMtrr(report, index, physicalAddressBits, rangeStart, rangeEnd, memoryType, valid))
        {
            return false;
        }
        if (valid && (physicalAddress >= rangeStart) && (physicalAddress < rangeEnd))
        {
            matched = true;
            uncached = uncached || (memoryType == UncachedMemoryType);
            writeBackOnly = writeBackOnly && (memoryType == WriteBackMemoryType);
        }
    }
    if (matched)
    {
        return uncached || writeBackOnly;
    }
    const Foundation::UInt64 defaultType = report.MtrrDefaultType & 0xFFU;
    return (defaultType == WriteBackMemoryType) || (defaultType == UncachedMemoryType);
}

[[nodiscard]] InitialAddressSpacePreparationError
ValidateProcessorState(const Kernel::Memory::InitialMappingPlan& mappings, const PagingCapabilities& capabilities,
                       const InitialProcessorStateReport& report) noexcept
{
    const Foundation::UInt64 requiredCr0 = RequiredControlRegister0Bits;
    const Foundation::UInt64 requiredCr4 = Cr4PhysicalAddressExtensionBit;
    const Foundation::UInt64 forbiddenCr4 = ForbiddenControlRegister4Bits;
    const Foundation::UInt64 requiredEfer = EferLongModeEnableBit | EferLongModeActiveBit | EferExecuteDisableBit;
    if (!PagingCapabilitiesAreValid(capabilities) || (report.Reserved16 != 0U) || (report.Reserved32 != 0U) ||
        (report.ReservedTail != 0U) || (report.VariableMtrrCount > MaximumInitialVariableMtrrCount) ||
        ((report.MtrrCapability & MtrrVariableCountMask) != report.VariableMtrrCount) ||
        (((report.MtrrCapability & MtrrFixedSupportedBit) != 0U) != report.FixedMtrrsSupported) ||
        ((report.Cr0 & requiredCr0) != requiredCr0) ||
        ((report.Cr0 & (Cr0NotWriteThroughBit | Cr0CacheDisableBit)) != 0U) ||
        ((report.Cr4 & requiredCr4) != requiredCr4) || ((report.Cr4 & forbiddenCr4) != 0U) ||
        ((report.Cr4 >> 32U) != 0U) || ((report.Efer & requiredEfer) != requiredEfer) ||
        ((report.Rflags & RflagsReservedOneBit) == 0U) ||
        ((report.Rflags & (RflagsTrapBit | RflagsDirectionBit | RflagsNestedTaskBit | RflagsVirtual8086Bit)) != 0U) ||
        ((report.Pat & 0xFFU) != WriteBackMemoryType) || (report.Cr3 == 0U) || ((report.Cr3 & PageOffsetMask) != 0U) ||
        (report.CodeSelector == 0U) || (report.StackSelector == 0U) || ((report.CodeSelector & 0x7U) != 0U) ||
        ((report.StackSelector & 0x7U) != 0U) || (report.CodeSelector == report.StackSelector) ||
        ((report.CodeSelector >> 3U) >= InitialGlobalDescriptorTableEntryCount) ||
        ((report.StackSelector >> 3U) >= InitialGlobalDescriptorTableEntryCount))
    {
        return InitialAddressSpacePreparationError::InvalidProcessorState;
    }

    for (Foundation::Size index{report.VariableMtrrCount}; index < MaximumInitialVariableMtrrCount; ++index)
    {
        if ((report.VariableMtrrBase[index] != 0U) || (report.VariableMtrrMask[index] != 0U))
        {
            return InitialAddressSpacePreparationError::InvalidProcessorState;
        }
    }
    if (!report.FixedMtrrsSupported)
    {
        for (Foundation::Size index{0U}; index < InitialFixedMtrrRegisterCount; ++index)
        {
            if (report.FixedMtrr[index] != 0U)
            {
                return InitialAddressSpacePreparationError::InvalidProcessorState;
            }
        }
    }

    for (Foundation::Size mappingIndex{0U}; mappingIndex < mappings.GetMappingCount(); ++mappingIndex)
    {
        const auto mappingResult = mappings.GetMapping(mappingIndex);
        if (!mappingResult.HasValue())
        {
            return InitialAddressSpacePreparationError::InvalidProcessorState;
        }
        const InitialMapping mapping = mappingResult.GetValue();
        if (mapping.Permissions.Contains(MappingPermission::Global))
        {
            return InitialAddressSpacePreparationError::GlobalMappingUnsupported;
        }
        Foundation::UInt64 physicalAddress = mapping.PhysicalStart.GetValue();
        for (Foundation::UInt64 page{0U}; page < mapping.Pages.GetValue(); ++page)
        {
            if (!IsOrderedMemoryTypePage(report, capabilities.PhysicalAddressBits, physicalAddress))
            {
                return InitialAddressSpacePreparationError::InvalidMemoryTypeConfiguration;
            }
            physicalAddress += PageSizeBytes;
        }
    }
    return InitialAddressSpacePreparationError::None;
}

[[nodiscard]] InitialAddressSpacePreparationError
ValidateMemoryStack(const Kernel::Memory::InitialMappingPlan& mappings,
                    const BootstrapMappingPlanSummary& mappingSummary,
                    const WritableInitialMemoryStackView& memoryStack) noexcept
{
    if (!MappingSummaryIsValid(mappingSummary) || (memoryStack.Data == nullptr) ||
        (memoryStack.SizeBytes != InitialMemoryStackSizeBytes) ||
        (memoryStack.PhysicalStart != mappingSummary.StackPhysicalStart) ||
        (static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(memoryStack.Data)) !=
         memoryStack.PhysicalStart) ||
        ((memoryStack.PhysicalStart & PageOffsetMask) != 0U))
    {
        return InitialAddressSpacePreparationError::InvalidMemoryStack;
    }

    Foundation::Size stackMappings{0U};
    const Foundation::UInt64 stackEnd = memoryStack.PhysicalStart + InitialMemoryStackSizeBytes;
    for (Foundation::Size index{0U}; index < mappings.GetMappingCount(); ++index)
    {
        const auto mappingResult = mappings.GetMapping(index);
        if (!mappingResult.HasValue())
        {
            return InitialAddressSpacePreparationError::InvalidMemoryStack;
        }
        const InitialMapping mapping = mappingResult.GetValue();
        const Foundation::UInt64 mappingBytes = mapping.Pages.GetValue() << 12U;
        Foundation::UInt64 mappingEnd{0U};
        if (!TryGetRangeEnd(mapping.PhysicalStart.GetValue(), mappingBytes, mappingEnd))
        {
            return InitialAddressSpacePreparationError::InvalidMemoryStack;
        }
        const bool overlaps =
            RangesOverlap(mapping.PhysicalStart.GetValue(), mappingEnd, memoryStack.PhysicalStart, stackEnd);
        if (mapping.Owner == InitialMappingOwner::NucleusStack)
        {
            ++stackMappings;
            if (!overlaps || (mapping.VirtualStart.GetValue() != memoryStack.PhysicalStart) ||
                (mapping.PhysicalStart.GetValue() != memoryStack.PhysicalStart) ||
                (mapping.Pages.GetValue() != BootstrapStackPageCount) ||
                (mapping.Permissions.GetBits() != RequiredStackPermissions) ||
                (mapping.CachePolicy != MemoryCachePolicy::WriteBack) || (mapping.Reserved != 0U))
            {
                return InitialAddressSpacePreparationError::InvalidMemoryStack;
            }
        }
        else if (overlaps)
        {
            return InitialAddressSpacePreparationError::InvalidMemoryStack;
        }
    }
    return stackMappings == 1U ? InitialAddressSpacePreparationError::None
                               : InitialAddressSpacePreparationError::InvalidMemoryStack;
}

[[nodiscard]] InitialAddressSpacePreparationError
ValidateDescriptorTables(const Kernel::Memory::InitialMappingPlan& mappings,
                         const BootstrapMappingPlanSummary& mappingSummary,
                         const WritableInitialDescriptorTableView& descriptorTables) noexcept
{
    if (!MappingSummaryIsValid(mappingSummary) || (descriptorTables.Data == nullptr) ||
        (descriptorTables.SizeBytes != InitialDescriptorTableStorageSizeBytes) ||
        (descriptorTables.PhysicalStart != mappingSummary.DescriptorTableStoragePhysicalStart) ||
        (static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(descriptorTables.Data)) !=
         descriptorTables.PhysicalStart) ||
        ((descriptorTables.PhysicalStart & PageOffsetMask) != 0U))
    {
        return InitialAddressSpacePreparationError::InvalidDescriptorTableStorage;
    }

    Foundation::Size descriptorMappings{0U};
    const Foundation::UInt64 descriptorEnd = descriptorTables.PhysicalStart + InitialDescriptorTableStorageSizeBytes;
    for (Foundation::Size index{0U}; index < mappings.GetMappingCount(); ++index)
    {
        const auto mappingResult = mappings.GetMapping(index);
        if (!mappingResult.HasValue())
        {
            return InitialAddressSpacePreparationError::InvalidDescriptorTableStorage;
        }
        const InitialMapping mapping = mappingResult.GetValue();
        Foundation::UInt64 mappingEnd{0U};
        if (!TryGetRangeEnd(mapping.PhysicalStart.GetValue(), mapping.Pages.GetValue() << 12U, mappingEnd))
        {
            return InitialAddressSpacePreparationError::InvalidDescriptorTableStorage;
        }
        const bool overlaps =
            RangesOverlap(mapping.PhysicalStart.GetValue(), mappingEnd, descriptorTables.PhysicalStart, descriptorEnd);
        if (mapping.Owner == InitialMappingOwner::NucleusDescriptorTables)
        {
            ++descriptorMappings;
            if (!overlaps || (mapping.VirtualStart.GetValue() != descriptorTables.PhysicalStart) ||
                (mapping.PhysicalStart.GetValue() != descriptorTables.PhysicalStart) ||
                (mapping.Pages.GetValue() != BootstrapDescriptorTableStoragePageCount) ||
                (mapping.Permissions.GetBits() != RequiredStackPermissions) ||
                (mapping.CachePolicy != MemoryCachePolicy::WriteBack) || (mapping.Reserved != 0U))
            {
                return InitialAddressSpacePreparationError::InvalidDescriptorTableStorage;
            }
        }
        else if (overlaps)
        {
            return InitialAddressSpacePreparationError::InvalidDescriptorTableStorage;
        }
    }
    return descriptorMappings == 1U ? InitialAddressSpacePreparationError::None
                                    : InitialAddressSpacePreparationError::InvalidDescriptorTableStorage;
}

void WriteDescriptorRegister(Foundation::UInt8* const output, const Foundation::UInt16 limit,
                             const Foundation::UInt64 base) noexcept
{
    output[0U] = static_cast<Foundation::UInt8>(limit);
    output[1U] = static_cast<Foundation::UInt8>(limit >> 8U);
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        output[index + 2U] = static_cast<Foundation::UInt8>(base >> (index * 8U));
    }
}

[[nodiscard]] bool DescriptorRegisterMatches(const Foundation::UInt8* const image, const Foundation::UInt16 limit,
                                             const Foundation::UInt64 base) noexcept
{
    if ((image[0U] != static_cast<Foundation::UInt8>(limit)) ||
        (image[1U] != static_cast<Foundation::UInt8>(limit >> 8U)))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        if (image[index + 2U] != static_cast<Foundation::UInt8>(base >> (index * 8U)))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Foundation::UInt64 EncodeInterruptGateLow(const Foundation::UInt64 handler,
                                                        const Foundation::UInt16 selector) noexcept
{
    return (handler & 0xFFFFU) | (static_cast<Foundation::UInt64>(selector) << 16U) |
           (static_cast<Foundation::UInt64>(InterruptGateAttributes) << 40U) | (((handler >> 16U) & 0xFFFFU) << 48U);
}

[[nodiscard]] Foundation::UInt64 EncodeInterruptGateHigh(const Foundation::UInt64 handler) noexcept
{
    return handler >> 32U;
}

[[nodiscard]] bool DescriptorStateIsExact(const WritableInitialDescriptorTableView& descriptorTables,
                                          const Foundation::UInt16 codeSelector, const Foundation::UInt16 stackSelector,
                                          const Foundation::UInt8* const globalDescriptorRegister,
                                          const Foundation::UInt8* const interruptDescriptorRegister) noexcept
{
    const Foundation::Size codeIndex = static_cast<Foundation::Size>(codeSelector >> 3U);
    const Foundation::Size stackIndex = static_cast<Foundation::Size>(stackSelector >> 3U);
    const Foundation::Size lastDescriptorIndex = codeIndex > stackIndex ? codeIndex : stackIndex;
    if ((descriptorTables.Data == nullptr) || (codeIndex >= InitialGlobalDescriptorTableEntryCount) ||
        (stackIndex >= InitialGlobalDescriptorTableEntryCount))
    {
        return false;
    }
    const Foundation::UInt16 globalDescriptorTableLimit =
        static_cast<Foundation::UInt16>(((lastDescriptorIndex + 1U) * sizeof(Foundation::UInt64)) - 1U);
    const Foundation::UInt16 interruptDescriptorTableLimit =
        static_cast<Foundation::UInt16>((InitialInterruptDescriptorTableEntryCount * 16U) - 1U);
    const Foundation::UInt64* const globalDescriptorTable = descriptorTables.Data;
    const Foundation::UInt64* const interruptDescriptorTable =
        descriptorTables.Data + InitialGlobalDescriptorTableEntryCount;
    for (Foundation::Size index{0U}; index < InitialGlobalDescriptorTableEntryCount; ++index)
    {
        const Foundation::UInt64 expected =
            index == codeIndex ? CodeDescriptor : (index == stackIndex ? DataDescriptor : 0U);
        if (globalDescriptorTable[index] != expected)
        {
            return false;
        }
    }
    const Foundation::UInt64 handlerAddress =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&AsteriaAmd64FailStopInterrupt));
    const Foundation::UInt64 gateLow = EncodeInterruptGateLow(handlerAddress, codeSelector);
    const Foundation::UInt64 gateHigh = EncodeInterruptGateHigh(handlerAddress);
    for (Foundation::Size index{0U}; index < InitialInterruptDescriptorTableEntryCount; ++index)
    {
        if ((interruptDescriptorTable[index * 2U] != gateLow) ||
            (interruptDescriptorTable[(index * 2U) + 1U] != gateHigh))
        {
            return false;
        }
    }
    return DescriptorRegisterMatches(globalDescriptorRegister, globalDescriptorTableLimit,
                                     descriptorTables.PhysicalStart) &&
           DescriptorRegisterMatches(interruptDescriptorRegister, interruptDescriptorTableLimit,
                                     descriptorTables.PhysicalStart + GlobalDescriptorTableSizeBytes);
}
} // Namespace

InitialProcessorStateCaptureError
InitialAddressSpacePreparation::CaptureProcessorState(const PagingCapabilities& capabilities,
                                                      InitialProcessorStateReport& output) noexcept
{
    if (!PagingCapabilitiesAreValid(capabilities))
    {
        return InitialProcessorStateCaptureError::InvalidCapabilities;
    }

#if defined(ASTERIA_TARGET_AMD64) && defined(__x86_64__)
    Foundation::UInt32 eax{0U};
    Foundation::UInt32 ebx{0U};
    Foundation::UInt32 ecx{0U};
    Foundation::UInt32 edx{0U};
    ExecuteCpuId(CpuIdBasicFeatureLeaf, eax, ebx, ecx, edx);
    if ((edx & CpuIdMtrrBit) == 0U)
    {
        return InitialProcessorStateCaptureError::MemoryTypeRangeRegistersUnavailable;
    }
    const Foundation::UInt64 mtrrCapability =
        ReadModelSpecificRegister(static_cast<Foundation::UInt32>(ModelSpecificRegisterMtrrCapability));
    const Foundation::UInt64 variableCount = mtrrCapability & MtrrVariableCountMask;
    if (variableCount > MaximumInitialVariableMtrrCount)
    {
        return InitialProcessorStateCaptureError::TooManyVariableMemoryTypeRanges;
    }
    static_cast<void>(memset(&output, 0, sizeof(output)));
    __asm__ volatile("movq %%cr0, %0" : "=r"(output.Cr0) : : "memory");
    __asm__ volatile("movq %%cr3, %0" : "=r"(output.Cr3) : : "memory");
    __asm__ volatile("movq %%cr4, %0" : "=r"(output.Cr4) : : "memory");
    __asm__ volatile("pushfq; popq %0" : "=r"(output.Rflags) : : "memory");
    __asm__ volatile("movw %%cs, %0" : "=r"(output.CodeSelector));
    __asm__ volatile("movw %%ss, %0" : "=r"(output.StackSelector));
    output.Efer = ReadModelSpecificRegister(static_cast<Foundation::UInt32>(ModelSpecificRegisterEfer));
    const InitialControlState established =
        EstablishInitialControlState(InitialControlState{.Cr0 = output.Cr0, .Cr4 = output.Cr4, .Efer = output.Efer});
    output.Cr0 = established.Cr0;
    output.Cr4 = established.Cr4;
    output.Efer = established.Efer;
    output.Pat = ReadModelSpecificRegister(static_cast<Foundation::UInt32>(ModelSpecificRegisterPat));
    output.MtrrCapability = mtrrCapability;
    output.MtrrDefaultType =
        ReadModelSpecificRegister(static_cast<Foundation::UInt32>(ModelSpecificRegisterMtrrDefaultType));
    output.VariableMtrrCount = static_cast<Foundation::UInt8>(variableCount);
    output.FixedMtrrsSupported = (mtrrCapability & MtrrFixedSupportedBit) != 0U;
    for (Foundation::Size index{0U}; index < variableCount; ++index)
    {
        const Foundation::UInt32 baseIndex = static_cast<Foundation::UInt32>(0x200U + (index * 2U));
        output.VariableMtrrBase[index] = ReadModelSpecificRegister(baseIndex);
        output.VariableMtrrMask[index] = ReadModelSpecificRegister(baseIndex + 1U);
    }
    if (output.FixedMtrrsSupported)
    {
        for (Foundation::Size index{0U}; index < InitialFixedMtrrRegisterCount; ++index)
        {
            output.FixedMtrr[index] = ReadModelSpecificRegister(static_cast<Foundation::UInt32>(FixedMtrrMsrs[index]));
        }
    }
#else
    static_cast<void>(memset(&output, 0, sizeof(output)));
    output.Cr0 = 0x8001'0033U;
    output.Cr3 = 0x0010'0000U;
    output.Cr4 = 0x0000'0668U;
    output.Efer = 0x0000'0D00U;
    output.Pat = ExpectedInitialPatValue;
    output.MtrrCapability = 0U;
    output.MtrrDefaultType = MtrrEnableBit | WriteBackMemoryType;
    output.Rflags = 0x2U;
    output.CodeSelector = 0x38U;
    output.StackSelector = 0x30U;
#endif
    return InitialProcessorStateCaptureError::None;
}

Foundation::Result<InitialAddressSpacePreparationStatus, InitialAddressSpacePreparationError>
InitialAddressSpacePreparation::Prepare(const Kernel::Memory::InitialMappingPlan& mappings,
                                        const BootstrapMappingPlanSummary& mappingSummary,
                                        const PagingCapabilities& capabilities, const PageTableStorage& tableStorage,
                                        const WritableInitialMemoryStackView& memoryStack,
                                        const WritableInitialDescriptorTableView& descriptorTables,
                                        const InitialProcessorStateReport& processorState) noexcept
{
    using ResultType = Foundation::Result<InitialAddressSpacePreparationStatus, InitialAddressSpacePreparationError>;
    if (m_state == State::Prepared || m_state == State::ActivationClaimed)
    {
        return ResultType::Failure(InitialAddressSpacePreparationError::AlreadyPrepared);
    }
    if (m_state == State::Failed)
    {
        return ResultType::Failure(InitialAddressSpacePreparationError::PreviousPreparationFailed);
    }

    const Foundation::UInt64 stackStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(memoryStack.Data));
    Foundation::UInt64 stackEnd{0U};
    const Foundation::UInt64 tableStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(tableStorage.Entries));
    Foundation::UInt64 tableEnd{0U};
    const Foundation::UInt64 descriptorStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(descriptorTables.Data));
    Foundation::UInt64 descriptorEnd{0U};
    const bool hasStackRange =
        (memoryStack.Data != nullptr) && TryGetRangeEnd(stackStart, memoryStack.SizeBytes, stackEnd);
    const bool hasTableRange =
        (tableStorage.Entries != nullptr) && (tableStorage.PageCapacity <= (MaximumUInt64 >> 12U)) &&
        TryGetRangeEnd(tableStart, static_cast<Foundation::UInt64>(tableStorage.PageCapacity) << 12U, tableEnd);
    const bool hasDescriptorRange = (descriptorTables.Data != nullptr) &&
                                    TryGetRangeEnd(descriptorStart, descriptorTables.SizeBytes, descriptorEnd);
    if ((hasStackRange &&
         (RangeOverlapsObject(stackStart, stackEnd, *this) || RangeOverlapsObject(stackStart, stackEnd, mappings) ||
          RangeOverlapsObject(stackStart, stackEnd, mappingSummary) ||
          RangeOverlapsObject(stackStart, stackEnd, capabilities) ||
          RangeOverlapsObject(stackStart, stackEnd, tableStorage) ||
          RangeOverlapsObject(stackStart, stackEnd, memoryStack) ||
          RangeOverlapsObject(stackStart, stackEnd, descriptorTables) ||
          RangeOverlapsObject(stackStart, stackEnd, processorState))) ||
        (hasTableRange &&
         (RangeOverlapsObject(tableStart, tableEnd, *this) || RangeOverlapsObject(tableStart, tableEnd, mappings) ||
          RangeOverlapsObject(tableStart, tableEnd, mappingSummary) ||
          RangeOverlapsObject(tableStart, tableEnd, capabilities) ||
          RangeOverlapsObject(tableStart, tableEnd, tableStorage) ||
          RangeOverlapsObject(tableStart, tableEnd, memoryStack) ||
          RangeOverlapsObject(tableStart, tableEnd, descriptorTables) ||
          RangeOverlapsObject(tableStart, tableEnd, processorState) ||
          (hasStackRange && RangesOverlap(tableStart, tableEnd, stackStart, stackEnd)) ||
          (hasDescriptorRange && RangesOverlap(tableStart, tableEnd, descriptorStart, descriptorEnd)))) ||
        (hasDescriptorRange &&
         (RangeOverlapsObject(descriptorStart, descriptorEnd, *this) ||
          RangeOverlapsObject(descriptorStart, descriptorEnd, mappings) ||
          RangeOverlapsObject(descriptorStart, descriptorEnd, mappingSummary) ||
          RangeOverlapsObject(descriptorStart, descriptorEnd, capabilities) ||
          RangeOverlapsObject(descriptorStart, descriptorEnd, tableStorage) ||
          RangeOverlapsObject(descriptorStart, descriptorEnd, memoryStack) ||
          RangeOverlapsObject(descriptorStart, descriptorEnd, descriptorTables) ||
          RangeOverlapsObject(descriptorStart, descriptorEnd, processorState) ||
          (hasStackRange && RangesOverlap(descriptorStart, descriptorEnd, stackStart, stackEnd)))))
    {
        return ResultType::Failure(InitialAddressSpacePreparationError::StorageAliasesPreparation);
    }

    m_state = State::Failed;
    if (!MappingSummaryIsValid(mappingSummary))
    {
        m_failure = MakeFailure(InitialAddressSpacePreparationError::InvalidMappingSummary);
        return ResultType::Failure(m_failure.Error);
    }
    if ((tableStorage.Entries == nullptr) ||
        (tableStorage.PhysicalStart.GetValue() != mappingSummary.PageTableStoragePhysicalStart) ||
        (static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(tableStorage.Entries)) !=
         tableStorage.PhysicalStart.GetValue()) ||
        (tableStorage.PageCapacity != BootstrapPageTableStoragePageCount) || (tableStorage.Reserved != 0U))
    {
        m_failure = MakeFailure(InitialAddressSpacePreparationError::InvalidPageTableStorage);
        return ResultType::Failure(m_failure.Error);
    }
    const InitialAddressSpacePreparationError stackError = ValidateMemoryStack(mappings, mappingSummary, memoryStack);
    if (stackError != InitialAddressSpacePreparationError::None)
    {
        m_failure = MakeFailure(stackError);
        return ResultType::Failure(m_failure.Error);
    }
    if (ContainsGlobalMapping(mappings))
    {
        m_failure = MakeFailure(InitialAddressSpacePreparationError::GlobalMappingUnsupported);
        return ResultType::Failure(m_failure.Error);
    }
    if (!ActivationMappingsAreValid(mappings, mappingSummary))
    {
        m_failure = MakeFailure(InitialAddressSpacePreparationError::InvalidActivationMapping);
        return ResultType::Failure(m_failure.Error);
    }
    const InitialAddressSpacePreparationError processorError =
        ValidateProcessorState(mappings, capabilities, processorState);
    if (processorError != InitialAddressSpacePreparationError::None)
    {
        m_failure = MakeFailure(processorError);
        return ResultType::Failure(m_failure.Error);
    }
    const InitialAddressSpacePreparationError descriptorError =
        ValidateDescriptorTables(mappings, mappingSummary, descriptorTables);
    if (descriptorError != InitialAddressSpacePreparationError::None)
    {
        m_failure = MakeFailure(descriptorError);
        return ResultType::Failure(m_failure.Error);
    }

    const auto pageTableResult = m_pageTables.Prepare(mappings, capabilities, tableStorage);
    if (!pageTableResult.HasValue())
    {
        InitialPageTableBuildError buildError{InitialPageTableBuildError::None};
        Kernel::Memory::InitialMappingPlanError mappingError{Kernel::Memory::InitialMappingPlanError::None};
        Foundation::Size mappingIndex{0U};
        if (m_pageTables.HasFailed())
        {
            const InitialPageTablePreparationFailure& childFailure = m_pageTables.GetFailure();
            buildError = childFailure.BuildError;
            mappingError = childFailure.MappingError;
            mappingIndex = childFailure.MappingIndex;
        }
        m_failure = MakeFailure(InitialAddressSpacePreparationError::PageTablePreparationRejected,
                                InitialProcessorStateCaptureError::None, pageTableResult.GetError(), buildError,
                                InitialPageTableVerificationError::None, mappingError, mappingIndex);
        return ResultType::Failure(m_failure.Error);
    }
    const auto pageTableSummaryResult = m_pageTables.TryGetSummary();
    const auto retainedPlanResult = m_pageTables.TryGetMappingPlan();
    if (!pageTableSummaryResult.HasValue() || !retainedPlanResult.HasValue())
    {
        m_failure = MakeFailure(InitialAddressSpacePreparationError::PageTablePreparationRejected);
        return ResultType::Failure(m_failure.Error);
    }
    const InitialPageTableVerificationError verificationError = InitialPageTableVerifier::Verify(
        *retainedPlanResult.GetValue(), capabilities, *pageTableSummaryResult.GetValue());
    if (verificationError != InitialPageTableVerificationError::None)
    {
        m_failure = MakeFailure(InitialAddressSpacePreparationError::PageTableVerificationRejected,
                                InitialProcessorStateCaptureError::None, InitialPageTablePreparationError::None,
                                InitialPageTableBuildError::None, verificationError);
        return ResultType::Failure(m_failure.Error);
    }

    const Foundation::Size codeIndex = static_cast<Foundation::Size>(processorState.CodeSelector >> 3U);
    const Foundation::Size stackIndex = static_cast<Foundation::Size>(processorState.StackSelector >> 3U);
    const Foundation::Size lastDescriptorIndex = codeIndex > stackIndex ? codeIndex : stackIndex;
    const Foundation::UInt16 globalDescriptorTableLimit =
        static_cast<Foundation::UInt16>(((lastDescriptorIndex + 1U) * sizeof(Foundation::UInt64)) - 1U);
    const Foundation::UInt16 interruptDescriptorTableLimit =
        static_cast<Foundation::UInt16>((InitialInterruptDescriptorTableEntryCount * 16U) - 1U);
    const Foundation::UInt64 handlerAddress =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&AsteriaAmd64FailStopInterrupt));

    static_cast<void>(memset(descriptorTables.Data, 0, descriptorTables.SizeBytes));
    Foundation::UInt64* const globalDescriptorTable = descriptorTables.Data;
    Foundation::UInt64* const interruptDescriptorTable = descriptorTables.Data + InitialGlobalDescriptorTableEntryCount;
    globalDescriptorTable[codeIndex] = CodeDescriptor;
    globalDescriptorTable[stackIndex] = DataDescriptor;
    const Foundation::UInt64 gateLow = EncodeInterruptGateLow(handlerAddress, processorState.CodeSelector);
    const Foundation::UInt64 gateHigh = EncodeInterruptGateHigh(handlerAddress);
    for (Foundation::Size index{0U}; index < InitialInterruptDescriptorTableEntryCount; ++index)
    {
        interruptDescriptorTable[index * 2U] = gateLow;
        interruptDescriptorTable[(index * 2U) + 1U] = gateHigh;
    }
    WriteDescriptorRegister(
        m_globalDescriptorRegister, globalDescriptorTableLimit,
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(globalDescriptorTable)));
    WriteDescriptorRegister(
        m_interruptDescriptorRegister, interruptDescriptorTableLimit,
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(interruptDescriptorTable)));
    if (!DescriptorStateIsExact(descriptorTables, processorState.CodeSelector, processorState.StackSelector,
                                m_globalDescriptorRegister, m_interruptDescriptorRegister))
    {
        m_failure = MakeFailure(InitialAddressSpacePreparationError::DescriptorTablesCorrupt);
        return ResultType::Failure(m_failure.Error);
    }
    static_cast<void>(memset(memoryStack.Data, 0, memoryStack.SizeBytes));
#if defined(__x86_64__)
    __asm__ volatile("mfence" : : : "memory");
#endif
    if (!IsZeroBytes(memoryStack.Data, memoryStack.SizeBytes))
    {
        m_failure = MakeFailure(InitialAddressSpacePreparationError::MemoryStackNotZero);
        return ResultType::Failure(m_failure.Error);
    }

    m_mappingSummary = mappingSummary;
    m_capabilities = capabilities;
    m_memoryStack = memoryStack;
    m_descriptorTables = descriptorTables;
    m_processorState = processorState;
    m_summary = InitialAddressSpacePreparationSummary{
        .RootPhysicalAddress = pageTableSummaryResult.GetValue()->Storage.RootPhysicalAddress,
        .StackPhysicalStart = memoryStack.PhysicalStart,
        .InitialStackPointer = memoryStack.PhysicalStart + memoryStack.SizeBytes,
        .GlobalDescriptorTableAddress = descriptorTables.PhysicalStart,
        .InterruptDescriptorTableAddress = descriptorTables.PhysicalStart + GlobalDescriptorTableSizeBytes,
        .UsedTablePages = pageTableSummaryResult.GetValue()->Storage.UsedTablePages,
        .CodeSelector = processorState.CodeSelector,
        .StackSelector = processorState.StackSelector,
        .GlobalDescriptorTableLimit = globalDescriptorTableLimit,
        .InterruptDescriptorTableLimit = interruptDescriptorTableLimit,
        .Reserved = 0U,
        .ReservedTail = 0U,
    };
    m_state = State::Prepared;
    return ResultType::Success(InitialAddressSpacePreparationStatus::Prepared);
}

InitialAddressSpacePreparationError InitialAddressSpacePreparation::ValidatePreparedState() const noexcept
{
    if (m_state != State::Prepared)
    {
        return m_state == State::Failed ? InitialAddressSpacePreparationError::PreviousPreparationFailed
                                        : InitialAddressSpacePreparationError::NotPrepared;
    }
    if (!MappingSummaryIsValid(m_mappingSummary) || (m_summary.Reserved != 0U) || (m_summary.ReservedTail != 0U) ||
        (m_reserved != 0U))
    {
        return InitialAddressSpacePreparationError::InvalidMappingSummary;
    }
    if (!PagingCapabilitiesAreValid(m_capabilities))
    {
        return InitialAddressSpacePreparationError::InvalidProcessorState;
    }
    const auto planResult = m_pageTables.TryGetMappingPlan();
    const auto pageTableSummaryResult = m_pageTables.TryGetSummary();
    if (!planResult.HasValue() || !pageTableSummaryResult.HasValue())
    {
        return InitialAddressSpacePreparationError::PageTablePreparationRejected;
    }
    if (ContainsGlobalMapping(*planResult.GetValue()))
    {
        return InitialAddressSpacePreparationError::GlobalMappingUnsupported;
    }
    if (!ActivationMappingsAreValid(*planResult.GetValue(), m_mappingSummary))
    {
        return InitialAddressSpacePreparationError::InvalidActivationMapping;
    }
    const InitialAddressSpacePreparationError stackError =
        ValidateMemoryStack(*planResult.GetValue(), m_mappingSummary, m_memoryStack);
    if (stackError != InitialAddressSpacePreparationError::None)
    {
        return stackError;
    }
    const InitialAddressSpacePreparationError descriptorError =
        ValidateDescriptorTables(*planResult.GetValue(), m_mappingSummary, m_descriptorTables);
    if (descriptorError != InitialAddressSpacePreparationError::None)
    {
        return descriptorError;
    }
    const InitialPageTablePreparationSummary& pageTableSummary = *pageTableSummaryResult.GetValue();
    if ((pageTableSummary.Storage.RootPhysicalAddress.GetValue() != m_mappingSummary.PageTableStoragePhysicalStart) ||
        (static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(pageTableSummary.Storage.Entries)) !=
         m_mappingSummary.PageTableStoragePhysicalStart) ||
        (pageTableSummary.Storage.PageCapacity != BootstrapPageTableStoragePageCount) ||
        (pageTableSummary.Storage.UsedTablePages == 0U) ||
        (pageTableSummary.Storage.UsedTablePages > pageTableSummary.Storage.PageCapacity) ||
        (pageTableSummary.MappingCount != BootstrapInitialMappingCount) || (pageTableSummary.Reserved16 != 0U) ||
        (pageTableSummary.Reserved32 != 0U))
    {
        return InitialAddressSpacePreparationError::InvalidPageTableStorage;
    }
    const InitialAddressSpacePreparationError processorError =
        ValidateProcessorState(*planResult.GetValue(), m_capabilities, m_processorState);
    if (processorError != InitialAddressSpacePreparationError::None)
    {
        return processorError;
    }
    const Foundation::Size codeIndex = static_cast<Foundation::Size>(m_processorState.CodeSelector >> 3U);
    const Foundation::Size stackIndex = static_cast<Foundation::Size>(m_processorState.StackSelector >> 3U);
    const Foundation::Size lastDescriptorIndex = codeIndex > stackIndex ? codeIndex : stackIndex;
    const Foundation::UInt16 expectedGlobalDescriptorTableLimit =
        static_cast<Foundation::UInt16>(((lastDescriptorIndex + 1U) * sizeof(Foundation::UInt64)) - 1U);
    const Foundation::UInt16 expectedInterruptDescriptorTableLimit =
        static_cast<Foundation::UInt16>((InitialInterruptDescriptorTableEntryCount * 16U) - 1U);
    if ((m_summary.RootPhysicalAddress.GetValue() != pageTableSummary.Storage.RootPhysicalAddress.GetValue()) ||
        (m_summary.StackPhysicalStart != m_memoryStack.PhysicalStart) ||
        (m_summary.InitialStackPointer != (m_memoryStack.PhysicalStart + m_memoryStack.SizeBytes)) ||
        (m_summary.UsedTablePages != pageTableSummary.Storage.UsedTablePages) ||
        (m_summary.GlobalDescriptorTableAddress != m_descriptorTables.PhysicalStart) ||
        (m_summary.InterruptDescriptorTableAddress !=
         (m_descriptorTables.PhysicalStart + GlobalDescriptorTableSizeBytes)) ||
        (m_summary.CodeSelector != m_processorState.CodeSelector) ||
        (m_summary.StackSelector != m_processorState.StackSelector) ||
        (m_summary.GlobalDescriptorTableLimit != expectedGlobalDescriptorTableLimit) ||
        (m_summary.InterruptDescriptorTableLimit != expectedInterruptDescriptorTableLimit))
    {
        return InitialAddressSpacePreparationError::InvalidMappingSummary;
    }
    if (!IsZeroBytes(m_memoryStack.Data, m_memoryStack.SizeBytes))
    {
        return InitialAddressSpacePreparationError::MemoryStackNotZero;
    }
    const InitialPageTableVerificationError pageTableError =
        InitialPageTableVerifier::Verify(*planResult.GetValue(), m_capabilities, pageTableSummary);
    if (pageTableError != InitialPageTableVerificationError::None)
    {
        return InitialAddressSpacePreparationError::PageTableVerificationRejected;
    }
    if (!DescriptorStateIsExact(m_descriptorTables, m_processorState.CodeSelector, m_processorState.StackSelector,
                                m_globalDescriptorRegister, m_interruptDescriptorRegister))
    {
        return InitialAddressSpacePreparationError::DescriptorTablesCorrupt;
    }

    if (!ProcessorStateMatchesCurrent(m_capabilities, m_processorState))
    {
        return InitialAddressSpacePreparationError::ProcessorStateChanged;
    }
    return InitialAddressSpacePreparationError::None;
}

bool InitialAddressSpacePreparation::IsPrepared() const noexcept
{
    return m_state == State::Prepared;
}

bool InitialAddressSpacePreparation::HasFailed() const noexcept
{
    return m_state == State::Failed;
}

Foundation::Result<const InitialAddressSpacePreparationSummary*, InitialAddressSpacePreparationError>
InitialAddressSpacePreparation::TryGetSummary() const noexcept
{
    using ResultType =
        Foundation::Result<const InitialAddressSpacePreparationSummary*, InitialAddressSpacePreparationError>;
    if (m_state != State::Prepared)
    {
        return ResultType::Failure(m_state == State::Failed
                                       ? InitialAddressSpacePreparationError::PreviousPreparationFailed
                                       : InitialAddressSpacePreparationError::NotPrepared);
    }
    return ResultType::Success(&m_summary);
}

Foundation::Result<const InitialPageTablePreparation*, InitialAddressSpacePreparationError>
InitialAddressSpacePreparation::TryGetPageTablePreparation() const noexcept
{
    using ResultType = Foundation::Result<const InitialPageTablePreparation*, InitialAddressSpacePreparationError>;
    if (m_state != State::Prepared)
    {
        return ResultType::Failure(m_state == State::Failed
                                       ? InitialAddressSpacePreparationError::PreviousPreparationFailed
                                       : InitialAddressSpacePreparationError::NotPrepared);
    }
    return ResultType::Success(&m_pageTables);
}

InitialAddressSpacePreparationError InitialAddressSpacePreparation::Activate() noexcept
{
    const InitialAddressSpacePreparationError validationError = ValidatePreparedState();
    if (validationError != InitialAddressSpacePreparationError::None)
    {
        return validationError;
    }
    m_state = State::ActivationClaimed;
#if defined(ASTERIA_TARGET_AMD64) && defined(__x86_64__)
    EstablishCurrentControlState();
#endif
    const Foundation::UInt64 packedSelectors =
        (static_cast<Foundation::UInt64>(m_summary.CodeSelector) << 16U) | m_summary.StackSelector;
    const Foundation::UInt64 adapterStatus = AsteriaAmd64ActivateInitialAddressSpace(
        m_summary.RootPhysicalAddress.GetValue(), m_summary.InitialStackPointer,
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(m_globalDescriptorRegister)),
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(m_interruptDescriptorRegister)),
        m_processorState.Cr3, packedSelectors);
    m_state = State::Failed;
    m_failure =
        MakeFailure(adapterStatus == 0U ? InitialAddressSpacePreparationError::UnexpectedAdapterReturn
                                        : InitialAddressSpacePreparationError::UnsupportedProcessorState,
                    InitialProcessorStateCaptureError::None, InitialPageTablePreparationError::None,
                    InitialPageTableBuildError::None, InitialPageTableVerificationError::None,
                    Kernel::Memory::InitialMappingPlanError::None, 0U, static_cast<Foundation::UInt32>(adapterStatus));
    return m_failure.Error;
}

const InitialAddressSpacePreparationFailure& InitialAddressSpacePreparation::GetFailure() const noexcept
{
    return m_failure;
}
} // Namespace Asteria::Platform::Amd64::Memory
