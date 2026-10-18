// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_BLOCK_REQUESTS_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_BLOCK_REQUESTS_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Component/BlockFrame.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// The requests an AHCI driver holds, as a pure state machine over fixed arrays
/// (Source/User/System/AhciPort.hpp to 7): no system calls, no MMIO, no heap. The driver does the
/// effects (mapping a request's Frame, programming a command slot, sending replies, closing
/// capabilities) with the copies this table hands back.
inline constexpr Foundation::Size BlockRequestCapacity{8U};

/// Where a request is: Queued (admitted, waiting for a command slot) or Issued
/// (its command runs on Slot). A Free entry holds nothing.
enum class BlockRequestState : Foundation::UInt8
{
    Free = 0U,
    Queued = 1U,
    Issued = 2U,
};

/// One request. Sender is the sender identity of its delivery's header
/// (offset 24), the key of the client's credit; Method is BlockReadMethod or
/// BlockWriteMethod; Reply and Data are the decoded frame's handles in the
/// driver's space, which the driver closes when it answers; Slot is meaningful
/// while Issued; Arrival orders the Queued requests (a wrapping counter);
/// MediumGeneration is checked again before DMA, so queued work cannot
/// cross a medium change. Flags is the request's flags word (zero or BlockCrashAfterIssue).
struct BlockRequest
{
    Foundation::UInt64 Sender;
    Foundation::UInt64 Transaction;
    Foundation::UInt64 Lba;
    Foundation::UInt64 MediumGeneration;
    System::CapabilityHandle Reply;
    System::CapabilityHandle Data;
    Foundation::UInt32 Arrival;
    Foundation::UInt16 Sectors;
    Foundation::UInt8 Method;
    BlockRequestState State;
    Foundation::UInt8 Slot;
    Foundation::UInt8 Flags;
};

/// What Admit did. Refused answers the request BlockRefused with
/// BlockMaximumOutstanding as detail and keeps nothing; Malformed
/// is a frame that is no Read or Write ReadBlockFrame accepts, which the
/// driver drops as it drops a frame it cannot decode.
enum class BlockAdmitOutcome : Foundation::UInt8
{
    Queued,
    Refused,
    Malformed,
};

struct BlockAdmitResult
{
    BlockAdmitOutcome Outcome;
    /// The table index of a Queued request, else zero.
    Foundation::Size Index;
};

/// What Complete found: the request that held the slot, or nothing issued
/// there (a spurious or repeated completion, which the driver counts).
enum class BlockCompleteOutcome : Foundation::UInt8
{
    Completed,
    NothingIssued,
};

/// What Cancel did: Cancelled when the request was Queued and
/// left the table (the driver answers it BlockCancelled, and the Cancel
/// BlockCancelled with the request's transaction as detail); TooLate when it
/// was issued (it completes normally), or answered already, never known or
/// another sender's (the driver answers the Cancel BlockTooLate).
enum class BlockCancelOutcome : Foundation::UInt8
{
    Cancelled,
    TooLate,
};

