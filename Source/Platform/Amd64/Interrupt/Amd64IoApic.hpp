// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_IO_APIC_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_IO_APIC_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Interrupt/IInterruptSource.hpp"
#include "Platform/Device/IPhysicalMemoryAccess.hpp"

namespace Asteria::Platform::Amd64::Interrupt
{
/// One I/O APIC (ACPI MADT type 1), reached through uncached physical accesses: its version
/// register, its redirection table through IOREGSEL and IOWIN, and its EOI register, which clears a
/// level-triggered line's Remote IRR by vector. Older controllers use a masked edge transition. A
/// select and its window access are two accesses, so every operation runs on the one processor that
/// owns the controller with external interrupts disabled; nothing else may access it meanwhile.
class Amd64IoApic final
{
public:
    static constexpr Foundation::UInt64 SelectOffset{0x00U};
    static constexpr Foundation::UInt64 WindowOffset{0x10U};
    static constexpr Foundation::UInt64 EndOfInterruptOffset{0x40U};
    static constexpr Foundation::UInt32 VersionRegister{0x01U};
    static constexpr Foundation::UInt32 FirstEntryRegister{0x10U};
    /// Minimum supported IOAPIC version; directed EOI is available from 0x20.
    static constexpr Foundation::UInt32 MinimumVersion{0x10U};
    static constexpr Foundation::UInt32 EntryVector{0xFFU};
    static constexpr Foundation::UInt32 EntryActiveLow{1U << 13U};
    static constexpr Foundation::UInt32 EntryRemoteRequest{1U << 14U};
    static constexpr Foundation::UInt32 EntryLevel{1U << 15U};
    static constexpr Foundation::UInt32 EntryMasked{1U << 16U};

    constexpr Amd64IoApic() noexcept = default;

    Amd64IoApic(const Amd64IoApic&) = delete;
    Amd64IoApic& operator=(const Amd64IoApic&) = delete;

    /// Takes the controller at physicalBase whose first pin is
    /// globalSystemInterruptBase; reads its version and pin count. Once only.
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
    void EndOfInterrupt(Foundation::UInt32 pin, Foundation::UInt8 vector) const noexcept;

private:
    [[nodiscard]] Foundation::UInt32 Read(Foundation::UInt32 index) const noexcept;
    void Write(Foundation::UInt32 index, Foundation::UInt32 value) const noexcept;

    const Platform::Device::IPhysicalMemoryAccess* m_access{nullptr};
    Foundation::UInt64 m_physicalBase{0U};
    Foundation::UInt32 m_base{0U};
    Foundation::UInt32 m_pins{0U};
    Foundation::UInt32 m_version{0U};
};

/// A publisher-configured line of an I/O APIC as one kernel Interrupt's source: electrical policy
/// comes from the publisher, one vector delivered to one processor. Its mask is the entry's mask
/// bit; acknowledging masks the entry and clears its Remote IRR, so a line still asserted is
/// delivered again only once the driver's Rearm unmasks it, by which time the driver acknowledged
/// its device. Claim and Release leave the entry masked with no Remote IRR left from an earlier
/// owner.
class Amd64IoApicLine final : public Hal::Interrupt::IInterruptSource
{
public:
    constexpr Amd64IoApicLine() noexcept = default;

    Amd64IoApicLine(const Amd64IoApicLine&) = delete;
    Amd64IoApicLine& operator=(const Amd64IoApicLine&) = delete;

    /// The line of gsi on apic, delivered as vector to the processor whose
    /// local APIC has destinationId; refuses a pin the controller lacks or a
    /// claimed line. Until then Claim is NotReady.
    [[nodiscard]] bool Configure(const Amd64IoApic& apic, Foundation::UInt32 gsi, Foundation::UInt8 vector,
                                 Foundation::UInt32 destinationId, bool level, bool activeLow) noexcept;

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
    const Amd64IoApic* m_apic{nullptr};
    Foundation::UInt32 m_pin{0U};
    /// The entry's low half as programmed, unmasked, and its high half, the
    /// destination.
    Foundation::UInt32 m_entry{0U};
    Foundation::UInt32 m_destination{0U};
    Foundation::UInt8 m_vector{0U};
    bool m_claimed{false};
};
} // Namespace Asteria::Platform::Amd64::Interrupt

#endif // ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_IO_APIC_HPP_INCLUDED
