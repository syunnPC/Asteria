// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_TLB_SHOOTDOWN_MAILBOX_TRANSPORT_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_TLB_SHOOTDOWN_MAILBOX_TRANSPORT_HPP_INCLUDED

#include "Hal/Memory/ITlbShootdownNotifier.hpp"
#include "Hal/Memory/ITlbShootdownTransport.hpp"
#include "Kernel/Memory/TlbShootdownDispatcher.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Kernel/Scheduler/ProcessorSet.hpp"

namespace Asteria::Kernel::Memory
{
constexpr Foundation::UInt8 TlbShootdownMailboxSlotCount{8U};

enum class TlbShootdownReceiveError : Foundation::UInt8
{
    None,
    Empty,
    InvalidProcessor,
    CorruptState,
};

/// A fixed-capacity, allocation-free transport for complete shootdown batches. Each slot is
/// published with one release store after its complete batch has been copied. A processor-local
/// consumer acquire-loads that state, copies only its work item, then atomically clears its own
/// target bit. The slot is reusable only after every target bit is clear. This gives concurrent
/// publishers one all-or-none commit word without a shared blocking lock. Publish may run
/// concurrently on different processors. TryReceive is called by at most one non-reentrant drain
/// path for each processor; different processors may drain concurrently. A successful TryReceive
/// transfers the work's lifetime obligation from the slot to that CPU's drain path: clearing the
/// last slot bit does not complete or cancel the work. The drain path calls TlbShootdownDispatcher:
/// HandleRemote before discarding the copy. The notifier and this object outlive all calls,
/// published slots, and received copies. Shutdown first stops new publishers and asynchronous
/// notification, synchronously drains every still-published target through HandleRemote, joins
/// every CPU-local drain handler, and verifies all slots empty. Only then may it reclaim referenced
/// AddressSpace transactions. Slot emptiness without the handler join is not sufficient.
class TlbShootdownMailboxTransport final : public Hal::Memory::ITlbShootdownTransport
{
public:
    explicit constexpr TlbShootdownMailboxTransport(Hal::Memory::ITlbShootdownNotifier& notifier,
                                                    const Foundation::ProcessorMask onlineProcessorMask) noexcept
        : m_notifier{&notifier}, m_onlineProcessorMask{onlineProcessorMask}
    {
    }

    ~TlbShootdownMailboxTransport() noexcept = default;

    TlbShootdownMailboxTransport(const TlbShootdownMailboxTransport&) = delete;
    TlbShootdownMailboxTransport& operator=(const TlbShootdownMailboxTransport&) = delete;
    TlbShootdownMailboxTransport(TlbShootdownMailboxTransport&&) = delete;
    TlbShootdownMailboxTransport& operator=(TlbShootdownMailboxTransport&&) = delete;

    [[nodiscard]] Hal::Memory::TlbShootdownTransportError Publish(const TlbShootdownBatch& batch) noexcept override;

    /// Copies one published work item for processor and retires that CPU's
    /// slot claim. Output remains unchanged on failure. On success, the caller
    /// owns the copied work until HandleRemote returns and must keep its owner
    /// transaction alive throughout that call.
    [[nodiscard]] TlbShootdownReceiveError TryReceive(Scheduler::CpuId processor, TlbShootdownWork& output) noexcept;

    /// Advisory acquire scan, without consuming work. False may become stale on publication:
    /// the next local progress pass must drain regardless of
    /// notification state. Corruption returns true so the normal receive path reports it rather
    /// than skipping it.
    [[nodiscard]] bool HasPendingWork(Scheduler::CpuId processor) const noexcept;

    /// Admitted online mask has one writer, the processor set. Bootstrap publishes this binding to
    /// existing consumers.
    void BindProcessorSet(const Scheduler::ProcessorSet& processors) noexcept
    {
        __atomic_store_n(&m_processors, &processors, __ATOMIC_RELEASE);
    }

    [[nodiscard]] Foundation::ProcessorMask GetOnlineProcessorMask() const noexcept
    {
        const auto* processors = __atomic_load_n(&m_processors, __ATOMIC_ACQUIRE);
        return processors != nullptr ? processors->OnlineMask() : m_onlineProcessorMask;
    }

private:
    struct Slot final
    {
        TlbShootdownBatch Batch{};
        Platform::Synchronization::AtomicUInt64 State{};
    };

    Slot m_slots[TlbShootdownMailboxSlotCount]{};
    Hal::Memory::ITlbShootdownNotifier* m_notifier;
    Foundation::UInt8 m_nextSlot[Scheduler::MaximumProcessorCount]{};
    Foundation::ProcessorMask m_onlineProcessorMask;
    const Scheduler::ProcessorSet* m_processors{nullptr};
    [[maybe_unused]] Foundation::UInt8 m_reserved[3]{};
};

static_assert(sizeof(TlbShootdownReceiveError) == 1U);
static_assert((TlbShootdownMailboxSlotCount & (TlbShootdownMailboxSlotCount - 1U)) == 0U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_TLB_SHOOTDOWN_MAILBOX_TRANSPORT_HPP_INCLUDED
