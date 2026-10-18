// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Boot/BootInfo.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Platform/Ia64/Interrupt/Ia64LocalSapicIpiController.hpp"
#include "Platform/Ia64/Memory/ApplicationProcessorExecutionStorageOwner.hpp"
#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimeWorkerStorageOwner.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

extern "C" void AsteriaIa64WriteProcessorInterruptBlock(Asteria::Foundation::UInt64,
                                                        Asteria::Foundation::UInt64) noexcept
{
}

namespace Asteria::Test
{
namespace
{
using Boot::Acpi::AcpiProcessorControllerKind;
using Boot::Acpi::AcpiProcessorTopology;
using Boot::Acpi::AcpiTopology;
using Kernel::Memory::BootstrapPageAllocator;
using Kernel::Memory::BootstrapPageAllocatorError;
using Kernel::Memory::PhysicalPageRange;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiController;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiControllerInitializationError;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiControllerSnapshot;
using Platform::Ia64::Interrupt::Ia64ProcessorInterruptBlockAccess;
using Platform::Ia64::Interrupt::Ia64ProcessorInterruptBlockBaseKind;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageDescriptor;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageDescriptorAccessError;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageOwner;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageOwnerState;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageSizeBytes;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeEncoderPhysicalAddressBits;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeWorkerPoolAlignmentPages;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeWorkerStorageAccessError;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeWorkerStorageFailure;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeWorkerStorageOwner;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeWorkerStorageOwnerError;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState;

constexpr Foundation::UInt32 EnabledProcessorFlag{1U};
constexpr Foundation::UInt32 FirmwareDescriptorSize{40U};
constexpr Foundation::UInt32 ConventionalMemoryType{7U};
constexpr Foundation::UInt64 WriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 RuntimeAttribute{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 PhysicalAddressUncachedBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 PibPhysicalBase{0x0000'0000'FEE0'0000ULL};
constexpr Foundation::UInt64 PibAccessBase{PibPhysicalBase | PhysicalAddressUncachedBit};
constexpr Foundation::UInt8 CanaryValue{0xA5U};
constexpr Foundation::Size StoragePageCount{256U};
constexpr Foundation::Size ExecutionRegionFirstPage{1U};
constexpr Foundation::Size ExecutionRegionPageCount{95U};
constexpr Foundation::Size WorkerRegionFirstPage{129U};
constexpr Foundation::Size WorkerRegionPageCount{64U};
constexpr Foundation::Size WorkerAllocationFirstPage{132U};
constexpr Foundation::Size ExecutionOwnerDescriptorBytes{
    Platform::Ia64::Memory::MaximumApplicationProcessorExecutionStorageDescriptorCount *
    sizeof(ApplicationProcessorExecutionStorageDescriptor)};
constexpr Foundation::Size ExecutionOwnerDescriptorCountByteOffset{ExecutionOwnerDescriptorBytes + 2U};

alignas(32U * 1024U) Foundation::UInt8 g_storage[StoragePageCount * Kernel::Memory::PhysicalPageSizeBytes];
Boot::BootInfo g_bootInfo{};

static_assert(__is_standard_layout(ApplicationProcessorExecutionStorageOwner));
static_assert(sizeof(ApplicationProcessorExecutionStorageOwner) == 200U);
static_assert(ExecutionOwnerDescriptorBytes == 192U);

[[nodiscard]] constexpr Foundation::UInt64 MakeCrLid(const Foundation::UInt8 hardwareId,
                                                     const Foundation::UInt8 hardwareEid) noexcept
{
    return (static_cast<Foundation::UInt64>(hardwareId) << 24U) | (static_cast<Foundation::UInt64>(hardwareEid) << 16U);
}

[[nodiscard]] constexpr AcpiProcessorTopology MakeProcessor(const Foundation::UInt32 acpiUid,
                                                            const Foundation::UInt8 hardwareId,
                                                            const Foundation::UInt8 hardwareEid) noexcept
{
    return AcpiProcessorTopology{
        .AcpiUid = acpiUid,
        .HardwareId = hardwareId,
        .HardwareEid = hardwareEid,
        .Flags = EnabledProcessorFlag,
        .ControllerKind = AcpiProcessorControllerKind::LocalSapic,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool MakeSnapshot(const Foundation::UInt8 processorCount,
                                Ia64LocalSapicIpiControllerSnapshot& snapshot) noexcept
{
    AcpiTopology topology{};
    topology.LocalControllerPhysicalAddress = PibPhysicalBase;
    topology.ProcessorCount = processorCount;
    for (Foundation::UInt8 index{0U}; index < processorCount; ++index)
    {
        topology.Processors[index] = MakeProcessor(100U + index, static_cast<Foundation::UInt8>(0x10U + index),
                                                   static_cast<Foundation::UInt8>(0x20U + index));
    }

    Ia64LocalSapicIpiController controller;
    const Ia64ProcessorInterruptBlockAccess access{
        .BaseAddress = PibAccessBase,
        .Kind = Ia64ProcessorInterruptBlockBaseKind::BootstrapPhysical,
        .Reserved = {},
    };
    if (controller.Initialize(topology, MakeCrLid(0x10U, 0x20U), access) !=
        Ia64LocalSapicIpiControllerInitializationError::None)
    {
        return false;
    }
    snapshot = controller.GetSnapshot();
    return true;
}

void ConfigureBootMemory(const Foundation::UInt64 physicalStart, const Foundation::UInt64 pageCount,
                         const Foundation::UInt64 attributes = WriteBackAttribute,
                         const Foundation::UInt32 firmwareType = ConventionalMemoryType) noexcept
{
    g_bootInfo = Boot::BootInfo{};
    g_bootInfo.Signature = Boot::BootInfoSignature;
    g_bootInfo.Version = Boot::BootInfoVersion;
    g_bootInfo.SizeBytes = static_cast<Foundation::UInt32>(sizeof(Boot::BootInfo));
    g_bootInfo.MemoryRegionCount = 1U;
    g_bootInfo.FirmwareDescriptorVersion = 1U;
    g_bootInfo.FirmwareDescriptorSize = FirmwareDescriptorSize;
    g_bootInfo.FirmwareMemoryMapSizeBytes = FirmwareDescriptorSize;
    g_bootInfo.MemoryRegions[0U] = Boot::PhysicalMemoryRegion{
        .PhysicalStart = physicalStart,
        .VirtualStart = 0U,
        .PageCount = pageCount,
        .Attributes = attributes,
        .FirmwareType = firmwareType,
        .Kind = Boot::PhysicalMemoryKind::Conventional,
    };
}

[[nodiscard]] Foundation::UInt64 StoragePhysicalStart() noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_storage));
}

[[nodiscard]] Foundation::UInt64 PagePhysicalStart(const Foundation::Size page) noexcept
{
    return StoragePhysicalStart() + (static_cast<Foundation::UInt64>(page) * Kernel::Memory::PhysicalPageSizeBytes);
}

[[nodiscard]] bool InitializeAllocator(BootstrapPageAllocator& allocator, const Foundation::UInt64 physicalStart,
                                       const Foundation::UInt64 pageCount,
                                       const Foundation::UInt64 attributes = WriteBackAttribute) noexcept
{
    ConfigureBootMemory(physicalStart, pageCount, attributes);
    return allocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::None;
}

void FillStorage() noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(g_storage); ++index)
    {
        g_storage[index] = CanaryValue;
    }
}

[[nodiscard]] bool PrepareExecutionOwner(ApplicationProcessorExecutionStorageOwner& owner,
                                         BootstrapPageAllocator& allocator,
                                         const Foundation::UInt8 processorCount) noexcept
{
    Ia64LocalSapicIpiControllerSnapshot snapshot{};
    if (!MakeSnapshot(processorCount, snapshot) ||
        !InitializeAllocator(allocator, PagePhysicalStart(ExecutionRegionFirstPage), ExecutionRegionPageCount))
    {
        return false;
    }
    return owner.Prepare(allocator, snapshot).HasValue();
}

[[nodiscard]] constexpr bool IsCanonicalZeroRange(const PhysicalPageRange& range) noexcept
{
    return (range.PhysicalStart == 0U) && (range.PageCount == 0U) && (range.Attributes == 0U) &&
           (range.FirmwareType == 0U) && (range.Reserved == 0U);
}

[[nodiscard]] constexpr bool
IsFailure(const Ia64ApplicationProcessorRuntimeWorkerStorageFailure& failure,
          const Ia64ApplicationProcessorRuntimeWorkerStorageOwnerError ownerError,
          const BootstrapPageAllocatorError allocatorError = BootstrapPageAllocatorError::None) noexcept
{
    return (failure.Error == ownerError) && (failure.AllocatorError == allocatorError);
}

[[nodiscard]] bool WorkerRegionMatches(const Foundation::UInt64 allocatedPageCount) noexcept
{
    const Foundation::Size regionStartBytes = WorkerRegionFirstPage * Kernel::Memory::PhysicalPageSizeBytes;
    const Foundation::Size allocationStartBytes = WorkerAllocationFirstPage * Kernel::Memory::PhysicalPageSizeBytes;
    const Foundation::Size allocationSizeBytes =
        static_cast<Foundation::Size>(allocatedPageCount) * Kernel::Memory::PhysicalPageSizeBytes;
    const Foundation::Size regionEndBytes =
        (WorkerRegionFirstPage + WorkerRegionPageCount) * Kernel::Memory::PhysicalPageSizeBytes;
    for (Foundation::Size index{regionStartBytes}; index < regionEndBytes; ++index)
    {
        const bool shouldBeZero =
            (index >= allocationStartBytes) && (index < (allocationStartBytes + allocationSizeBytes));
        const Foundation::UInt8 expected = shouldBeZero ? Foundation::UInt8{0U} : CanaryValue;
        if (g_storage[index] != expected)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] ApplicationProcessorExecutionStorageDescriptor&
GetFirstMutableExecutionDescriptor(ApplicationProcessorExecutionStorageOwner& owner) noexcept
{
    // The existing owner is asserted standard-layout and declares its fixed
    // descriptor array first, so it is pointer-interconvertible with element 0.
    return *reinterpret_cast<ApplicationProcessorExecutionStorageDescriptor*>(&owner);
}

void CorruptExecutionDescriptorCount(ApplicationProcessorExecutionStorageOwner& owner) noexcept
{
    Foundation::UInt8* const ownerBytes = reinterpret_cast<Foundation::UInt8*>(&owner);
    ownerBytes[ExecutionOwnerDescriptorCountByteOffset] = 0U;
}

void TestFreshAndUnreadyExecutionOwner(TestContext& runner) noexcept
{
    BootstrapPageAllocator allocator;
    ApplicationProcessorExecutionStorageOwner executionOwner;
    Ia64ApplicationProcessorRuntimeWorkerStorageOwner owner;
    const auto before = owner.GetWorkerPool();
    const auto preparation = owner.Prepare(allocator, executionOwner);
    const auto after = owner.GetWorkerPool();
    const auto repeated = owner.Prepare(allocator, executionOwner);
    ASTERIA_EXPECT(
        runner, "Fresh and terminally failed owners never expose worker storage",
        owner.GetState() == Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState::Failed,
        owner.GetProcessorCount() == 0U, owner.GetEnabledProcessorMask() == 0U, owner.GetWorkerStorageCount() == 0U,
        !before.HasValue(), before.GetError() == Ia64ApplicationProcessorRuntimeWorkerStorageAccessError::NotReady,
        !preparation.HasValue(),
        IsFailure(preparation.GetError(),
                  Ia64ApplicationProcessorRuntimeWorkerStorageOwnerError::ExecutionStorageOwnerNotReady),
        preparation.GetError().ExecutionStorageAccessError ==
            ApplicationProcessorExecutionStorageDescriptorAccessError::NotReady,
        !after.HasValue(), !repeated.HasValue(),
        IsFailure(repeated.GetError(), Ia64ApplicationProcessorRuntimeWorkerStorageOwnerError::AlreadyPrepared),
        allocator.GetAllocatedPageCount() == 0U);
}

void TestProcessorShape(TestContext& runner, const Foundation::UInt8 processorCount) noexcept
{
    FillStorage();
    BootstrapPageAllocator executionAllocator;
    ApplicationProcessorExecutionStorageOwner executionOwner;
    const bool executionReady = PrepareExecutionOwner(executionOwner, executionAllocator, processorCount);

    BootstrapPageAllocator workerAllocator;
    const bool allocatorReady =
        (processorCount == 1U) ||
        InitializeAllocator(workerAllocator, PagePhysicalStart(WorkerRegionFirstPage), WorkerRegionPageCount);
    const Foundation::UInt64 freePagesBefore = workerAllocator.GetFreePageCount();
    Ia64ApplicationProcessorRuntimeWorkerStorageOwner owner;
    const auto before = owner.GetWorkerPool();
    const auto preparation = owner.Prepare(workerAllocator, executionOwner);
    const auto workerPool = owner.GetWorkerPool();
    const Foundation::UInt64 workerStorageCount = static_cast<Foundation::UInt64>(processorCount - 1U);
    const Foundation::UInt64 expectedPages =
        workerStorageCount * Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor;
    const Foundation::UInt8 expectedMask =
        static_cast<Foundation::UInt8>((Foundation::UInt32{1U} << processorCount) - 1U);

    bool resultIsExact{false};
    if (preparation.HasValue() && workerPool.HasValue())
    {
        const auto& summary = preparation.GetValue();
        const PhysicalPageRange& range = workerPool.GetValue();
        bool reservedIsZero{true};
        for (const Foundation::UInt8 reserved : summary.Reserved)
        {
            reservedIsZero = reservedIsZero && (reserved == 0U);
        }
        resultIsExact =
            (summary.WorkerPageCount == expectedPages) && (summary.ProcessorCount == processorCount) &&
            (summary.EnabledProcessorMask == expectedMask) &&
            (summary.WorkerStorageCount == static_cast<Foundation::UInt8>(workerStorageCount)) && reservedIsZero &&
            ((processorCount == 1U)
                 ? IsCanonicalZeroRange(range)
                 : ((range.PhysicalStart == PagePhysicalStart(WorkerAllocationFirstPage)) &&
                    (range.PageCount == expectedPages) &&
                    ((range.PhysicalStart & ((Ia64ApplicationProcessorRuntimeWorkerPoolAlignmentPages *
                                              Kernel::Memory::PhysicalPageSizeBytes) -
                                             1U)) == 0U) &&
                    ((range.Attributes & WriteBackAttribute) != 0U) && ((range.Attributes & RuntimeAttribute) == 0U) &&
                    (range.FirmwareType == ConventionalMemoryType) && (range.Reserved == 0U)));
    }

    const Foundation::UInt64 allocatedBeforeRepeat = workerAllocator.GetAllocatedPageCount();
    const auto repeated = owner.Prepare(workerAllocator, executionOwner);
    const auto afterRepeat = owner.GetWorkerPool();
    ASTERIA_EXPECT(
        runner, "SMP1-4 publish exactly one bounded, cleared worker-pool shape", executionReady, allocatorReady,
        !before.HasValue(), preparation.HasValue(),
        owner.GetState() == Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState::Ready,
        owner.GetProcessorCount() == processorCount, owner.GetEnabledProcessorMask() == expectedMask,
        owner.GetWorkerStorageCount() == static_cast<Foundation::UInt8>(workerStorageCount), resultIsExact,
        workerAllocator.GetAllocatedPageCount() == expectedPages,
        (processorCount != 1U) || (!workerAllocator.IsInitialized() && (freePagesBefore == 0U)),
        (processorCount == 1U) || WorkerRegionMatches(expectedPages), !repeated.HasValue(),
        IsFailure(repeated.GetError(), Ia64ApplicationProcessorRuntimeWorkerStorageOwnerError::AlreadyPrepared),
        workerAllocator.GetAllocatedPageCount() == allocatedBeforeRepeat, afterRepeat.HasValue());
}

void TestAllocatorFailure(TestContext& runner) noexcept
{
    FillStorage();
    BootstrapPageAllocator executionAllocator;
    ApplicationProcessorExecutionStorageOwner executionOwner;
    const bool executionReady = PrepareExecutionOwner(executionOwner, executionAllocator, 2U);
    BootstrapPageAllocator unavailableAllocator;
    Ia64ApplicationProcessorRuntimeWorkerStorageOwner owner;
    const auto preparation = owner.Prepare(unavailableAllocator, executionOwner);
    const auto access = owner.GetWorkerPool();
    const auto repeated = owner.Prepare(unavailableAllocator, executionOwner);
    ASTERIA_EXPECT(
        runner, "allocator failure is terminal and publishes no physical range", executionReady,
        !preparation.HasValue(),
        IsFailure(preparation.GetError(), Ia64ApplicationProcessorRuntimeWorkerStorageOwnerError::AllocationFailed,
                  BootstrapPageAllocatorError::NotInitialized),
        owner.GetState() == Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState::Failed, !access.HasValue(),
        !repeated.HasValue(),
        IsFailure(repeated.GetError(), Ia64ApplicationProcessorRuntimeWorkerStorageOwnerError::AlreadyPrepared));
}

void TestMalformedExecutionDescriptorCount(TestContext& runner) noexcept
{
    FillStorage();
    BootstrapPageAllocator executionAllocator;
    ApplicationProcessorExecutionStorageOwner executionOwner;
    const bool executionReady = PrepareExecutionOwner(executionOwner, executionAllocator, 2U);
    CorruptExecutionDescriptorCount(executionOwner);
    BootstrapPageAllocator workerAllocator;
    const bool allocatorReady =
        InitializeAllocator(workerAllocator, PagePhysicalStart(WorkerRegionFirstPage), WorkerRegionPageCount);
    Ia64ApplicationProcessorRuntimeWorkerStorageOwner owner;
    const auto preparation = owner.Prepare(workerAllocator, executionOwner);

    ASTERIA_EXPECT(runner, "a malformed descriptor count is rejected before allocation", executionReady, allocatorReady,
                   !preparation.HasValue(),
                   IsFailure(preparation.GetError(),
                             Ia64ApplicationProcessorRuntimeWorkerStorageOwnerError::InvalidExecutionStorageOwner),
                   workerAllocator.GetAllocatedPageCount() == 0U, !owner.GetWorkerPool().HasValue());
}

void TestOverflowingIdleGeometry(TestContext& runner) noexcept
{
    FillStorage();
    BootstrapPageAllocator executionAllocator;
    ApplicationProcessorExecutionStorageOwner executionOwner;
    const bool executionReady = PrepareExecutionOwner(executionOwner, executionAllocator, 2U);
    ApplicationProcessorExecutionStorageDescriptor& overflowDescriptor =
        GetFirstMutableExecutionDescriptor(executionOwner);
    overflowDescriptor.AllocationPhysicalStart = ~Foundation::UInt64{0U} - (32U * 1024U - 1U);
    BootstrapPageAllocator workerAllocator;
    const bool allocatorReady =
        InitializeAllocator(workerAllocator, PagePhysicalStart(WorkerRegionFirstPage), WorkerRegionPageCount);
    Ia64ApplicationProcessorRuntimeWorkerStorageOwner owner;
    const auto preparation = owner.Prepare(workerAllocator, executionOwner);

    ASTERIA_EXPECT(runner, "overflowing idle geometry is rejected before allocation", executionReady, allocatorReady,
                   !preparation.HasValue(),
                   IsFailure(preparation.GetError(),
                             Ia64ApplicationProcessorRuntimeWorkerStorageOwnerError::InvalidExecutionStorageOwner),
                   workerAllocator.GetAllocatedPageCount() == 0U, !owner.GetWorkerPool().HasValue());
}

void TestPhysicalOverlapFailure(TestContext& runner) noexcept
{
    FillStorage();
    BootstrapPageAllocator executionAllocator;
    ApplicationProcessorExecutionStorageOwner executionOwner;
    const bool executionReady = PrepareExecutionOwner(executionOwner, executionAllocator, 2U);

    BootstrapPageAllocator overlappingAllocator;
    const bool allocatorReady = InitializeAllocator(overlappingAllocator, PagePhysicalStart(ExecutionRegionFirstPage),
                                                    ExecutionRegionPageCount);
    Ia64ApplicationProcessorRuntimeWorkerStorageOwner owner;
    const auto preparation = owner.Prepare(overlappingAllocator, executionOwner);
    const Foundation::Size prospectiveStartBytes = 4U * Kernel::Memory::PhysicalPageSizeBytes;
    bool untouchedBeforeIdle{true};
    for (Foundation::Size index{prospectiveStartBytes}; index < (8U * Kernel::Memory::PhysicalPageSizeBytes); ++index)
    {
        untouchedBeforeIdle = untouchedBeforeIdle && (g_storage[index] == CanaryValue);
    }
    ASTERIA_EXPECT(runner, "an overlapping allocation is consumed but never cleared or exposed", executionReady,
                   allocatorReady, !preparation.HasValue(),
                   IsFailure(preparation.GetError(),
                             Ia64ApplicationProcessorRuntimeWorkerStorageOwnerError::WorkerPoolOverlapsIdleStorage),
                   preparation.GetError().FailingProcessor.GetValue() == 1U,
                   overlappingAllocator.GetAllocatedPageCount() ==
                       Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor,
                   untouchedBeforeIdle, !owner.GetWorkerPool().HasValue());
}

void TestInvalidAllocatedWorkerPools(TestContext& runner) noexcept
{
    FillStorage();
    BootstrapPageAllocator executionAllocator;
    ApplicationProcessorExecutionStorageOwner executionOwner;
    const bool executionReady = PrepareExecutionOwner(executionOwner, executionAllocator, 2U);

    BootstrapPageAllocator nonWriteBackAllocator;
    const bool nonWriteBackReady =
        InitializeAllocator(nonWriteBackAllocator, PagePhysicalStart(WorkerRegionFirstPage), WorkerRegionPageCount, 0U);
    Ia64ApplicationProcessorRuntimeWorkerStorageOwner nonWriteBackOwner;
    const auto nonWriteBackPreparation = nonWriteBackOwner.Prepare(nonWriteBackAllocator, executionOwner);

    BootstrapPageAllocator overflowAllocator;
    constexpr Foundation::UInt64 overflowStart{0xFFFF'FFFF'FFFF'8000ULL};
    const bool overflowReady =
        InitializeAllocator(overflowAllocator, overflowStart, Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor);
    Ia64ApplicationProcessorRuntimeWorkerStorageOwner overflowOwner;
    const auto overflowPreparation = overflowOwner.Prepare(overflowAllocator, executionOwner);

    ASTERIA_EXPECT(runner, "invalid attributes and wrapping pools stay hidden", executionReady, nonWriteBackReady,
                   !nonWriteBackPreparation.HasValue(),
                   IsFailure(nonWriteBackPreparation.GetError(),
                             Ia64ApplicationProcessorRuntimeWorkerStorageOwnerError::InvalidWorkerPool),
                   nonWriteBackAllocator.GetAllocatedPageCount() ==
                       Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor,
                   WorkerRegionMatches(0U), !nonWriteBackOwner.GetWorkerPool().HasValue(), overflowReady,
                   !overflowPreparation.HasValue(),
                   IsFailure(overflowPreparation.GetError(),
                             Ia64ApplicationProcessorRuntimeWorkerStorageOwnerError::InvalidWorkerPool),
                   overflowAllocator.GetAllocatedPageCount() == Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor,
                   !overflowOwner.GetWorkerPool().HasValue());
}

void TestNonEncodableWorkerPool(TestContext& runner) noexcept
{
    FillStorage();
    BootstrapPageAllocator executionAllocator;
    ApplicationProcessorExecutionStorageOwner executionOwner;
    const bool executionReady = PrepareExecutionOwner(executionOwner, executionAllocator, 2U);

    BootstrapPageAllocator highAddressAllocator;
    constexpr Foundation::UInt64 highAddressStart{Foundation::UInt64{1U}
                                                  << Ia64ApplicationProcessorRuntimeEncoderPhysicalAddressBits};
    const bool highAddressReady = InitializeAllocator(highAddressAllocator, highAddressStart,
                                                      Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor);
    Ia64ApplicationProcessorRuntimeWorkerStorageOwner owner;
    const auto preparation = owner.Prepare(highAddressAllocator, executionOwner);

    ASTERIA_EXPECT(
        runner, "a non-encodable pool is consumed without being cleared or exposed", executionReady, highAddressReady,
        !preparation.HasValue(),
        IsFailure(preparation.GetError(), Ia64ApplicationProcessorRuntimeWorkerStorageOwnerError::InvalidWorkerPool),
        highAddressAllocator.GetAllocatedPageCount() == Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor,
        owner.GetState() == Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState::Failed,
        !owner.GetWorkerPool().HasValue());
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64ApplicationProcessorRuntimeWorkerStorageOwnerTests"};
    Asteria::Test::TestFreshAndUnreadyExecutionOwner(runner);
    for (Asteria::Foundation::UInt8 processorCount{1U};
         processorCount <= Asteria::Kernel::Scheduler::MaximumProcessorCount; ++processorCount)
    {
        Asteria::Test::TestProcessorShape(runner, processorCount);
    }
    Asteria::Test::TestAllocatorFailure(runner);
    Asteria::Test::TestMalformedExecutionDescriptorCount(runner);
    Asteria::Test::TestOverflowingIdleGeometry(runner);
    Asteria::Test::TestPhysicalOverlapFailure(runner);
    Asteria::Test::TestInvalidAllocatedWorkerPools(runner);
    Asteria::Test::TestNonEncodableWorkerPool(runner);
    return runner.Finish();
}
