// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_APPLICATION_PROCESSOR_EXECUTION_STORAGE_OWNER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_APPLICATION_PROCESSOR_EXECUTION_STORAGE_OWNER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Ia64/Interrupt/Ia64LocalSapicIpiController.hpp"
#include "Platform/Ia64/Memory/ApplicationProcessorExecutionStoragePreparer.hpp"

namespace Asteria::Platform::Ia64::Memory
{
constexpr Foundation::Size MaximumApplicationProcessorExecutionStorageDescriptorCount{
    static_cast<Foundation::Size>(Kernel::Scheduler::MaximumProcessorCount - 1U)};

enum class ApplicationProcessorExecutionStorageOwnerState : Foundation::UInt8
{
    Fresh,
    Failed,
    Ready,
};

enum class ApplicationProcessorExecutionStorageOwnerError : Foundation::UInt32
{
    None,
    AlreadyPrepared,
    InvalidControllerSnapshot,
    AllocationFailed,
    StoragePreparationFailed,
};

enum class ApplicationProcessorExecutionStorageDescriptorAccessError : Foundation::UInt32
{
    None,
    NotReady,
    InvalidCpuId,
};

struct ApplicationProcessorExecutionStorageOwnerPreparationSummary
{
    Foundation::UInt8 ProcessorCount;
    Foundation::UInt8 EnabledProcessorMask;
    Foundation::UInt8 DescriptorCount;
    Foundation::UInt8 Reserved;
};

struct ApplicationProcessorExecutionStorageOwnerFailure
{
    ApplicationProcessorExecutionStorageOwnerError Error;
    Kernel::Memory::BootstrapPageAllocatorError AllocatorError;
    ApplicationProcessorExecutionStoragePreparationError PreparationError;
    Kernel::Scheduler::CpuId FailingProcessor;
    Foundation::UInt8 Reserved[3];
};

/// Boot-lifetime owner of every enabled IA-64 AP's initial execution storage.
///
/// Prepare and every ordinary accessor are BSP-serialized. The owner performs
/// no internal synchronization. If APs later observe a Ready owner, the caller
/// must publish completion through an external release/acquire operation such
/// as rendezvous Arm. Prepare allocates only from the supplied boot-lifetime
/// allocator; allocation or preparation failure consumes any pages already
/// removed from it and terminally leaves this owner Failed.
class ApplicationProcessorExecutionStorageOwner final
{
public:
    constexpr ApplicationProcessorExecutionStorageOwner() noexcept = default;

    ApplicationProcessorExecutionStorageOwner(const ApplicationProcessorExecutionStorageOwner&) = delete;
    ApplicationProcessorExecutionStorageOwner& operator=(const ApplicationProcessorExecutionStorageOwner&) = delete;
    ApplicationProcessorExecutionStorageOwner(ApplicationProcessorExecutionStorageOwner&&) = delete;
    ApplicationProcessorExecutionStorageOwner& operator=(ApplicationProcessorExecutionStorageOwner&&) = delete;

    /// Validates one actual Ready local-SAPIC snapshot, then prepares exactly
    /// one 64 KiB identity-mapped allocation for each logical CpuId after the
    /// BSP. The call is single-use even when it fails. No rollback or retry is
    /// provided, and success grants no mapping, handler, wake, or runnable
    /// state.
    [[nodiscard]] Foundation::Result<ApplicationProcessorExecutionStorageOwnerPreparationSummary,
                                     ApplicationProcessorExecutionStorageOwnerFailure>
    Prepare(Kernel::Memory::BootstrapPageAllocator& allocator,
            const Interrupt::Ia64LocalSapicIpiControllerSnapshot& snapshot) noexcept;

    [[nodiscard]] constexpr ApplicationProcessorExecutionStorageOwnerState GetState() const noexcept
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

    [[nodiscard]] constexpr Foundation::UInt8 GetDescriptorCount() const noexcept
    {
        return m_descriptorCount;
    }

    /// Returns the immutable descriptor for exactly one enabled non-BSP
    /// logical CpuId. Descriptors are inaccessible before the Ready commit.
    [[nodiscard]] Foundation::Result<ApplicationProcessorExecutionStorageDescriptor,
                                     ApplicationProcessorExecutionStorageDescriptorAccessError>
    GetDescriptor(Kernel::Scheduler::CpuId processor) const noexcept;

    /// Returns all immutable AP descriptors in ascending logical CpuId order.
    /// A Ready one-CPU owner returns the canonical null, zero-length view.
    [[nodiscard]] Foundation::Result<ApplicationProcessorExecutionStorageDescriptorView,
                                     ApplicationProcessorExecutionStorageDescriptorAccessError>
    GetDescriptorView() const noexcept;

private:
    ApplicationProcessorExecutionStorageDescriptor
        m_descriptors[MaximumApplicationProcessorExecutionStorageDescriptorCount]{};
    Foundation::UInt8 m_processorCount{0U};
    Foundation::UInt8 m_enabledProcessorMask{0U};
    Foundation::UInt8 m_descriptorCount{0U};
    ApplicationProcessorExecutionStorageOwnerState m_state{ApplicationProcessorExecutionStorageOwnerState::Fresh};
    [[maybe_unused]] Foundation::UInt8 m_reserved[4]{};
};

static_assert(MaximumApplicationProcessorExecutionStorageDescriptorCount == 3U);
static_assert(sizeof(ApplicationProcessorExecutionStorageOwnerState) == 1U);
static_assert(sizeof(ApplicationProcessorExecutionStorageOwnerError) == 4U);
static_assert(sizeof(ApplicationProcessorExecutionStorageDescriptorAccessError) == 4U);
static_assert(sizeof(ApplicationProcessorExecutionStorageOwnerPreparationSummary) == 4U);
static_assert(__is_aggregate(ApplicationProcessorExecutionStorageOwnerPreparationSummary));
static_assert(__is_standard_layout(ApplicationProcessorExecutionStorageOwnerPreparationSummary));
static_assert(__is_trivially_copyable(ApplicationProcessorExecutionStorageOwnerPreparationSummary));
static_assert(sizeof(ApplicationProcessorExecutionStorageOwnerFailure) == 16U);
static_assert(alignof(ApplicationProcessorExecutionStorageOwnerFailure) == 4U);
static_assert(__is_aggregate(ApplicationProcessorExecutionStorageOwnerFailure));
static_assert(__is_standard_layout(ApplicationProcessorExecutionStorageOwnerFailure));
static_assert(__is_trivially_copyable(ApplicationProcessorExecutionStorageOwnerFailure));
static_assert(__builtin_offsetof(ApplicationProcessorExecutionStorageOwnerFailure, Error) == 0U);
static_assert(__builtin_offsetof(ApplicationProcessorExecutionStorageOwnerFailure, AllocatorError) == 4U);
static_assert(__builtin_offsetof(ApplicationProcessorExecutionStorageOwnerFailure, PreparationError) == 8U);
static_assert(__builtin_offsetof(ApplicationProcessorExecutionStorageOwnerFailure, FailingProcessor) == 12U);
static_assert(sizeof(ApplicationProcessorExecutionStorageOwner) == 200U);
static_assert(alignof(ApplicationProcessorExecutionStorageOwner) == 8U);
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_APPLICATION_PROCESSOR_EXECUTION_STORAGE_OWNER_HPP_INCLUDED
