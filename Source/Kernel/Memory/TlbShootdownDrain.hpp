// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_TLB_SHOOTDOWN_DRAIN_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_TLB_SHOOTDOWN_DRAIN_HPP_INCLUDED

#include "Kernel/Memory/TlbShootdownMailboxTransport.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"

namespace Asteria::Kernel::Memory
{
struct TlbShootdownDrainSummary
{
    Foundation::UInt8 ReceivedCount;
    Foundation::UInt8 CompletedCount;
    Foundation::UInt8 RejectedCount;
    TlbShootdownReceiveError ReceiveError;
    TlbShootdownError FirstRemoteError;
    Hal::Memory::LocalTlbInvalidationError FirstLocalError;
    AddressSpaceError FirstAddressSpaceError;
    Scheduler::CpuId Processor;
    Foundation::UInt8 Reserved[8];
};

/// Binds one processor's mailbox consumer to its local shootdown dispatcher. Drain is called only
/// on the bound processor with migration and re-entry disabled. It processes at most one complete
/// mailbox-capacity pass and never allocates, blocks, takes a lock, or invokes an unknown callback.
/// A concurrent publication after a slot was scanned is covered by that publisher's later
/// notification and a subsequent Drain call.
class ITlbShootdownDrain
{
public:
    [[nodiscard]] virtual TlbShootdownDrainSummary Drain() noexcept = 0;

    [[nodiscard]] virtual Scheduler::CpuId GetProcessor() const noexcept = 0;

protected:
    constexpr ITlbShootdownDrain() noexcept = default;
    ~ITlbShootdownDrain() noexcept = default;
};

/// Concrete fixed-mailbox drain bound to one processor-local dispatcher.
class TlbShootdownDrain final : public ITlbShootdownDrain
{
public:
    explicit constexpr TlbShootdownDrain(TlbShootdownMailboxTransport& mailbox,
                                         TlbShootdownDispatcher& dispatcher) noexcept
        : m_mailbox{&mailbox}, m_dispatcher{&dispatcher}
    {
    }

    ~TlbShootdownDrain() noexcept = default;

    TlbShootdownDrain(const TlbShootdownDrain&) = delete;
    TlbShootdownDrain& operator=(const TlbShootdownDrain&) = delete;
    TlbShootdownDrain(TlbShootdownDrain&&) = delete;
    TlbShootdownDrain& operator=(TlbShootdownDrain&&) = delete;

    [[nodiscard]] TlbShootdownDrainSummary Drain() noexcept override;

    [[nodiscard]] constexpr Scheduler::CpuId GetProcessor() const noexcept override
    {
        return m_dispatcher->GetProcessor();
    }

private:
    TlbShootdownMailboxTransport* m_mailbox;
    TlbShootdownDispatcher* m_dispatcher;
};

// Platform progress binds the initiator exactly like a target. A plain function keeps IA-64
// function descriptors explicit.
[[gnu::noinline, gnu::noipa]] inline void DrainProcessorShootdowns(void* const context) noexcept
{
    if (context == nullptr)
    {
        Platform::Synchronization::StopProcessorWait(Kernel::Diagnostics::StopCode::InvariantViolated, 0U);
    }
    const auto summary = static_cast<ITlbShootdownDrain*>(context)->Drain();
    if (summary.RejectedCount != 0U)
    {
        Platform::Synchronization::StopProcessorWait(Kernel::Diagnostics::StopCode::InvariantViolated,
                                                     summary.RejectedCount);
    }
}

static_assert(sizeof(TlbShootdownDrainSummary) == 16U);
static_assert(alignof(TlbShootdownDrainSummary) == 1U);
static_assert(sizeof(TlbShootdownDrain) == 24U);
static_assert(alignof(TlbShootdownDrain) == 8U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_TLB_SHOOTDOWN_DRAIN_HPP_INCLUDED
