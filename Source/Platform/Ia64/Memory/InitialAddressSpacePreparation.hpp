// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_ADDRESS_SPACE_PREPARATION_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_ADDRESS_SPACE_PREPARATION_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Ia64/Memory/InitialInterruptVectorTablePreparer.hpp"
#include "Platform/Ia64/Memory/InitialRseBackingStorePreparer.hpp"
#include "Platform/Ia64/Memory/InitialTranslationResourcePlan.hpp"

namespace Asteria::Platform::Ia64::Memory
{
enum class InitialAddressSpacePreparationStatus : Foundation::UInt32
{
    None,
    Prepared,
};

enum class InitialAddressSpacePreparationError : Foundation::UInt32
{
    None,
    AlreadyPrepared,
    PreviousPreparationFailed,
    MappingPlanRejected,
    TranslationResourcePlanRejected,
    InternalInvariantViolation,
    NotPrepared,
    InterruptVectorTablePreparationRejected,
    InterruptVectorTablePreflightRejected,
    RseBackingStorePreparationRejected,
    RseBackingStorePreflightRejected,
};

struct InitialAddressSpacePreparationSummary
{
    BootstrapMappingPlanSummary Mapping;
    InitialTranslationResourcePlanSummary Translation;
};

struct InitialAddressSpacePreparationFailure
{
    InitialAddressSpacePreparationError Error;
    InitialInterruptVectorTablePreparationError InterruptVectorTableError;
    InitialRseBackingStorePreparationError RseBackingStoreError;
    BootstrapMappingPlanFailure MappingFailure;
    InitialTranslationResourcePlanFailure TranslationFailure;
};

/// Owns the two immutable plans required before the IA-64 translation state is
/// installed. Preparation is single-use and must run on the non-preemptible BSP.
/// A failed transaction remains terminal; a successful instance publishes the
/// plans only after every child postcondition, the fail-stop IVT publication,
/// and the RSE storage preparation have completed. Either storage destination
/// aliasing this owner is rejected before a transaction begins and leaves the
/// owner fresh so that the rejection itself cannot modify the destination.
class InitialAddressSpacePreparation final
{
public:
    constexpr InitialAddressSpacePreparation() noexcept = default;

    InitialAddressSpacePreparation(const InitialAddressSpacePreparation&) = delete;
    InitialAddressSpacePreparation& operator=(const InitialAddressSpacePreparation&) = delete;
    InitialAddressSpacePreparation(InitialAddressSpacePreparation&&) = delete;
    InitialAddressSpacePreparation& operator=(InitialAddressSpacePreparation&&) = delete;

