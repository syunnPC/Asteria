// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimeLayout.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace Ia64Memory = Platform::Ia64::Memory;
using RuntimeLayoutPlanner = Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner;
using RuntimeLayoutError = Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutError;

struct Fixture
{
    Ia64Memory::ApplicationProcessorExecutionStorageDescriptor Idle[3U];
};

[[nodiscard]] constexpr Ia64Memory::ApplicationProcessorExecutionStorageDescriptor
IdleDescriptor(const Foundation::UInt64 physicalStart, const Foundation::UInt8 processor) noexcept
{
    return Ia64Memory::ApplicationProcessorExecutionStorageDescriptor{
        .AllocationPhysicalStart = physicalStart,
        .AllocationSizeBytes = Ia64Memory::ApplicationProcessorExecutionStorageSizeBytes,
        .MemoryStackPhysicalStart = physicalStart,
        .InitialStackPointer = physicalStart + Ia64Memory::Ia64ApplicationProcessorRuntimeIdleStackSizeBytes -
                               Ia64Memory::Ia64ApplicationProcessorRuntimeInitialStackScratchSizeBytes,
        .MemoryStackSizeBytes = Ia64Memory::Ia64ApplicationProcessorRuntimeIdleStackSizeBytes,
        .RseBackingStorePhysicalStart = physicalStart + Ia64Memory::Ia64ApplicationProcessorRuntimeIdleStackSizeBytes,
        .RseBackingStoreSizeBytes = Ia64Memory::Ia64ApplicationProcessorRuntimeIdleRseSizeBytes,
        .Processor = Kernel::Scheduler::CpuId{processor},
        .HardwareId = static_cast<Foundation::UInt8>(0x20U + processor),
        .HardwareEid = static_cast<Foundation::UInt8>(0x30U + processor),
        .Reserved = {},
    };
}

