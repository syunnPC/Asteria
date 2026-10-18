// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Interrupt/Ia64IoSapic.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
using Foundation::UInt32;
using Foundation::UInt64;
using Hal::Interrupt::InterruptSourceError;

bool Ia64IoSapic::Configure(const Platform::Device::IPhysicalMemoryAccess& access, const UInt64 physicalBase,
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
    if ((version & 0xFFU) < MinimumVersion)
    {
        m_access = nullptr;
        return false;
    }
    m_base = globalSystemInterruptBase;
    m_pins = ((version >> 16U) & 0xFFU) + 1U;
    return true;
}

bool Ia64IoSapic::Serves(const UInt32 gsi) const noexcept
{
    return (m_access != nullptr) && (gsi >= m_base) && ((gsi - m_base) < m_pins);
}

UInt32 Ia64IoSapic::Read(const UInt32 index) const noexcept
{
    m_access->WriteDoubleWord(m_physicalBase + SelectOffset, index);
    return static_cast<UInt32>(m_access->ReadDoubleWord(m_physicalBase + WindowOffset));
}

void Ia64IoSapic::Write(const UInt32 index, const UInt32 value) const noexcept
{
    m_access->WriteDoubleWord(m_physicalBase + SelectOffset, index);
    m_access->WriteDoubleWord(m_physicalBase + WindowOffset, value);
}

UInt32 Ia64IoSapic::ReadEntryLow(const UInt32 pin) const noexcept
{
    return Read(FirstEntryRegister + (pin << 1U));
}

void Ia64IoSapic::WriteEntryLow(const UInt32 pin, const UInt32 value) const noexcept
{
    Write(FirstEntryRegister + (pin << 1U), value);
}

void Ia64IoSapic::WriteEntryHigh(const UInt32 pin, const UInt32 value) const noexcept
{
    Write(FirstEntryRegister + (pin << 1U) + 1U, value);
}

void Ia64IoSapic::EndOfInterrupt(const Foundation::UInt8 vector) const noexcept
{
    m_access->WriteDoubleWord(m_physicalBase + EndOfInterruptOffset, vector);
}

bool Ia64IoSapicLine::Configure(const Ia64IoSapic& sapic, const UInt32 gsi, const Foundation::UInt8 vector,
                                const Foundation::UInt8 destinationId, const Foundation::UInt8 destinationEid,
                                const Ia64LinePolarity polarity, const Ia64LineTrigger trigger) noexcept
{
    if (m_claimed || !sapic.Serves(gsi) || (vector < 16U))
    {
        return false;
    }
    UInt32 polarityBit{0U};
    switch (polarity)
    {
    case Ia64LinePolarity::ActiveLow:
        polarityBit = Ia64IoSapic::EntryActiveLow;
        break;
    case Ia64LinePolarity::ActiveHigh:
        break;
    }
    UInt32 triggerBit{0U};
    switch (trigger)
    {
    case Ia64LineTrigger::Level:
        triggerBit = Ia64IoSapic::EntryLevel;
        break;
    case Ia64LineTrigger::Edge:
        break;
    }
    m_sapic = &sapic;
    m_pin = gsi - sapic.GetGlobalSystemInterruptBase();
    m_vector = vector;
    // Fixed delivery to the BSP. The vector-matched EOI that clears a level
    // entry's Remote IRR is harmless for an edge entry, which never sets it.
    m_entry = vector | triggerBit | polarityBit;
    m_destination = (static_cast<UInt32>(destinationId) << 24U) | (static_cast<UInt32>(destinationEid) << 16U);
    return true;
}

// The entry is first written masked and edge-triggered, which drops a Remote
// IRR an earlier owner left set (a redirection-table write never clears it
// for a level-triggered entry), and then masked as the line is; the EOI
// clears it on a controller that keeps it across the edge write.
InterruptSourceError Ia64IoSapicLine::Claim() noexcept
{
    if (m_sapic == nullptr)
    {
        return InterruptSourceError::NotReady;
    }
    if (m_claimed)
    {
        return InterruptSourceError::AlreadyClaimed;
    }
    const UInt32 edge = (m_entry & ~Ia64IoSapic::EntryLevel) | Ia64IoSapic::EntryMasked;
    m_sapic->WriteEntryLow(m_pin, edge);
    m_sapic->WriteEntryHigh(m_pin, m_destination);
    m_sapic->WriteEntryLow(m_pin, m_entry | Ia64IoSapic::EntryMasked);
    m_sapic->EndOfInterrupt(m_vector);
    m_claimed = true;
    return InterruptSourceError::None;
}

InterruptSourceError Ia64IoSapicLine::Release() noexcept
{
    if (!m_claimed)
    {
        return InterruptSourceError::NotClaimed;
    }
    m_sapic->WriteEntryLow(m_pin, m_entry | Ia64IoSapic::EntryMasked);
    m_sapic->EndOfInterrupt(m_vector);
    m_claimed = false;
    return InterruptSourceError::None;
}

// An asserted line is delivered again at once: the controller re-evaluates a
// level-triggered entry as it is written.
InterruptSourceError Ia64IoSapicLine::Unmask() noexcept
{
    if (!m_claimed)
    {
        return InterruptSourceError::NotClaimed;
    }
    m_sapic->WriteEntryLow(m_pin, m_entry);
    return InterruptSourceError::None;
}

InterruptSourceError Ia64IoSapicLine::Mask() noexcept
{
    if (!m_claimed)
    {
        return InterruptSourceError::NotClaimed;
    }
    m_sapic->WriteEntryLow(m_pin, m_entry | Ia64IoSapic::EntryMasked);
    return InterruptSourceError::None;
}

void Ia64IoSapicLine::Silence() noexcept
{
    if (m_sapic == nullptr)
    {
        return;
    }
    m_sapic->WriteEntryLow(m_pin, m_entry | Ia64IoSapic::EntryMasked);
    m_sapic->EndOfInterrupt(m_vector);
}

// Masked first, so the EOI that clears the Remote IRR cannot deliver the
// still-asserted line again before the driver acknowledged its device.
InterruptSourceError Ia64IoSapicLine::MaskAndAcknowledge() noexcept
{
    if (!m_claimed)
    {
        return InterruptSourceError::NotClaimed;
    }
    m_sapic->WriteEntryLow(m_pin, m_entry | Ia64IoSapic::EntryMasked);
    m_sapic->EndOfInterrupt(m_vector);
    return InterruptSourceError::None;
}
} // namespace Asteria::Platform::Ia64::Interrupt
