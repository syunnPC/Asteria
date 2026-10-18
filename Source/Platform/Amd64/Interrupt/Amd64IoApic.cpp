// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Interrupt/Amd64IoApic.hpp"

namespace Asteria::Platform::Amd64::Interrupt
{
using Foundation::UInt32;
using Foundation::UInt64;
using Hal::Interrupt::InterruptSourceError;

bool Amd64IoApic::Configure(const Platform::Device::IPhysicalMemoryAccess& access, const UInt64 physicalBase,
                            const UInt32 globalSystemInterruptBase) noexcept
{
    if ((m_access != nullptr) || (physicalBase == 0U) || ((physicalBase % 4096U) != 0U) ||
        !access.IsPhysicalRangeImplemented(physicalBase, EndOfInterruptOffset + 4U,
                                           access.GetMaximumPhysicalAddressBits()))
    {
        return false;
    }
    m_access = &access;
    m_physicalBase = physicalBase;
    const UInt32 version = Read(VersionRegister);
    // The version in bits 7:0, the highest redirection entry in bits 23:16.
    const UInt32 pins = ((version >> 16U) & 0xFFU) + 1U;
    // A floating bus is not an IOAPIC. Its 8-bit selector must also fit
    // both registers of every reported redirection entry.
    if ((version == 0xFFFF'FFFFU) || ((version & 0xFFU) < MinimumVersion) ||
        (pins > ((0x100U - FirstEntryRegister) >> 1U)))
    {
        m_access = nullptr;
        return false;
    }
    m_version = version & 0xFFU;
    m_base = globalSystemInterruptBase;
    m_pins = pins;
    return true;
}

bool Amd64IoApic::Serves(const UInt32 gsi) const noexcept
{
    return (m_access != nullptr) && (gsi >= m_base) && ((gsi - m_base) < m_pins);
}

UInt32 Amd64IoApic::Read(const UInt32 index) const noexcept
{
    m_access->WriteDoubleWord(m_physicalBase + SelectOffset, index);
    return static_cast<UInt32>(m_access->ReadDoubleWord(m_physicalBase + WindowOffset));
}

void Amd64IoApic::Write(const UInt32 index, const UInt32 value) const noexcept
{
    m_access->WriteDoubleWord(m_physicalBase + SelectOffset, index);
    m_access->WriteDoubleWord(m_physicalBase + WindowOffset, value);
}

UInt32 Amd64IoApic::ReadEntryLow(const UInt32 pin) const noexcept
{
    return Read(FirstEntryRegister + (pin << 1U));
}

void Amd64IoApic::WriteEntryLow(const UInt32 pin, const UInt32 value) const noexcept
{
    Write(FirstEntryRegister + (pin << 1U), value);
}

void Amd64IoApic::WriteEntryHigh(const UInt32 pin, const UInt32 value) const noexcept
{
    Write(FirstEntryRegister + (pin << 1U) + 1U, value);
}

void Amd64IoApic::EndOfInterrupt(const UInt32 pin, const Foundation::UInt8 vector) const noexcept
{
    if (m_version >= 0x20U)
    {
        m_access->WriteDoubleWord(m_physicalBase + EndOfInterruptOffset, vector);
        return;
    }
    // The caller has masked this source. Older controllers clear Remote IRR
    // by toggling only this line's entry; no scan runs in the handler.
    if (pin < m_pins)
    {
        const UInt32 entry = ReadEntryLow(pin);
        if (((entry & EntryVector) == vector) && ((entry & EntryLevel) != 0U) && ((entry & EntryMasked) != 0U))
        {
            WriteEntryLow(pin, entry & ~EntryLevel);
            WriteEntryLow(pin, entry);
        }
    }
}

bool Amd64IoApicLine::Configure(const Amd64IoApic& apic, const UInt32 gsi, const Foundation::UInt8 vector,
                                const Foundation::UInt32 destinationId, const bool level, const bool activeLow) noexcept
{
    if (m_claimed || !apic.Serves(gsi) || (vector < 16U) || (destinationId >= 0xFFU))
    {
        return false;
    }
    m_apic = &apic;
    m_pin = gsi - apic.GetGlobalSystemInterruptBase();
    m_vector = vector;
    // Fixed delivery (0) to one physical destination; publisher controls polarity and trigger.
    m_entry = vector | (activeLow ? Amd64IoApic::EntryActiveLow : 0U) | (level ? Amd64IoApic::EntryLevel : 0U);
    m_destination = static_cast<UInt32>(destinationId) << 24U;
    return true;
}

// The entry is first written masked and edge-triggered, which drops a Remote
// IRR an earlier owner left set (a redirection-table write never clears it
// for a level-triggered entry), and then masked as the line is; the EOI
// clears it on a controller that keeps it across the edge write.
InterruptSourceError Amd64IoApicLine::Claim() noexcept
{
    if (m_apic == nullptr)
    {
        return InterruptSourceError::NotReady;
    }
    if (m_claimed)
    {
        return InterruptSourceError::AlreadyClaimed;
    }
    const UInt32 edge = (m_entry & ~Amd64IoApic::EntryLevel) | Amd64IoApic::EntryMasked;
    m_apic->WriteEntryLow(m_pin, edge);
    m_apic->WriteEntryHigh(m_pin, m_destination);
    m_apic->WriteEntryLow(m_pin, m_entry | Amd64IoApic::EntryMasked);
    m_apic->EndOfInterrupt(m_pin, m_vector);
    m_claimed = true;
    return InterruptSourceError::None;
}

InterruptSourceError Amd64IoApicLine::Release() noexcept
{
    if (!m_claimed)
    {
        return InterruptSourceError::NotClaimed;
    }
    m_apic->WriteEntryLow(m_pin, m_entry | Amd64IoApic::EntryMasked);
    m_apic->EndOfInterrupt(m_pin, m_vector);
    m_claimed = false;
    return InterruptSourceError::None;
}

// An asserted line is delivered again at once: the controller re-evaluates a
// level-triggered entry as it is written.
InterruptSourceError Amd64IoApicLine::Unmask() noexcept
{
    if (!m_claimed)
    {
        return InterruptSourceError::NotClaimed;
    }
    m_apic->WriteEntryLow(m_pin, m_entry);
    return InterruptSourceError::None;
}

InterruptSourceError Amd64IoApicLine::Mask() noexcept
{
    if (!m_claimed)
    {
        return InterruptSourceError::NotClaimed;
    }
    m_apic->WriteEntryLow(m_pin, m_entry | Amd64IoApic::EntryMasked);
    return InterruptSourceError::None;
}

void Amd64IoApicLine::Silence() noexcept
{
    if (m_apic == nullptr)
    {
        return;
    }
    m_apic->WriteEntryLow(m_pin, m_entry | Amd64IoApic::EntryMasked);
    m_apic->EndOfInterrupt(m_pin, m_vector);
}

// Masked first, so the EOI that clears the Remote IRR cannot deliver the
// still-asserted line again before the driver acknowledged its device.
InterruptSourceError Amd64IoApicLine::MaskAndAcknowledge() noexcept
{
    if (!m_claimed)
    {
        return InterruptSourceError::NotClaimed;
    }
    m_apic->WriteEntryLow(m_pin, m_entry | Amd64IoApic::EntryMasked);
    m_apic->EndOfInterrupt(m_pin, m_vector);
    return InterruptSourceError::None;
}
} // namespace Asteria::Platform::Amd64::Interrupt
