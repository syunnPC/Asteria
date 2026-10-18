// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/TlbShootdownDispatcher.hpp"

namespace Asteria::Kernel::Memory
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::ProcessorMask ValidProcessorMask{
    static_cast<Foundation::ProcessorMask>((Foundation::ProcessorMask{1U} << Scheduler::MaximumProcessorCount) - 1U)};

using DispatchResult = Foundation::Result<TlbShootdownReceipt, TlbShootdownFailure>;

[[nodiscard]] constexpr bool RangeIsValid(const VirtualAddress start, const PageCount pages) noexcept
{
    const Foundation::UInt64 pageCount = pages.GetValue();
    if (((start.GetValue() % MappingPageSizeBytes) != 0U) || (pageCount == 0U) ||
        (pageCount > (MaximumUInt64 / MappingPageSizeBytes)))
    {
        return false;
    }
    const Foundation::UInt64 bytes = pageCount * MappingPageSizeBytes;
    return start.GetValue() <= (MaximumUInt64 - bytes);
}

[[nodiscard]] constexpr TlbShootdownFailure MakeFailure(const TlbShootdownError error,
                                                        const Hal::Memory::LocalTlbInvalidationError localError,
                                                        const Hal::Memory::TlbShootdownTransportError transportError,
                                                        const AddressSpaceError addressSpaceError,
                                                        const Scheduler::CpuId processor) noexcept
{
    return TlbShootdownFailure{
        .Error = error,
        .LocalError = localError,
        .TransportError = transportError,
        .AddressSpaceErrorCode = addressSpaceError,
        .Processor = processor,
        .Reserved = {},
    };
}

[[nodiscard]] DispatchResult FailFromThreadContext(AddressSpace& addressSpace, const AddressSpaceMappingToken token,
                                                   const TlbShootdownError error,
                                                   const Hal::Memory::LocalTlbInvalidationError localError,
                                                   const Hal::Memory::TlbShootdownTransportError transportError,
                                                   const Scheduler::CpuId processor) noexcept
{
    const AddressSpaceError transitionError = addressSpace.FailShootdown(token);
    return DispatchResult::Failure(MakeFailure(error, localError, transportError, transitionError, processor));
}

[[nodiscard]] DispatchResult RejectFromInterruptContext(const TlbShootdownWork& work, const Scheduler::CpuId processor,
                                                        const TlbShootdownError error,
                                                        const Hal::Memory::LocalTlbInvalidationError localError,
                                                        const AddressSpaceError reportedError) noexcept
{
    AddressSpaceError rejectionError = reportedError;
    AddressSpace* const owner = work.GetOwner();
    const AddressSpaceMappingToken token = work.GetToken();
    const Scheduler::CpuId targetProcessor = work.GetTargetProcessor();
    if ((owner != nullptr) && token.IsValid() && targetProcessor.IsValid() && token.IsFor(*owner))
    {
        rejectionError = owner->RejectShootdown(token, targetProcessor);
    }
    return DispatchResult::Failure(
        MakeFailure(error, localError, Hal::Memory::TlbShootdownTransportError::None, rejectionError, processor));
}
[[nodiscard, gnu::noinline]] bool ValidateDispatch(const AddressSpace& addressSpace,
                                                   const AddressSpaceMappingToken token, const VirtualAddress start,
                                                   const PageCount pages,
                                                   Foundation::ProcessorMask& targetMask) noexcept
{
    const AddressSpaceSnapshot snapshot = addressSpace.GetSnapshot();
    targetMask = snapshot.TargetProcessorMask;
    return !(!token.IsValid() || !token.IsFor(addressSpace) ||
             (token.GetSequence() > AddressSpaceMappingSequencePolicy::Maximum) || !RangeIsValid(start, pages) ||
             ((targetMask & static_cast<Foundation::ProcessorMask>(~ValidProcessorMask)) != 0U) ||
             (snapshot.State != AddressSpaceState::ShootdownPending) ||
             (snapshot.ShootdownSequence != token.GetSequence()) || (snapshot.TargetProcessorMask != targetMask));
}
} // namespace