    [[nodiscard, gnu::always_inline]] inline Foundation::Result<InitialAddressSpacePreparationStatus,
                                                                InitialAddressSpacePreparationError>
    Prepare(const Firmware::Ia64LoadedImageLayout& image, const Boot::BootInfo& finalBootInfo,
            const Kernel::Memory::PhysicalPageRange& reservedPages,
            const Kernel::Memory::PhysicalPageRange& runtimeMemoryPages,
            const PalVirtualMemoryCapabilities& capabilities, const Firmware::SalSystemTableInformation& firmware,
            const InitialTranslationRegionSet& regions, const WritableInterruptVectorTableView& interruptVectorTable,
            const WritableRseBackingStoreView& rseBackingStore) noexcept
    {
        using ResultType =
            Foundation::Result<InitialAddressSpacePreparationStatus, InitialAddressSpacePreparationError>;
        InitialAddressSpacePreparation* const self = PreserveRuntimeAddress(this);

        if (self->m_state == State::Prepared)
        {
            return ResultType::Failure(InitialAddressSpacePreparationError::AlreadyPrepared);
        }
        if (self->m_state == State::Failed)
        {
            return ResultType::Failure(InitialAddressSpacePreparationError::PreviousPreparationFailed);
        }

        const InitialInterruptVectorTablePreparationError interruptVectorTablePreflightError =
            InitialInterruptVectorTablePreparer::ValidatePreparationStorage(
                *self, image, finalBootInfo, reservedPages, runtimeMemoryPages, capabilities, firmware, regions,
                interruptVectorTable, rseBackingStore);
        if (interruptVectorTablePreflightError ==
            InitialInterruptVectorTablePreparationError::StorageAliasesPreparation)
        {
            return ResultType::Failure(InitialAddressSpacePreparationError::InterruptVectorTablePreflightRejected);
        }

        const InitialRseBackingStorePreparationError rsePreflightError =
            InitialRseBackingStorePreparer::ValidatePreparationStorage(*self, image, finalBootInfo, reservedPages,
                                                                       runtimeMemoryPages, capabilities, firmware,
                                                                       regions, rseBackingStore);
        if (rsePreflightError == InitialRseBackingStorePreparationError::StorageAliasesPreparation)
        {
            return ResultType::Failure(InitialAddressSpacePreparationError::RseBackingStorePreflightRejected);
        }

        self->m_state = State::Failed;
        const auto mappingResult = BootstrapMappingPlanBuilder::Build(image, finalBootInfo, reservedPages,
                                                                      runtimeMemoryPages, self->m_mappingPlan);
        if (!mappingResult.HasValue())
        {
            self->m_failure = InitialAddressSpacePreparationFailure{
                .Error = InitialAddressSpacePreparationError::MappingPlanRejected,
                .InterruptVectorTableError = InitialInterruptVectorTablePreparationError::None,
                .RseBackingStoreError = InitialRseBackingStorePreparationError::None,
                .MappingFailure = mappingResult.GetError(),
                .TranslationFailure = {},
            };
            return ResultType::Failure(self->m_failure.Error);
        }
        self->m_summary.Mapping = mappingResult.GetValue();

        const auto translationResult = InitialTranslationResourcePlanner::Build(
            self->m_mappingPlan, capabilities, firmware, regions, self->m_translationResourcePlan);
        if (!translationResult.HasValue())
        {
            self->m_failure = InitialAddressSpacePreparationFailure{
                .Error = InitialAddressSpacePreparationError::TranslationResourcePlanRejected,
                .InterruptVectorTableError = InitialInterruptVectorTablePreparationError::None,
                .RseBackingStoreError = InitialRseBackingStorePreparationError::None,
                .MappingFailure = {},
                .TranslationFailure = translationResult.GetError(),
            };
            return ResultType::Failure(self->m_failure.Error);
        }
        self->m_summary.Translation = translationResult.GetValue();

        if (!self->m_mappingPlan.IsSealed() ||
            (self->m_summary.Mapping.StackPhysicalStart != reservedPages.PhysicalStart) ||
            (self->m_summary.Mapping.TranslationStoragePhysicalStart !=
             (self->m_summary.Mapping.StackPhysicalStart +
              (BootstrapTranslationStoragePageOffset * Kernel::Memory::MappingPageSizeBytes))) ||
            (self->m_mappingPlan.GetMappingCount() != self->m_summary.Mapping.MappingCount) ||
            !IsBootstrapMappingCountValid(self->m_summary.Mapping.MappingCount) ||
            (self->m_summary.Mapping.Reserved16 != 0U) || (self->m_summary.Mapping.Reserved32 != 0U) ||
            !self->m_translationResourcePlan.IsBuilt() ||
            (self->m_translationResourcePlan.GetEntryCount() != self->m_summary.Translation.EntryCount) ||
            (self->m_translationResourcePlan.GetDataRegisterCount() != self->m_summary.Translation.DataRegisterCount) ||
            (self->m_translationResourcePlan.GetInstructionRegisterCount() !=
             self->m_summary.Translation.InstructionRegisterCount) ||
            (self->m_summary.Translation.Reserved != 0U))
        {
            self->m_failure = InitialAddressSpacePreparationFailure{
                .Error = InitialAddressSpacePreparationError::InternalInvariantViolation,
                .InterruptVectorTableError = InitialInterruptVectorTablePreparationError::None,
                .RseBackingStoreError = InitialRseBackingStorePreparationError::None,
                .MappingFailure = {},
                .TranslationFailure = {},
            };
            return ResultType::Failure(self->m_failure.Error);
        }

        const InitialInterruptVectorTablePreparationError interruptVectorTablePreparationError =
            interruptVectorTablePreflightError != InitialInterruptVectorTablePreparationError::None
                ? interruptVectorTablePreflightError
                : InitialInterruptVectorTablePreparer::Validate(self->m_mappingPlan, self->m_summary.Mapping,
                                                                interruptVectorTable);
        if (interruptVectorTablePreparationError != InitialInterruptVectorTablePreparationError::None)
        {
            self->m_failure = InitialAddressSpacePreparationFailure{
                .Error = InitialAddressSpacePreparationError::InterruptVectorTablePreparationRejected,
                .InterruptVectorTableError = interruptVectorTablePreparationError,
                .RseBackingStoreError = InitialRseBackingStorePreparationError::None,
                .MappingFailure = {},
                .TranslationFailure = {},
            };
            return ResultType::Failure(self->m_failure.Error);
        }

        const InitialRseBackingStorePreparationError rsePreparationError =
            rsePreflightError != InitialRseBackingStorePreparationError::None
                ? rsePreflightError
                : InitialRseBackingStorePreparer::Validate(self->m_mappingPlan, self->m_summary.Mapping,
                                                           rseBackingStore);
        if (rsePreparationError != InitialRseBackingStorePreparationError::None)
        {
            self->m_failure = InitialAddressSpacePreparationFailure{
                .Error = InitialAddressSpacePreparationError::RseBackingStorePreparationRejected,
                .InterruptVectorTableError = InitialInterruptVectorTablePreparationError::None,
                .RseBackingStoreError = rsePreparationError,
                .MappingFailure = {},
                .TranslationFailure = {},
            };
            return ResultType::Failure(self->m_failure.Error);
        }

        const InitialInterruptVectorTablePreparationError interruptVectorTableCommitError =
            InitialInterruptVectorTablePreparer::Prepare(self->m_mappingPlan, self->m_summary.Mapping,
                                                         interruptVectorTable);
        if (interruptVectorTableCommitError != InitialInterruptVectorTablePreparationError::None)
        {
            self->m_failure = InitialAddressSpacePreparationFailure{
                .Error = InitialAddressSpacePreparationError::InterruptVectorTablePreparationRejected,
                .InterruptVectorTableError = interruptVectorTableCommitError,
                .RseBackingStoreError = InitialRseBackingStorePreparationError::None,
                .MappingFailure = {},
                .TranslationFailure = {},
            };
            return ResultType::Failure(self->m_failure.Error);
        }

        const InitialRseBackingStorePreparationError rseCommitError =
            InitialRseBackingStorePreparer::Prepare(self->m_mappingPlan, self->m_summary.Mapping, rseBackingStore);
        if (rseCommitError != InitialRseBackingStorePreparationError::None)
        {
            self->m_failure = InitialAddressSpacePreparationFailure{
                .Error = InitialAddressSpacePreparationError::RseBackingStorePreparationRejected,
                .InterruptVectorTableError = InitialInterruptVectorTablePreparationError::None,
                .RseBackingStoreError = rseCommitError,
                .MappingFailure = {},
                .TranslationFailure = {},
            };
            return ResultType::Failure(self->m_failure.Error);
        }

        self->m_state = State::Prepared;
        return ResultType::Success(InitialAddressSpacePreparationStatus::Prepared);
    }

