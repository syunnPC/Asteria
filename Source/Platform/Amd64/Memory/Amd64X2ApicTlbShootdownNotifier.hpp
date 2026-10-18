// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_MEMORY_AMD64_X2_APIC_TLB_SHOOTDOWN_NOTIFIER_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_MEMORY_AMD64_X2_APIC_TLB_SHOOTDOWN_NOTIFIER_HPP_INCLUDED

#include "Hal/Memory/ITlbShootdownNotifier.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Amd64/Interrupt/Amd64LocalApic.hpp"

namespace Asteria::Platform::Amd64::Memory
{
enum class Amd64X2ApicTlbShootdownNotifierState : Foundation::UInt8
{
    Uninitialized,
    Ready,
};

enum class Amd64X2ApicTlbShootdownNotifierInitializationError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    InvalidVector,
    InvalidProcessorMask,
    InvalidX2ApicId,
    LocalApicUnavailable,
};

struct Amd64X2ApicTlbShootdownNotifierSnapshot
{
    Foundation::UInt32 X2ApicIds[Kernel::Scheduler::MaximumProcessorCount];
    Foundation::ProcessorMask OnlineProcessorMask;
    Foundation::UInt8 Vector;
    Amd64X2ApicTlbShootdownNotifierState State;
    Foundation::UInt8 Reserved;
};

/// Sends fixed, edge-triggered physical-destination IPIs through x2APIC. Initialization binds
/// kernel CpuId values to distinct x2APIC IDs before the object is published. Every processor that
/// may call Notify has already entered x2APIC mode and invokes it with migration and same-CPU
/// re-entry disabled. Calls on different processors may overlap because each writes only its own
/// local interrupt-command MSR. The native adapter orders all preceding mailbox stores before
/// issuing the interrupt command.
class Amd64X2ApicTlbShootdownNotifier final : public Hal::Memory::ITlbShootdownNotifier
{
public:
    constexpr Amd64X2ApicTlbShootdownNotifier() noexcept = default;

    /// Preconditions:
    /// - x2ApicIds comes from trusted SMP topology discovery, and each online
    /// processor has verified its own identifier before publication.
    /// - vector is exclusively reserved for the shootdown interrupt path on
    /// every online processor.
    [[nodiscard]] Amd64X2ApicTlbShootdownNotifierInitializationError
    Initialize(Foundation::UInt8 vector, Foundation::ProcessorMask onlineProcessorMask,
               const Foundation::UInt32 (&x2ApicIds)[Kernel::Scheduler::MaximumProcessorCount]) noexcept;

    /// Preconditions:
    /// - State is Ready.
    /// - targetProcessorMask is nonzero and a subset of OnlineProcessorMask.
    /// - Complete mailbox work was release-published before this call.
    void Notify(Foundation::ProcessorMask targetProcessorMask) noexcept override;

    [[nodiscard]] constexpr Amd64X2ApicTlbShootdownNotifierSnapshot GetSnapshot() const noexcept
    {
        return Amd64X2ApicTlbShootdownNotifierSnapshot{
            .X2ApicIds =
                {
                    m_x2ApicIds[0U],
                    m_x2ApicIds[1U],
                    m_x2ApicIds[2U],
                    m_x2ApicIds[3U],
                },
            .OnlineProcessorMask = m_onlineProcessorMask,
            .Vector = m_vector,
            .State = m_state,
            .Reserved = 0U,
        };
    }

    [[nodiscard]] const Interrupt::Amd64LocalApic& GetApic() const noexcept
    {
        return m_apic;
    }

    ~Amd64X2ApicTlbShootdownNotifier() noexcept = default;

    Amd64X2ApicTlbShootdownNotifier(const Amd64X2ApicTlbShootdownNotifier&) = delete;
    Amd64X2ApicTlbShootdownNotifier& operator=(const Amd64X2ApicTlbShootdownNotifier&) = delete;
    Amd64X2ApicTlbShootdownNotifier(Amd64X2ApicTlbShootdownNotifier&&) = delete;
    Amd64X2ApicTlbShootdownNotifier& operator=(Amd64X2ApicTlbShootdownNotifier&&) = delete;

private:
    Interrupt::Amd64LocalApic m_apic;
    Foundation::UInt32 m_x2ApicIds[Kernel::Scheduler::MaximumProcessorCount]{};
    Foundation::ProcessorMask m_onlineProcessorMask{0U};
    Foundation::UInt8 m_vector{0U};
    Amd64X2ApicTlbShootdownNotifierState m_state{Amd64X2ApicTlbShootdownNotifierState::Uninitialized};
    [[maybe_unused]] Foundation::UInt8 m_reserved{0U};
};

static_assert(sizeof(Amd64X2ApicTlbShootdownNotifierState) == 1U);
static_assert(sizeof(Amd64X2ApicTlbShootdownNotifierInitializationError) == 1U);
static_assert(sizeof(Amd64X2ApicTlbShootdownNotifierSnapshot) == 32U);
static_assert(alignof(Amd64X2ApicTlbShootdownNotifierSnapshot) == 8U);
static_assert(sizeof(Amd64X2ApicTlbShootdownNotifier) == 56U);
static_assert(alignof(Amd64X2ApicTlbShootdownNotifier) == 8U);
} // Namespace Asteria::Platform::Amd64::Memory

#endif // ASTERIA_PLATFORM_AMD64_MEMORY_AMD64_X2_APIC_TLB_SHOOTDOWN_NOTIFIER_HPP_INCLUDED
