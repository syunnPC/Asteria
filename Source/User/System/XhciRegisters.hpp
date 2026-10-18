// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SYSTEM_XHCI_REGISTERS_HPP_INCLUDED
#define ASTERIA_USER_SYSTEM_XHCI_REGISTERS_HPP_INCLUDED

#include "Abi/Clock.hpp"
#include "User/System/DeviceAccess.hpp"
#include "User/Component/ComponentProgram.hpp"
#include "User/Usb/Xhci.hpp"

namespace Asteria::User::System
{
/// Uncached BAR mapping and coherent WB DMA, serialized in the driver thread.
/// Split qword writes publish the low dword first, as xHCI permits. The mapping
/// owner validates all ranges before the controller accesses them. No allocation.
class XhciRegisters final : public Usb::IXhciMmio
{
public:
    constexpr XhciRegisters() noexcept = default;
    void Configure(volatile Foundation::UInt32* registers, Component::ComponentClock clock,
                   Foundation::UInt64 ticksPerMillisecond) noexcept
    {
        m_registers = registers;
        m_clock = clock;
        m_ticksPerMillisecond = ticksPerMillisecond;
    }
    [[nodiscard]] Foundation::UInt32 Read32(Foundation::UInt32 offset) noexcept override
    {
        return ReadRegister(m_registers, offset / 4U);
    }
    [[nodiscard]] Foundation::UInt64 Read64(Foundation::UInt32 offset) noexcept override
    {
        const Foundation::UInt32 low = Read32(offset);
        return Foundation::UInt64{low} | (Foundation::UInt64{Read32(offset + 4U)} << 32U);
    }
    void Write32(Foundation::UInt32 offset, Foundation::UInt32 value) noexcept override
    {
        m_registers[offset / 4U] = value;
    }
    void WriteOsOwned(Foundation::UInt32 offset) noexcept override
    {
        // MMIO boundary: BAR bytes share the mapping's lifetime; USBLEGSUP's
        // semaphore specifically requires an independent byte write.
        auto* const bytes = reinterpret_cast<volatile Foundation::UInt8*>(m_registers);
        bytes[offset + 3U] = 1U;
    }
    void Write64(Foundation::UInt32 offset, Foundation::UInt64 value) noexcept override
    {
        Write32(offset, static_cast<Foundation::UInt32>(value & 0xFFFF'FFFFU));
        Write32(offset + 4U, static_cast<Foundation::UInt32>(value >> 32U));
    }
    void PublishDma() noexcept override
    {
        OrderDmaStorage();
    }
    void ObserveDma() noexcept override
    {
        OrderDmaStorage();
    }
    void PausePoll() noexcept override
    {
        const Foundation::UInt64 start = m_clock();
        while ((Abi::ElapsedClockTicks(m_clock(), start)) < m_ticksPerMillisecond)
        {
        }
    }

private:
    Component::ComponentClock m_clock{nullptr};
    Foundation::UInt64 m_ticksPerMillisecond{0U};
    volatile Foundation::UInt32* m_registers{nullptr};
};
} // namespace Asteria::User::System
#endif