[[nodiscard]] constexpr Fixture ValidFixture() noexcept
{
    return Fixture{
        .Idle = {IdleDescriptor(0x20'0000U, 1U), IdleDescriptor(0x21'0000U, 2U), IdleDescriptor(0x22'0000U, 3U)}};
}

[[nodiscard]] constexpr Foundation::UInt8 ProcessorCountForMask(const Foundation::UInt8 mask) noexcept
{
    return mask == 0x1U ? 1U : mask == 0x3U ? 2U : mask == 0x7U ? 3U : mask == 0xFU ? 4U : 0U;
}

[[nodiscard]] constexpr Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutRequest
ValidRequest(const Fixture& fixture, const Foundation::UInt8 mask) noexcept
{
    const Foundation::UInt8 processorCount = ProcessorCountForMask(mask);
    const Foundation::Size apCount = static_cast<Foundation::Size>(processorCount - 1U);
    return Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutRequest{
        .IdleStorage =
            Ia64Memory::ApplicationProcessorExecutionStorageDescriptorView{
                .Data = apCount == 0U ? nullptr : fixture.Idle, .Count = apCount},
        .WorkerPool =
            apCount == 0U
                ? Kernel::Memory::PhysicalPageRange{}
                : Kernel::Memory::
                      PhysicalPageRange{.PhysicalStart = 0x30'0000U,
                                        .PageCount = apCount *
                                                     Ia64Memory::Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor,
                                        .Attributes = 0x8U,
                                        .FirmwareType = 7U,
                                        .Reserved = 0U},
        .ProcessorCount = processorCount,
        .EnabledProcessorMask = mask,
        .CommonPhysicalAddressBits = 48U,
        .CommonImplementedVirtualAddressMostSignificantBit = 53U};
}

[[nodiscard]] constexpr bool PlansEqual(const Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutPlan& left,
                                        const Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutPlan& right) noexcept
{
    bool equal = (left.ProcessorCount == right.ProcessorCount) &&
                 (left.EnabledProcessorMask == right.EnabledProcessorMask) && (left.LayoutCount == right.LayoutCount);
    for (Foundation::Size reservedIndex{0U}; reservedIndex < sizeof(left.Reserved); ++reservedIndex)
    {
        equal = equal && (left.Reserved[reservedIndex] == right.Reserved[reservedIndex]);
    }
    for (Foundation::Size index{0U}; index < 3U; ++index)
    {
        equal = equal && (left.Processors[index].SlotVirtualBase == right.Processors[index].SlotVirtualBase) &&
                (left.Processors[index].IdlePhysicalBase == right.Processors[index].IdlePhysicalBase) &&
                (left.Processors[index].WorkerPhysicalBase == right.Processors[index].WorkerPhysicalBase) &&
                (left.Processors[index].Processor.GetValue() == right.Processors[index].Processor.GetValue());
        for (Foundation::Size reservedIndex{0U}; reservedIndex < sizeof(left.Processors[index].Reserved);
             ++reservedIndex)
        {
            equal = equal &&
                    (left.Processors[index].Reserved[reservedIndex] == right.Processors[index].Reserved[reservedIndex]);
        }
    }
    return equal;
}

[[nodiscard]] constexpr bool
IsRejectedAs(const Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutRequest& request,
             const Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutError expected) noexcept
{
    Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutPlan output{};
    output.Processors[0U].SlotVirtualBase = 0xA5A5'A5A5'A5A5'A5A5ULL;
    output.Processors[1U].IdlePhysicalBase = 0x5A5A'5A5A'5A5A'5A5AULL;
    output.ProcessorCount = 0xA5U;
    for (Foundation::Size index{0U}; index < 3U; ++index)
    {
        for (Foundation::Size reservedIndex{0U}; reservedIndex < sizeof(output.Processors[index].Reserved);
             ++reservedIndex)
        {
            output.Processors[index].Reserved[reservedIndex] =
                static_cast<Foundation::UInt8>(0x40U + (index * 8U) + reservedIndex);
        }
    }
    for (Foundation::Size reservedIndex{0U}; reservedIndex < sizeof(output.Reserved); ++reservedIndex)
    {
        output.Reserved[reservedIndex] = static_cast<Foundation::UInt8>(0x70U + reservedIndex);
    }
    const Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutPlan before = output;
    return (RuntimeLayoutPlanner::Validate(request) == expected) &&
           (RuntimeLayoutPlanner::Plan(request, output) == expected) && PlansEqual(before, output);
}

[[nodiscard]] constexpr bool CompileTimePlanIsExact() noexcept
{
    const Fixture fixture = ValidFixture();
    const auto request = ValidRequest(fixture, 0xFU);
    Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutPlan plan{};
    return (RuntimeLayoutPlanner::Plan(request, plan) ==
            Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutError::None) &&
           (plan.LayoutCount == 3U) && (plan.Processors[2U].Processor.GetValue() == 3U) &&
           (RuntimeLayoutPlanner::GetWorkerInitialStackPointer(plan.Processors[2U]) ==
            Ia64Memory::Ia64ApplicationProcessorRuntimeRegionOneBase +
                (2U * Ia64Memory::Ia64ApplicationProcessorRuntimeProcessorStride) +
                Ia64Memory::Ia64ApplicationProcessorRuntimeWorkerAliasOffset +
                Ia64Memory::Ia64ApplicationProcessorRuntimeWorkerStackSizeBytes -
                Ia64Memory::Ia64ApplicationProcessorRuntimeInitialStackScratchSizeBytes);
}

static_assert(CompileTimePlanIsExact());
static_assert(sizeof(Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutRequest) == 56U);
static_assert(sizeof(Ia64Memory::Ia64ApplicationProcessorRuntimeProcessorLayout) == 32U);
static_assert(sizeof(Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutPlan) == 104U);
static_assert(offsetof(Ia64Memory::Ia64ApplicationProcessorRuntimeProcessorLayout, Processor) == 24U);

void TestMasksAndFormulas(TestContext& runner) noexcept
{
    constexpr Foundation::UInt8 masks[4U]{0x1U, 0x3U, 0x7U, 0xFU};
    bool masksExact{true};
    for (const Foundation::UInt8 mask : masks)
    {
        const Fixture fixture = ValidFixture();
        const auto request = ValidRequest(fixture, mask);
        Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutPlan plan{};
        masksExact = masksExact &&
                     (RuntimeLayoutPlanner::Plan(request, plan) ==
                      Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutError::None) &&
                     (plan.ProcessorCount == request.ProcessorCount) && (plan.EnabledProcessorMask == mask) &&
                     (plan.LayoutCount == request.IdleStorage.Count);
    }
    ASTERIA_EXPECT(runner, "masks 0x1, 0x3, 0x7, and 0xF plan exactly", masksExact);

    const Fixture fixture = ValidFixture();
    const auto request = ValidRequest(fixture, 0xFU);
    Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutPlan plan{};
    bool formulas =
        RuntimeLayoutPlanner::Plan(request, plan) == Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutError::None;
    bool guards{formulas};
    for (Foundation::Size index{0U}; formulas && (index < 3U); ++index)
    {
        const auto& layout = plan.Processors[index];
        const Foundation::UInt64 slot = Ia64Memory::Ia64ApplicationProcessorRuntimeRegionOneBase +
                                        (index * Ia64Memory::Ia64ApplicationProcessorRuntimeProcessorStride);
        const Foundation::UInt64 worker = slot + Ia64Memory::Ia64ApplicationProcessorRuntimeWorkerAliasOffset;
        const Foundation::UInt64 workerPhysical =
            request.WorkerPool.PhysicalStart +
            (index * Ia64Memory::Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor *
             Kernel::Memory::PhysicalPageSizeBytes);
        formulas = formulas && (layout.Processor.GetValue() == index + 1U) && (layout.SlotVirtualBase == slot) &&
                   (layout.IdlePhysicalBase == fixture.Idle[index].MemoryStackPhysicalStart) &&
                   (RuntimeLayoutPlanner::GetIdleMemoryStackVirtualStart(layout) == slot) &&
                   (RuntimeLayoutPlanner::GetIdleRseBackingStoreVirtualStart(layout) ==
                    slot + Ia64Memory::Ia64ApplicationProcessorRuntimeIdleStackSizeBytes) &&
                   (RuntimeLayoutPlanner::GetIdleRseBackingStorePhysicalStart(layout) ==
                    fixture.Idle[index].RseBackingStorePhysicalStart) &&
                   (RuntimeLayoutPlanner::GetIdleInitialStackPointer(layout) ==
                    RuntimeLayoutPlanner::GetIdleInitialBspstore(layout) - 16U) &&
                   (RuntimeLayoutPlanner::GetWorkerAliasVirtualStart(layout) == worker) &&
                   (layout.WorkerPhysicalBase == workerPhysical) &&
                   (RuntimeLayoutPlanner::GetWorkerRseBackingStoreVirtualStart(layout) ==
                    worker + Ia64Memory::Ia64ApplicationProcessorRuntimeWorkerStackSizeBytes) &&
                   (RuntimeLayoutPlanner::GetWorkerRseBackingStorePhysicalStart(layout) ==
                    workerPhysical + Ia64Memory::Ia64ApplicationProcessorRuntimeWorkerStackSizeBytes) &&
                   (RuntimeLayoutPlanner::GetWorkerInitialStackPointer(layout) ==
                    RuntimeLayoutPlanner::GetWorkerInitialBspstore(layout) - 16U);
        guards = guards &&
                 (slot + Ia64Memory::Ia64ApplicationProcessorRuntimeIdleAliasSizeBytes ==
                  RuntimeLayoutPlanner::GetFirstGuardVirtualStart(layout)) &&
                 (RuntimeLayoutPlanner::GetFirstGuardVirtualStart(layout) +
                      Ia64Memory::Ia64ApplicationProcessorRuntimeFirstGuardSizeBytes ==
                  RuntimeLayoutPlanner::GetWorkerAliasVirtualStart(layout)) &&
                 (RuntimeLayoutPlanner::GetWorkerAliasVirtualStart(layout) +
                      Ia64Memory::Ia64ApplicationProcessorRuntimeWorkerAliasSizeBytes ==
                  RuntimeLayoutPlanner::GetSecondGuardVirtualStart(layout)) &&
                 (RuntimeLayoutPlanner::GetSecondGuardVirtualStart(layout) +
                      Ia64Memory::Ia64ApplicationProcessorRuntimeSecondGuardSizeBytes ==
                  slot + Ia64Memory::Ia64ApplicationProcessorRuntimeProcessorStride);
    }
    ASTERIA_EXPECT(runner, "CPUs 1 through 3 derive exact stack, RSE, SP, and BSPSTORE values", formulas);
    ASTERIA_EXPECT(runner, "both guards exactly fill every AP slot gap", guards);

    const auto smpOneRequest = ValidRequest(fixture, 0x1U);
    ASTERIA_EXPECT(runner, "SMP1 has canonical empty idle and worker inputs", smpOneRequest.IdleStorage.Data == nullptr,
                   smpOneRequest.WorkerPool.PhysicalStart == 0U, smpOneRequest.WorkerPool.PageCount == 0U);
}

void TestTopologyAndGeometryMutations(TestContext& runner) noexcept
{
    Fixture fixture = ValidFixture();
    auto request = ValidRequest(fixture, 0x7U);
    request.ProcessorCount = 0U;
    ASTERIA_EXPECT(runner, "zero CPUs are rejected atomically",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidProcessorTopology));
    request = ValidRequest(fixture, 0x7U);
    request.ProcessorCount = 5U;
    ASTERIA_EXPECT(runner, "more than four CPUs are rejected atomically",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidProcessorTopology));
    request = ValidRequest(fixture, 0x7U);
    request.EnabledProcessorMask = 0x5U;
    ASTERIA_EXPECT(runner, "sparse topology masks are rejected atomically",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidProcessorTopology));
    request = ValidRequest(fixture, 0x7U);
    request.IdleStorage.Count = 1U;
    ASTERIA_EXPECT(runner, "idle count must equal enabled AP count",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidProcessorTopology));
    request = ValidRequest(fixture, 0x1U);
    request.IdleStorage.Data = fixture.Idle;
    ASTERIA_EXPECT(runner, "SMP1 requires a canonical null idle view",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidProcessorTopology));

    request = ValidRequest(fixture, 0x3U);
    fixture.Idle[0U].AllocationPhysicalStart += 0x1000U;
    ASTERIA_EXPECT(runner, "misaligned idle storage is rejected",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidIdleStorage));
    fixture = ValidFixture();
    request = ValidRequest(fixture, 0x3U);
    ++fixture.Idle[0U].InitialStackPointer;
    ASTERIA_EXPECT(runner, "idle SP must be exact end-minus-sixteen",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidIdleStorage));
    fixture = ValidFixture();
    request = ValidRequest(fixture, 0x7U);
    fixture.Idle[1U].Processor = Kernel::Scheduler::CpuId{3U};
    ASTERIA_EXPECT(runner, "idle descriptors require exact CpuId order",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidIdleStorage));
    fixture = ValidFixture();
    request = ValidRequest(fixture, 0x3U);
    fixture.Idle[0U] = IdleDescriptor(0xFFFF'FFFF'FFFF'8000ULL, 1U);
    ASTERIA_EXPECT(runner, "wrapping idle storage is rejected",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidIdleStorage));

    fixture = ValidFixture();
    request = ValidRequest(fixture, 0x1U);
    request.WorkerPool.Attributes = 1U;
    ASTERIA_EXPECT(runner, "SMP1 requires an exactly zero worker pool",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidWorkerPool));
    request = ValidRequest(fixture, 0x3U);
    request.WorkerPool.PhysicalStart += 0x1000U;
    ASTERIA_EXPECT(runner, "worker pool requires four-page alignment",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidWorkerPool));
    request = ValidRequest(fixture, 0x3U);
    --request.WorkerPool.PageCount;
    ASTERIA_EXPECT(runner, "worker pool rejects seven pages per AP",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidWorkerPool));
    request = ValidRequest(fixture, 0x3U);
    ++request.WorkerPool.PageCount;
    ASTERIA_EXPECT(runner, "worker pool rejects nine pages per AP",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidWorkerPool));
    request = ValidRequest(fixture, 0x3U);
    request.WorkerPool.Attributes = 0U;
    ASTERIA_EXPECT(runner, "worker pool requires write-back memory",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidWorkerPool));
    request = ValidRequest(fixture, 0x3U);
    request.WorkerPool.Attributes |= Foundation::UInt64{1U} << 63U;
    ASTERIA_EXPECT(runner, "worker pool rejects runtime memory",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidWorkerPool));
    request = ValidRequest(fixture, 0x3U);
    request.WorkerPool.FirmwareType = 11U;
    ASTERIA_EXPECT(runner, "worker pool requires conventional memory",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidWorkerPool));
    request = ValidRequest(fixture, 0x3U);
    request.WorkerPool.PhysicalStart = 0xFFFF'FFFF'FFFF'C000ULL;
    ASTERIA_EXPECT(runner, "wrapping worker storage is rejected",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidWorkerPool));
}

