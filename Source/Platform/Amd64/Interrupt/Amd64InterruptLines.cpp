// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Interrupt/Amd64InterruptLines.hpp"

namespace Asteria::Platform::Amd64::Interrupt
{
bool Amd64IoApicAccess::IsPhysicalRangeImplemented(const Foundation::UInt64 start, const Foundation::UInt64 length,
                                                   const Foundation::UInt8 bits) const noexcept
{
    if ((bits > 52U) || (start >= (Foundation::UInt64{1U} << bits)) ||
        (length > (Foundation::UInt64{1U} << bits) - start))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < m_count; ++index)
    {
        if ((start >= m_pages[index]) && (start - m_pages[index] <= 4096U) &&
            (length <= 4096U - (start - m_pages[index])))
        {
            return true;
        }
    }
    return false;
}
void Amd64IoApicAccess::AllowRegisterPage(const Foundation::UInt64 page) noexcept
{
    if (m_count < Boot::Acpi::MaximumAcpiIoControllers)
    {
        m_pages[m_count++] = page;
    }
}

Foundation::UInt64 Amd64IoApicAccess::ReadDoubleWord(const Foundation::UInt64 address) const noexcept
{
    const auto* const pointer =
        reinterpret_cast<const volatile Foundation::UInt32*>(static_cast<Foundation::UIntPointer>(address));
    return *pointer;
}
void Amd64IoApicAccess::WriteDoubleWord(const Foundation::UInt64 address, const Foundation::UInt32 value) const noexcept
{
    auto* const pointer = reinterpret_cast<volatile Foundation::UInt32*>(static_cast<Foundation::UIntPointer>(address));
    *pointer = value;
    // The directed EOI register is write-only. UC writes preserve ordering.
    if ((address & 0xFFFU) != Amd64IoApic::EndOfInterruptOffset)
    {
        static_cast<void>(*pointer);
    }
}

bool Amd64InterruptLines::Configure(const Boot::Acpi::AcpiTopology& topology,
                                    const Platform::Device::IPhysicalMemoryAccess& access,
                                    Amd64DeviceVectors& vectors) noexcept
{
    if ((m_vectors != nullptr) || (topology.IoControllerCount > Boot::Acpi::MaximumAcpiIoControllers) ||
        (topology.InterruptOverrideCount > Boot::Acpi::MaximumAcpiInterruptOverrides))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < topology.IoControllerCount; ++index)
    {
        const auto& controller = topology.IoControllers[index];
        if (controller.ControllerKind != Boot::Acpi::AcpiIoControllerKind::IoApic)
        {
            continue;
        }
        if (!m_controllers[m_count].Configure(access, controller.PhysicalAddress, controller.GlobalSystemInterruptBase))
        {
            continue;
        }
        for (Foundation::UInt32 pin{0U}; pin < m_controllers[m_count].GetPinCount(); ++pin)
        {
            m_controllers[m_count].WriteEntryLow(pin,
                                                 m_controllers[m_count].ReadEntryLow(pin) | Amd64IoApic::EntryMasked);
        }
        ++m_count;
    }
    for (Foundation::Size index{0U}; index < topology.InterruptOverrideCount; ++index)
    {
        const auto& override = topology.InterruptOverrides[index];
        const Foundation::UInt16 polarity = override.Flags & 3U;
        const Foundation::UInt16 trigger = (override.Flags >> 2U) & 3U;
        if ((override.Bus != 0U) || ((override.Flags & ~0xFU) != 0U) || (polarity == 2U) || (trigger == 2U) ||
            (override.Source > 15U))
        {
            continue;
        }
        static_cast<void>(SetPolicy(override.GlobalSystemInterrupt, trigger == 3U, polarity == 3U));
    }
    m_vectors = &vectors;
    return m_count != 0U;
}

bool Amd64InterruptLines::CanRoute(const Foundation::UInt32 gsi) const noexcept
{
    Foundation::Size serving{0U};
    for (Foundation::Size index{0U}; index < m_count; ++index)
    {
        serving += m_controllers[index].Serves(gsi) ? 1U : 0U;
    }
    return serving == 1U;
}

bool Amd64InterruptLines::SetPolicy(const Foundation::UInt32 gsi, const bool level, const bool activeLow) noexcept
{
    for (Foundation::Size index{0U}; index < m_policyCount; ++index)
    {
        if (m_policies[index].Gsi == gsi)
        {
            m_policies[index] = {.Gsi = gsi, .Level = level, .ActiveLow = activeLow};
            return true;
        }
    }
    if (m_policyCount == Boot::Acpi::MaximumAcpiInterruptOverrides + 2U)
    {
        return false;
    }
    m_policies[m_policyCount++] = {.Gsi = gsi, .Level = level, .ActiveLow = activeLow};
    return true;
}

Hal::Interrupt::LineReservation Amd64InterruptLines::Reserve(const Foundation::UInt32 gsi) noexcept
{
    if (m_vectors == nullptr)
    {
        return {};
    }
    Entry* free{nullptr};
    for (Entry& entry : m_entries)
    {
        if ((entry.Vector != 0U) && (entry.Gsi == gsi))
        {
            return {};
        }
        if (entry.Vector == 0U)
        {
            free = &entry;
        }
    }
    if (free == nullptr)
    {
        return {};
    }
    const Amd64IoApic* serving{nullptr};
    for (Foundation::Size index{0U}; index < m_count; ++index)
    {
        if (m_controllers[index].Serves(gsi))
        {
            if (serving != nullptr)
            {
                return {};
            }
            serving = &m_controllers[index];
        }
    }
    if (serving == nullptr)
    {
        return {};
    }
    // ACPI ISA defaults are edge/high; other GSIs need publisher policy.
    bool explicitPolicy{gsi < 16U};
    bool level{false};
    bool activeLow{false};
    for (Foundation::Size index{0U}; index < m_policyCount; ++index)
    {
        if (m_policies[index].Gsi == gsi)
        {
            explicitPolicy = true;
            level = m_policies[index].Level;
            activeLow = m_policies[index].ActiveLow;
        }
    }
    const Foundation::UInt64 destination = m_vectors->GetDestination();
    if (!explicitPolicy || (destination >= 0xFFU))
    {
        return {};
    }
    const Foundation::UInt64 vector = m_vectors->Reserve();
    if (vector == 0U)
    {
        return {};
    }
    if (!free->Source.Configure(*serving, gsi, static_cast<Foundation::UInt8>(vector),
                                static_cast<Foundation::UInt8>(destination), level, activeLow))
    {
        m_vectors->Release(vector);
        return {};
    }
    free->Gsi = gsi;
    free->Vector = vector;
    return {.Source = &free->Source, .Vector = vector};
}

bool Amd64InterruptLines::Publish(const Foundation::UInt64 vector, Kernel::Interrupt::Interrupt& interrupt) noexcept
{
    for (const Entry& entry : m_entries)
    {
        if (entry.Vector == vector)
        {
            return m_vectors->Publish(vector, interrupt);
        }
    }
    return false;
}
void Amd64InterruptLines::Release(const Foundation::UInt64 vector) noexcept
{
    for (Entry& entry : m_entries)
    {
        if ((entry.Vector != 0U) && (entry.Vector == vector))
        {
            entry.Source.Silence();
            m_vectors->Release(vector);
            entry.Vector = 0U;
            return;
        }
    }
}
}
