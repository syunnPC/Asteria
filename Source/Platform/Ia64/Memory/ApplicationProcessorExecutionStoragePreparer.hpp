// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_APPLICATION_PROCESSOR_EXECUTION_STORAGE_PREPARER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_APPLICATION_PROCESSOR_EXECUTION_STORAGE_PREPARER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Ia64/Memory/InitialExecutionContextSwitcher.hpp"
#include "Platform/Ia64/Memory/InitialRseBackingStorePreparer.hpp"

namespace Asteria::Platform::Ia64::Memory
{
constexpr Foundation::UInt64 ApplicationProcessorExecutionStoragePageCount{16U};
constexpr Foundation::UInt64 ApplicationProcessorExecutionStorageAlignmentPages{8U};
constexpr Foundation::UInt64 ApplicationProcessorMemoryStackPageCount{8U};
constexpr Foundation::UInt64 ApplicationProcessorRseBackingStorePageCount{8U};
constexpr Foundation::UInt64 ApplicationProcessorMemoryStackSizeBytes{ApplicationProcessorMemoryStackPageCount *
                                                                      Kernel::Memory::PhysicalPageSizeBytes};
constexpr Foundation::UInt64 ApplicationProcessorRseBackingStoreSizeBytes{ApplicationProcessorRseBackingStorePageCount *
                                                                          Kernel::Memory::PhysicalPageSizeBytes};
constexpr Foundation::UInt64 ApplicationProcessorExecutionStorageSizeBytes{
    ApplicationProcessorExecutionStoragePageCount * Kernel::Memory::PhysicalPageSizeBytes};
constexpr Foundation::UInt64 ApplicationProcessorInitialStackScratchSizeBytes{16U};
constexpr Foundation::Size MaximumPriorApplicationProcessorStorageDescriptors{
    static_cast<Foundation::Size>(Kernel::Scheduler::MaximumProcessorCount - 1U)};

/// The logical and Local SAPIC identity already accepted by the immutable
/// BSP-first processor map. Processor must identify an enabled non-BSP CPU.
struct ApplicationProcessorExecutionStorageIdentity
{
    Kernel::Scheduler::CpuId Processor;
    Foundation::UInt8 HardwareId;
    Foundation::UInt8 HardwareEid;
    Foundation::UInt8 Reserved;
};

/// Immutable, inert metadata for one successfully initialized AP-owned
/// execution-storage allocation. It grants no mapping, runnable, wake, or
/// scheduler state and contains no writable alias.
struct ApplicationProcessorExecutionStorageDescriptor
{
    Foundation::UInt64 AllocationPhysicalStart;
    Foundation::UInt64 AllocationSizeBytes;
    Foundation::UInt64 MemoryStackPhysicalStart;
    Foundation::UInt64 InitialStackPointer;
    Foundation::UInt64 MemoryStackSizeBytes;
    Foundation::UInt64 RseBackingStorePhysicalStart;
    Foundation::UInt64 RseBackingStoreSizeBytes;
    Kernel::Scheduler::CpuId Processor;
    Foundation::UInt8 HardwareId;
    Foundation::UInt8 HardwareEid;
    Foundation::UInt8 Reserved[5];
};

/// Previously returned descriptors. The preparer revalidates their complete
/// inert shape before using them to reject duplicate CPU/LID identities and
/// overlapping AP allocations.
struct ApplicationProcessorExecutionStorageDescriptorView
{
    const ApplicationProcessorExecutionStorageDescriptor* Data;
    Foundation::Size Count;
};

enum class ApplicationProcessorExecutionStoragePreparationError : Foundation::UInt32
{
    None,
    InvalidProcessorIdentity,
    DuplicateProcessorIdentity,
    InvalidAllocationGeometry,
    InvalidAllocationAttributes,
    InvalidMemoryStackView,
    InvalidRseBackingStoreView,
    StorageAddressMismatch,
    StorageViewsNotContiguous,
    InvalidPriorDescriptorView,
    InvalidPriorDescriptor,
    StorageAliasesInput,
    StorageOverlapsPriorAllocation,
};

/// Validates and initializes exactly one enabled non-BSP CPU's physical
/// execution-storage allocation before SAL handler configuration or AP wake.
///
/// Every identity, allocation, view, and prior-descriptor check is
/// complete before the first destination write. Validation failure therefore
/// leaves every destination byte unchanged. After validation, both exact
/// 32 KiB halves are zeroed and fenced. Only then is inert metadata returned.
/// The allocator has no rollback operation; any failure remains a
/// caller-owned allocation with no descriptor publication or wake authority.
class ApplicationProcessorExecutionStoragePreparer final
{
public:
    [[nodiscard]] static Foundation::Result<ApplicationProcessorExecutionStorageDescriptor,
                                            ApplicationProcessorExecutionStoragePreparationError>
    Prepare(ApplicationProcessorExecutionStorageIdentity identity, Kernel::Memory::PhysicalPageRange allocation,
            WritableInitialMemoryStackView memoryStack, WritableRseBackingStoreView rseBackingStore,
            ApplicationProcessorExecutionStorageDescriptorView priorDescriptors) noexcept;

private:
    ApplicationProcessorExecutionStoragePreparer() = delete;
};

static_assert(ApplicationProcessorExecutionStoragePageCount == 16U);
static_assert(ApplicationProcessorExecutionStorageAlignmentPages == 8U);
static_assert(ApplicationProcessorMemoryStackSizeBytes == 32U * 1024U);
static_assert(ApplicationProcessorRseBackingStoreSizeBytes == 32U * 1024U);
static_assert(ApplicationProcessorExecutionStorageSizeBytes == 64U * 1024U);
static_assert(ApplicationProcessorInitialStackScratchSizeBytes == 16U);
static_assert(sizeof(ApplicationProcessorExecutionStorageIdentity) == 4U);
static_assert(__is_aggregate(ApplicationProcessorExecutionStorageIdentity));
static_assert(__is_standard_layout(ApplicationProcessorExecutionStorageIdentity));
static_assert(__is_trivially_copyable(ApplicationProcessorExecutionStorageIdentity));
static_assert(sizeof(ApplicationProcessorExecutionStorageDescriptor) == 64U);
static_assert(alignof(ApplicationProcessorExecutionStorageDescriptor) == 8U);
static_assert(__is_aggregate(ApplicationProcessorExecutionStorageDescriptor));
static_assert(__is_standard_layout(ApplicationProcessorExecutionStorageDescriptor));
static_assert(__is_trivially_copyable(ApplicationProcessorExecutionStorageDescriptor));
static_assert(__builtin_offsetof(ApplicationProcessorExecutionStorageDescriptor, AllocationPhysicalStart) == 0U);
static_assert(__builtin_offsetof(ApplicationProcessorExecutionStorageDescriptor, MemoryStackPhysicalStart) == 16U);
static_assert(__builtin_offsetof(ApplicationProcessorExecutionStorageDescriptor, InitialStackPointer) == 24U);
static_assert(__builtin_offsetof(ApplicationProcessorExecutionStorageDescriptor, RseBackingStorePhysicalStart) == 40U);
static_assert(__builtin_offsetof(ApplicationProcessorExecutionStorageDescriptor, Processor) == 56U);
static_assert(sizeof(ApplicationProcessorExecutionStorageDescriptorView) == 16U);
static_assert(alignof(ApplicationProcessorExecutionStorageDescriptorView) == 8U);
static_assert(__is_aggregate(ApplicationProcessorExecutionStorageDescriptorView));
static_assert(__is_standard_layout(ApplicationProcessorExecutionStorageDescriptorView));
static_assert(__is_trivially_copyable(ApplicationProcessorExecutionStorageDescriptorView));
static_assert(sizeof(ApplicationProcessorExecutionStoragePreparationError) == 4U);
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_APPLICATION_PROCESSOR_EXECUTION_STORAGE_PREPARER_HPP_INCLUDED
