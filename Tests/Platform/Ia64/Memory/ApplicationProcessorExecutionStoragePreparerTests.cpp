// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/ApplicationProcessorExecutionStoragePreparer.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Memory::PhysicalPageRange;
using Kernel::Scheduler::CpuId;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageAlignmentPages;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageDescriptor;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageDescriptorView;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageIdentity;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStoragePageCount;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStoragePreparationError;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStoragePreparer;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageSizeBytes;
using Platform::Ia64::Memory::ApplicationProcessorInitialStackScratchSizeBytes;
using Platform::Ia64::Memory::ApplicationProcessorMemoryStackSizeBytes;
using Platform::Ia64::Memory::ApplicationProcessorRseBackingStoreSizeBytes;
using Platform::Ia64::Memory::MaximumPriorApplicationProcessorStorageDescriptors;
using Platform::Ia64::Memory::WritableInitialMemoryStackView;
using Platform::Ia64::Memory::WritableRseBackingStoreView;

constexpr Foundation::UInt64 AllocationPhysicalStart{0x0100'0000U};
constexpr Foundation::UInt64 PriorAllocationPhysicalStart{0x0200'0000U};
constexpr Foundation::UInt64 SecondPriorAllocationPhysicalStart{0x0300'0000U};
constexpr Foundation::UInt64 WriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 RuntimeAttribute{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt32 ConventionalMemoryType{7U};
constexpr Foundation::Size CanarySizeBytes{4096U};
constexpr Foundation::UInt8 CanaryValue{0xA5U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UIntPointer MaximumUIntPointer{~Foundation::UIntPointer{0U}};
constexpr Foundation::UInt64 RegionZeroSizeBytes{Foundation::UInt64{1U} << 61U};

alignas(4096)
    Foundation::UInt8 g_guardedStorage[ApplicationProcessorExecutionStorageSizeBytes + (2U * CanarySizeBytes)];
ApplicationProcessorExecutionStorageDescriptor g_priorDescriptors[MaximumPriorApplicationProcessorStorageDescriptors]{};

[[nodiscard]] Foundation::UInt8* StorageStart() noexcept
{
    return g_guardedStorage + CanarySizeBytes;
}

void FillGuardedStorage(const Foundation::UInt8 value) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(g_guardedStorage); ++index)
    {
        g_guardedStorage[index] = value;
    }
}

[[nodiscard]] bool IsGuardedStorageFilled(const Foundation::UInt8 value) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(g_guardedStorage); ++index)
    {
        if (g_guardedStorage[index] != value)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool SuccessStorageIsExact() noexcept
{
    for (Foundation::Size index{0U}; index < CanarySizeBytes; ++index)
    {
        if ((g_guardedStorage[index] != CanaryValue) ||
            (g_guardedStorage[CanarySizeBytes + ApplicationProcessorExecutionStorageSizeBytes + index] != CanaryValue))
        {
            return false;
        }
    }
    for (Foundation::Size index{0U}; index < ApplicationProcessorExecutionStorageSizeBytes; ++index)
    {
        if (StorageStart()[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr ApplicationProcessorExecutionStorageIdentity
ValidIdentity(const Foundation::UInt8 processor = 1U, const Foundation::UInt8 hardwareId = 0x12U,
              const Foundation::UInt8 hardwareEid = 0x34U) noexcept
{
    return ApplicationProcessorExecutionStorageIdentity{
        .Processor = CpuId{processor},
        .HardwareId = hardwareId,
        .HardwareEid = hardwareEid,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr PhysicalPageRange
ValidAllocation(const Foundation::UInt64 physicalStart = AllocationPhysicalStart) noexcept
{
    return PhysicalPageRange{
        .PhysicalStart = physicalStart,
        .PageCount = ApplicationProcessorExecutionStoragePageCount,
        .Attributes = WriteBackAttribute,
        .FirmwareType = ConventionalMemoryType,
        .Reserved = 0U,
    };
}

[[nodiscard]] WritableInitialMemoryStackView ValidMemoryStack() noexcept
{
    return WritableInitialMemoryStackView{
        .Data = StorageStart(),
        .SizeBytes = ApplicationProcessorMemoryStackSizeBytes,
        .PhysicalStart = AllocationPhysicalStart,
    };
}

[[nodiscard]] WritableRseBackingStoreView ValidRseBackingStore() noexcept
{
    return WritableRseBackingStoreView{
        .Data = StorageStart() + ApplicationProcessorMemoryStackSizeBytes,
        .SizeBytes = ApplicationProcessorRseBackingStoreSizeBytes,
        .PhysicalStart = AllocationPhysicalStart + ApplicationProcessorMemoryStackSizeBytes,
    };
}

[[nodiscard]] constexpr ApplicationProcessorExecutionStorageDescriptor
ValidPriorDescriptor(const Foundation::UInt64 physicalStart = PriorAllocationPhysicalStart,
                     const Foundation::UInt8 processor = 2U, const Foundation::UInt8 hardwareId = 0x56U,
                     const Foundation::UInt8 hardwareEid = 0x78U) noexcept
{
    return ApplicationProcessorExecutionStorageDescriptor{
        .AllocationPhysicalStart = physicalStart,
        .AllocationSizeBytes = ApplicationProcessorExecutionStorageSizeBytes,
        .MemoryStackPhysicalStart = physicalStart,
        .InitialStackPointer =
            physicalStart + ApplicationProcessorMemoryStackSizeBytes - ApplicationProcessorInitialStackScratchSizeBytes,
        .MemoryStackSizeBytes = ApplicationProcessorMemoryStackSizeBytes,
        .RseBackingStorePhysicalStart = physicalStart + ApplicationProcessorMemoryStackSizeBytes,
        .RseBackingStoreSizeBytes = ApplicationProcessorRseBackingStoreSizeBytes,
        .Processor = CpuId{processor},
        .HardwareId = hardwareId,
        .HardwareEid = hardwareEid,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr ApplicationProcessorExecutionStorageDescriptorView NoPriorDescriptors() noexcept
{
    return ApplicationProcessorExecutionStorageDescriptorView{
        .Data = nullptr,
        .Count = 0U,
    };
}

void CheckFailureIsInert(TestContext& runner, const ApplicationProcessorExecutionStorageIdentity identity,
                         const PhysicalPageRange allocation, const WritableInitialMemoryStackView memoryStack,
                         const WritableRseBackingStoreView rseBackingStore,
                         const ApplicationProcessorExecutionStorageDescriptorView priorDescriptors,
                         const ApplicationProcessorExecutionStoragePreparationError expected,
                         const char* const description) noexcept
{
    FillGuardedStorage(CanaryValue);
    const auto result = ApplicationProcessorExecutionStoragePreparer::Prepare(identity, allocation, memoryStack,
                                                                              rseBackingStore, priorDescriptors);
    ASTERIA_EXPECT(runner, description, !result.HasValue(), result.GetError() == expected,
                   IsGuardedStorageFilled(CanaryValue));
}

void TestSuccess(TestContext& runner) noexcept
{
    g_priorDescriptors[0U] = ValidPriorDescriptor(PriorAllocationPhysicalStart, 2U, 0x12U, 0x35U);
    g_priorDescriptors[1U] = ValidPriorDescriptor(SecondPriorAllocationPhysicalStart, 3U, 0x13U, 0x34U);

    FillGuardedStorage(CanaryValue);
    const auto result = ApplicationProcessorExecutionStoragePreparer::Prepare(
        ValidIdentity(), ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(),
        ApplicationProcessorExecutionStorageDescriptorView{
            .Data = g_priorDescriptors,
            .Count = 2U,
        });
    const ApplicationProcessorExecutionStorageDescriptor expected{
        .AllocationPhysicalStart = AllocationPhysicalStart,
        .AllocationSizeBytes = ApplicationProcessorExecutionStorageSizeBytes,
        .MemoryStackPhysicalStart = AllocationPhysicalStart,
        .InitialStackPointer = AllocationPhysicalStart + ApplicationProcessorMemoryStackSizeBytes -
                               ApplicationProcessorInitialStackScratchSizeBytes,
        .MemoryStackSizeBytes = ApplicationProcessorMemoryStackSizeBytes,
        .RseBackingStorePhysicalStart = AllocationPhysicalStart + ApplicationProcessorMemoryStackSizeBytes,
        .RseBackingStoreSizeBytes = ApplicationProcessorRseBackingStoreSizeBytes,
        .Processor = CpuId{1U},
        .HardwareId = 0x12U,
        .HardwareEid = 0x34U,
        .Reserved = {},
    };
    bool descriptorMatches{result.HasValue()};
    if (result.HasValue())
    {
        const auto& descriptor = result.GetValue();
        descriptorMatches = (descriptor.AllocationPhysicalStart == expected.AllocationPhysicalStart) &&
                            (descriptor.AllocationSizeBytes == expected.AllocationSizeBytes) &&
                            (descriptor.MemoryStackPhysicalStart == expected.MemoryStackPhysicalStart) &&
                            (descriptor.InitialStackPointer == expected.InitialStackPointer) &&
                            (descriptor.MemoryStackSizeBytes == expected.MemoryStackSizeBytes) &&
                            (descriptor.RseBackingStorePhysicalStart == expected.RseBackingStorePhysicalStart) &&
                            (descriptor.RseBackingStoreSizeBytes == expected.RseBackingStoreSizeBytes) &&
                            (descriptor.Processor.GetValue() == expected.Processor.GetValue()) &&
                            (descriptor.HardwareId == expected.HardwareId) &&
                            (descriptor.HardwareEid == expected.HardwareEid);
        for (const Foundation::UInt8 reserved : descriptor.Reserved)
        {
            descriptorMatches = descriptorMatches && (reserved == 0U);
        }
    }
    ASTERIA_EXPECT(runner, "success permits partial ID/EID matches, zeroes exact halves, and returns inert metadata",
                   result.HasValue(), result.GetError() == ApplicationProcessorExecutionStoragePreparationError::None,
                   descriptorMatches, SuccessStorageIsExact());
}

void TestIdentityFailures(TestContext& runner) noexcept
{
    ApplicationProcessorExecutionStorageIdentity identity = ValidIdentity();
    identity.Processor = CpuId{0U};
    CheckFailureIsInert(runner, identity, ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(),
                        NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidProcessorIdentity,
                        "CpuId zero is reserved for the BSP and writes nothing");

    identity = ValidIdentity();
    identity.Processor = CpuId{};
    CheckFailureIsInert(runner, identity, ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(),
                        NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidProcessorIdentity,
                        "the invalid CpuId sentinel writes nothing");

    identity = ValidIdentity();
    identity.Processor = CpuId{Kernel::Scheduler::MaximumProcessorCount};
    CheckFailureIsInert(runner, identity, ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(),
                        NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidProcessorIdentity,
                        "an out-of-range CpuId writes nothing");

    identity = ValidIdentity();
    identity.Reserved = 1U;
    CheckFailureIsInert(runner, identity, ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(),
                        NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidProcessorIdentity,
                        "nonzero identity reserved state writes nothing");
}

void TestAllocationFailures(TestContext& runner) noexcept
{
    PhysicalPageRange allocation = ValidAllocation();
    allocation.PhysicalStart = 0U;
    CheckFailureIsInert(runner, ValidIdentity(), allocation, ValidMemoryStack(), ValidRseBackingStore(),
                        NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidAllocationGeometry,
                        "page-zero allocation geometry writes nothing");

    allocation = ValidAllocation();
    allocation.PhysicalStart += Kernel::Memory::PhysicalPageSizeBytes;
    CheckFailureIsInert(runner, ValidIdentity(), allocation, ValidMemoryStack(), ValidRseBackingStore(),
                        NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidAllocationGeometry,
                        "a reservation not aligned to eight pages writes nothing");

    allocation = ValidAllocation();
    allocation.PageCount = ApplicationProcessorExecutionStoragePageCount - 1U;
    CheckFailureIsInert(runner, ValidIdentity(), allocation, ValidMemoryStack(), ValidRseBackingStore(),
                        NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidAllocationGeometry,
                        "a non-16-page reservation writes nothing");

    allocation = ValidAllocation();
    allocation.PhysicalStart =
        MaximumUInt64 &
        ~((ApplicationProcessorExecutionStorageAlignmentPages * Kernel::Memory::PhysicalPageSizeBytes) - 1U);
    CheckFailureIsInert(runner, ValidIdentity(), allocation, ValidMemoryStack(), ValidRseBackingStore(),
                        NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidAllocationGeometry,
                        "overflowing allocation geometry writes nothing");

    allocation = ValidAllocation();
    allocation.PhysicalStart = RegionZeroSizeBytes - (ApplicationProcessorExecutionStorageSizeBytes / 2U);
    CheckFailureIsInert(runner, ValidIdentity(), allocation, ValidMemoryStack(), ValidRseBackingStore(),
                        NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidAllocationGeometry,
                        "an allocation crossing IA-64 virtual region zero writes nothing");

    allocation = ValidAllocation();
    allocation.Attributes = 0U;
    CheckFailureIsInert(runner, ValidIdentity(), allocation, ValidMemoryStack(), ValidRseBackingStore(),
                        NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidAllocationAttributes,
                        "an allocation without write-back capability writes nothing");

    allocation = ValidAllocation();
    allocation.Attributes |= RuntimeAttribute;
    CheckFailureIsInert(runner, ValidIdentity(), allocation, ValidMemoryStack(), ValidRseBackingStore(),
                        NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidAllocationAttributes,
                        "a runtime-attributed allocation writes nothing");

    allocation = ValidAllocation();
    allocation.FirmwareType = 4U;
    CheckFailureIsInert(runner, ValidIdentity(), allocation, ValidMemoryStack(), ValidRseBackingStore(),
                        NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidAllocationAttributes,
                        "a non-Conventional allocation writes nothing");

    allocation = ValidAllocation();
    allocation.Reserved = 1U;
    CheckFailureIsInert(runner, ValidIdentity(), allocation, ValidMemoryStack(), ValidRseBackingStore(),
                        NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidAllocationAttributes,
                        "nonzero allocation reserved state writes nothing");
}

void TestWritableViewFailures(TestContext& runner) noexcept
{
    WritableInitialMemoryStackView stack = ValidMemoryStack();
    stack.Data = nullptr;
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), stack, ValidRseBackingStore(), NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidMemoryStackView,
                        "a null memory-stack view writes nothing");

    stack = ValidMemoryStack();
    --stack.SizeBytes;
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), stack, ValidRseBackingStore(), NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidMemoryStackView,
                        "an inexact memory-stack size writes nothing");

    stack = ValidMemoryStack();
    ++stack.Data;
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), stack, ValidRseBackingStore(), NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidMemoryStackView,
                        "a misaligned memory-stack alias writes nothing");

    stack = ValidMemoryStack();
    stack.Data =
        reinterpret_cast<Foundation::UInt8*>(MaximumUIntPointer & ~(Kernel::Memory::PhysicalPageSizeBytes - 1U));
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), stack, ValidRseBackingStore(), NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidMemoryStackView,
                        "an overflowing memory-stack alias writes nothing");

    stack = ValidMemoryStack();
    stack.PhysicalStart += Kernel::Memory::PhysicalPageSizeBytes;
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), stack, ValidRseBackingStore(), NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::StorageAddressMismatch,
                        "a memory-stack physical-address mismatch writes nothing");

    WritableRseBackingStoreView rse = ValidRseBackingStore();
    rse.Data = nullptr;
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), rse, NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidRseBackingStoreView,
                        "a null RSE view writes nothing");

    rse = ValidRseBackingStore();
    ++rse.SizeBytes;
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), rse, NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidRseBackingStoreView,
                        "an inexact RSE size writes nothing");

    rse = ValidRseBackingStore();
    ++rse.Data;
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), rse, NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidRseBackingStoreView,
                        "a misaligned RSE alias writes nothing");

    rse = ValidRseBackingStore();
    rse.Data = reinterpret_cast<Foundation::UInt8*>(MaximumUIntPointer & ~(Kernel::Memory::PhysicalPageSizeBytes - 1U));
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), rse, NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::InvalidRseBackingStoreView,
                        "an overflowing RSE alias writes nothing");

    rse = ValidRseBackingStore();
    rse.PhysicalStart += Kernel::Memory::PhysicalPageSizeBytes;
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), rse, NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::StorageAddressMismatch,
                        "an RSE physical-address mismatch writes nothing");

    rse = ValidRseBackingStore();
    rse.Data += Kernel::Memory::PhysicalPageSizeBytes;
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), rse, NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::StorageViewsNotContiguous,
                        "a gap between writable halves writes nothing");

    rse = ValidRseBackingStore();
    rse.Data = StorageStart();
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), rse, NoPriorDescriptors(),
                        ApplicationProcessorExecutionStoragePreparationError::StorageViewsNotContiguous,
                        "overlapping writable halves write nothing");
}