Foundation::Result<TlbShootdownReceipt, TlbShootdownFailure>
TlbShootdownDispatcher::Dispatch(AddressSpace& addressSpace, const AddressSpaceShootdownRequest& request,
                                 Hal::Memory::ITlbShootdownTransport& transport) noexcept
{
    const Scheduler::CpuId currentProcessor = GetProcessor();
    const AddressSpaceMappingToken token = request.GetToken();
    const VirtualAddress start = request.GetInvalidationStart();
    const PageCount pages = request.GetInvalidationPages();
    Foundation::ProcessorMask targetMask{};
    if (!currentProcessor.IsValid() || !ValidateDispatch(addressSpace, token, start, pages, targetMask))
    {
        return FailFromThreadContext(addressSpace, token, TlbShootdownError::InvalidRequest,
                                     Hal::Memory::LocalTlbInvalidationError::None,
                                     Hal::Memory::TlbShootdownTransportError::None, currentProcessor);
    }

    Foundation::ProcessorMask acknowledgedMask{0U};
    const Foundation::ProcessorMask currentProcessorBit =
        static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << currentProcessor.GetValue());
    if ((targetMask & currentProcessorBit) != 0U)
    {
        const Hal::Memory::LocalTlbInvalidationError localError = m_localInvalidator->Invalidate(start, pages);
        if (localError != Hal::Memory::LocalTlbInvalidationError::None)
        {
            return FailFromThreadContext(addressSpace, token, TlbShootdownError::LocalInvalidationRejected, localError,
                                         Hal::Memory::TlbShootdownTransportError::None, currentProcessor);
        }
        const AddressSpaceError acknowledgementError = addressSpace.AcknowledgeShootdown(token, currentProcessor);
        if (acknowledgementError != AddressSpaceError::None)
        {
            return FailFromThreadContext(addressSpace, token, TlbShootdownError::AcknowledgementRejected,
                                         Hal::Memory::LocalTlbInvalidationError::None,
                                         Hal::Memory::TlbShootdownTransportError::None, currentProcessor);
        }
        acknowledgedMask = currentProcessorBit;
    }

    TlbShootdownBatch batch{};
    Foundation::ProcessorMask queuedMask{0U};
    for (Foundation::UInt8 processorValue{0U}; processorValue < Scheduler::MaximumProcessorCount; ++processorValue)
    {
        const Foundation::ProcessorMask processorBit =
            static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << processorValue);
        if (((targetMask & processorBit) == 0U) || (processorValue == currentProcessor.GetValue()))
        {
            continue;
        }
        batch.m_work[batch.m_count] =
            TlbShootdownWork{addressSpace, token, start, pages, Scheduler::CpuId{processorValue}};
        ++batch.m_count;
        queuedMask = static_cast<Foundation::ProcessorMask>(queuedMask | processorBit);
    }

    if (batch.GetCount() != 0U)
    {
        const Hal::Memory::TlbShootdownTransportError transportError = transport.Publish(batch);
        if (transportError != Hal::Memory::TlbShootdownTransportError::None)
        {
            return FailFromThreadContext(addressSpace, token, TlbShootdownError::TransportRejected,
                                         Hal::Memory::LocalTlbInvalidationError::None, transportError,
                                         currentProcessor);
        }
    }

    return DispatchResult::Success(TlbShootdownReceipt{
        .Token = token,
        .QueuedProcessorMask = queuedMask,
        .AcknowledgedProcessorMask = acknowledgedMask,
        .Processor = currentProcessor,
        .RejectionObserved = false,
        .Reserved = {},
    });
}

Foundation::Result<TlbShootdownReceipt, TlbShootdownFailure>
TlbShootdownDispatcher::HandleRemote(const TlbShootdownWork& work) noexcept
{
    const Scheduler::CpuId currentProcessor = GetProcessor();
    AddressSpace* const owner = work.GetOwner();
    const AddressSpaceMappingToken token = work.GetToken();
    const VirtualAddress start = work.GetInvalidationStart();
    const PageCount pages = work.GetInvalidationPages();
    const Scheduler::CpuId targetProcessor = work.GetTargetProcessor();
    if ((owner == nullptr) || !currentProcessor.IsValid() || !targetProcessor.IsValid() ||
        (currentProcessor.GetValue() != targetProcessor.GetValue()) || !token.IsValid() ||
        ((owner != nullptr) && !token.IsFor(*owner)) || !RangeIsValid(start, pages))
    {
        return RejectFromInterruptContext(work, currentProcessor, TlbShootdownError::InvalidRequest,
                                          Hal::Memory::LocalTlbInvalidationError::None,
                                          AddressSpaceError::InvalidShootdownToken);
    }

    const Hal::Memory::LocalTlbInvalidationError localError = m_localInvalidator->Invalidate(start, pages);
    if (localError != Hal::Memory::LocalTlbInvalidationError::None)
    {
        return RejectFromInterruptContext(work, currentProcessor, TlbShootdownError::LocalInvalidationRejected,
                                          localError, AddressSpaceError::None);
    }

    const AddressSpaceError acknowledgementError = owner->AcknowledgeShootdown(token, currentProcessor);
    if ((acknowledgementError != AddressSpaceError::None) &&
        (acknowledgementError != AddressSpaceError::ShootdownFailed))
    {
        return RejectFromInterruptContext(work, currentProcessor, TlbShootdownError::AcknowledgementRejected,
                                          Hal::Memory::LocalTlbInvalidationError::None, acknowledgementError);
    }

    const Foundation::ProcessorMask processorBit =
        static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << currentProcessor.GetValue());
    return DispatchResult::Success(TlbShootdownReceipt{
        .Token = token,
        .QueuedProcessorMask = 0U,
        .AcknowledgedProcessorMask = processorBit,
        .Processor = currentProcessor,
        .RejectionObserved = acknowledgementError == AddressSpaceError::ShootdownFailed,
        .Reserved = {},
    });
}
} // namespace Asteria::Kernel::Memory
