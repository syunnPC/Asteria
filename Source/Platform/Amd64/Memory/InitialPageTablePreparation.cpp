// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Memory/InitialPageTablePreparation.hpp"

namespace Asteria::Platform::Amd64::Memory
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 PageSizeBytes{4096U};

[[nodiscard]] constexpr InitialPageTablePreparationFailure
MakeFailure(const InitialPageTablePreparationError error,
            const InitialPageTableBuildError buildError = InitialPageTableBuildError::None,
            const Kernel::Memory::InitialMappingPlanError mappingError = Kernel::Memory::InitialMappingPlanError::None,
            const Foundation::Size mappingIndex = 0U) noexcept
{
    return InitialPageTablePreparationFailure{
        .Error = error,
        .BuildError = buildError,
        .MappingError = mappingError,
        .Reserved = 0U,
        .MappingIndex = mappingIndex,
    };
}

[[nodiscard]] bool TryGetStorageRange(const PageTableStorage& storage, Foundation::UInt64& start,
                                      Foundation::UInt64& end) noexcept
{
    if ((storage.Entries == nullptr) || (storage.PageCapacity == 0U) ||
        (storage.PageCapacity > MaximumInitialPageTablePages))
    {
        return false;
    }
    start = static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(storage.Entries));
    const Foundation::UInt64 bytes = static_cast<Foundation::UInt64>(storage.PageCapacity) << 12U;
    if (((start & (PageSizeBytes - 1U)) != 0U) || (start > (MaximumUInt64 - bytes)))
    {
        return false;
    }
    end = start + bytes;
    return true;
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

template <typename TObject>
[[nodiscard]] bool StorageOverlapsObject(const Foundation::UInt64 storageStart, const Foundation::UInt64 storageEnd,
                                         const TObject& object) noexcept
{
    const Foundation::UInt64 objectStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&object));
    return RangesOverlap(storageStart, storageEnd, objectStart, objectStart + sizeof(object));
}
} // namespace

Foundation::Result<InitialPageTablePreparationStatus, InitialPageTablePreparationError>
InitialPageTablePreparation::Prepare(const Kernel::Memory::InitialMappingPlan& mappings,
                                     const PagingCapabilities& capabilities, const PageTableStorage& storage) noexcept
{
    using ResultType = Foundation::Result<InitialPageTablePreparationStatus, InitialPageTablePreparationError>;

    const State state = LoadState();
    if ((state == State::Prepared) || (state == State::RuntimeClaimed))
    {
        return ResultType::Failure(InitialPageTablePreparationError::AlreadyPrepared);
    }
    if (state == State::Failed)
    {
        return ResultType::Failure(InitialPageTablePreparationError::PreviousPreparationFailed);
    }

    Foundation::UInt64 storageStart{0U};
    Foundation::UInt64 storageEnd{0U};
    if (TryGetStorageRange(storage, storageStart, storageEnd) &&
        (StorageOverlapsObject(storageStart, storageEnd, *this) ||
         StorageOverlapsObject(storageStart, storageEnd, mappings) ||
         StorageOverlapsObject(storageStart, storageEnd, capabilities) ||
         StorageOverlapsObject(storageStart, storageEnd, storage)))
    {
        return ResultType::Failure(InitialPageTablePreparationError::StorageAliasesPreparation);
    }

    m_state.StoreRelease(static_cast<Foundation::UInt64>(State::Failed));
    if (!mappings.IsSealed())
    {
        m_failure = MakeFailure(InitialPageTablePreparationError::PageTableBuildRejected,
                                InitialPageTableBuildError::PlanNotSealed);
        return ResultType::Failure(m_failure.Error);
    }

    Foundation::UInt64 expectedMappedPages{0U};
    const Foundation::Size mappingCount = mappings.GetMappingCount();
    for (Foundation::Size index{0U}; index < mappingCount; ++index)
    {
        const auto mappingResult = mappings.GetMapping(index);
        if (!mappingResult.HasValue())
        {
            m_failure = MakeFailure(InitialPageTablePreparationError::MappingSnapshotRejected,
                                    InitialPageTableBuildError::None, mappingResult.GetError(), index);
            return ResultType::Failure(m_failure.Error);
        }
        const Kernel::Memory::InitialMapping mapping = mappingResult.GetValue();
        const Kernel::Memory::InitialMappingPlanError addError = m_mappingPlan.AddMapping(mapping);
        if (addError != Kernel::Memory::InitialMappingPlanError::None)
        {
            m_failure = MakeFailure(InitialPageTablePreparationError::MappingSnapshotRejected,
                                    InitialPageTableBuildError::None, addError, index);
            return ResultType::Failure(m_failure.Error);
        }
        expectedMappedPages += mapping.Pages.GetValue();
    }
    const Kernel::Memory::InitialMappingPlanError sealError = m_mappingPlan.Seal();
    if (sealError != Kernel::Memory::InitialMappingPlanError::None)
    {
        m_failure = MakeFailure(InitialPageTablePreparationError::MappingSnapshotRejected,
                                InitialPageTableBuildError::None, sealError, mappingCount);
        return ResultType::Failure(m_failure.Error);
    }

    InitialPageTableBuildResult built{};
    const InitialPageTableBuildError buildError =
        InitialPageTableBuilder::Build(m_mappingPlan, capabilities, storage, built);
    if (buildError != InitialPageTableBuildError::None)
    {
        m_failure = MakeFailure(InitialPageTablePreparationError::PageTableBuildRejected, buildError);
        return ResultType::Failure(m_failure.Error);
    }
    if (!(m_mappingPlan.IsSealed()) || (mappingCount == 0U) ||
        (mappingCount > Kernel::Memory::MaximumInitialMappings) ||
        (built.RootPhysicalAddress.GetValue() != storage.PhysicalStart.GetValue()) || (built.UsedTablePages == 0U) ||
        (built.UsedTablePages > storage.PageCapacity) || (built.MappedPages != expectedMappedPages) ||
        (built.Reserved != 0U))
    {
        m_failure = MakeFailure(InitialPageTablePreparationError::InternalInvariantViolation);
        return ResultType::Failure(m_failure.Error);
    }

    m_summary = InitialPageTablePreparationSummary{
        .Storage =
            PreparedPageTableStorage{
                .RootPhysicalAddress = built.RootPhysicalAddress,
                .Entries = storage.Entries,
                .PageCapacity = storage.PageCapacity,
                .UsedTablePages = built.UsedTablePages,
            },
        .MappedPages = built.MappedPages,
        .MappingCount = static_cast<Foundation::UInt16>(mappingCount),
        .Reserved16 = 0U,
        .Reserved32 = 0U,
    };
    m_state.StoreRelease(static_cast<Foundation::UInt64>(State::Prepared));
    return ResultType::Success(InitialPageTablePreparationStatus::Prepared);
}