void TestCapabilityLimitsAndOverlap(TestContext& runner) noexcept
{
    Fixture fixture = ValidFixture();
    auto request = ValidRequest(fixture, 0x3U);
    request.CommonPhysicalAddressBits = 31U;
    ASTERIA_EXPECT(runner, "PAL physical width below thirty-two is rejected",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidCapabilities));
    request = ValidRequest(fixture, 0x3U);
    request.CommonPhysicalAddressBits = 64U;
    ASTERIA_EXPECT(runner, "PAL physical width above sixty-three is rejected",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidCapabilities));
    request = ValidRequest(fixture, 0x3U);
    request.CommonImplementedVirtualAddressMostSignificantBit = 49U;
    ASTERIA_EXPECT(runner, "virtual MSB below fifty is rejected",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidCapabilities));
    request = ValidRequest(fixture, 0x3U);
    request.CommonImplementedVirtualAddressMostSignificantBit = 61U;
    ASTERIA_EXPECT(runner, "virtual MSB above sixty is rejected",
                   IsRejectedAs(request, RuntimeLayoutError::InvalidCapabilities));

    request = ValidRequest(fixture, 0x3U);
    request.CommonPhysicalAddressBits = 32U;
    request.WorkerPool.PhysicalStart = (Foundation::UInt64{1U} << 32U) - (8U * Kernel::Memory::PhysicalPageSizeBytes);
    Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutPlan plan{};
    ASTERIA_EXPECT(runner, "pool ending exactly at PAL limit is accepted",
                   RuntimeLayoutPlanner::Plan(request, plan) ==
                       Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutError::None);
    request.WorkerPool.PhysicalStart += 0x4000U;
    ASTERIA_EXPECT(runner, "pool above PAL limit is rejected",
                   IsRejectedAs(request, RuntimeLayoutError::PhysicalAddressOutOfRange));

    request = ValidRequest(fixture, 0x3U);
    request.CommonPhysicalAddressBits = 63U;
    request.WorkerPool.PhysicalStart = (Foundation::UInt64{1U} << 50U) - (8U * Kernel::Memory::PhysicalPageSizeBytes);
    ASTERIA_EXPECT(runner, "pool ending at encoder 50-bit limit is accepted",
                   RuntimeLayoutPlanner::Plan(request, plan) ==
                       Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutError::None);
    request.WorkerPool.PhysicalStart += 0x4000U;
    ASTERIA_EXPECT(runner, "encoder 50-bit ceiling caps a wider PAL profile",
                   IsRejectedAs(request, RuntimeLayoutError::PhysicalAddressOutOfRange));

    request = ValidRequest(fixture, 0x3U);
    fixture.Idle[0U] = IdleDescriptor(Foundation::UInt64{1U} << 48U, 1U);
    ASTERIA_EXPECT(runner, "idle storage at PAL limit is rejected",
                   IsRejectedAs(request, RuntimeLayoutError::PhysicalAddressOutOfRange));
    fixture = ValidFixture();
    request = ValidRequest(fixture, 0x7U);
    fixture.Idle[1U] = IdleDescriptor(fixture.Idle[0U].AllocationPhysicalStart, 2U);
    ASTERIA_EXPECT(runner, "overlapping idle ranges are rejected",
                   IsRejectedAs(request, RuntimeLayoutError::PhysicalRangeOverlap));
    fixture = ValidFixture();
    request = ValidRequest(fixture, 0x3U);
    request.WorkerPool.PhysicalStart = fixture.Idle[0U].AllocationPhysicalStart;
    ASTERIA_EXPECT(runner, "worker and idle ranges cannot overlap",
                   IsRejectedAs(request, RuntimeLayoutError::PhysicalRangeOverlap));
    request.WorkerPool.PhysicalStart =
        fixture.Idle[0U].AllocationPhysicalStart + Ia64Memory::ApplicationProcessorExecutionStorageSizeBytes;
    ASTERIA_EXPECT(runner, "adjacent idle and worker ranges are accepted",
                   RuntimeLayoutPlanner::Plan(request, plan) ==
                       Ia64Memory::Ia64ApplicationProcessorRuntimeLayoutError::None);
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64ApplicationProcessorRuntimeLayoutTests"};
    Asteria::Test::TestMasksAndFormulas(runner);
    Asteria::Test::TestTopologyAndGeometryMutations(runner);
    Asteria::Test::TestCapabilityLimitsAndOverlap(runner);
    return runner.Finish();
}