void TestPriorDescriptorFailures(TestContext& runner) noexcept
{
    ApplicationProcessorExecutionStorageDescriptorView view{
        .Data = g_priorDescriptors,
        .Count = 0U,
    };
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(), view,
                        ApplicationProcessorExecutionStoragePreparationError::InvalidPriorDescriptorView,
                        "a nonnull empty prior-descriptor view writes nothing");

    view = ApplicationProcessorExecutionStorageDescriptorView{
        .Data = nullptr,
        .Count = 1U,
    };
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(), view,
                        ApplicationProcessorExecutionStoragePreparationError::InvalidPriorDescriptorView,
                        "a null nonempty prior-descriptor view writes nothing");

    view = ApplicationProcessorExecutionStorageDescriptorView{
        .Data = g_priorDescriptors,
        .Count = MaximumPriorApplicationProcessorStorageDescriptors + 1U,
    };
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(), view,
                        ApplicationProcessorExecutionStoragePreparationError::InvalidPriorDescriptorView,
                        "an over-capacity prior-descriptor view writes nothing");

    const Foundation::UIntPointer priorDescriptorAddress =
        reinterpret_cast<Foundation::UIntPointer>(g_priorDescriptors);
    const Foundation::UIntPointer misalignedPriorDescriptorAddress = priorDescriptorAddress + 1U;
    view = ApplicationProcessorExecutionStorageDescriptorView{
        .Data =
            reinterpret_cast<const ApplicationProcessorExecutionStorageDescriptor*>(misalignedPriorDescriptorAddress),
        .Count = 1U,
    };
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(), view,
                        ApplicationProcessorExecutionStoragePreparationError::InvalidPriorDescriptorView,
                        "a misaligned prior-descriptor array writes nothing");

    view = ApplicationProcessorExecutionStorageDescriptorView{
        .Data = reinterpret_cast<const ApplicationProcessorExecutionStorageDescriptor*>(MaximumUIntPointer &
                                                                                        ~Foundation::UIntPointer{7U}),
        .Count = 1U,
    };
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(), view,
                        ApplicationProcessorExecutionStoragePreparationError::InvalidPriorDescriptorView,
                        "an overflowing prior-descriptor address writes nothing");

    const Foundation::UIntPointer destinationStorageAddress = reinterpret_cast<Foundation::UIntPointer>(StorageStart());
    view = ApplicationProcessorExecutionStorageDescriptorView{
        .Data = reinterpret_cast<const ApplicationProcessorExecutionStorageDescriptor*>(destinationStorageAddress),
        .Count = 1U,
    };
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(), view,
                        ApplicationProcessorExecutionStoragePreparationError::StorageAliasesInput,
                        "destination storage cannot contain the prior-descriptor array");

    g_priorDescriptors[0U] = ValidPriorDescriptor();
    g_priorDescriptors[0U].MemoryStackSizeBytes -= 1U;
    view = ApplicationProcessorExecutionStorageDescriptorView{
        .Data = g_priorDescriptors,
        .Count = 1U,
    };
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(), view,
                        ApplicationProcessorExecutionStoragePreparationError::InvalidPriorDescriptor,
                        "malformed prior storage geometry writes nothing");

    g_priorDescriptors[0U] = ValidPriorDescriptor();
    g_priorDescriptors[0U].AllocationPhysicalStart =
        MaximumUInt64 &
        ~((ApplicationProcessorExecutionStorageAlignmentPages * Kernel::Memory::PhysicalPageSizeBytes) - 1U);
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(), view,
                        ApplicationProcessorExecutionStoragePreparationError::InvalidPriorDescriptor,
                        "an overflowing prior allocation descriptor writes nothing");

    g_priorDescriptors[0U] = ValidPriorDescriptor();
    g_priorDescriptors[0U].Processor = CpuId{0U};
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(), view,
                        ApplicationProcessorExecutionStoragePreparationError::InvalidPriorDescriptor,
                        "a BSP prior descriptor writes nothing");

    g_priorDescriptors[0U] = ValidPriorDescriptor();
    g_priorDescriptors[0U].Reserved[0U] = 1U;
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(), view,
                        ApplicationProcessorExecutionStoragePreparationError::InvalidPriorDescriptor,
                        "nonzero prior identity reserved state writes nothing");

    g_priorDescriptors[0U] = ValidPriorDescriptor(PriorAllocationPhysicalStart, 1U, 0x56U, 0x78U);
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(), view,
                        ApplicationProcessorExecutionStoragePreparationError::DuplicateProcessorIdentity,
                        "duplicate logical CpuId publication writes nothing");

    g_priorDescriptors[0U] = ValidPriorDescriptor(PriorAllocationPhysicalStart, 2U, 0x12U, 0x34U);
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(), view,
                        ApplicationProcessorExecutionStoragePreparationError::DuplicateProcessorIdentity,
                        "duplicate Local SAPIC ID/EID publication writes nothing");

    g_priorDescriptors[0U] = ValidPriorDescriptor(AllocationPhysicalStart, 2U, 0x56U, 0x78U);
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(), view,
                        ApplicationProcessorExecutionStoragePreparationError::StorageOverlapsPriorAllocation,
                        "overlap with a prior AP allocation writes nothing");

    g_priorDescriptors[0U] = ValidPriorDescriptor();
    g_priorDescriptors[1U] = ValidPriorDescriptor(SecondPriorAllocationPhysicalStart, 2U, 0x9AU, 0xBCU);
    view.Count = 2U;
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(), view,
                        ApplicationProcessorExecutionStoragePreparationError::InvalidPriorDescriptor,
                        "duplicate identities inside the prior set write nothing");

    g_priorDescriptors[0U] = ValidPriorDescriptor();
    g_priorDescriptors[1U] = ValidPriorDescriptor(
        PriorAllocationPhysicalStart + (ApplicationProcessorExecutionStorageSizeBytes / 2U), 3U, 0x9AU, 0xBCU);
    view.Count = 2U;
    CheckFailureIsInert(runner, ValidIdentity(), ValidAllocation(), ValidMemoryStack(), ValidRseBackingStore(), view,
                        ApplicationProcessorExecutionStoragePreparationError::InvalidPriorDescriptor,
                        "overlapping allocations inside the prior set write nothing");
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"ApplicationProcessorExecutionStoragePreparerTests"};
    Asteria::Test::TestSuccess(runner);
    Asteria::Test::TestIdentityFailures(runner);
    Asteria::Test::TestAllocationFailures(runner);
    Asteria::Test::TestWritableViewFailures(runner);
    Asteria::Test::TestPriorDescriptorFailures(runner);
    return runner.Finish();
}