    [[nodiscard, gnu::always_inline]] inline bool IsPrepared() const noexcept
    {
        return PreserveRuntimeAddress(this)->m_state == State::Prepared;
    }

    [[nodiscard, gnu::always_inline]] inline bool HasFailed() const noexcept
    {
        return PreserveRuntimeAddress(this)->m_state == State::Failed;
    }

    /// Returns the summary only after successful atomic publication.
    [[nodiscard, gnu::always_inline]] inline Foundation::Result<const InitialAddressSpacePreparationSummary*,
                                                                InitialAddressSpacePreparationError>
    TryGetSummary() const noexcept
    {
        using ResultType =
            Foundation::Result<const InitialAddressSpacePreparationSummary*, InitialAddressSpacePreparationError>;
        const InitialAddressSpacePreparation* const self = PreserveRuntimeAddress(this);
        if (self->m_state != State::Prepared)
        {
            return ResultType::Failure(self->m_state == State::Failed
                                           ? InitialAddressSpacePreparationError::PreviousPreparationFailed
                                           : InitialAddressSpacePreparationError::NotPrepared);
        }
        return ResultType::Success(&self->m_summary);
    }

    /// Returns the sealed mapping plan only after successful publication.
    [[nodiscard, gnu::always_inline]] inline Foundation::Result<const Kernel::Memory::InitialMappingPlan*,
                                                                InitialAddressSpacePreparationError>
    TryGetMappingPlan() const noexcept
    {
        using ResultType =
            Foundation::Result<const Kernel::Memory::InitialMappingPlan*, InitialAddressSpacePreparationError>;
        const InitialAddressSpacePreparation* const self = PreserveRuntimeAddress(this);
        if (self->m_state != State::Prepared)
        {
            return ResultType::Failure(self->m_state == State::Failed
                                           ? InitialAddressSpacePreparationError::PreviousPreparationFailed
                                           : InitialAddressSpacePreparationError::NotPrepared);
        }
        return ResultType::Success(&self->m_mappingPlan);
    }

