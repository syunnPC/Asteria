// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Object/CapabilityTransfer.hpp"

#include "Kernel/Object/CapabilitySpace.hpp"

namespace Asteria::Kernel::Object
{
namespace
{
[[nodiscard]] constexpr bool IsKnownDisposition(const CapabilityTransferDisposition disposition) noexcept
{
    switch (disposition)
    {
    case CapabilityTransferDisposition::Move:
    case CapabilityTransferDisposition::Copy:
        return true;

    case CapabilityTransferDisposition::Invalid:
        return false;
    }
    return false;
}
} // namespace

Foundation::Result<CapabilityTransferBatch, CapabilityError>
CapabilitySpace::TransferBatchTo(CapabilitySpace& destination, const CapabilityTransferRequestView requests) noexcept
{
    using ResultType = Foundation::Result<CapabilityTransferBatch, CapabilityError>;

    if (requests.Count == 0U)
    {
        return ResultType::Success(CapabilityTransferBatch{});
    }
    if (!SharesTransferDomain(destination))
    {
        return ResultType::Failure(CapabilityError::InvalidConfiguration);
    }

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((requests.Count > CapabilityTransferBatch::MaximumTransfers) || (requests.Data == nullptr))
    {
        return ResultType::Failure(CapabilityError::InvalidArgument);
    }
    const Foundation::Result<MonotonicTime, CapabilityError> now = SampleTransferTimeLocked(destination);
    if (!now.HasValue())
    {
        return ResultType::Failure(now.GetError());
    }
    return TransferBatchToLocked(destination, requests, now.GetValue(), MovedSourceCommit::Release);
}

bool CapabilitySpace::SharesTransferDomain(const CapabilitySpace& destination) const noexcept
{
    return m_isConfigurationValid && destination.m_isConfigurationValid &&
           (&m_objectRegistry == &destination.m_objectRegistry) &&
           (&m_revocationRegistry == &destination.m_revocationRegistry) && (&m_clock == &destination.m_clock) &&
           (&m_synchronization == &destination.m_synchronization);
}

Foundation::Result<MonotonicTime, CapabilityError>
CapabilitySpace::SampleTransferTimeLocked(CapabilitySpace& destination) noexcept
{
    const auto processor = ClockProcessor();
    const MonotonicTime now{m_clock.GetTicks()};
    if (now.IsInfinite() || (now.GetTicks() < m_lastObservedTime[processor].GetTicks()) ||
        (now.GetTicks() < destination.m_lastObservedTime[processor].GetTicks()))
    {
        return Foundation::Result<MonotonicTime, CapabilityError>::Failure(CapabilityError::InvalidConfiguration);
    }
    m_lastObservedTime[processor] = now;
    destination.m_lastObservedTime[processor] = now;
    return Foundation::Result<MonotonicTime, CapabilityError>::Success(now);
}

Foundation::Result<CapabilityTransferBatch, CapabilityError>
CapabilitySpace::TransferBatchToLocked(CapabilitySpace& destination, const CapabilityTransferRequestView requests,
                                       const MonotonicTime now, const MovedSourceCommit movedSourceCommit) noexcept
{
    using ResultType = Foundation::Result<CapabilityTransferBatch, CapabilityError>;

    if (requests.Count == 0U)
    {
        return ResultType::Success(CapabilityTransferBatch{});
    }
    if (!SharesTransferDomain(destination))
    {
        return ResultType::Failure(CapabilityError::InvalidConfiguration);
    }
    if ((requests.Count > CapabilityTransferBatch::MaximumTransfers) || (requests.Data == nullptr))
    {
        return ResultType::Failure(CapabilityError::InvalidArgument);
    }

    CapabilityTransferPlan plan{};
    const CapabilityError preparationError =
        PrepareTransferPlanLocked(destination, requests, now, movedSourceCommit, plan);
    if (preparationError != CapabilityError::Ok)
    {
        return ResultType::Failure(preparationError);
    }
    return CommitTransferPlanLocked(destination, plan, requests.Count, movedSourceCommit);
}

CapabilityError CapabilitySpace::PrepareTransferPlanLocked(CapabilitySpace& destination,
                                                           const CapabilityTransferRequestView requests,
                                                           const MonotonicTime now,
                                                           const MovedSourceCommit movedSourceCommit,
                                                           CapabilityTransferPlan& plan) noexcept
{
    for (Foundation::Size index{0U}; index < requests.Count; ++index)
    {
        const CapabilityTransferRequest& request = requests.Data[index];
        if ((request.Reserved != 0U) || !request.RequestedRights.IsValid() ||
            (request.RequestedRights.GetRawValue() == 0U) || !IsKnownObjectType(request.ExpectedType) ||
            !IsKnownDisposition(request.Disposition))
        {
            return CapabilityError::InvalidArgument;
        }

        const CapabilityRights requiredRights{request.Disposition == CapabilityTransferDisposition::Move
                                                  ? CapabilityRight::Transfer
                                                  : CapabilityRight::Duplicate};
        const Foundation::Result<CapabilityDescriptor, CapabilityError> descriptorResult =
            GetUsableDescriptorLocked(request.Source, requiredRights, now);
        if (!descriptorResult.HasValue())
        {
            return descriptorResult.GetError();
        }

        const CapabilityDescriptor sourceDescriptor = descriptorResult.GetValue();
        if (sourceDescriptor.Type != request.ExpectedType)
        {
            return CapabilityError::TypeMismatch;
        }
        // A copy stays in its source's revocation domain, so Revoke on it
        // would let the receiver revoke the sender's own handle.
        if (!request.RequestedRights.IsSubsetOf(sourceDescriptor.Rights) ||
            ((request.Disposition == CapabilityTransferDisposition::Copy) &&
             request.RequestedRights.ContainsAll(CapabilityRights{CapabilityRight::Revoke})))
        {
            return CapabilityError::RightsAmplification;
        }
        if (IsExpired(request.RequestedDeadline, now))
        {
            return CapabilityError::Expired;
        }
        if (!sourceDescriptor.LeaseDeadline.IsInfinite() &&
            (request.RequestedDeadline.IsInfinite() ||
             (request.RequestedDeadline.GetTicks() > sourceDescriptor.LeaseDeadline.GetTicks())))
        {
            return CapabilityError::LeaseExtension;
        }

        const Foundation::Result<Foundation::Size, CapabilityError> sourceIndexResult =
            ResolveHandleLocked(request.Source);
        if (!sourceIndexResult.HasValue())
        {
            return CapabilityError::InvalidHandle;
        }
        for (Foundation::Size previous{0U}; previous < index; ++previous)
        {
            if (plan.SourceIndices[previous] == sourceIndexResult.GetValue())
            {
                return CapabilityError::InvalidArgument;
            }
        }

        plan.Descriptors[index] = sourceDescriptor;
        plan.Descriptors[index].Rights = request.RequestedRights;
        plan.Descriptors[index].LeaseDeadline = request.RequestedDeadline;
        plan.SourceIndices[index] = sourceIndexResult.GetValue();
        plan.RevocationEpochs[index] = m_entries[sourceIndexResult.GetValue()].RevocationEpoch;
        plan.Dispositions[index] = request.Disposition;
    }

    Foundation::Size selectedCount{0U};
    for (Foundation::Size slot{0U}; (slot < MaximumSlots) && (selectedCount < requests.Count); ++slot)
    {
        const SlotEntry& destinationEntry = destination.m_entries[slot];
        bool available = !destinationEntry.IsOccupied && !destinationEntry.IsRetired;
        // A released same-space move may reuse its own source slot; a held
        // source keeps its slot so that a cancellation can restore it.
        if ((this == &destination) && !available && (movedSourceCommit == MovedSourceCommit::Release))
        {
            for (Foundation::Size source{0U}; source < requests.Count; ++source)
            {
                if ((plan.Dispositions[source] == CapabilityTransferDisposition::Move) &&
                    (plan.SourceIndices[source] == slot) &&
                    CapabilityGenerationPolicy::CanAdvance(m_entries[slot].Generation))
                {
                    available = true;
                    break;
                }
            }
        }
        if (!available)
        {
            continue;
        }

        plan.DestinationIndices[selectedCount] = slot;
        ++selectedCount;
    }
    if (selectedCount != requests.Count)
    {
        return CapabilityError::SpaceExhausted;
    }

    return CapabilityError::Ok;
}

Foundation::Result<CapabilityTransferBatch, CapabilityError>
CapabilitySpace::CommitTransferPlanLocked(CapabilitySpace& destination, const CapabilityTransferPlan& plan,
                                          const Foundation::Size count,
                                          const MovedSourceCommit movedSourceCommit) noexcept
{
    if (count > CapabilityTransferBatch::MaximumTransfers)
    {
        return Foundation::Result<CapabilityTransferBatch, CapabilityError>::Failure(CapabilityError::InvalidArgument);
    }

    CapabilityTransferBatch transferred{};

    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (plan.Dispositions[index] != CapabilityTransferDisposition::Move)
        {
            continue;
        }

        if (movedSourceCommit == MovedSourceCommit::Hold)
        {
            m_entries[plan.SourceIndices[index]].IsHeld = true;
            continue;
        }
        CloseIndexLocked(plan.SourceIndices[index]);
    }

    for (Foundation::Size index{0U}; index < count; ++index)
    {
        SlotEntry& destinationEntry = destination.m_entries[plan.DestinationIndices[index]];
        if (destinationEntry.Generation == 0U)
        {
            destinationEntry.Generation = 1U;
        }
        destinationEntry.IsOccupied = true;
        destinationEntry.RevocationEpoch = plan.RevocationEpochs[index];
        destinationEntry.Descriptor = plan.Descriptors[index];
        ++destination.m_activeCount;
        transferred.DestinationHandles[index] =
            destination.EncodeHandleLocked(plan.DestinationIndices[index], destinationEntry.Generation);
    }
    transferred.Count = static_cast<Foundation::UInt8>(count);

    return Foundation::Result<CapabilityTransferBatch, CapabilityError>::Success(transferred);
}
} // namespace Asteria::Kernel::Object
