// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_EXECUTION_CONTEXT_SWITCHER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_EXECUTION_CONTEXT_SWITCHER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Ia64/Memory/InitialRseBackingStorePreparer.hpp"

namespace Asteria::Platform::Ia64::Memory
{
class InitialAddressSpacePreparation;

constexpr Foundation::Size InitialMemoryStackSizeBytes{static_cast<Foundation::Size>(BootstrapStackPageCount) *
                                                       Kernel::Memory::MappingPageSizeBytes};
constexpr Foundation::Size InitialMemoryStackScratchSizeBytes{16U};

/// Describes the writable identity alias of the reserved initial memory stack.
/// The caller keeps this view and its storage stable until PrepareAndSwitch
/// commits.
struct WritableInitialMemoryStackView
{
    Foundation::UInt8* Data;
    Foundation::Size SizeBytes;
    Foundation::UInt64 PhysicalStart;
};

enum class InitialMemoryStackPreparationError : Foundation::UInt32
{
    None,
    InvalidMappingPlan,
    InvalidMappingSummary,
    InvalidStorageView,
    StorageAddressMismatch,
    StorageAliasesInput,
    MissingStackMapping,
    InvalidStackMapping,
    ConflictingStackMapping,
};

enum class InitialExecutionContextSwitchError : Foundation::UInt32
{
    None,
    AddressSpaceNotPrepared,
    MemoryStackRejected,
    RseBackingStoreRejected,
    StorageRangesOverlap,
    RseBackingStoreNotZero,
    MemoryStackNotZero,
    UnsupportedProcessorState,
    UnexpectedAdapterReturn,
};

struct InitialExecutionContextValidationSummary
{
    Foundation::UInt64 StackPhysicalStart;
    Foundation::UInt64 InitialStackPointer;
    Foundation::UInt64 RseBackingStorePhysicalStart;
    Foundation::UInt32 StackSizeBytes;
    Foundation::UInt32 RseBackingStoreSizeBytes;
};

struct InitialExecutionContextSwitchFailure
{
    InitialExecutionContextSwitchError Error;
    InitialMemoryStackPreparationError MemoryStackError;
    InitialRseBackingStorePreparationError RseBackingStoreError;
    Foundation::UInt32 Reserved;
};

/// Validates and initializes the processor-owned stack pair before entering a
/// fresh IA-64 execution context. All validation is performed on the
/// non-preemptible BSP after firmware exit. PrepareAndSwitch is the sole commit
/// boundary: it validates every retained input, clears the exact memory-stack
/// range, publishes the clear with an IA-64 memory fence, rechecks both owned
/// ranges, and immediately enters the architectural adapter. No authorization
/// object or prepared summary crosses back to the caller. A successful switch
/// never returns; any returned value is a pre-commit failure, a defensive
/// post-clear check failure, or an unexpected adapter return.
class InitialExecutionContextSwitcher final
{
public:
    [[nodiscard]] static Foundation::Result<InitialExecutionContextValidationSummary,
                                            InitialExecutionContextSwitchFailure>
    Validate(const InitialAddressSpacePreparation& preparation, const WritableInitialMemoryStackView& memoryStack,
             const WritableRseBackingStoreView& rseBackingStore) noexcept;

    /// Performs the inseparable prepare-and-commit sequence. On success the
    /// old memory stack and RSE context are abandoned. The new context calls
    /// the fixed AsteriaIa64RunOwnedExecutionContext symbol and fail-stops if
    /// that continuation ever returns.
    [[nodiscard]] static InitialExecutionContextSwitchFailure
    PrepareAndSwitch(const InitialAddressSpacePreparation& preparation,
                     const WritableInitialMemoryStackView& memoryStack,
                     const WritableRseBackingStoreView& rseBackingStore) noexcept;

private:
    InitialExecutionContextSwitcher() = delete;
};

static_assert(InitialMemoryStackSizeBytes == 32U * 1024U);
static_assert(InitialMemoryStackScratchSizeBytes == 16U);
static_assert(sizeof(WritableInitialMemoryStackView) == 24U);
static_assert(alignof(WritableInitialMemoryStackView) == 8U);
static_assert(__is_aggregate(WritableInitialMemoryStackView));
static_assert(__is_standard_layout(WritableInitialMemoryStackView));
static_assert(__is_trivially_copyable(WritableInitialMemoryStackView));
static_assert(__builtin_offsetof(WritableInitialMemoryStackView, Data) == 0U);
static_assert(__builtin_offsetof(WritableInitialMemoryStackView, SizeBytes) == 8U);
static_assert(__builtin_offsetof(WritableInitialMemoryStackView, PhysicalStart) == 16U);
static_assert(sizeof(InitialMemoryStackPreparationError) == 4U);
static_assert(sizeof(InitialExecutionContextSwitchError) == 4U);
static_assert(sizeof(InitialExecutionContextValidationSummary) == 32U);
static_assert(alignof(InitialExecutionContextValidationSummary) == 8U);
static_assert(__is_aggregate(InitialExecutionContextValidationSummary));
static_assert(__is_standard_layout(InitialExecutionContextValidationSummary));
static_assert(__is_trivially_copyable(InitialExecutionContextValidationSummary));
static_assert(__builtin_offsetof(InitialExecutionContextValidationSummary, StackPhysicalStart) == 0U);
static_assert(__builtin_offsetof(InitialExecutionContextValidationSummary, InitialStackPointer) == 8U);
static_assert(__builtin_offsetof(InitialExecutionContextValidationSummary, RseBackingStorePhysicalStart) == 16U);
static_assert(__builtin_offsetof(InitialExecutionContextValidationSummary, StackSizeBytes) == 24U);
static_assert(__builtin_offsetof(InitialExecutionContextValidationSummary, RseBackingStoreSizeBytes) == 28U);
static_assert(sizeof(InitialExecutionContextSwitchFailure) == 16U);
static_assert(alignof(InitialExecutionContextSwitchFailure) == 4U);
static_assert(__is_aggregate(InitialExecutionContextSwitchFailure));
static_assert(__is_standard_layout(InitialExecutionContextSwitchFailure));
static_assert(__is_trivially_copyable(InitialExecutionContextSwitchFailure));
static_assert(__builtin_offsetof(InitialExecutionContextSwitchFailure, Error) == 0U);
static_assert(__builtin_offsetof(InitialExecutionContextSwitchFailure, MemoryStackError) == 4U);
static_assert(__builtin_offsetof(InitialExecutionContextSwitchFailure, RseBackingStoreError) == 8U);
static_assert(__builtin_offsetof(InitialExecutionContextSwitchFailure, Reserved) == 12U);
static_assert(
    sizeof(Foundation::Result<InitialExecutionContextValidationSummary, InitialExecutionContextSwitchFailure>) == 56U);
static_assert(
    alignof(Foundation::Result<InitialExecutionContextValidationSummary, InitialExecutionContextSwitchFailure>) == 8U);
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_EXECUTION_CONTEXT_SWITCHER_HPP_INCLUDED
