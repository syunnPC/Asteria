// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_POLLED_TLB_SHOOTDOWN_NOTIFIER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_POLLED_TLB_SHOOTDOWN_NOTIFIER_HPP_INCLUDED

#include "Hal/Memory/ITlbShootdownNotifier.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Platform::Ia64::Memory
{
enum class Ia64PolledTlbShootdownNotifierState : Foundation::UInt8
{
    Uninitialized,
    Ready,
};

enum class Ia64PolledTlbShootdownNotifierInitializationError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    InvalidProcessorMask,
};

enum class Ia64PolledTlbShootdownPollResult : Foundation::UInt8
{
    Pending,
    Empty,
    NotReady,
    InvalidProcessor,
};

struct Ia64PolledTlbShootdownNotifierSnapshot
{
    Foundation::ProcessorMask ProfiledProcessorMask;
    Foundation::ProcessorMask PendingProcessorMask;
    Ia64PolledTlbShootdownNotifierState State;
    Foundation::UInt8 Reserved;
};

/// Coalesces remote TLB-shootdown notifications for cooperative CPU polling.
///
/// Initialize binds a fixed nonempty subset of the four profiled CPUs before
/// the object is published. Notify performs no interrupt or callback: it
/// release-latches one pending word for each selected CPU. Each profiled CPU
/// must call Poll at a bounded cadence from its non-migrating, non-reentrant
/// service loop and invoke its processor-local drain after Pending is returned.
///
/// Poll clears the latch before the drain begins. A notification concurrent
/// with a drain therefore either participates in that drain or remains latched
/// for a later poll. Repeated notifications may coalesce, and the mailbox's
/// bounded drain is authoritative for the complete set of published work.
/// The object allocates nothing and all atomic operations are always lock-free.
class Ia64PolledTlbShootdownNotifier final : public Hal::Memory::ITlbShootdownNotifier
{
public:
    constexpr Ia64PolledTlbShootdownNotifier() noexcept = default;

    /// Preconditions:
    /// - Initialization does not overlap any other operation on this object.
    /// - The object is published to other CPUs only after success.
    [[nodiscard]] Ia64PolledTlbShootdownNotifierInitializationError
    Initialize(Foundation::ProcessorMask profiledProcessorMask) noexcept;

    /// Release-latches each selected profiled CPU without sending an IPI.
    ///
    /// Preconditions:
    /// - State is Ready.
    /// - targetProcessorMask is nonzero and a subset of the profiled mask.
    /// - Complete mailbox work was release-published before this call.
    void Notify(Foundation::ProcessorMask targetProcessorMask) noexcept override;

    /// Cooperatively claims one coalesced notification for processor.
    ///
    /// Pending means the caller must run its bounded processor-local drain
    /// before polling again. Empty has no side effect beyond the atomic poll.
    /// NotReady and InvalidProcessor leave every pending word unchanged.
    [[nodiscard]] Ia64PolledTlbShootdownPollResult Poll(Kernel::Scheduler::CpuId processor) noexcept;

    /// Returns an acquire-observed diagnostic view. PendingProcessorMask is a
    /// per-slot observation and is not an atomic multi-CPU snapshot.
    [[nodiscard]] Ia64PolledTlbShootdownNotifierSnapshot GetSnapshot() const noexcept;

    ~Ia64PolledTlbShootdownNotifier() noexcept = default;

    Ia64PolledTlbShootdownNotifier(const Ia64PolledTlbShootdownNotifier&) = delete;
    Ia64PolledTlbShootdownNotifier& operator=(const Ia64PolledTlbShootdownNotifier&) = delete;
    Ia64PolledTlbShootdownNotifier(Ia64PolledTlbShootdownNotifier&&) = delete;
    Ia64PolledTlbShootdownNotifier& operator=(Ia64PolledTlbShootdownNotifier&&) = delete;

private:
    Synchronization::AtomicUInt64 m_pending[Kernel::Scheduler::MaximumProcessorCount]{};
    Foundation::ProcessorMask m_profiledProcessorMask{0U};
    Ia64PolledTlbShootdownNotifierState m_state{Ia64PolledTlbShootdownNotifierState::Uninitialized};
    [[maybe_unused]] Foundation::UInt8 m_reserved[6]{};
};

static_assert(sizeof(Ia64PolledTlbShootdownNotifierState) == 1U);
static_assert(sizeof(Ia64PolledTlbShootdownNotifierInitializationError) == 1U);
static_assert(sizeof(Ia64PolledTlbShootdownPollResult) == 1U);
static_assert(sizeof(Ia64PolledTlbShootdownNotifierSnapshot) == 24U);
static_assert(alignof(Ia64PolledTlbShootdownNotifierSnapshot) == 8U);
static_assert(sizeof(Ia64PolledTlbShootdownNotifier) == 56U);
static_assert(alignof(Ia64PolledTlbShootdownNotifier) == 8U);
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_POLLED_TLB_SHOOTDOWN_NOTIFIER_HPP_INCLUDED
