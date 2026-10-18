// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Firmware/PalStaticCallAdapter.hpp"

extern "C" Asteria::Platform::Ia64::Memory::PalStaticCallResult
AsteriaIa64InvokePhysicalPalStatic(Asteria::Foundation::UInt64 entryPhysicalAddress,
                                   Asteria::Foundation::UInt64 procedureIndex, Asteria::Foundation::UInt64 argument1,
                                   Asteria::Foundation::UInt64 argument2, Asteria::Foundation::UInt64 argument3,
                                   Asteria::Foundation::UInt64* invocationState) noexcept;

// The target entry point is in PalStaticCallAdapter.S.

namespace Asteria::Platform::Ia64::Firmware
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 PageSizeBytes{4096U};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};
constexpr Foundation::UInt64 MaximumPageCount{Foundation::UInt64{1U} << 52U};
constexpr Foundation::UInt64 PhysicalAddressTopBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 MaximumPhysicalAddress{PhysicalAddressTopBit - 1U};
constexpr Foundation::UInt64 PalProcedureAccessBytes{16U};
constexpr Foundation::UInt32 PalCodeMemoryType{13U};
constexpr Foundation::UInt64 MemoryAttributeProcedureIndex{5U};
constexpr Foundation::UInt64 TranslationCachePurgeProcedureIndex{6U};
constexpr Foundation::UInt64 VirtualMemorySummaryProcedureIndex{8U};
constexpr Foundation::UInt64 RseInformationProcedureIndex{19U};
constexpr Foundation::UInt64 VirtualMemoryPageSizeProcedureIndex{34U};

[[nodiscard]] Foundation::Result<Memory::PalStaticCallResult, PalStaticCallAdapterError>
Fail(const PalStaticCallAdapterError error) noexcept
{
    return Foundation::Result<Memory::PalStaticCallResult, PalStaticCallAdapterError>::Failure(error);
}

[[nodiscard]] bool TryGetRegionEnd(const Boot::PhysicalMemoryRegion& region, Foundation::UInt64& physicalEnd) noexcept
{
    if ((region.PageCount == 0U) || (region.PageCount > MaximumPageCount) ||
        ((region.PhysicalStart & PageOffsetMask) != 0U) ||
        ((region.VirtualStart != 0U) && ((region.VirtualStart & PageOffsetMask) != 0U)))
    {
        return false;
    }

    if (region.PageCount == MaximumPageCount)
    {
        if ((region.PhysicalStart != 0U) || (region.VirtualStart != 0U))
        {
            return false;
        }
        physicalEnd = MaximumUInt64;
        return true;
    }

    const Foundation::UInt64 lastByteOffset = (region.PageCount << 12U) - 1U;
    if ((region.PhysicalStart > (MaximumUInt64 - lastByteOffset)) ||
        ((region.VirtualStart != 0U) && (region.VirtualStart > (MaximumUInt64 - lastByteOffset))))
    {
        return false;
    }
    physicalEnd = region.PhysicalStart + lastByteOffset;
    return true;
}

[[nodiscard]] bool IsPalBindingValid(const Foundation::UInt64 entry,
                                     const Boot::PhysicalMemoryRegion& palCodeRegion) noexcept
{
    if ((entry == 0U) || ((entry & (PalProcedureAccessBytes - 1U)) != 0U) ||
        (entry > (MaximumPhysicalAddress - (PalProcedureAccessBytes - 1U))) ||
        (palCodeRegion.FirmwareType != PalCodeMemoryType) || (palCodeRegion.Kind != Boot::PhysicalMemoryKind::PalCode))
    {
        return false;
    }

    Foundation::UInt64 regionEnd{0U};
    if (!TryGetRegionEnd(palCodeRegion, regionEnd))
    {
        return false;
    }
    const Foundation::UInt64 entryEnd = entry + PalProcedureAccessBytes - 1U;
    return (entry >= palCodeRegion.PhysicalStart) && (entryEnd <= regionEnd);
}
} // namespace

Foundation::Result<Memory::PalStaticCallResult, PalStaticCallAdapterError>
PalStaticCallAdapter::Invoke(const SalEntrypointBinding& binding, const Foundation::UInt64 procedureIndex) noexcept
{
    const Foundation::UInt64 entryPhysicalAddress = binding.PalProcedurePhysicalAddress;
    const Boot::PhysicalMemoryRegion palCodeRegion = binding.PalCodeRegion;
    if (!IsPalBindingValid(entryPhysicalAddress, palCodeRegion))
    {
        return Fail(PalStaticCallAdapterError::InvalidBinding);
    }

    Foundation::UInt64 invocationState{~Foundation::UInt64{0U}};
    const Memory::PalStaticCallResult result =
        AsteriaIa64InvokePhysicalPalStatic(entryPhysicalAddress, procedureIndex, 0U, 0U, 0U, &invocationState);
    if (invocationState != 0U)
    {
        return Fail(PalStaticCallAdapterError::UnsupportedProcessorState);
    }
    return Foundation::Result<Memory::PalStaticCallResult, PalStaticCallAdapterError>::Success(result);
}

Foundation::Result<Memory::PalStaticCallResult, PalStaticCallAdapterError>
PalStaticCallAdapter::QueryMemoryAttributes(const SalEntrypointBinding& binding) noexcept
{
    return Invoke(binding, MemoryAttributeProcedureIndex);
}

Foundation::Result<Memory::PalStaticCallResult, PalStaticCallAdapterError>
PalStaticCallAdapter::QueryTranslationCachePurgeInformation(const SalEntrypointBinding& binding) noexcept
{
    return Invoke(binding, TranslationCachePurgeProcedureIndex);
}

Foundation::Result<Memory::PalStaticCallResult, PalStaticCallAdapterError>
PalStaticCallAdapter::QueryRseInformation(const SalEntrypointBinding& binding) noexcept
{
    return Invoke(binding, RseInformationProcedureIndex);
}

Foundation::Result<Memory::PalStaticCallResult, PalStaticCallAdapterError>
PalStaticCallAdapter::QueryVirtualMemorySummary(const SalEntrypointBinding& binding) noexcept
{
    return Invoke(binding, VirtualMemorySummaryProcedureIndex);
}

Foundation::Result<Memory::PalStaticCallResult, PalStaticCallAdapterError>
PalStaticCallAdapter::QueryVirtualMemoryPageSizes(const SalEntrypointBinding& binding) noexcept
{
    return Invoke(binding, VirtualMemoryPageSizeProcedureIndex);
}
} // namespace Asteria::Platform::Ia64::Firmware