bool InitialPageTablePreparation::IsPrepared() const noexcept
{
    const State state = LoadState();
    return (state == State::Prepared) || (state == State::RuntimeClaimed);
}

bool InitialPageTablePreparation::HasFailed() const noexcept
{
    return LoadState() == State::Failed;
}

Foundation::Result<const InitialPageTablePreparationSummary*, InitialPageTablePreparationError>
InitialPageTablePreparation::TryGetSummary() const noexcept
{
    using ResultType = Foundation::Result<const InitialPageTablePreparationSummary*, InitialPageTablePreparationError>;
    const State state = LoadState();
    if (state != State::Prepared)
    {
        return ResultType::Failure(state == State::Failed ? InitialPageTablePreparationError::PreviousPreparationFailed
                                                          : InitialPageTablePreparationError::NotPrepared);
    }
    return ResultType::Success(&m_summary);
}

Foundation::Result<const Kernel::Memory::InitialMappingPlan*, InitialPageTablePreparationError>
InitialPageTablePreparation::TryGetMappingPlan() const noexcept
{
    using ResultType = Foundation::Result<const Kernel::Memory::InitialMappingPlan*, InitialPageTablePreparationError>;
    const State state = LoadState();
    if ((state != State::Prepared) && (state != State::RuntimeClaimed))
    {
        return ResultType::Failure(state == State::Failed ? InitialPageTablePreparationError::PreviousPreparationFailed
                                                          : InitialPageTablePreparationError::NotPrepared);
    }
    return ResultType::Success(&m_mappingPlan);
}

const InitialPageTablePreparationFailure& InitialPageTablePreparation::GetFailure() const noexcept
{
    return m_failure;
}

InitialPageTablePreparationError InitialPageTablePreparation::ClaimRuntimeOwnership() noexcept
{
    Foundation::UInt64 expected = static_cast<Foundation::UInt64>(State::Prepared);
    if (m_state.CompareExchangeStrongAcquireRelease(expected, static_cast<Foundation::UInt64>(State::RuntimeClaimed)))
    {
        return InitialPageTablePreparationError::None;
    }
    const State observed = static_cast<State>(expected);
    if (observed == State::RuntimeClaimed)
    {
        return InitialPageTablePreparationError::AlreadyPrepared;
    }
    return (observed == State::Failed) ? InitialPageTablePreparationError::PreviousPreparationFailed
                                       : InitialPageTablePreparationError::NotPrepared;
}

InitialPageTablePreparation::State InitialPageTablePreparation::LoadState() const noexcept
{
    return static_cast<State>(m_state.LoadAcquire());
}
} // namespace Asteria::Platform::Amd64::Memory
