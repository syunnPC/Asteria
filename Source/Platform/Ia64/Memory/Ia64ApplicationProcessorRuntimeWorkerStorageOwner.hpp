// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_APPLICATION_PROCESSOR_RUNTIME_WORKER_STORAGE_OWNER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_APPLICATION_PROCESSOR_RUNTIME_WORKER_STORAGE_OWNER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Ia64/Memory/ApplicationProcessorExecutionStorageOwner.hpp"
#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimeLayout.hpp"

namespace Asteria::Platform::Ia64::Memory
{
enum class Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState : Foundation::UInt8
{
    Fresh,
    Failed,
    Ready,
};

enum class Ia64ApplicationProcessorRuntimeWorkerStorageOwnerError : Foundation::UInt32
{
    None,
    AlreadyPrepared,
    ExecutionStorageOwnerNotReady,
    InvalidExecutionStorageOwner,
    AllocationFailed,
    InvalidWorkerPool,
    WorkerPoolOverlapsIdleStorage,
};

enum class Ia64ApplicationProcessorRuntimeWorkerStorageAccessError : Foundation::UInt32
{
    None,
    NotReady,
};

struct Ia64ApplicationProcessorRuntimeWorkerStoragePreparationSummary
{
    Foundation::UInt64 WorkerPageCount;
    Foundation::UInt8 ProcessorCount;
    Foundation::UInt8 EnabledProcessorMask;
    Foundation::UInt8 WorkerStorageCount;
    Foundation::UInt8 Reserved[5];
};

struct Ia64ApplicationProcessorRuntimeWorkerStorageFailure
{
    Ia64ApplicationProcessorRuntimeWorkerStorageOwnerError Error;
    Kernel::Memory::BootstrapPageAllocatorError AllocatorError;
    ApplicationProcessorExecutionStorageDescriptorAccessError ExecutionStorageAccessError;
    Kernel::Scheduler::CpuId FailingProcessor;
    Foundation::UInt8 Reserved[3];
};

/// Boot-lifetime owner of the one contiguous IA-64 AP runtime worker pool.
///
/// Prepare and every accessor are BSP-serialized and perform no internal
/// synchronization. The supplied execution-storage owner is the sole topology
/// source, tying the idle allocations and worker allocation to one boot. A
/// valid first call is terminal on failure, and allocated pages are never
/// released or exposed unless the final Ready commit succeeds.
class Ia64ApplicationProcessorRuntimeWorkerStorageOwner final
{
public:
    constexpr Ia64ApplicationProcessorRuntimeWorkerStorageOwner() noexcept = default;

    Ia64ApplicationProcessorRuntimeWorkerStorageOwner(const Ia64ApplicationProcessorRuntimeWorkerStorageOwner&) =
        delete;
    Ia64ApplicationProcessorRuntimeWorkerStorageOwner&
    operator=(const Ia64ApplicationProcessorRuntimeWorkerStorageOwner&) = delete;
    Ia64ApplicationProcessorRuntimeWorkerStorageOwner(Ia64ApplicationProcessorRuntimeWorkerStorageOwner&&) = delete;
    Ia64ApplicationProcessorRuntimeWorkerStorageOwner&
    operator=(Ia64ApplicationProcessorRuntimeWorkerStorageOwner&&) = delete;

    /// Validates the complete Ready execution-storage view before allocation.
    /// SMP1 commits the canonical zero range without touching the allocator.
    /// SMP2-4 allocate and clear exactly one planner-compatible worker pool.
    [[nodiscard]] Foundation::Result<Ia64ApplicationProcessorRuntimeWorkerStoragePreparationSummary,
                                     Ia64ApplicationProcessorRuntimeWorkerStorageFailure>
    Prepare(Kernel::Memory::BootstrapPageAllocator& allocator,
            const ApplicationProcessorExecutionStorageOwner& executionStorageOwner) noexcept;

    [[nodiscard]] constexpr Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState GetState() const noexcept
    {
        return m_state;
    }

    [[nodiscard]] constexpr Foundation::UInt8 GetProcessorCount() const noexcept
    {
        return m_processorCount;
    }

    [[nodiscard]] constexpr Foundation::UInt8 GetEnabledProcessorMask() const noexcept
    {
        return m_enabledProcessorMask;
    }

    [[nodiscard]] constexpr Foundation::UInt8 GetWorkerStorageCount() const noexcept
    {
        return m_workerStorageCount;
    }

    /// Returns the immutable contiguous pool only after its clear and fence.
    [[nodiscard]] Foundation::Result<Kernel::Memory::PhysicalPageRange,
                                     Ia64ApplicationProcessorRuntimeWorkerStorageAccessError>
    GetWorkerPool() const noexcept;

private:
    Kernel::Memory::PhysicalPageRange m_workerPool{};
    Foundation::UInt8 m_processorCount{0U};
    Foundation::UInt8 m_enabledProcessorMask{0U};
    Foundation::UInt8 m_workerStorageCount{0U};
    Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState m_state{
        Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState::Fresh};
    [[maybe_unused]] Foundation::UInt8 m_reserved[4]{};
};

static_assert(sizeof(Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeWorkerStorageOwnerError) == 4U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeWorkerStorageAccessError) == 4U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeWorkerStoragePreparationSummary) == 16U);
static_assert(__is_aggregate(Ia64ApplicationProcessorRuntimeWorkerStoragePreparationSummary));
static_assert(__is_standard_layout(Ia64ApplicationProcessorRuntimeWorkerStoragePreparationSummary));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorRuntimeWorkerStoragePreparationSummary));
static_assert(sizeof(Ia64ApplicationProcessorRuntimeWorkerStorageFailure) == 16U);
static_assert(alignof(Ia64ApplicationProcessorRuntimeWorkerStorageFailure) == 4U);
static_assert(__is_aggregate(Ia64ApplicationProcessorRuntimeWorkerStorageFailure));
static_assert(__is_standard_layout(Ia64ApplicationProcessorRuntimeWorkerStorageFailure));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorRuntimeWorkerStorageFailure));
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeWorkerStorageFailure, Error) == 0U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeWorkerStorageFailure, AllocatorError) == 4U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeWorkerStorageFailure, ExecutionStorageAccessError) ==
              8U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorRuntimeWorkerStorageFailure, FailingProcessor) == 12U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeWorkerStorageOwner) == 40U);
static_assert(alignof(Ia64ApplicationProcessorRuntimeWorkerStorageOwner) == 8U);
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_APPLICATION_PROCESSOR_RUNTIME_WORKER_STORAGE_OWNER_HPP_INCLUDED
