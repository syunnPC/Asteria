// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimePivot.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
Foundation::Size g_pivotCallCount{0U};
Foundation::UInt64 g_forwardedProcessor{0U};
Foundation::UInt64 g_forwardedStackPointer{0U};
Foundation::UInt64 g_forwardedBspstore{0U};
Foundation::UInt64 g_pivotAdapterStatus{0U};
} // namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64
AsteriaIa64PivotApplicationProcessorRuntime(const Asteria::Foundation::UInt64 processorValue,
                                            const Asteria::Foundation::UInt64 idleInitialStackPointer,
                                            const Asteria::Foundation::UInt64 idleInitialBspstore) noexcept
{
    ++Asteria::Test::g_pivotCallCount;
    Asteria::Test::g_forwardedProcessor = processorValue;
    Asteria::Test::g_forwardedStackPointer = idleInitialStackPointer;
    Asteria::Test::g_forwardedBspstore = idleInitialBspstore;
    return Asteria::Test::g_pivotAdapterStatus;
}

namespace Asteria::Test
{
namespace
{
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeIdleAliasSizeBytes;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeIdleStackSizeBytes;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeInitialStackScratchSizeBytes;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimePivot;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimePivotError;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeProcessorLayout;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeProcessorStride;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeRegionOneBase;

constexpr Foundation::UInt8 ValidPhysicalAddressBits{48U};
constexpr Foundation::UInt8 ValidVirtualAddressMostSignificantBit{53U};
constexpr Foundation::UInt64 AdapterRejectedState{1U};

static_assert(__is_same(decltype(&AsteriaIa64PivotApplicationProcessorRuntime),
                        Foundation::UInt64 (*)(Foundation::UInt64, Foundation::UInt64, Foundation::UInt64) noexcept));

[[nodiscard]] constexpr Ia64ApplicationProcessorRuntimeProcessorLayout
ValidLayout(const Foundation::UInt8 processorValue) noexcept
{
    const Foundation::UInt64 processorIndex = static_cast<Foundation::UInt64>(processorValue - 1U);
    return Ia64ApplicationProcessorRuntimeProcessorLayout{
        .SlotVirtualBase = Ia64ApplicationProcessorRuntimeRegionOneBase +
                           (processorIndex * Ia64ApplicationProcessorRuntimeProcessorStride),
        .IdlePhysicalBase = 0x20'0000U + (processorIndex * 0x1'0000U),
        .WorkerPhysicalBase = 0x30'0000U + (processorIndex * 0x8000U),
        .Processor = Kernel::Scheduler::CpuId{processorValue},
        .Reserved = {},
    };
}

void ResetAdapter(const Foundation::UInt64 status = AdapterRejectedState) noexcept
{
    g_pivotCallCount = 0U;
    g_forwardedProcessor = 0U;
    g_forwardedStackPointer = 0U;
    g_forwardedBspstore = 0U;
    g_pivotAdapterStatus = status;
}

[[nodiscard]] bool AdapterWasNotCalled() noexcept
{
    return (g_pivotCallCount == 0U) && (g_forwardedProcessor == 0U) && (g_forwardedStackPointer == 0U) &&
           (g_forwardedBspstore == 0U);
}

[[nodiscard]] Ia64ApplicationProcessorRuntimePivotError
Invoke(const Kernel::Scheduler::CpuId processor, const Ia64ApplicationProcessorRuntimeProcessorLayout& layout,
       const Foundation::UInt8 physicalAddressBits = ValidPhysicalAddressBits,
       const Foundation::UInt8 virtualAddressMostSignificantBit = ValidVirtualAddressMostSignificantBit) noexcept
{
    return Ia64ApplicationProcessorRuntimePivot::Pivot(processor, layout, physicalAddressBits,
                                                       virtualAddressMostSignificantBit);
}

void CheckRejectedBeforeAdapter(TestContext& runner, const Kernel::Scheduler::CpuId processor,
                                const Ia64ApplicationProcessorRuntimeProcessorLayout& layout,
                                const Foundation::UInt8 physicalAddressBits,
                                const Foundation::UInt8 virtualAddressMostSignificantBit,
                                const Ia64ApplicationProcessorRuntimePivotError expected,
                                const char* const description) noexcept
{
    ResetAdapter();
    ASTERIA_EXPECT(runner, description,
                   Invoke(processor, layout, physicalAddressBits, virtualAddressMostSignificantBit) == expected,
                   AdapterWasNotCalled());
}

void CheckAcceptedByAdapter(TestContext& runner, const Ia64ApplicationProcessorRuntimeProcessorLayout& layout,
                            const Foundation::UInt8 physicalAddressBits,
                            const Foundation::UInt8 virtualAddressMostSignificantBit,
                            const char* const description) noexcept
{
    ResetAdapter();
    ASTERIA_EXPECT(runner, description,
                   Invoke(layout.Processor, layout, physicalAddressBits, virtualAddressMostSignificantBit) ==
                       Ia64ApplicationProcessorRuntimePivotError::UnsupportedProcessorState,
                   g_pivotCallCount == 1U);
}

void TestExactArgumentsForEveryApplicationProcessor(TestContext& runner) noexcept
{
    for (Foundation::UInt8 processorValue{1U}; processorValue <= 3U; ++processorValue)
    {
        const auto layout = ValidLayout(processorValue);
        ResetAdapter(AdapterRejectedState);
        const Ia64ApplicationProcessorRuntimePivotError error =
            Invoke(Kernel::Scheduler::CpuId{processorValue}, layout);
        const Foundation::UInt64 expectedBspstore =
            layout.SlotVirtualBase + Ia64ApplicationProcessorRuntimeIdleStackSizeBytes;
        const Foundation::UInt64 expectedStackPointer =
            expectedBspstore - Ia64ApplicationProcessorRuntimeInitialStackScratchSizeBytes;
        ASTERIA_EXPECT(runner, "each AP forwards its exact CpuId and translated idle SP/BSPSTORE",
                       error == Ia64ApplicationProcessorRuntimePivotError::UnsupportedProcessorState,
                       g_pivotCallCount == 1U, g_forwardedProcessor == processorValue,
                       g_forwardedStackPointer == expectedStackPointer, g_forwardedBspstore == expectedBspstore);
    }
}

void TestInvalidProcessorAndLayout(TestContext& runner) noexcept
{
    const auto valid = ValidLayout(1U);
    CheckRejectedBeforeAdapter(runner, Kernel::Scheduler::CpuId{}, valid, ValidPhysicalAddressBits,
                               ValidVirtualAddressMostSignificantBit,
                               Ia64ApplicationProcessorRuntimePivotError::InvalidProcessor,
                               "the default invalid CpuId is rejected before native entry");
    CheckRejectedBeforeAdapter(runner, Kernel::Scheduler::CpuId{0U}, valid, ValidPhysicalAddressBits,
                               ValidVirtualAddressMostSignificantBit,
                               Ia64ApplicationProcessorRuntimePivotError::InvalidProcessor,
                               "the BSP CpuId cannot enter an application-processor pivot");
    CheckRejectedBeforeAdapter(runner, Kernel::Scheduler::CpuId{4U}, valid, ValidPhysicalAddressBits,
                               ValidVirtualAddressMostSignificantBit,
                               Ia64ApplicationProcessorRuntimePivotError::InvalidProcessor,
                               "a CpuId beyond the processor topology is rejected");

    auto invalidLayoutProcessor = valid;
    invalidLayoutProcessor.Processor = Kernel::Scheduler::CpuId{};
    CheckRejectedBeforeAdapter(runner, Kernel::Scheduler::CpuId{1U}, invalidLayoutProcessor, ValidPhysicalAddressBits,
                               ValidVirtualAddressMostSignificantBit,
                               Ia64ApplicationProcessorRuntimePivotError::InvalidLayout,
                               "an invalid processor embedded in the compact layout is rejected");

    auto mismatchedLayout = ValidLayout(2U);
    CheckRejectedBeforeAdapter(runner, Kernel::Scheduler::CpuId{1U}, mismatchedLayout, ValidPhysicalAddressBits,
                               ValidVirtualAddressMostSignificantBit,
                               Ia64ApplicationProcessorRuntimePivotError::InvalidLayout,
                               "a layout for another AP cannot select this processor's aliases");
}

void TestAddressWidthValidation(TestContext& runner) noexcept
{
    const auto layout = ValidLayout(1U);
    struct AddressWidthCase
    {
        Foundation::UInt8 PhysicalBits;
        Foundation::UInt8 VirtualMostSignificantBit;
        const char* Description;
    };
    constexpr AddressWidthCase cases[]{
        AddressWidthCase{
            .PhysicalBits = 31U,
            .VirtualMostSignificantBit = 53U,
            .Description = "a physical-address width below 32 is rejected",
        },
        AddressWidthCase{
            .PhysicalBits = 64U,
            .VirtualMostSignificantBit = 53U,
            .Description = "an unrepresentable IA-64 physical width is rejected",
        },
        AddressWidthCase{
            .PhysicalBits = 48U,
            .VirtualMostSignificantBit = 49U,
            .Description = "an address width below the Region-1 alias minimum is rejected",
        },
        AddressWidthCase{
            .PhysicalBits = 48U,
            .VirtualMostSignificantBit = 61U,
            .Description = "a virtual offset width beyond bit 60 is rejected",
        },
    };
    for (const AddressWidthCase& invalid : cases)
    {
        CheckRejectedBeforeAdapter(runner, Kernel::Scheduler::CpuId{1U}, layout, invalid.PhysicalBits,
                                   invalid.VirtualMostSignificantBit,
                                   Ia64ApplicationProcessorRuntimePivotError::InvalidAddressWidth, invalid.Description);
    }
}

void TestReservedValidation(TestContext& runner) noexcept
{
    for (Foundation::Size index{0U}; index < 7U; ++index)
    {
        auto layout = ValidLayout(1U);
        layout.Reserved[index] = static_cast<Foundation::UInt8>(index + 1U);
        CheckRejectedBeforeAdapter(runner, Kernel::Scheduler::CpuId{1U}, layout, ValidPhysicalAddressBits,
                                   ValidVirtualAddressMostSignificantBit,
                                   Ia64ApplicationProcessorRuntimePivotError::NonzeroReserved,
                                   "every compact-layout reserved byte is checked before native entry");
    }
}

void TestGeometryValidation(TestContext& runner) noexcept
{
    struct GeometryCase
    {
        Ia64ApplicationProcessorRuntimeProcessorLayout Layout;
        Foundation::UInt8 PhysicalBits;
        const char* Description;
    };

    auto wrongSlot = ValidLayout(1U);
    ++wrongSlot.SlotVirtualBase;
    auto zeroIdle = ValidLayout(1U);
    zeroIdle.IdlePhysicalBase = 0U;
    auto unalignedIdle = ValidLayout(1U);
    ++unalignedIdle.IdlePhysicalBase;
    auto zeroWorker = ValidLayout(1U);
    zeroWorker.WorkerPhysicalBase = 0U;
    auto unalignedWorker = ValidLayout(1U);
    ++unalignedWorker.WorkerPhysicalBase;
    auto overlapping = ValidLayout(1U);
    overlapping.WorkerPhysicalBase = overlapping.IdlePhysicalBase;
    auto outsideAddressWidth = ValidLayout(1U);
    outsideAddressWidth.IdlePhysicalBase = 0xFFFF'8000U;

    const GeometryCase cases[]{
        GeometryCase{
            .Layout = wrongSlot,
            .PhysicalBits = ValidPhysicalAddressBits,
            .Description = "a noncanonical AP slot base is rejected",
        },
        GeometryCase{
            .Layout = zeroIdle,
            .PhysicalBits = ValidPhysicalAddressBits,
            .Description = "physical page zero cannot back the idle alias",
        },
        GeometryCase{
            .Layout = unalignedIdle,
            .PhysicalBits = ValidPhysicalAddressBits,
            .Description = "a misaligned idle physical base is rejected",
        },
        GeometryCase{
            .Layout = zeroWorker,
            .PhysicalBits = ValidPhysicalAddressBits,
            .Description = "physical page zero cannot back the worker alias",
        },
        GeometryCase{
            .Layout = unalignedWorker,
            .PhysicalBits = ValidPhysicalAddressBits,
            .Description = "a misaligned worker physical base is rejected",
        },
        GeometryCase{
            .Layout = overlapping,
            .PhysicalBits = ValidPhysicalAddressBits,
            .Description = "one physical range cannot back idle and worker aliases",
        },
        GeometryCase{
            .Layout = outsideAddressWidth,
            .PhysicalBits = 32U,
            .Description = "an idle range crossing the physical width is rejected",
        },
    };

    for (const GeometryCase& invalid : cases)
    {
        CheckRejectedBeforeAdapter(runner, Kernel::Scheduler::CpuId{1U}, invalid.Layout, invalid.PhysicalBits,
                                   ValidVirtualAddressMostSignificantBit,
                                   Ia64ApplicationProcessorRuntimePivotError::InvalidGeometry, invalid.Description);
    }
}

void TestBoundaryGeometry(TestContext& runner) noexcept
{
    const auto valid = ValidLayout(1U);
    CheckAcceptedByAdapter(runner, valid, 32U, 53U,
                           "the minimum accepted physical and virtual widths reach the adapter");
    CheckAcceptedByAdapter(runner, valid, 63U, 60U,
                           "the maximum accepted physical and virtual widths reach the adapter");

    auto idleAtPhysicalLimit = valid;
    idleAtPhysicalLimit.IdlePhysicalBase = 0xFFFF'0000U;
    CheckAcceptedByAdapter(runner, idleAtPhysicalLimit, 32U, 53U,
                           "an idle range ending exactly at the PAL physical limit is accepted");

    auto workerAtPhysicalLimit = valid;
    workerAtPhysicalLimit.WorkerPhysicalBase = 0xFFFF'8000U;
    CheckAcceptedByAdapter(runner, workerAtPhysicalLimit, 32U, 53U,
                           "a worker range ending exactly at the PAL physical limit is accepted");

    auto idleAtEncoderLimit = valid;
    idleAtEncoderLimit.IdlePhysicalBase =
        (Foundation::UInt64{1U} << 50U) - Ia64ApplicationProcessorRuntimeIdleAliasSizeBytes;
    CheckAcceptedByAdapter(runner, idleAtEncoderLimit, 63U, 53U,
                           "an idle range ending exactly at the encoder limit is accepted");

    auto idleCrossingEncoderLimit = valid;
    idleCrossingEncoderLimit.IdlePhysicalBase = (Foundation::UInt64{1U} << 50U) - 0x8000U;
    CheckRejectedBeforeAdapter(runner, Kernel::Scheduler::CpuId{1U}, idleCrossingEncoderLimit, 63U, 53U,
                               Ia64ApplicationProcessorRuntimePivotError::InvalidGeometry,
                               "an idle range crossing the encoder limit is rejected");

    auto wrappingIdle = valid;
    wrappingIdle.IdlePhysicalBase = 0xFFFF'FFFF'FFFF'8000ULL;
    CheckRejectedBeforeAdapter(runner, Kernel::Scheduler::CpuId{1U}, wrappingIdle, 63U, 53U,
                               Ia64ApplicationProcessorRuntimePivotError::InvalidGeometry,
                               "an idle range whose exclusive end overflows is rejected");

    auto adjacent = valid;
    adjacent.WorkerPhysicalBase = adjacent.IdlePhysicalBase + Ia64ApplicationProcessorRuntimeIdleAliasSizeBytes;
    CheckAcceptedByAdapter(runner, adjacent, ValidPhysicalAddressBits, ValidVirtualAddressMostSignificantBit,
                           "exactly adjacent idle and worker physical ranges are accepted");

    auto partiallyOverlapping = valid;
    partiallyOverlapping.WorkerPhysicalBase = partiallyOverlapping.IdlePhysicalBase + 0xC000U;
    CheckRejectedBeforeAdapter(runner, Kernel::Scheduler::CpuId{1U}, partiallyOverlapping, ValidPhysicalAddressBits,
                               ValidVirtualAddressMostSignificantBit,
                               Ia64ApplicationProcessorRuntimePivotError::InvalidGeometry,
                               "partially overlapping idle and worker physical ranges are rejected");
}

void TestReturnedAdapterStatusHandling(TestContext& runner) noexcept
{
    const auto layout = ValidLayout(3U);
    struct StatusCase
    {
        Foundation::UInt64 Status;
        Ia64ApplicationProcessorRuntimePivotError Expected;
        const char* Description;
    };
    constexpr StatusCase cases[]{
        StatusCase{
            .Status = AdapterRejectedState,
            .Expected = Ia64ApplicationProcessorRuntimePivotError::UnsupportedProcessorState,
            .Description = "native status one is a pre-commit state rejection",
        },
        StatusCase{
            .Status = 0U,
            .Expected = Ia64ApplicationProcessorRuntimePivotError::UnexpectedAdapterReturn,
            .Description = "an impossible returned success status fails closed",
        },
        StatusCase{
            .Status = 2U,
            .Expected = Ia64ApplicationProcessorRuntimePivotError::UnexpectedAdapterReturn,
            .Description = "an unknown returned native status fails closed",
        },
        StatusCase{
            .Status = ~Foundation::UInt64{0U},
            .Expected = Ia64ApplicationProcessorRuntimePivotError::UnexpectedAdapterReturn,
            .Description = "the maximum returned adapter word fails closed",
        },
    };

    for (const StatusCase& status : cases)
    {
        ResetAdapter(status.Status);
        ASTERIA_EXPECT(
            runner, status.Description, Invoke(Kernel::Scheduler::CpuId{3U}, layout) == status.Expected,
            g_pivotCallCount == 1U, g_forwardedProcessor == 3U,
            g_forwardedStackPointer == Ia64ApplicationProcessorRuntimeLayoutPlanner::GetIdleInitialStackPointer(layout),
            g_forwardedBspstore == Ia64ApplicationProcessorRuntimeLayoutPlanner::GetIdleInitialBspstore(layout));
    }
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64ApplicationProcessorRuntimePivotTests"};
    Asteria::Test::TestExactArgumentsForEveryApplicationProcessor(runner);
    Asteria::Test::TestInvalidProcessorAndLayout(runner);
    Asteria::Test::TestAddressWidthValidation(runner);
    Asteria::Test::TestReservedValidation(runner);
    Asteria::Test::TestGeometryValidation(runner);
    Asteria::Test::TestBoundaryGeometry(runner);
    Asteria::Test::TestReturnedAdapterStatusHandling(runner);
    return runner.Finish();
}
