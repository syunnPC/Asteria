// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/TlbShootdownDrain.hpp"

namespace Asteria::Kernel::Memory
{
TlbShootdownDrainSummary TlbShootdownDrain::Drain() noexcept
{
    const Scheduler::CpuId processor = m_dispatcher->GetProcessor();
    TlbShootdownDrainSummary summary{
        .ReceivedCount = 0U,
        .CompletedCount = 0U,
        .RejectedCount = 0U,
        .ReceiveError = TlbShootdownReceiveError::None,
        .FirstRemoteError = TlbShootdownError::None,
        .FirstLocalError = Hal::Memory::LocalTlbInvalidationError::None,
        .FirstAddressSpaceError = AddressSpaceError::None,
        .Processor = processor,
        .Reserved = {},
    };

    for (Foundation::UInt8 workIndex{0U}; workIndex < TlbShootdownMailboxSlotCount; ++workIndex)
    {
        TlbShootdownWork work{};
        const TlbShootdownReceiveError receiveError = m_mailbox->TryReceive(processor, work);
        if (receiveError == TlbShootdownReceiveError::Empty)
        {
            break;
        }
        if (receiveError != TlbShootdownReceiveError::None)
        {
            summary.ReceiveError = receiveError;
            break;
        }

        ++summary.ReceivedCount;
        const auto result = m_dispatcher->HandleRemote(work);
        if (result.HasValue())
        {
            ++summary.CompletedCount;
            continue;
        }

        ++summary.RejectedCount;
        if (summary.FirstRemoteError == TlbShootdownError::None)
        {
            const TlbShootdownFailure& failure = result.GetError();
            summary.FirstRemoteError = failure.Error;
            summary.FirstLocalError = failure.LocalError;
            summary.FirstAddressSpaceError = failure.AddressSpaceErrorCode;
        }
    }
    return summary;
}
} // namespace Asteria::Kernel::Memory
