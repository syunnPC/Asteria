// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_IO_SAPIC_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_IO_SAPIC_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Interrupt/IInterruptSource.hpp"
#include "Platform/Device/IPhysicalMemoryAccess.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
/// A redirection entry's input polarity: PCI INTx is active low; an ISA line
/// is active high unless its MADT source override or, for a namespace-only
/// i8042, its _CRS descriptor says low (ACPI 6.5 5.2.12.5, 6.4.2.1).
enum class Ia64LinePolarity : Foundation::UInt8
{
    ActiveLow,
    ActiveHigh,
};

/// A redirection entry's trigger mode: PCI INTx is level-triggered; an ISA line is edge-triggered
/// unless the same evidence says level.
enum class Ia64LineTrigger : Foundation::UInt8
{
    Level,
    Edge,
};

/// One I/O SAPIC (ACPI MADT type 6), reached through uncached physical accesses: its version
/// register, its redirection table through IOREGSEL and IOWIN, and its EOI register, which clears a
/// level-triggered line's Remote IRR (the processor's cr.eoi reaches only its local SAPIC). A
/// select and its window access are two accesses, so every operation runs on the one processor that
/// owns the controller with external interrupts disabled; nothing else may access it meanwhile.
class Ia64IoSapic final
{
public:
    static constexpr Foundation::UInt64 SelectOffset{0x00U};
    static constexpr Foundation::UInt64 WindowOffset{0x10U};
    static constexpr Foundation::UInt64 EndOfInterruptOffset{0x40U};
    static constexpr Foundation::UInt32 VersionRegister{0x01U};
    static constexpr Foundation::UInt32 FirstEntryRegister{0x10U};
    /// The earliest version whose EOI register exists.
    static constexpr Foundation::UInt32 MinimumVersion{0x11U};
    static constexpr Foundation::UInt32 EntryVector{0xFFU};
    static constexpr Foundation::UInt32 EntryActiveLow{1U << 13U};
    static constexpr Foundation::UInt32 EntryRemoteRequest{1U << 14U};
    static constexpr Foundation::UInt32 EntryLevel{1U << 15U};
    static constexpr Foundation::UInt32 EntryMasked{1U << 16U};

    constexpr Ia64IoSapic() noexcept = default;

    Ia64IoSapic(const Ia64IoSapic&) = delete;
    Ia64IoSapic& operator=(const Ia64IoSapic&) = delete;

    /// Takes the controller at physicalBase whose first pin is
    /// globalSystemInterruptBase; reads its version, which must have the EOI
    /// register, and its pin count. Once only.
    [[nodiscard]] bool Configure(const Platform::Device::IPhysicalMemoryAccess& access, Foundation::UInt64 physicalBase,
                                 Foundation::UInt32 globalSystemInterruptBase) noexcept;

    /// Whether gsi is one of the controller's pins.
    [[nodiscard]] bool Serves(Foundation::UInt32 gsi) const noexcept;
    [[nodiscard]] Foundation::UInt32 GetPinCount() const noexcept
    {
        return m_pins;
    }
    [[nodiscard]] Foundation::UInt32 GetGlobalSystemInterruptBase() const noexcept
    {
        return m_base;
    }

    /// The low and high halves of pin's redirection entry.
    [[nodiscard]] Foundation::UInt32 ReadEntryLow(Foundation::UInt32 pin) const noexcept;
    void WriteEntryLow(Foundation::UInt32 pin, Foundation::UInt32 value) const noexcept;
    void WriteEntryHigh(Foundation::UInt32 pin, Foundation::UInt32 value) const noexcept;
    /// Clears the Remote IRR of every level-triggered pin delivering vector.
    void EndOfInterrupt(Foundation::UInt8 vector) const noexcept;

private:
    [[nodiscard]] Foundation::UInt32 Read(Foundation::UInt32 index) const noexcept;
    void Write(Foundation::UInt32 index, Foundation::UInt32 value) const noexcept;

    const Platform::Device::IPhysicalMemoryAccess* m_access{nullptr};
    Foundation::UInt64 m_physicalBase{0U};
    Foundation::UInt32 m_base{0U};
    Foundation::UInt32 m_pins{0U};
};

/// A PCI INTx or ISA line of an I/O SAPIC as one kernel Interrupt's source: level-triggered with
/// the configured polarity, one vector delivered to one processor. Its mask is the entry's mask
/// bit; acknowledging masks the entry and clears its Remote IRR, so a line still asserted is
/// delivered again only once the driver's Rearm unmasks it, by which time the driver acknowledged
/// its device. Claim and Release leave the entry masked with no Remote IRR left from an earlier
/// owner.
class Ia64IoSapicLine final : public Hal::Interrupt::IInterruptSource
{
public:
    constexpr Ia64IoSapicLine() noexcept = default;

    Ia64IoSapicLine(const Ia64IoSapicLine&) = delete;
    Ia64IoSapicLine& operator=(const Ia64IoSapicLine&) = delete;

    /// The line of gsi on sapic, delivered as vector to the processor whose
    /// local SAPIC is id and eid, with the given polarity and trigger;
    /// refuses a pin the controller lacks or a claimed line. Until then
    /// Claim is NotReady.
    [[nodiscard]] bool Configure(const Ia64IoSapic& sapic, Foundation::UInt32 gsi, Foundation::UInt8 vector,
                                 Foundation::UInt8 destinationId, Foundation::UInt8 destinationEid,
                                 Ia64LinePolarity polarity, Ia64LineTrigger trigger) noexcept;

    [[nodiscard]] Hal::Interrupt::InterruptSourceError Claim() noexcept override;
    [[nodiscard]] Hal::Interrupt::InterruptSourceError Release() noexcept override;
    [[nodiscard]] Hal::Interrupt::InterruptSourceError Unmask() noexcept override;
    [[nodiscard]] Hal::Interrupt::InterruptSourceError Mask() noexcept override;
    [[nodiscard]] Hal::Interrupt::InterruptSourceError MaskAndAcknowledge() noexcept override;

    /// Masks the configured line and clears its Remote IRR, claimed or not:
    /// what the dispatch of its vector leaves behind whatever the Interrupt
    /// did with it (one that ended no longer reaches its source).
    void Silence() noexcept;

    [[nodiscard]] bool IsClaimed() const noexcept
    {
        return m_claimed;
    }
    [[nodiscard]] Foundation::UInt8 GetVector() const noexcept
    {
        return m_vector;
    }

private:
    const Ia64IoSapic* m_sapic{nullptr};
    Foundation::UInt32 m_pin{0U};
    /// The entry's low half as programmed, unmasked, and its high half, the
    /// destination.
    Foundation::UInt32 m_entry{0U};
    Foundation::UInt32 m_destination{0U};
    Foundation::UInt8 m_vector{0U};
    bool m_claimed{false};
};
} // Namespace Asteria::Platform::Ia64::Interrupt

#endif // ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_IO_SAPIC_HPP_INCLUDED
