// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Boot/BootInfo.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Platform/Ia64/Interrupt/Ia64LocalSapicIpiController.hpp"
#include "Platform/Ia64/Memory/ApplicationProcessorExecutionStorageOwner.hpp"

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
using Kernel::Scheduler::CpuId;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiController;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiControllerInitializationError;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiControllerSnapshot;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiControllerState;
using Platform::Ia64::Interrupt::Ia64ProcessorInterruptBlockAccess;
using Platform::Ia64::Interrupt::Ia64ProcessorInterruptBlockBaseKind;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageDescriptor;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageDescriptorAccessError;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageOwner;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageOwnerError;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageOwnerFailure;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageOwnerState;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStoragePageCount;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStoragePreparationError;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageSizeBytes;
using Platform::Ia64::Memory::ApplicationProcessorInitialStackScratchSizeBytes;
using Platform::Ia64::Memory::ApplicationProcessorMemoryStackSizeBytes;
using Platform::Ia64::Memory::ApplicationProcessorRseBackingStoreSizeBytes;

constexpr Foundation::UInt32 EnabledProcessorFlag{1U};
constexpr Foundation::UInt32 FirmwareDescriptorSize{40U};
constexpr Foundation::UInt32 ConventionalMemoryType{7U};
constexpr Foundation::UInt64 WriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 PhysicalAddressUncachedBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 PibPhysicalBase{0x0000'0000'FEE0'0000ULL};
constexpr Foundation::UInt64 PibAccessBase{PibPhysicalBase | PhysicalAddressUncachedBit};
constexpr Foundation::UInt64 PinnedVirtualPibAccessBase{0xC000'0000'FEE0'0000ULL};
constexpr Foundation::UInt64 ProcessorInterruptBlockSizeBytes{2U * 1024U * 1024U};
constexpr Foundation::Size TestStoragePageCount{64U};
constexpr Foundation::Size FirstAllocationPage{8U};
constexpr Foundation::UInt8 CanaryValue{0xA5U};

alignas(32U * 1024U) Foundation::UInt8 g_storage[TestStoragePageCount * Kernel::Memory::PhysicalPageSizeBytes];
Boot::BootInfo g_bootInfo{};

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

[[nodiscard]] bool MakeSnapshot(Ia64LocalSapicIpiController& controller, const Foundation::UInt8 processorCount,
                                Ia64LocalSapicIpiControllerSnapshot& snapshot,
                                const Foundation::UInt64 accessBaseAddress = PibAccessBase,
                                const Ia64ProcessorInterruptBlockBaseKind baseKind =
                                    Ia64ProcessorInterruptBlockBaseKind::BootstrapPhysical) noexcept
{
    AcpiTopology topology{};
    topology.LocalControllerPhysicalAddress = PibPhysicalBase;
    topology.ProcessorCount = processorCount;
    for (Foundation::UInt8 index{0U}; index < processorCount; ++index)
    {
        topology.Processors[index] = MakeProcessor(100U + index, static_cast<Foundation::UInt8>(0x10U + index),
                                                   static_cast<Foundation::UInt8>(0x20U + index));
    }
    const Ia64ProcessorInterruptBlockAccess access{
        .BaseAddress = accessBaseAddress,
        .Kind = baseKind,
        .Reserved = {},
    };
    const Ia64LocalSapicIpiControllerInitializationError error =
        controller.Initialize(topology, MakeCrLid(0x10U, 0x20U), access);
    if (error != Ia64LocalSapicIpiControllerInitializationError::None)
    {
        return false;
    }
    snapshot = controller.GetSnapshot();
    return true;
}

void ConfigureBootMemory(const Foundation::UInt64 physicalStart, const Foundation::UInt64 pageCount) noexcept
{
    g_bootInfo.Signature = Boot::BootInfoSignature;
    g_bootInfo.Version = Boot::BootInfoVersion;
    g_bootInfo.SizeBytes = static_cast<Foundation::UInt32>(sizeof(Boot::BootInfo));
    g_bootInfo.MemoryRegionCount = 1U;
    g_bootInfo.FirmwareDescriptorVersion = 1U;
    g_bootInfo.FirmwareDescriptorSize = FirmwareDescriptorSize;
    g_bootInfo.Reserved = 0U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = FirmwareDescriptorSize;
    g_bootInfo.MemoryRegions[0U] = Boot::PhysicalMemoryRegion{
        .PhysicalStart = physicalStart,
        .VirtualStart = 0U,
        .PageCount = pageCount,
        .Attributes = WriteBackAttribute,
        .FirmwareType = ConventionalMemoryType,
        .Kind = Boot::PhysicalMemoryKind::Conventional,
    };
}

[[nodiscard]] Foundation::UInt64 StoragePhysicalStart() noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_storage));
}