    /// Returns the resource plan only after successful atomic publication.
    [[nodiscard, gnu::always_inline]] inline Foundation::Result<const InitialTranslationResourcePlan*,
                                                                InitialAddressSpacePreparationError>
    TryGetTranslationResourcePlan() const noexcept
    {
        using ResultType =
            Foundation::Result<const InitialTranslationResourcePlan*, InitialAddressSpacePreparationError>;
        const InitialAddressSpacePreparation* const self = PreserveRuntimeAddress(this);
        if (self->m_state != State::Prepared)
        {
            return ResultType::Failure(self->m_state == State::Failed
                                           ? InitialAddressSpacePreparationError::PreviousPreparationFailed
                                           : InitialAddressSpacePreparationError::NotPrepared);
        }
        return ResultType::Success(&self->m_translationResourcePlan);
    }

    /// Returns the first detailed failure retained by Prepare.
    /// Preconditions:
    /// - HasFailed is true.
    [[nodiscard, gnu::always_inline]] inline const InitialAddressSpacePreparationFailure& GetFailure() const noexcept
    {
        return PreserveRuntimeAddress(this)->m_failure;
    }

private:
    enum class State : Foundation::UInt32
    {
        Fresh,
        Failed,
        Prepared,
    };

    /// Keeps member offsets as runtime arithmetic on IA-64 instead of emitting
    /// relocated read-only pointer literals. This is not a memory-ordering fence.
    [[nodiscard, gnu::always_inline]] static inline InitialAddressSpacePreparation*
    PreserveRuntimeAddress(InitialAddressSpacePreparation* value) noexcept
    {
#if defined(__ia64__)
        __asm__ volatile("" : "+r"(value));
#endif
        return value;
    }

    [[nodiscard, gnu::always_inline]] static inline const InitialAddressSpacePreparation*
    PreserveRuntimeAddress(const InitialAddressSpacePreparation* value) noexcept
    {
#if defined(__ia64__)
        __asm__ volatile("" : "+r"(value));
#endif
        return value;
    }

    Kernel::Memory::InitialMappingPlan m_mappingPlan{};
    InitialTranslationResourcePlan m_translationResourcePlan{};
    InitialAddressSpacePreparationSummary m_summary{};
    InitialAddressSpacePreparationFailure m_failure{};
    State m_state{State::Fresh};
    [[maybe_unused]] Foundation::UInt32 m_reserved{0U};
};

static_assert(sizeof(InitialAddressSpacePreparationStatus) == 4U);
static_assert(sizeof(InitialAddressSpacePreparationError) == 4U);
static_assert(sizeof(InitialAddressSpacePreparationSummary) == 40U);
static_assert(alignof(InitialAddressSpacePreparationSummary) == 8U);
static_assert(__is_aggregate(InitialAddressSpacePreparationSummary));
static_assert(__is_standard_layout(InitialAddressSpacePreparationSummary));
static_assert(__is_trivially_copyable(InitialAddressSpacePreparationSummary));
static_assert(__builtin_offsetof(InitialAddressSpacePreparationSummary, Mapping) == 0U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationSummary, Translation) == 32U);
static_assert(alignof(InitialAddressSpacePreparationFailure) == 8U);
static_assert(__is_aggregate(InitialAddressSpacePreparationFailure));
static_assert(__is_standard_layout(InitialAddressSpacePreparationFailure));
static_assert(__is_trivially_copyable(InitialAddressSpacePreparationFailure));
static_assert(__builtin_offsetof(InitialAddressSpacePreparationFailure, Error) == 0U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationFailure, InterruptVectorTableError) == 4U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationFailure, RseBackingStoreError) == 8U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationFailure, MappingFailure) == 16U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationFailure, TranslationFailure) == 32U);
static_assert(sizeof(InitialAddressSpacePreparationFailure) == 56U);
static_assert(sizeof(InitialAddressSpacePreparation) == 20'696U);
static_assert(alignof(InitialAddressSpacePreparation) == 8U);
static_assert(sizeof(Foundation::Result<InitialAddressSpacePreparationStatus, InitialAddressSpacePreparationError>) ==
              12U);
static_assert(alignof(Foundation::Result<InitialAddressSpacePreparationStatus, InitialAddressSpacePreparationError>) ==
              4U);
static_assert(sizeof(Foundation::Result<const InitialTranslationResourcePlan*, InitialAddressSpacePreparationError>) ==
              16U);
static_assert(alignof(Foundation::Result<const InitialTranslationResourcePlan*, InitialAddressSpacePreparationError>) ==
              8U);
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_ADDRESS_SPACE_PREPARATION_HPP_INCLUDED
