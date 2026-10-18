// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Interrupt/Ia64InterruptLines.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;

bool Ia64InterruptLines::Configure(const Ia64IoSapic& sapic, const Foundation::UInt8 destinationId,
                                   const Foundation::UInt8 destinationEid) noexcept
{
    if ((m_controllerCount != 0U) || (sapic.GetPinCount() == 0U))
    {
        return false;
    }
    m_controllers[m_controllerCount++] = &sapic;
    m_destinationId = destinationId;
    m_destinationEid = destinationEid;
    return true;
}

bool Ia64InterruptLines::AddController(const Ia64IoSapic& sapic) noexcept
{
    if ((m_controllerCount == 0U) || (m_controllerCount == Boot::Acpi::MaximumAcpiIoControllers) ||
        (sapic.GetPinCount() == 0U))
    {
        return false;
    }
    for (Size index{0U}; index < m_controllerCount; ++index)
    {
        const auto* const existing = m_controllers[index];
        if (existing->Serves(sapic.GetGlobalSystemInterruptBase()) ||
            sapic.Serves(existing->GetGlobalSystemInterruptBase()))
        {
            return false;
        }
    }
    m_controllers[m_controllerCount++] = &sapic;
    return true;
}

const Ia64IoSapic* Ia64InterruptLines::Controller(const UInt32 line) const noexcept
{
    for (Size index{0U}; index < m_controllerCount; ++index)
    {
        if (m_controllers[index]->Serves(line))
        {
            return m_controllers[index];
        }
    }
    return nullptr;
}

bool Ia64InterruptLines::CanRoute(const UInt32 line) const noexcept
{
    return Controller(line) != nullptr;
}

bool Ia64InterruptLines::SetIsaPolicy(const UInt32 line, const Ia64LinePolarity polarity,
                                      const Ia64LineTrigger trigger) noexcept
{
    if (!CanRoute(line) || (m_isaPolicyCount == 2U))
    {
        return false;
    }
    for (const Entry& entry : m_entries)
    {
        if (entry.Reserved && (entry.GlobalSystemInterrupt == line))
        {
            return false;
        }
    }
    for (Size index{0U}; index < m_isaPolicyCount; ++index)
    {
        if (m_isaPolicies[index].Line == line)
        {
            return false;
        }
    }
    m_isaPolicies[m_isaPolicyCount++] = {line, polarity, trigger};
    return true;
}

// One Interrupt per line; the vectors rotate, so a late delivery of a line
// just released rarely meets a new owner.
Hal::Interrupt::LineReservation Ia64InterruptLines::Reserve(const UInt32 line) noexcept
{
    const Ia64IoSapic* const sapic = Controller(line);
    if (sapic == nullptr)
    {
        return {};
    }
    for (const Entry& entry : m_entries)
    {
        if (entry.Reserved && (entry.GlobalSystemInterrupt == line))
        {
            return {};
        }
    }
    Ia64LinePolarity polarity{Ia64LinePolarity::ActiveLow};
    Ia64LineTrigger trigger{Ia64LineTrigger::Level};
    for (Size index{0U}; index < m_isaPolicyCount; ++index)
    {
        if (m_isaPolicies[index].Line == line)
        {
            polarity = m_isaPolicies[index].Polarity;
            trigger = m_isaPolicies[index].Trigger;
        }
    }
    for (Size step{0U}; step < VectorCount; ++step)
    {
        const Size index = (m_next + step) % VectorCount;
        Entry& entry = m_entries[index];
        const auto vector = static_cast<Foundation::UInt8>(FirstVector + index);
        if (entry.Reserved ||
            !entry.Line.Configure(*sapic, line, vector, m_destinationId, m_destinationEid, polarity, trigger))
        {
            continue;
        }
        entry.Interrupt = nullptr;
        entry.GlobalSystemInterrupt = line;
        entry.Reserved = true;
        m_next = index + 1U;
        return Hal::Interrupt::LineReservation{.Source = &entry.Line, .Vector = vector};
    }
    return {};
}

bool Ia64InterruptLines::Publish(const UInt64 vector, Kernel::Interrupt::Interrupt& interrupt) noexcept
{
    if (!IsDeviceVector(vector) || !m_entries[vector - FirstVector].Reserved)
    {
        return false;
    }
    m_entries[vector - FirstVector].Interrupt = &interrupt;
    return true;
}

// The Interrupt released its claim as it quiesced; a reservation never
// claimed leaves the entry masked too.
void Ia64InterruptLines::Release(const UInt64 vector) noexcept
{
    if (!IsDeviceVector(vector))
    {
        return;
    }
    Entry& entry = m_entries[vector - FirstVector];
    entry.Line.Silence();
    entry.Interrupt = nullptr;
    entry.Reserved = false;
}

bool Ia64InterruptLines::HasBoundLines() const noexcept
{
    for (const Entry& entry : m_entries)
    {
        if (entry.Interrupt != nullptr)
        {
            return true;
        }
    }
    return false;
}

void Ia64InterruptLines::Dispatch(const UInt64 vector) noexcept
{
    if (!IsDeviceVector(vector))
    {
        ++m_spurious;
        return;
    }
    Entry& entry = m_entries[vector - FirstVector];
    // A released entry's Release silenced its line, whose pin a later
    // reservation may route under another vector by now: a late delivery
    // touches nothing.
    if (!entry.Reserved)
    {
        ++m_spurious;
        return;
    }
    if (entry.Interrupt != nullptr)
    {
        // The Interrupt's own state machine refuses a stale or masked
        // delivery.
        static_cast<void>(entry.Interrupt->HandleInterrupt());
        ++m_delivered;
    }
    else
    {
        ++m_spurious;
    }
    entry.Line.Silence();
}
} // namespace Asteria::Platform::Ia64::Interrupt