[[nodiscard]] bool InitializeStorageAllocator(BootstrapPageAllocator& allocator,
                                              const Foundation::UInt64 pageCount = TestStoragePageCount - 1U) noexcept
{
    ConfigureBootMemory(StoragePhysicalStart() + Kernel::Memory::PhysicalPageSizeBytes, pageCount);
    return allocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::None;
}

void FillStorage() noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(g_storage); ++index)
    {
        g_storage[index] = CanaryValue;
    }
}

[[nodiscard]] bool StorageMatchesExactAllocations(const Foundation::UInt8 descriptorCount) noexcept
{
    const Foundation::Size zeroStart = FirstAllocationPage * Kernel::Memory::PhysicalPageSizeBytes;
    const Foundation::Size zeroSize = static_cast<Foundation::Size>(descriptorCount) *
                                      static_cast<Foundation::Size>(ApplicationProcessorExecutionStorageSizeBytes);
    for (Foundation::Size index{0U}; index < sizeof(g_storage); ++index)
    {
        const bool shouldBeZero = (index >= zeroStart) && (index < (zeroStart + zeroSize));
        const Foundation::UInt8 expected = shouldBeZero ? Foundation::UInt8{0U} : CanaryValue;
        if (g_storage[index] != expected)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsDescriptorExact(const ApplicationProcessorExecutionStorageDescriptor& descriptor,
                                     const Ia64LocalSapicIpiControllerSnapshot& snapshot,
                                     const Foundation::UInt8 processor) noexcept
{
    const Foundation::UInt64 expectedStart =
        StoragePhysicalStart() + (FirstAllocationPage * Kernel::Memory::PhysicalPageSizeBytes) +
        (static_cast<Foundation::UInt64>(processor - 1U) * ApplicationProcessorExecutionStorageSizeBytes);
    bool reservedIsZero{true};
    for (const Foundation::UInt8 reserved : descriptor.Reserved)
    {
        reservedIsZero = reservedIsZero && (reserved == 0U);
    }
    return (descriptor.AllocationPhysicalStart == expectedStart) &&
           (descriptor.AllocationSizeBytes == ApplicationProcessorExecutionStorageSizeBytes) &&
           (descriptor.MemoryStackPhysicalStart == expectedStart) &&
           (descriptor.InitialStackPointer == (expectedStart + ApplicationProcessorMemoryStackSizeBytes -
                                               ApplicationProcessorInitialStackScratchSizeBytes)) &&
           (descriptor.MemoryStackSizeBytes == ApplicationProcessorMemoryStackSizeBytes) &&
           (descriptor.RseBackingStorePhysicalStart == (expectedStart + ApplicationProcessorMemoryStackSizeBytes)) &&
           (descriptor.RseBackingStoreSizeBytes == ApplicationProcessorRseBackingStoreSizeBytes) &&
           (descriptor.Processor.GetValue() == processor) &&
           (descriptor.HardwareId == snapshot.Processors[processor].HardwareId) &&
           (descriptor.HardwareEid == snapshot.Processors[processor].HardwareEid) && reservedIsZero;
}

[[nodiscard]] bool DescriptorsEqual(const ApplicationProcessorExecutionStorageDescriptor& first,
                                    const ApplicationProcessorExecutionStorageDescriptor& second) noexcept
{
    bool reservedMatches{true};
    for (Foundation::Size index{0U}; index < sizeof(first.Reserved); ++index)
    {
        reservedMatches = reservedMatches && (first.Reserved[index] == second.Reserved[index]);
    }
    return (first.AllocationPhysicalStart == second.AllocationPhysicalStart) &&
           (first.AllocationSizeBytes == second.AllocationSizeBytes) &&
           (first.MemoryStackPhysicalStart == second.MemoryStackPhysicalStart) &&
           (first.InitialStackPointer == second.InitialStackPointer) &&
           (first.MemoryStackSizeBytes == second.MemoryStackSizeBytes) &&
           (first.RseBackingStorePhysicalStart == second.RseBackingStorePhysicalStart) &&
           (first.RseBackingStoreSizeBytes == second.RseBackingStoreSizeBytes) &&
           (first.Processor.GetValue() == second.Processor.GetValue()) && (first.HardwareId == second.HardwareId) &&
           (first.HardwareEid == second.HardwareEid) && reservedMatches;
}

[[nodiscard]] bool FailureIsExact(const ApplicationProcessorExecutionStorageOwnerFailure& failure,
                                  const ApplicationProcessorExecutionStorageOwnerError ownerError,
                                  const BootstrapPageAllocatorError allocatorError,
                                  const ApplicationProcessorExecutionStoragePreparationError preparationError,
                                  const Foundation::UInt8 failingProcessor) noexcept
{
    return (failure.Error == ownerError) && (failure.AllocatorError == allocatorError) &&
           (failure.PreparationError == preparationError) &&
           (failure.FailingProcessor.GetValue() == failingProcessor) && (failure.Reserved[0U] == 0U) &&
           (failure.Reserved[1U] == 0U) && (failure.Reserved[2U] == 0U);
}

void TestOneProcessorSuccess(TestContext& runner) noexcept
{
    FillStorage();
    Ia64LocalSapicIpiController controller;
    Ia64LocalSapicIpiControllerSnapshot snapshot{};
    BootstrapPageAllocator allocator;
    ApplicationProcessorExecutionStorageOwner owner;
    const bool snapshotReady = MakeSnapshot(controller, 1U, snapshot);
    const auto initialView = owner.GetDescriptorView();
    const auto result = owner.Prepare(allocator, snapshot);
    const auto view = owner.GetDescriptorView();
    ASTERIA_EXPECT(runner, "one CPU commits Ready with no allocation and a canonical empty view", snapshotReady,
                   !initialView.HasValue(),
                   initialView.GetError() == ApplicationProcessorExecutionStorageDescriptorAccessError::NotReady,
                   result.HasValue(), owner.GetState() == ApplicationProcessorExecutionStorageOwnerState::Ready,
                   owner.GetProcessorCount() == 1U, owner.GetEnabledProcessorMask() == 0x01U,
                   owner.GetDescriptorCount() == 0U, allocator.GetAllocatedPageCount() == 0U, view.HasValue(),
                   view.GetValue().Data == nullptr, view.GetValue().Count == 0U, StorageMatchesExactAllocations(0U));

    const auto bsp = owner.GetDescriptor(CpuId{0U});
    const auto absentAp = owner.GetDescriptor(CpuId{1U});
    const auto invalid = owner.GetDescriptor(CpuId{});
    ASTERIA_EXPECT(runner, "descriptor access rejects BSP, absent AP, and invalid CpuId", !bsp.HasValue(),
                   !absentAp.HasValue(), !invalid.HasValue(),
                   bsp.GetError() == ApplicationProcessorExecutionStorageDescriptorAccessError::InvalidCpuId,
                   absentAp.GetError() == ApplicationProcessorExecutionStorageDescriptorAccessError::InvalidCpuId,
                   invalid.GetError() == ApplicationProcessorExecutionStorageDescriptorAccessError::InvalidCpuId);
}

void TestMultiProcessorSuccess(TestContext& runner, const Foundation::UInt8 processorCount) noexcept
{
    FillStorage();
    Ia64LocalSapicIpiController controller;
    Ia64LocalSapicIpiControllerSnapshot snapshot{};
    BootstrapPageAllocator allocator;
    ApplicationProcessorExecutionStorageOwner owner;
    const bool setup = MakeSnapshot(controller, processorCount, snapshot) && InitializeStorageAllocator(allocator);
    const auto result = owner.Prepare(allocator, snapshot);
    const auto view = owner.GetDescriptorView();
    const Foundation::UInt8 descriptorCount = static_cast<Foundation::UInt8>(processorCount - 1U);
    const Foundation::UInt8 expectedMask =
        static_cast<Foundation::UInt8>((Foundation::UInt32{1U} << processorCount) - 1U);
    bool descriptorsAreExact{result.HasValue() && view.HasValue()};
    if (view.HasValue())
    {
        descriptorsAreExact =
            descriptorsAreExact && (view.GetValue().Count == descriptorCount) && (view.GetValue().Data != nullptr);
        for (Foundation::UInt8 processor{1U}; processor < processorCount; ++processor)
        {
            const auto descriptor = owner.GetDescriptor(CpuId{processor});
            descriptorsAreExact = descriptorsAreExact && descriptor.HasValue() &&
                                  IsDescriptorExact(descriptor.GetValue(), snapshot, processor) &&
                                  IsDescriptorExact(view.GetValue().Data[processor - 1U], snapshot, processor);
        }
    }
    ASTERIA_EXPECT(runner,
                   (processorCount == 2U)
                       ? "two CPUs allocate and publish one exact descriptor"
                       : ((processorCount == 3U) ? "three CPUs allocate and publish two ordered descriptors"
                                                 : "four CPUs allocate, zero, and publish three ordered descriptors"),
                   setup, result.HasValue(), result.GetValue().ProcessorCount == processorCount,
                   result.GetValue().EnabledProcessorMask == expectedMask,
                   result.GetValue().DescriptorCount == descriptorCount, result.GetValue().Reserved == 0U,
                   owner.GetState() == ApplicationProcessorExecutionStorageOwnerState::Ready,
                   owner.GetProcessorCount() == processorCount, owner.GetEnabledProcessorMask() == expectedMask,
                   owner.GetDescriptorCount() == descriptorCount,
                   allocator.GetAllocatedPageCount() == (static_cast<Foundation::UInt64>(descriptorCount) *
                                                         ApplicationProcessorExecutionStoragePageCount),
                   descriptorsAreExact, StorageMatchesExactAllocations(descriptorCount));

    const auto outOfRange = owner.GetDescriptor(CpuId{processorCount});
    ASTERIA_EXPECT(runner, "Ready descriptor access rejects the exact upper CpuId bound", !outOfRange.HasValue(),
                   outOfRange.GetError() == ApplicationProcessorExecutionStorageDescriptorAccessError::InvalidCpuId);

    ApplicationProcessorExecutionStorageDescriptor descriptorsBefore[3U]{};
    if (view.HasValue())
    {
        for (Foundation::Size index{0U}; index < view.GetValue().Count; ++index)
        {
            descriptorsBefore[index] = view.GetValue().Data[index];
        }
    }
    const Foundation::UInt64 allocatedPagesBefore = allocator.GetAllocatedPageCount();
    const Foundation::UInt64 freePagesBefore = allocator.GetFreePageCount();
    const Foundation::Size freeRangeCountBefore = allocator.GetFreeRangeCount();
    const auto second = owner.Prepare(allocator, snapshot);
    const auto viewAfterSecond = owner.GetDescriptorView();
    bool descriptorsUnchanged{view.HasValue() && viewAfterSecond.HasValue()};
    if (view.HasValue() && viewAfterSecond.HasValue())
    {
        descriptorsUnchanged = descriptorsUnchanged && (viewAfterSecond.GetValue().Data == view.GetValue().Data) &&
                               (viewAfterSecond.GetValue().Count == view.GetValue().Count);
        for (Foundation::Size index{0U}; index < viewAfterSecond.GetValue().Count; ++index)
        {
            descriptorsUnchanged = descriptorsUnchanged &&
                                   DescriptorsEqual(descriptorsBefore[index], viewAfterSecond.GetValue().Data[index]);
        }
    }
    ASTERIA_EXPECT(
        runner, "a second successful-owner Prepare is inert and reports AlreadyPrepared", !second.HasValue(),
        FailureIsExact(second.GetError(), ApplicationProcessorExecutionStorageOwnerError::AlreadyPrepared,
                       BootstrapPageAllocatorError::None, ApplicationProcessorExecutionStoragePreparationError::None,
                       CpuId{}.GetValue()),
        allocator.GetAllocatedPageCount() == allocatedPagesBefore, allocator.GetFreePageCount() == freePagesBefore,
        allocator.GetFreeRangeCount() == freeRangeCountBefore,
        owner.GetState() == ApplicationProcessorExecutionStorageOwnerState::Ready,
        owner.GetProcessorCount() == processorCount, owner.GetEnabledProcessorMask() == expectedMask,
        owner.GetDescriptorCount() == descriptorCount, result.HasValue(),
        result.GetValue().ProcessorCount == processorCount, result.GetValue().EnabledProcessorMask == expectedMask,
        result.GetValue().DescriptorCount == descriptorCount, result.GetValue().Reserved == 0U, descriptorsUnchanged,
        StorageMatchesExactAllocations(descriptorCount));
}

void TestPinnedVirtualSnapshotSuccess(TestContext& runner) noexcept
{
    FillStorage();
    Ia64LocalSapicIpiController controller;
    Ia64LocalSapicIpiControllerSnapshot snapshot{};
    BootstrapPageAllocator allocator;
    ApplicationProcessorExecutionStorageOwner owner;
    const bool setup = MakeSnapshot(controller, 2U, snapshot, PinnedVirtualPibAccessBase,
                                    Ia64ProcessorInterruptBlockBaseKind::PinnedUncachedVirtual) &&
                       InitializeStorageAllocator(allocator);
    const auto result = owner.Prepare(allocator, snapshot);
    const auto descriptor = owner.GetDescriptor(CpuId{1U});
    ASTERIA_EXPECT(runner, "a valid pinned-uncached virtual snapshot prepares AP storage", setup,
                   snapshot.BaseKind == Ia64ProcessorInterruptBlockBaseKind::PinnedUncachedVirtual,
                   snapshot.PhysicalBaseAddress == PibPhysicalBase,
                   snapshot.AccessBaseAddress == PinnedVirtualPibAccessBase, result.HasValue(),
                   result.GetValue().ProcessorCount == 2U, result.GetValue().EnabledProcessorMask == 0x03U,
                   result.GetValue().DescriptorCount == 1U,
                   owner.GetState() == ApplicationProcessorExecutionStorageOwnerState::Ready, descriptor.HasValue(),
                   IsDescriptorExact(descriptor.GetValue(), snapshot, 1U),
                   allocator.GetAllocatedPageCount() == ApplicationProcessorExecutionStoragePageCount,
                   StorageMatchesExactAllocations(1U));
}

void CheckInvalidSnapshot(TestContext& runner, const Ia64LocalSapicIpiControllerSnapshot& snapshot,
                          const char* const description,
                          const Foundation::UInt8 failingProcessor = CpuId{}.GetValue()) noexcept
{
    BootstrapPageAllocator allocator;
    ApplicationProcessorExecutionStorageOwner owner;
    const auto result = owner.Prepare(allocator, snapshot);
    const auto view = owner.GetDescriptorView();
    ASTERIA_EXPECT(runner, description, !result.HasValue(),
                   FailureIsExact(result.GetError(),
                                  ApplicationProcessorExecutionStorageOwnerError::InvalidControllerSnapshot,
                                  BootstrapPageAllocatorError::None,
                                  ApplicationProcessorExecutionStoragePreparationError::None, failingProcessor),
                   owner.GetState() == ApplicationProcessorExecutionStorageOwnerState::Failed,
                   owner.GetProcessorCount() == 0U, owner.GetEnabledProcessorMask() == 0U,
                   owner.GetDescriptorCount() == 0U, allocator.GetAllocatedPageCount() == 0U, !view.HasValue());
}

void TestInvalidSnapshots(TestContext& runner) noexcept
{
    Ia64LocalSapicIpiController controller;
    Ia64LocalSapicIpiControllerSnapshot valid{};
    ASTERIA_EXPECT(runner, "invalid-snapshot tests start from an actual controller snapshot",
                   MakeSnapshot(controller, 2U, valid));

    Ia64LocalSapicIpiControllerSnapshot snapshot{};
    CheckInvalidSnapshot(runner, snapshot, "a non-Ready snapshot is rejected");
    snapshot = valid;
    snapshot.ProcessorCount = 0U;
    CheckInvalidSnapshot(runner, snapshot, "processor count zero is rejected");
    snapshot = valid;
    snapshot.ProcessorCount = 5U;
    CheckInvalidSnapshot(runner, snapshot, "processor count above four is rejected");
    snapshot = valid;
    snapshot.EnabledProcessorMask = 0x05U;
    CheckInvalidSnapshot(runner, snapshot, "a non-exact processor mask is rejected");
    snapshot = valid;
    snapshot.Reserved[3U] = 1U;
    CheckInvalidSnapshot(runner, snapshot, "top-level reserved bytes are rejected");
    snapshot = valid;
    snapshot.BaseKind = static_cast<Ia64ProcessorInterruptBlockBaseKind>(2U);
    CheckInvalidSnapshot(runner, snapshot, "an invalid PIB base kind is rejected");
    snapshot = valid;
    snapshot.PhysicalBaseAddress += Kernel::Memory::PhysicalPageSizeBytes;
    CheckInvalidSnapshot(runner, snapshot, "invalid physical PIB geometry is rejected");
    snapshot = valid;
    snapshot.AccessBaseAddress += Kernel::Memory::PhysicalPageSizeBytes;
    CheckInvalidSnapshot(runner, snapshot, "invalid access PIB geometry is rejected");
    snapshot = valid;
    snapshot.AccessBaseAddress += ProcessorInterruptBlockSizeBytes;
    for (Foundation::UInt8 index{0U}; index < snapshot.ProcessorCount; ++index)
    {
        snapshot.Processors[index].InterruptAddress += ProcessorInterruptBlockSizeBytes;
    }
    CheckInvalidSnapshot(runner, snapshot, "an aligned but mismatched bootstrap access base is rejected");
    snapshot = valid;
    ++snapshot.Processors[1U].InterruptAddress;
    CheckInvalidSnapshot(runner, snapshot, "an inexact interrupt address is rejected", 1U);
    snapshot = valid;
    snapshot.Processors[1U].Reserved[1U] = 1U;
    CheckInvalidSnapshot(runner, snapshot, "used-record reserved bytes are rejected", 1U);
    snapshot = valid;
    snapshot.Processors[1U].AcpiUid = snapshot.Processors[0U].AcpiUid;
    CheckInvalidSnapshot(runner, snapshot, "duplicate ACPI UIDs are rejected", 1U);
    snapshot = valid;
    snapshot.Processors[1U].HardwareId = snapshot.Processors[0U].HardwareId;
    snapshot.Processors[1U].HardwareEid = snapshot.Processors[0U].HardwareEid;
    snapshot.Processors[1U].InterruptAddress = snapshot.Processors[0U].InterruptAddress;
    CheckInvalidSnapshot(runner, snapshot, "duplicate hardware ID/EID pairs are rejected", 1U);
    snapshot = valid;
    snapshot.Processors[2U].AcpiUid = 1U;
    CheckInvalidSnapshot(runner, snapshot, "nonzero unused processor records are rejected", 2U);
}

void TestUninitializedAllocatorAndTerminalFailure(TestContext& runner) noexcept
{
    Ia64LocalSapicIpiController controller;
    Ia64LocalSapicIpiControllerSnapshot snapshot{};
    BootstrapPageAllocator allocator;
    ApplicationProcessorExecutionStorageOwner owner;
    const bool setup = MakeSnapshot(controller, 2U, snapshot);
    const auto first = owner.Prepare(allocator, snapshot);
    const auto second = owner.Prepare(allocator, snapshot);
    const auto descriptor = owner.GetDescriptor(CpuId{1U});
    const auto view = owner.GetDescriptorView();
    ASTERIA_EXPECT(runner, "an uninitialized allocator fails terminally with no publication or retry", setup,
                   !first.HasValue(),
                   FailureIsExact(first.GetError(), ApplicationProcessorExecutionStorageOwnerError::AllocationFailed,
                                  BootstrapPageAllocatorError::NotInitialized,
                                  ApplicationProcessorExecutionStoragePreparationError::None, 1U),
                   !second.HasValue(),
                   FailureIsExact(second.GetError(), ApplicationProcessorExecutionStorageOwnerError::AlreadyPrepared,
                                  BootstrapPageAllocatorError::None,
                                  ApplicationProcessorExecutionStoragePreparationError::None, CpuId{}.GetValue()),
                   owner.GetState() == ApplicationProcessorExecutionStorageOwnerState::Failed,
                   owner.GetProcessorCount() == 0U, !descriptor.HasValue(), !view.HasValue());
}

void TestInsufficientAllocator(TestContext& runner) noexcept
{
    Ia64LocalSapicIpiController controller;
    Ia64LocalSapicIpiControllerSnapshot snapshot{};
    BootstrapPageAllocator allocator;
    ApplicationProcessorExecutionStorageOwner owner;
    const bool setup = MakeSnapshot(controller, 2U, snapshot);
    ConfigureBootMemory(StoragePhysicalStart(), ApplicationProcessorExecutionStoragePageCount - 1U);
    const bool initialized = allocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::None;
    const auto result = owner.Prepare(allocator, snapshot);
    ASTERIA_EXPECT(runner, "insufficient memory reports the allocator error and publishes nothing", setup, initialized,
                   !result.HasValue(),
                   FailureIsExact(result.GetError(), ApplicationProcessorExecutionStorageOwnerError::AllocationFailed,
                                  BootstrapPageAllocatorError::InsufficientMemory,
                                  ApplicationProcessorExecutionStoragePreparationError::None, 1U),
                   allocator.GetAllocatedPageCount() == 0U, !owner.GetDescriptorView().HasValue());
}

void TestPartialAllocationFailureRetainsPages(TestContext& runner) noexcept
{
    FillStorage();
    Ia64LocalSapicIpiController controller;
    Ia64LocalSapicIpiControllerSnapshot snapshot{};
    BootstrapPageAllocator allocator;
    ApplicationProcessorExecutionStorageOwner owner;
    const bool setup = MakeSnapshot(controller, 4U, snapshot) && InitializeStorageAllocator(allocator, 39U);
    const auto result = owner.Prepare(allocator, snapshot);
    ASTERIA_EXPECT(runner, "a later allocation failure retains earlier zeroed pages without publication", setup,
                   !result.HasValue(),
                   FailureIsExact(result.GetError(), ApplicationProcessorExecutionStorageOwnerError::AllocationFailed,
                                  BootstrapPageAllocatorError::InsufficientMemory,
                                  ApplicationProcessorExecutionStoragePreparationError::None, 3U),
                   allocator.GetAllocatedPageCount() == (2U * ApplicationProcessorExecutionStoragePageCount),
                   StorageMatchesExactAllocations(2U), owner.GetProcessorCount() == 0U,
                   owner.GetEnabledProcessorMask() == 0U, owner.GetDescriptorCount() == 0U,
                   !owner.GetDescriptor(CpuId{1U}).HasValue(), !owner.GetDescriptorView().HasValue());
}

void TestPreparerFailureRetainsAllocation(TestContext& runner) noexcept
{
    Ia64LocalSapicIpiController controller;
    Ia64LocalSapicIpiControllerSnapshot snapshot{};
    BootstrapPageAllocator allocator;
    ApplicationProcessorExecutionStorageOwner owner;
    const bool setup = MakeSnapshot(controller, 2U, snapshot);
    ConfigureBootMemory(Foundation::UInt64{1U} << 61U, ApplicationProcessorExecutionStoragePageCount);
    const bool initialized = allocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::None;
    const auto result = owner.Prepare(allocator, snapshot);
    ASTERIA_EXPECT(runner, "preparer failure retains its allocation and exposes no descriptor view", setup, initialized,
                   !result.HasValue(),
                   FailureIsExact(result.GetError(),
                                  ApplicationProcessorExecutionStorageOwnerError::StoragePreparationFailed,
                                  BootstrapPageAllocatorError::None,
                                  ApplicationProcessorExecutionStoragePreparationError::InvalidAllocationGeometry, 1U),
                   allocator.GetAllocatedPageCount() == ApplicationProcessorExecutionStoragePageCount,
                   !owner.GetDescriptorView().HasValue());
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"ApplicationProcessorExecutionStorageOwnerTests"};
    Asteria::Test::TestOneProcessorSuccess(runner);
    Asteria::Test::TestMultiProcessorSuccess(runner, 2U);
    Asteria::Test::TestMultiProcessorSuccess(runner, 3U);
    Asteria::Test::TestMultiProcessorSuccess(runner, 4U);
    Asteria::Test::TestPinnedVirtualSnapshotSuccess(runner);
    Asteria::Test::TestInvalidSnapshots(runner);
    Asteria::Test::TestUninitializedAllocatorAndTerminalFailure(runner);
    Asteria::Test::TestInsufficientAllocator(runner);
    Asteria::Test::TestPartialAllocationFailureRetainsPages(runner);
    Asteria::Test::TestPreparerFailureRetainsAllocation(runner);
    return runner.Finish();
}
