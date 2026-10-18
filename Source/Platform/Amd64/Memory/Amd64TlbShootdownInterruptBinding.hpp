// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_MEMORY_AMD64_TLB_SHOOTDOWN_INTERRUPT_BINDING_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_MEMORY_AMD64_TLB_SHOOTDOWN_INTERRUPT_BINDING_HPP_INCLUDED

#include "Kernel/Memory/TlbShootdownDrain.hpp"
#include "Platform/Amd64/Memory/Amd64X2ApicTlbShootdownNotifier.hpp"

extern "C" void AsteriaAmd64TlbShootdownInterruptEntry() noexcept;

namespace Asteria::Platform::Amd64::Memory
{
enum class Amd64TlbShootdownInterruptBindingState : Foundation::UInt8
{
    Uninitialized,
    Ready,
};

enum class Amd64TlbShootdownInterruptBindingInitializationError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    NotifierNotReady,
    InvalidDrain,
    BindingAlreadyInstalled,
};

enum class Amd64TlbShootdownInterruptError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidProcessor,
};

struct Amd64TlbShootdownInterruptResult
{
    Kernel::Memory::TlbShootdownDrainSummary Drain;
    Foundation::UInt64 ObservedX2ApicId;
    Amd64TlbShootdownInterruptError Error;
    Foundation::UInt8 Reserved[7];
};

struct Amd64TlbShootdownInterruptBindingSnapshot
{
    Foundation::UInt32 X2ApicIds[Kernel::Scheduler::MaximumProcessorCount];
    Foundation::ProcessorMask OnlineProcessorMask;
    Foundation::UInt8 Vector;
    Amd64TlbShootdownInterruptBindingState State;
    Foundation::UInt8 Reserved;
};

/// Publishes one immutable mapping from the current x2APIC identity to its bounded processor-local
/// TLB-shootdown drain. Initialize runs once before the interrupt gate is made reachable. The
/// object and every retained drain then outlive all online processors and all interrupt entries.
/// Shutdown disables notification, masks/removes the gate, joins entered handlers, and drains
/// outstanding work before reclamation.
class Amd64TlbShootdownInterruptBinding final
{
public:
    constexpr Amd64TlbShootdownInterruptBinding() noexcept = default;

    /// Preconditions:
    /// - Initialization and publication are serialized with the platform's
    /// SMP/interrupt setup domain.
    /// - Each online drain and its complete dependency graph remain alive and
    /// immutable until interrupt shutdown has joined every handler.
    [[nodiscard]] Amd64TlbShootdownInterruptBindingInitializationError
    Initialize(const Amd64X2ApicTlbShootdownNotifier& notifier,
               Kernel::Memory::ITlbShootdownDrain* const (&drains)[Kernel::Scheduler::MaximumProcessorCount]) noexcept;

    /// Handles one already accepted shootdown interrupt on the current CPU. The x2APIC EOI is
    /// written exactly once after the drain attempt. Preconditions: - The caller runs at CPL0 in
    /// x2APIC mode on the CPU that accepted the binding's fixed shootdown vector. - The interrupt
    /// gate suppresses same-vector re-entry and migration for the complete call. - The current
    /// entry frame has no hardware error code and uses the current kernel stack (IST zero in this
    /// initial interrupt layer).
    [[nodiscard]] Amd64TlbShootdownInterruptResult HandleInterrupt() noexcept;

    [[nodiscard]] constexpr Amd64TlbShootdownInterruptBindingSnapshot GetSnapshot() const noexcept
    {
        return Amd64TlbShootdownInterruptBindingSnapshot{
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

    ~Amd64TlbShootdownInterruptBinding() noexcept = default;

    Amd64TlbShootdownInterruptBinding(const Amd64TlbShootdownInterruptBinding&) = delete;
    Amd64TlbShootdownInterruptBinding& operator=(const Amd64TlbShootdownInterruptBinding&) = delete;
    Amd64TlbShootdownInterruptBinding(Amd64TlbShootdownInterruptBinding&&) = delete;
    Amd64TlbShootdownInterruptBinding& operator=(Amd64TlbShootdownInterruptBinding&&) = delete;

private:
    Kernel::Memory::ITlbShootdownDrain* m_drains[Kernel::Scheduler::MaximumProcessorCount]{};
    Foundation::UInt32 m_x2ApicIds[Kernel::Scheduler::MaximumProcessorCount]{};
    Foundation::ProcessorMask m_onlineProcessorMask{0U};
    Foundation::UInt8 m_vector{0U};
    Amd64TlbShootdownInterruptBindingState m_state{Amd64TlbShootdownInterruptBindingState::Uninitialized};
    [[maybe_unused]] Foundation::UInt8 m_reserved{0U};
};

static_assert(sizeof(Amd64TlbShootdownInterruptBindingState) == 1U);
static_assert(sizeof(Amd64TlbShootdownInterruptBindingInitializationError) == 1U);
static_assert(sizeof(Amd64TlbShootdownInterruptError) == 1U);
static_assert(sizeof(Amd64TlbShootdownInterruptResult) == 32U);
static_assert(alignof(Amd64TlbShootdownInterruptResult) == 8U);
static_assert(sizeof(Amd64TlbShootdownInterruptBindingSnapshot) == 32U);
static_assert(alignof(Amd64TlbShootdownInterruptBindingSnapshot) == 8U);
static_assert(sizeof(Amd64TlbShootdownInterruptBinding) == 64U);
static_assert(alignof(Amd64TlbShootdownInterruptBinding) == 8U);
} // Namespace Asteria::Platform::Amd64::Memory

#endif // ASTERIA_PLATFORM_AMD64_MEMORY_AMD64_TLB_SHOOTDOWN_INTERRUPT_BINDING_HPP_INCLUDED