/// BlockRequestCapacity requests and Slots command slots (numbered 0 to
/// Slots - 1, at most 32). Every operation keeps: a sender holds at most
/// BlockMaximumOutstanding Queued or Issued requests, never two with one
/// transaction; a slot holds at most one request; Queued requests issue in
/// arrival order across senders. The object is compact (under 1 KiB) so the
/// driver keeps it static; no function here has a large local.
template <Foundation::Size Slots> class BlockRequestTable
{
    static_assert((Slots >= 1U) && (Slots <= 32U), "a table has 1 to 32 command slots");

public:
    constexpr BlockRequestTable() noexcept = default;

    /// A table whose first arrival number is firstArrival (tests start it
    /// near the counter's wrap).
    constexpr explicit BlockRequestTable(const Foundation::UInt32 firstArrival) noexcept : m_nextArrival{firstArrival}
    {
    }

    /// Forgets every request: the driver clears its static table as an
    /// instance starts, so it never holds another instance's requests.
    void Clear() noexcept
    {
        for (BlockRequest& entry : m_requests)
        {
            entry.State = BlockRequestState::Free;
        }
    }

    /// Queues a Read or Write that arrived from sender, unless the sender
    /// already holds its credit, the table is full, or the sender holds a
    /// request with this transaction: then Refused and nothing is kept.
    [[nodiscard]] BlockAdmitResult Admit(const Foundation::UInt64 sender, const BlockMessage& message) noexcept
    {
        if (((message.Method != BlockReadMethod) && (message.Method != BlockWriteMethod)) ||
            (message.Words[2U] == 0U) || (message.Words[2U] > BlockMaximumSectors) ||
            ((message.Words[3U] & ~BlockCrashAfterIssue) != 0U) || (message.Words[4U] == 0U))
        {
            return BlockAdmitResult{.Outcome = BlockAdmitOutcome::Malformed, .Index = 0U};
        }
        Foundation::Size held{0U};
        Foundation::Size spare{BlockRequestCapacity};
        bool duplicate{false};
        for (Foundation::Size index{0U}; index < BlockRequestCapacity; ++index)
        {
            const BlockRequest& entry = m_requests[index];
            if (entry.State == BlockRequestState::Free)
            {
                spare = index;
            }
            else if (entry.Sender == sender)
            {
                ++held;
                duplicate = duplicate || (entry.Transaction == message.Transaction);
            }
        }
        if (duplicate || (held >= BlockMaximumOutstanding) || (spare == BlockRequestCapacity))
        {
            return BlockAdmitResult{.Outcome = BlockAdmitOutcome::Refused, .Index = 0U};
        }
        m_requests[spare] = BlockRequest{.Sender = sender,
                                         .Transaction = message.Transaction,
                                         .Lba = message.Words[1U],
                                         .MediumGeneration = message.Words[4U],
                                         .Reply = message.Reply,
                                         .Data = message.Data,
                                         .Arrival = m_nextArrival,
                                         .Sectors = static_cast<Foundation::UInt16>(message.Words[2U]),
                                         .Method = static_cast<Foundation::UInt8>(message.Method),
                                         .State = BlockRequestState::Queued,
                                         .Slot = 0U,
                                         .Flags = static_cast<Foundation::UInt8>(message.Words[3U])};
        ++m_nextArrival;
        return BlockAdmitResult{.Outcome = BlockAdmitOutcome::Queued, .Index = spare};
    }

    /// When a command slot is free and a request is Queued, issues the oldest
    /// on the lowest free slot: it becomes Issued, issued is its copy (Slot
    /// set) and the result is true. False otherwise, changing nothing.
    [[nodiscard]] bool NextToIssue(BlockRequest& issued) noexcept
    {
        Foundation::UInt32 used{0U};
        Foundation::Size oldest{BlockRequestCapacity};
        for (Foundation::Size index{0U}; index < BlockRequestCapacity; ++index)
        {
            const BlockRequest& entry = m_requests[index];
            if (entry.State == BlockRequestState::Issued)
            {
                used |= Foundation::UInt32{1U} << entry.Slot;
            }
            else if ((entry.State == BlockRequestState::Queued) &&
                     ((oldest == BlockRequestCapacity) || Precedes(entry.Arrival, m_requests[oldest].Arrival)))
            {
                oldest = index;
            }
        }
        if (oldest == BlockRequestCapacity)
        {
            return false;
        }
        for (Foundation::UInt8 slot{0U}; slot < Slots; ++slot)
        {
            if ((used & (Foundation::UInt32{1U} << slot)) == 0U)
            {
                m_requests[oldest].State = BlockRequestState::Issued;
                m_requests[oldest].Slot = slot;
                issued = m_requests[oldest];
                return true;
            }
        }
        return false;
    }

    /// The command on slot finished: its request leaves the table and
    /// completed is its copy, for the driver to unmap its Frame, send the
    /// final reply and close its capabilities. NothingIssued, changing
    /// nothing, when no request is issued on slot (or slot is out of range).
    [[nodiscard]] BlockCompleteOutcome Complete(const Foundation::Size slot, BlockRequest& completed) noexcept
    {
        for (Foundation::Size index{0U}; (slot < Slots) && (index < BlockRequestCapacity); ++index)
        {
            if ((m_requests[index].State == BlockRequestState::Issued) && (m_requests[index].Slot == slot))
            {
                completed = m_requests[index];
                m_requests[index].State = BlockRequestState::Free;
                return BlockCompleteOutcome::Completed;
            }
        }
        return BlockCompleteOutcome::NothingIssued;
    }

    /// A Cancel from sender for transaction (it takes no credit). A Queued
    /// request of that sender and transaction leaves the table: Cancelled,
    /// and cancelled is its copy. Anything else, an Issued request included,
    /// is left alone: TooLate.
    [[nodiscard]] BlockCancelOutcome Cancel(const Foundation::UInt64 sender, const Foundation::UInt64 transaction,
                                            BlockRequest& cancelled) noexcept
    {
        for (Foundation::Size index{0U}; index < BlockRequestCapacity; ++index)
        {
            if ((m_requests[index].State == BlockRequestState::Queued) && (m_requests[index].Sender == sender) &&
                (m_requests[index].Transaction == transaction))
            {
                cancelled = m_requests[index];
                m_requests[index].State = BlockRequestState::Free;
                return BlockCancelOutcome::Cancelled;
            }
        }
        return BlockCancelOutcome::TooLate;
    }

    /// The Queued and Issued requests of sender.
    [[nodiscard]] Foundation::Size CountOutstanding(const Foundation::UInt64 sender) const noexcept
    {
        Foundation::Size count{0U};
        for (const BlockRequest& entry : m_requests)
        {
            count += ((entry.State != BlockRequestState::Free) && (entry.Sender == sender)) ? 1U : 0U;
        }
        return count;
    }

    [[nodiscard]] Foundation::Size CountState(const BlockRequestState state) const noexcept
    {
        Foundation::Size count{0U};
        for (const BlockRequest& entry : m_requests)
        {
            count += (entry.State == state) ? 1U : 0U;
        }
        return count;
    }

    /// No request is held.
    [[nodiscard]] bool IsIdle() const noexcept
    {
        return CountState(BlockRequestState::Free) == BlockRequestCapacity;
    }

    /// Whether the invariants hold: credit, unique transactions per sender,
    /// issued slots in range and unshared, Queued arrivals distinct.
    [[nodiscard]] bool IsConsistent() const noexcept
    {
        for (Foundation::Size first{0U}; first < BlockRequestCapacity; ++first)
        {
            const BlockRequest& left = m_requests[first];
            if (left.State == BlockRequestState::Free)
            {
                continue;
            }
            if ((CountOutstanding(left.Sender) > BlockMaximumOutstanding) ||
                ((left.State == BlockRequestState::Issued) && (left.Slot >= Slots)))
            {
                return false;
            }
            for (Foundation::Size second{first + 1U}; second < BlockRequestCapacity; ++second)
            {
                const BlockRequest& right = m_requests[second];
                if (right.State == BlockRequestState::Free)
                {
                    continue;
                }
                if (((left.Sender == right.Sender) && (left.Transaction == right.Transaction)) ||
                    ((left.State == BlockRequestState::Issued) && (right.State == BlockRequestState::Issued) &&
                     (left.Slot == right.Slot)) ||
                    (left.Arrival == right.Arrival))
                {
                    return false;
                }
            }
        }
        return true;
    }

private:
    /// First arrived before second; correct across the counter's wrap while fewer than 2^31
    /// arrivals separate them (at most eight are held).
    [[nodiscard]] static constexpr bool Precedes(const Foundation::UInt32 first,
                                                 const Foundation::UInt32 second) noexcept
    {
        return static_cast<Foundation::UInt32>(first - second) >= 0x8000'0000U;
    }

    BlockRequest m_requests[BlockRequestCapacity]{};
    Foundation::UInt32 m_nextArrival{0U};
};
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_BLOCK_REQUESTS_HPP_INCLUDED
