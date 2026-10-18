// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_FIRMWARE_ACPI_POWER_CONTROL_HPP_INCLUDED
#define ASTERIA_PLATFORM_FIRMWARE_ACPI_POWER_CONTROL_HPP_INCLUDED
#include "Abi/SystemCallAbi.hpp"
#include "Boot/Acpi/PowerRegisters.hpp"
#include "Hal/IPowerControl.hpp"
#include "Hal/Device/IPortIo.hpp"
namespace Asteria::Platform::Firmware
{
enum class PowerResult
{
    CpuUnconfirmed,
    AcpiUnconfirmed,
    Issued
};
// PowerOff.md; ACPI 6.5 §§4.8.3.2.1, 4.8.3.6, 16.1.7. Native adapters
// supply terminal CPU parking and exact-width UC MMIO; no AML in this layer.
class AcpiPowerControl : public Hal::IPowerControl
{
public:
    void Configure(const Boot::Acpi::PowerRegisters& facts, Hal::Device::IPortIo* ports) noexcept
    {
        m_facts = facts;
        m_ports = ports;
    }
    [[nodiscard]] Foundation::UInt64 Available() const noexcept override
    {
        const bool off = m_facts.SleepKnown == 1U && m_facts.TypeA <= 7U && m_facts.TypeB <= 7U &&
                         Usable(m_facts.ControlA, 2U) &&
                         ((m_facts.ControlB.Address == 0U) || Usable(m_facts.ControlB, 2U));
        return (off ? Abi::DevicePowerOffAvailable : 0U) |
               (Usable(m_facts.Reset, 1U) ? Abi::DeviceRebootAvailable : 0U);
    }
    [[nodiscard]] bool Transition(const bool reboot) noexcept override
    {
        if ((Available() & (reboot ? Abi::DeviceRebootAvailable : Abi::DevicePowerOffAvailable)) == 0U)
        {
            return false;
        }
        if (!ParkOthers())
        {
            Returned(PowerResult::CpuUnconfirmed);
        }
        if (reboot)
        {
            Write(m_facts.Reset, m_facts.ResetValue);
        }
        else
        {
            if (!AcquireAcpi())
            {
                Returned(PowerResult::AcpiUnconfirmed);
            }
            const auto a =
                Boot::Acpi::EncodeSleep(static_cast<Foundation::UInt16>(Read(m_facts.ControlA)), m_facts.TypeA, false);
            const auto b = m_facts.ControlB.Address == 0U
                               ? Foundation::UInt16{0U}
                               : Boot::Acpi::EncodeSleep(static_cast<Foundation::UInt16>(Read(m_facts.ControlB)),
                                                         m_facts.TypeB, false);
            Write(m_facts.ControlA, a);
            if (m_facts.ControlB.Address != 0U)
            {
                Write(m_facts.ControlB, b);
            }
            Write(m_facts.ControlA, a | Boot::Acpi::SleepEnable);
            if (m_facts.ControlB.Address != 0U)
            {
                Write(m_facts.ControlB, b | Boot::Acpi::SleepEnable);
            }
        }
        Returned(PowerResult::Issued);
        __builtin_unreachable();
    }

protected:
    constexpr AcpiPowerControl() noexcept = default;
    ~AcpiPowerControl() noexcept = default;
    [[nodiscard]] virtual bool ParkOthers() noexcept = 0;
    [[nodiscard]] virtual Foundation::UInt32 ReadMemory(const Boot::Acpi::PowerRegister&) noexcept = 0;
    virtual void WriteMemory(const Boot::Acpi::PowerRegister&, Foundation::UInt32) noexcept = 0;
    [[noreturn]] virtual void Returned(PowerResult result) noexcept = 0;

private:
    [[nodiscard]] bool AcquireAcpi() noexcept
    {
        // ACPI 6.5 §§4.8.2.5, 5.2.9: synchronous boot-processor ownership.
        if ((Read(m_facts.ControlA) & Boot::Acpi::SciEnable) != 0U)
        {
            return true;
        }
        if (m_ports == nullptr || m_facts.SmiCommand == 0U || m_facts.SmiCommand > 0xFFFFU || m_facts.AcpiEnable == 0U)
        {
            return false;
        }
        m_ports->Write(static_cast<Foundation::UInt16>(m_facts.SmiCommand), 1U, m_facts.AcpiEnable);
        for (Foundation::Size poll{0U}; poll < 1'000'000U; ++poll)
        {
            if ((Read(m_facts.ControlA) & Boot::Acpi::SciEnable) != 0U)
            {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] bool Usable(const Boot::Acpi::PowerRegister& reg, const Foundation::UInt8 width) const noexcept
    {
        return Boot::Acpi::ValidPowerRegister(reg, width) && (reg.Space == 0U || m_ports != nullptr);
    }
    [[nodiscard]] Foundation::UInt32 Read(const Boot::Acpi::PowerRegister& reg) noexcept
    {
        return reg.Space == 1U ? m_ports->Read(static_cast<Foundation::UInt16>(reg.Address), reg.Width)
                               : ReadMemory(reg);
    }
    void Write(const Boot::Acpi::PowerRegister& reg, const Foundation::UInt32 value) noexcept
    {
        if (reg.Space == 1U)
        {
            m_ports->Write(static_cast<Foundation::UInt16>(reg.Address), reg.Width, value);
        }
        else
        {
            WriteMemory(reg, value);
        }
    }
    Boot::Acpi::PowerRegisters m_facts{};
    Hal::Device::IPortIo* m_ports{nullptr};
};
} // namespace Asteria::Platform::Firmware
#endif
