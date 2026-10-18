// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimePivot.hpp"

#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimePivotRegisters.hpp"

extern "C" Asteria::Foundation::UInt64
AsteriaIa64PivotApplicationProcessorRuntime(Asteria::Foundation::UInt64 processorValue,
                                            Asteria::Foundation::UInt64 idleInitialStackPointer,
                                            Asteria::Foundation::UInt64 idleInitialBspstore) noexcept;

extern "C" [[noreturn]] void
AsteriaIa64RunApplicationProcessorTranslatedKernel(Asteria::Foundation::UInt64 processorValue) noexcept;

static_assert(__is_same(decltype(&AsteriaIa64PivotApplicationProcessorRuntime),
                        Asteria::Foundation::UInt64 (*)(Asteria::Foundation::UInt64, Asteria::Foundation::UInt64,
                                                        Asteria::Foundation::UInt64) noexcept));
static_assert(__is_same(decltype(&AsteriaIa64RunApplicationProcessorTranslatedKernel),
                        void (*)(Asteria::Foundation::UInt64) noexcept));

// bsw.1 replaces r16 through r31. These are the only four values retained
// across that boundary, and the assembly operands of
// Ia64ApplicationProcessorRuntimePivot.S are built from these exact asserted
// numbers rather than disconnected register-name literals.
static_assert(ASTERIA_IA64_AP_PIVOT_CPU_REGISTER_NUMBER < 16);
static_assert(ASTERIA_IA64_AP_PIVOT_STACK_REGISTER_NUMBER < 16);
static_assert(ASTERIA_IA64_AP_PIVOT_BSPSTORE_REGISTER_NUMBER < 16);
static_assert(ASTERIA_IA64_AP_PIVOT_LANDING_REGISTER_NUMBER < 16);
static_assert(ASTERIA_IA64_AP_PIVOT_CPU_REGISTER_NUMBER != ASTERIA_IA64_AP_PIVOT_STACK_REGISTER_NUMBER);
static_assert(ASTERIA_IA64_AP_PIVOT_CPU_REGISTER_NUMBER != ASTERIA_IA64_AP_PIVOT_BSPSTORE_REGISTER_NUMBER);
static_assert(ASTERIA_IA64_AP_PIVOT_CPU_REGISTER_NUMBER != ASTERIA_IA64_AP_PIVOT_LANDING_REGISTER_NUMBER);
static_assert(ASTERIA_IA64_AP_PIVOT_STACK_REGISTER_NUMBER != ASTERIA_IA64_AP_PIVOT_BSPSTORE_REGISTER_NUMBER);
static_assert(ASTERIA_IA64_AP_PIVOT_STACK_REGISTER_NUMBER != ASTERIA_IA64_AP_PIVOT_LANDING_REGISTER_NUMBER);
static_assert(ASTERIA_IA64_AP_PIVOT_BSPSTORE_REGISTER_NUMBER != ASTERIA_IA64_AP_PIVOT_LANDING_REGISTER_NUMBER);

// The target entry points are in Ia64ApplicationProcessorRuntimePivot.S.

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt8 MinimumPhysicalAddressBits{32U};
constexpr Foundation::UInt8 MaximumPhysicalAddressBits{63U};
constexpr Foundation::UInt8 MaximumImplementedVirtualAddressBit{60U};
constexpr Foundation::UInt64 AdapterUnsupportedProcessorState{1U};

