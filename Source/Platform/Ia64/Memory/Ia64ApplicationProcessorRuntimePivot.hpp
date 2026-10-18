// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_APPLICATION_PROCESSOR_RUNTIME_PIVOT_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_APPLICATION_PROCESSOR_RUNTIME_PIVOT_HPP_INCLUDED

#include "Foundation/Compiler.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimeLayout.hpp"

namespace Asteria::Platform::Ia64::Memory
{
enum class Ia64ApplicationProcessorRuntimePivotError : Foundation::UInt32
{
    None,
    InvalidProcessor,
    InvalidLayout,
    InvalidAddressWidth,
    NonzeroReserved,
    InvalidGeometry,
    UnsupportedProcessorState,
    UnexpectedAdapterReturn,
};

/// Replaces one AP's physical Region-0 stack and RSE context with the exact
/// translated idle aliases from its compact runtime layout.
///
/// The caller has already installed the runtime IVT and active translation
/// root, committed every required mapping, disabled local interrupts, excluded
/// preemption/migration/re-entry, and retained execution wholly in physical
/// Region 0 with translation disabled. Pivot validates every value argument
/// before native entry. The native leaf rejects visible nonzero PSR.ic,
/// PSR.i, PSR.pk, PSR.dt, PSR.rt, or PSR.it before commit. CPL0 and PSR.vm=0
/// remain architectural caller preconditions because mov-from-PSR cannot
/// validate them.
///
/// Native rejection may return UnsupportedProcessorState. Once the leaf has
/// selected bank one and abandoned the inherited RSE frame, it cannot return
/// or roll back. It enters the fixed translated landing, passes processor to
/// AsteriaIa64RunApplicationProcessorTranslatedKernel, and fail-stops if that
/// continuation returns.
class Ia64ApplicationProcessorRuntimePivot final
{
public:
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA static Ia64ApplicationProcessorRuntimePivotError
    Pivot(Kernel::Scheduler::CpuId processor, const Ia64ApplicationProcessorRuntimeProcessorLayout& layout,
          Foundation::UInt8 physicalAddressBits,
          Foundation::UInt8 implementedVirtualAddressMostSignificantBit) noexcept;

private:
    Ia64ApplicationProcessorRuntimePivot() = delete;
};

static_assert(sizeof(Ia64ApplicationProcessorRuntimePivotError) == 4U);
static_assert(static_cast<Foundation::UInt32>(Ia64ApplicationProcessorRuntimePivotError::None) == 0U);
static_assert(static_cast<Foundation::UInt32>(Ia64ApplicationProcessorRuntimePivotError::InvalidProcessor) == 1U);
static_assert(static_cast<Foundation::UInt32>(Ia64ApplicationProcessorRuntimePivotError::InvalidLayout) == 2U);
static_assert(static_cast<Foundation::UInt32>(Ia64ApplicationProcessorRuntimePivotError::InvalidAddressWidth) == 3U);
static_assert(static_cast<Foundation::UInt32>(Ia64ApplicationProcessorRuntimePivotError::NonzeroReserved) == 4U);
static_assert(static_cast<Foundation::UInt32>(Ia64ApplicationProcessorRuntimePivotError::InvalidGeometry) == 5U);
static_assert(static_cast<Foundation::UInt32>(Ia64ApplicationProcessorRuntimePivotError::UnsupportedProcessorState) ==
              6U);
static_assert(static_cast<Foundation::UInt32>(Ia64ApplicationProcessorRuntimePivotError::UnexpectedAdapterReturn) ==
              7U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeProcessorLayout) == 32U);
static_assert(alignof(Ia64ApplicationProcessorRuntimeProcessorLayout) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorRuntimeProcessorLayout));
static_assert(__is_standard_layout(Ia64ApplicationProcessorRuntimeProcessorLayout));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorRuntimeProcessorLayout));
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeProcessorLayout, SlotVirtualBase) == 0U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeProcessorLayout, IdlePhysicalBase) == 8U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeProcessorLayout, WorkerPhysicalBase) == 16U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeProcessorLayout, Processor) == 24U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeProcessorLayout, Reserved) == 25U);
static_assert(Ia64ApplicationProcessorRuntimeIdleAliasSizeBytes ==
              Ia64ApplicationProcessorRuntimeIdleStackSizeBytes + Ia64ApplicationProcessorRuntimeIdleRseSizeBytes);
static_assert(Ia64ApplicationProcessorRuntimeWorkerAliasSizeBytes ==
              Ia64ApplicationProcessorRuntimeWorkerStackSizeBytes + Ia64ApplicationProcessorRuntimeWorkerRseSizeBytes);
static_assert(Ia64ApplicationProcessorRuntimeInitialStackScratchSizeBytes == 16U);
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_APPLICATION_PROCESSOR_RUNTIME_PIVOT_HPP_INCLUDED
