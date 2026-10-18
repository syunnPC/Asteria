// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_INTERRUPT_LINES_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_INTERRUPT_LINES_HPP_INCLUDED

#include "Boot/Acpi/AcpiTopology.hpp"
#include "Foundation/Types.hpp"
#include "Hal/Interrupt/IInterruptLines.hpp"
#include "Kernel/Interrupt/Interrupt.hpp"
#include "Platform/Ia64/Interrupt/IIa64DeviceInterruptDispatch.hpp"
#include "Platform/Ia64/Interrupt/Ia64IoSapic.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
/// The INTx lines the IA-64 kernel routes to drivers' Interrupts: a reserved line of the I/O SAPIC
/// gets one of the vectors 0x40-0x4F, delivered to one processor (the BSP), allocated in rotation.
/// Dispatch enters the Interrupt the vector is bound to, then leaves the line masked with its
/// Remote IRR cleared whatever the Interrupt did with it: one that ended no longer reaches its
/// source, and a vector reserved but bound to nothing must not keep its line blocked. A vector
/// whose reservation was released is left alone: its Release silenced the line, whose pin a later
/// reservation may route under another vector. A dispatch is counted as delivered, or as spurious
/// when no Interrupt is bound. Every operation runs on that processor with external interrupts
/// disabled, so none interleaves with another.
class Ia64InterruptLines final : public Hal::Interrupt::IInterruptLines, public IIa64DeviceInterruptDispatch
{
public:
    static constexpr Foundation::UInt64 FirstVector{0x40U};
    static constexpr Foundation::Size VectorCount{16U};

    constexpr Ia64InterruptLines() noexcept = default;

    Ia64InterruptLines(const Ia64InterruptLines&) = delete;
    Ia64InterruptLines& operator=(const Ia64InterruptLines&) = delete;

    [[nodiscard]] static constexpr bool IsDeviceVector(const Foundation::UInt64 vector) noexcept
    {
        return (vector >= FirstVector) && (vector < (FirstVector + VectorCount));
    }

    /// The controller and the local SAPIC (id, eid) its lines deliver to;
    /// once only. Until then every Reserve refuses.
    [[nodiscard]] bool Configure(const Ia64IoSapic& sapic, Foundation::UInt8 destinationId,
                                 Foundation::UInt8 destinationEid) noexcept;

    /// Adds a further MADT I/O SAPIC before any reservation, delivering to the
    /// same processor; refuses one whose GSI range overlaps a configured one.
    /// No mappings or allocation.
    [[nodiscard]] bool AddController(const Ia64IoSapic& sapic) noexcept;
    /// Whether a configured controller serves line.
    [[nodiscard]] bool CanRoute(Foundation::UInt32 line) const noexcept;
    /// Records the polarity and trigger of one of the two ISA lines (the i8042's IRQ 1 and 12 after
    /// MADT overrides) before its reservation; boot only, once per line. Every
    /// other line is PCI INTx, active low and level-triggered.
    [[nodiscard]] bool SetIsaPolicy(Foundation::UInt32 line, Ia64LinePolarity polarity,
                                    Ia64LineTrigger trigger) noexcept;

    [[nodiscard]] Hal::Interrupt::LineReservation Reserve(Foundation::UInt32 line) noexcept override;
    [[nodiscard]] bool Publish(Foundation::UInt64 vector, Kernel::Interrupt::Interrupt& interrupt) noexcept override;
    void Release(Foundation::UInt64 vector) noexcept override;

    /// Enters the Interrupt bound to vector, a device vector the processor
    /// took, and leaves its line masked and acknowledged.
    void Dispatch(Foundation::UInt64 vector) noexcept override;

    /// Whether an Interrupt is bound to a line: a wake an idle processor
    /// may wait for.
    [[nodiscard]] bool HasBoundLines() const noexcept;

    [[nodiscard]] Foundation::UInt64 GetDelivered() const noexcept
    {
        return m_delivered;
    }
    [[nodiscard]] Foundation::UInt64 GetSpurious() const noexcept
    {
        return m_spurious;
    }

private:
    struct Entry
    {
        Ia64IoSapicLine Line;
        Kernel::Interrupt::Interrupt* Interrupt;
        Foundation::UInt32 GlobalSystemInterrupt;
        bool Reserved;
    };

    struct IsaPolicy
    {
        Foundation::UInt32 Line;
        Ia64LinePolarity Polarity;
        Ia64LineTrigger Trigger;
    };
    [[nodiscard]] const Ia64IoSapic* Controller(Foundation::UInt32 line) const noexcept;
    const Ia64IoSapic* m_controllers[Boot::Acpi::MaximumAcpiIoControllers]{};
    Foundation::Size m_controllerCount{0U};
    IsaPolicy m_isaPolicies[2U]{};
    Foundation::Size m_isaPolicyCount{0U};
    Foundation::UInt8 m_destinationId{0U};
    Foundation::UInt8 m_destinationEid{0U};
    Entry m_entries[VectorCount]{};
    Foundation::Size m_next{0U};
    Foundation::UInt64 m_delivered{0U};
    Foundation::UInt64 m_spurious{0U};
};
} // Namespace Asteria::Platform::Ia64::Interrupt

#endif // ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_INTERRUPT_LINES_HPP_INCLUDED