[[nodiscard]] constexpr bool ReservedBytesAreZero(const Ia64ApplicationProcessorRuntimeProcessorLayout& layout) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(layout.Reserved); ++index)
    {
        if (layout.Reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool IsAligned(const Foundation::UInt64 value, const Foundation::UInt64 alignment) noexcept
{
    return (value & (alignment - 1U)) == 0U;
}

[[nodiscard]] constexpr bool TryGetExclusiveEnd(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes,
                                                Foundation::UInt64& end) noexcept
{
    if ((sizeBytes == 0U) || (start > (MaximumUInt64 - sizeBytes)))
    {
        return false;
    }
    end = start + sizeBytes;
    return true;
}

[[nodiscard]] constexpr bool IsVirtualAddressImplemented(const Foundation::UInt64 address,
                                                         const Foundation::UInt8 implementedMostSignificantBit) noexcept
{
    if (implementedMostSignificantBit == MaximumImplementedVirtualAddressBit)
    {
        return true;
    }
    const Foundation::UInt8 unimplementedBitCount =
        static_cast<Foundation::UInt8>(MaximumImplementedVirtualAddressBit - implementedMostSignificantBit);
    const Foundation::UInt64 mask = (Foundation::UInt64{1U} << unimplementedBitCount) - 1U;
    const Foundation::UInt64 actual = (address >> (implementedMostSignificantBit + 1U)) & mask;
    const Foundation::UInt64 expected =
        ((address & (Foundation::UInt64{1U} << implementedMostSignificantBit)) != 0U) ? mask : 0U;
    return actual == expected;
}

[[nodiscard]] constexpr bool IsPhysicalRangeEncodable(const Foundation::UInt64 start,
                                                      const Foundation::UInt64 sizeBytes,
                                                      const Foundation::UInt8 physicalAddressBits,
                                                      Foundation::UInt64& end) noexcept
{
    if (!TryGetExclusiveEnd(start, sizeBytes, end))
    {
        return false;
    }
    const Foundation::UInt8 encoderBits =
        physicalAddressBits < Ia64ApplicationProcessorRuntimeEncoderPhysicalAddressBits
            ? physicalAddressBits
            : Ia64ApplicationProcessorRuntimeEncoderPhysicalAddressBits;
    return (start != 0U) && (end <= (Foundation::UInt64{1U} << encoderBits));
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] constexpr bool
IsGeometryValid(const Kernel::Scheduler::CpuId processor, const Ia64ApplicationProcessorRuntimeProcessorLayout& layout,
                const Foundation::UInt8 physicalAddressBits,
                const Foundation::UInt8 implementedVirtualAddressMostSignificantBit) noexcept
{
    const Foundation::UInt64 processorIndex = static_cast<Foundation::UInt64>(processor.GetValue() - 1U);
    const Foundation::UInt64 expectedSlotBase = Ia64ApplicationProcessorRuntimeRegionOneBase +
                                                (processorIndex * Ia64ApplicationProcessorRuntimeProcessorStride);
    if (layout.SlotVirtualBase != expectedSlotBase)
    {
        return false;
    }

    constexpr Foundation::UInt64 idleAlignment =
        ApplicationProcessorExecutionStorageAlignmentPages * Kernel::Memory::PhysicalPageSizeBytes;
    constexpr Foundation::UInt64 workerAlignment =
        Ia64ApplicationProcessorRuntimeWorkerPoolAlignmentPages * Kernel::Memory::PhysicalPageSizeBytes;
    if (!IsAligned(layout.IdlePhysicalBase, idleAlignment) || !IsAligned(layout.WorkerPhysicalBase, workerAlignment))
    {
        return false;
    }

    Foundation::UInt64 idlePhysicalEnd{0U};
    Foundation::UInt64 workerPhysicalEnd{0U};
    if (!IsPhysicalRangeEncodable(layout.IdlePhysicalBase, Ia64ApplicationProcessorRuntimeIdleAliasSizeBytes,
                                  physicalAddressBits, idlePhysicalEnd) ||
        !IsPhysicalRangeEncodable(layout.WorkerPhysicalBase, Ia64ApplicationProcessorRuntimeWorkerAliasSizeBytes,
                                  physicalAddressBits, workerPhysicalEnd) ||
        RangesOverlap(layout.IdlePhysicalBase, idlePhysicalEnd, layout.WorkerPhysicalBase, workerPhysicalEnd))
    {
        return false;
    }

    Foundation::UInt64 slotEnd{0U};
    if (!TryGetExclusiveEnd(layout.SlotVirtualBase, Ia64ApplicationProcessorRuntimeProcessorStride, slotEnd) ||
        !IsVirtualAddressImplemented(layout.SlotVirtualBase, implementedVirtualAddressMostSignificantBit) ||
        !IsVirtualAddressImplemented(slotEnd - 1U, implementedVirtualAddressMostSignificantBit))
    {
        return false;
    }

    const Foundation::UInt64 idleStackPointer =
        Ia64ApplicationProcessorRuntimeLayoutPlanner::GetIdleInitialStackPointer(layout);
    const Foundation::UInt64 idleBspstore =
        Ia64ApplicationProcessorRuntimeLayoutPlanner::GetIdleInitialBspstore(layout);
    return (idleStackPointer == Ia64ApplicationProcessorRuntimeLayoutPlanner::GetIdleMemoryStackVirtualStart(layout) +
                                    Ia64ApplicationProcessorRuntimeIdleStackSizeBytes -
                                    Ia64ApplicationProcessorRuntimeInitialStackScratchSizeBytes) &&
           (idleBspstore == Ia64ApplicationProcessorRuntimeLayoutPlanner::GetIdleRseBackingStoreVirtualStart(layout)) &&
           IsAligned(idleBspstore, Kernel::Memory::PhysicalPageSizeBytes) && (idleStackPointer < idleBspstore) &&
           (idleBspstore < slotEnd);
}
} // namespace

Ia64ApplicationProcessorRuntimePivotError Ia64ApplicationProcessorRuntimePivot::Pivot(
    const Kernel::Scheduler::CpuId processor, const Ia64ApplicationProcessorRuntimeProcessorLayout& layout,
    const Foundation::UInt8 physicalAddressBits,
    const Foundation::UInt8 implementedVirtualAddressMostSignificantBit) noexcept
{
    // Retain one bounded value snapshot so validation and native arguments
    // cannot observe different layout words. The caller still owns ordinary
    // C++ data-race exclusion while this copy is collected.
    const Ia64ApplicationProcessorRuntimeProcessorLayout layoutSnapshot{layout};
    if (!processor.IsValid() || (processor.GetValue() == 0U))
    {
        return Ia64ApplicationProcessorRuntimePivotError::InvalidProcessor;
    }
    if (!layoutSnapshot.Processor.IsValid() || (layoutSnapshot.Processor.GetValue() != processor.GetValue()))
    {
        return Ia64ApplicationProcessorRuntimePivotError::InvalidLayout;
    }
    if ((physicalAddressBits < MinimumPhysicalAddressBits) || (physicalAddressBits > MaximumPhysicalAddressBits) ||
        (implementedVirtualAddressMostSignificantBit <
         Ia64ApplicationProcessorRuntimeMinimumVirtualAddressMostSignificantBit) ||
        (implementedVirtualAddressMostSignificantBit > MaximumImplementedVirtualAddressBit))
    {
        return Ia64ApplicationProcessorRuntimePivotError::InvalidAddressWidth;
    }
    if (!ReservedBytesAreZero(layoutSnapshot))
    {
        return Ia64ApplicationProcessorRuntimePivotError::NonzeroReserved;
    }
    if (!IsGeometryValid(processor, layoutSnapshot, physicalAddressBits, implementedVirtualAddressMostSignificantBit))
    {
        return Ia64ApplicationProcessorRuntimePivotError::InvalidGeometry;
    }

    const Foundation::UInt64 idleInitialStackPointer =
        Ia64ApplicationProcessorRuntimeLayoutPlanner::GetIdleInitialStackPointer(layoutSnapshot);
    const Foundation::UInt64 idleInitialBspstore =
        Ia64ApplicationProcessorRuntimeLayoutPlanner::GetIdleInitialBspstore(layoutSnapshot);
    const Foundation::UInt64 adapterStatus = AsteriaIa64PivotApplicationProcessorRuntime(
        static_cast<Foundation::UInt64>(processor.GetValue()), idleInitialStackPointer, idleInitialBspstore);
    if (adapterStatus == AdapterUnsupportedProcessorState)
    {
        return Ia64ApplicationProcessorRuntimePivotError::UnsupportedProcessorState;
    }
    return Ia64ApplicationProcessorRuntimePivotError::UnexpectedAdapterReturn;
}
} // namespace Asteria::Platform::Ia64::Memory
