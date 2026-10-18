// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SYSTEM_USB_HOST_REGISTERS_HPP_INCLUDED
#define ASTERIA_USER_SYSTEM_USB_HOST_REGISTERS_HPP_INCLUDED

#include "Abi/Clock.hpp"
#include "User/System/DeviceAccess.hpp"
#include "User/Component/ComponentProgram.hpp"
#include "User/Usb/Ehci.hpp"
#include "User/Usb/Ohci.hpp"
#include "User/Usb/Uhci.hpp"

namespace Asteria::User::System
{
/// One USB host controller function's narrow configuration access
/// (Abi::DeviceReadConfiguration, Abi::DeviceWriteLegacySupport), and the
/// pacing of a firmware-handoff poll: while pacing, each read first waits a
/// millisecond on the component clock, so a library poll count of N bounds
/// the wait at N milliseconds (Source/User/Usb/Readme.md: a poll count is not
/// time). Serialized in the driver thread; no allocation.
class UsbHostAccess
{
public:
    constexpr UsbHostAccess() noexcept = default;
    void Configure(const SystemCallClient& client, const CapabilityHandle function,
                   const Component::ComponentClock clock, const Foundation::UInt64 ticksPerMillisecond) noexcept
    {
        m_client = &client;
        m_function = function;
        m_clock = clock;
        m_ticksPerMillisecond = ticksPerMillisecond;
    }
    void SetPacing(const bool pacing) noexcept
    {
        m_pacing = pacing;
    }
    /// The configuration dword at offset; false (value all ones) when the
    /// kernel refused, which a refused write also records.
    [[nodiscard]] bool ReadConfiguration(const Foundation::UInt32 offset, Foundation::UInt32& value) noexcept
    {
        Pace();
        const Abi::SystemCallResponse read =
            m_client->ControlDevice(m_function, Abi::DeviceReadConfiguration, CapabilityHandle{offset, 0U});
        value =
            (read.Error == Abi::SystemCallError::None) ? static_cast<Foundation::UInt32>(read.Value0) : 0xFFFF'FFFFU;
        m_refused = m_refused || (read.Error != Abi::SystemCallError::None);
        return read.Error == Abi::SystemCallError::None;
    }
    [[nodiscard]] bool WriteLegacySupport(const Foundation::UInt32 offset, const Foundation::UInt32 value) noexcept
    {
        const Abi::SystemCallError written =
            m_client->ControlDevice(m_function, Abi::DeviceWriteLegacySupport, CapabilityHandle{offset, value}).Error;
        m_refused = m_refused || (written != Abi::SystemCallError::None);
        return written == Abi::SystemCallError::None;
    }
    /// Whether any configuration access was refused so far.
    [[nodiscard]] bool Refused() const noexcept
    {
        return m_refused;
    }
    void Pace() const noexcept
    {
        if (!m_pacing || (m_clock == nullptr))
        {
            return;
        }
        const Foundation::UInt64 start = m_clock();
        while ((Abi::ElapsedClockTicks(m_clock(), start)) < m_ticksPerMillisecond)
        {
        }
    }

private:
    const SystemCallClient* m_client{nullptr};
    CapabilityHandle m_function{};
    Component::ComponentClock m_clock{nullptr};
    Foundation::UInt64 m_ticksPerMillisecond{0U};
    bool m_pacing{false};
    bool m_refused{false};
};

/// An EHCI's or OHCI's uncached register block, mapped from its BAR's page
/// at the BAR's offset within it, and the function's configuration access
/// (EHCI's USBLEGSUP lives in configuration space, EHCI 1.0 section 2.1.7).
/// Coherent write-back DMA: ordering is a fence. Serialized; no allocation.
class UsbHostMmio final : public Usb::IEhciMmio, public Usb::IOhciMmio
{
public:
    constexpr UsbHostMmio() noexcept = default;
    void Configure(volatile Foundation::UInt32* const registers) noexcept
    {
        m_registers = registers;
    }
    [[nodiscard]] UsbHostAccess& Access() noexcept
    {
        return m_access;
    }
    [[nodiscard]] Foundation::UInt32 Read32(const Foundation::UInt32 offset) noexcept override
    {
        m_access.Pace();
        return ReadRegister(m_registers, offset / 4U);
    }
    void Write32(const Foundation::UInt32 offset, const Foundation::UInt32 value) noexcept override
    {
        m_registers[offset / 4U] = value;
    }
    [[nodiscard]] Foundation::UInt32 ReadConfig32(const Foundation::UInt32 offset) noexcept override
    {
        Foundation::UInt32 value{0U};
        static_cast<void>(m_access.ReadConfiguration(offset, value));
        return value;
    }
    void WriteConfig32(const Foundation::UInt32 offset, const Foundation::UInt32 value) noexcept override
    {
        static_cast<void>(m_access.WriteLegacySupport(offset, value));
    }
    void PublishDma() noexcept override
    {
        OrderDmaStorage();
    }
    void ObserveDma() noexcept override
    {
        OrderDmaStorage();
    }

private:
    volatile Foundation::UInt32* m_registers{nullptr};
    UsbHostAccess m_access{};
};

/// A UHCI's 32 bytes of I/O ports, reached through the PortIo system call on
/// its DeviceRegion (UHCI never maps its registers), and its LEGSUP access.
/// A refused port access reads all ones and is remembered, so the controller
/// fails rather than drives a device it cannot see. Serialized; no allocation.
class UsbHostPorts final : public Usb::IUhciPortIo, public Usb::IUhciConfig
{
public:
    constexpr UsbHostPorts() noexcept = default;
    void Configure(const SystemCallClient& client, const CapabilityHandle region) noexcept
    {
        m_client = &client;
        m_region = region;
    }
    [[nodiscard]] UsbHostAccess& Access() noexcept
    {
        return m_access;
    }
    [[nodiscard]] bool Refused() const noexcept
    {
        return m_refused || m_access.Refused();
    }
    [[nodiscard]] Foundation::UInt8 Read8(const Foundation::UInt32 offset) noexcept override
    {
        return static_cast<Foundation::UInt8>(Read(offset, 1U));
    }
    [[nodiscard]] Foundation::UInt16 Read16(const Foundation::UInt32 offset) noexcept override
    {
        return static_cast<Foundation::UInt16>(Read(offset, 2U));
    }
    [[nodiscard]] Foundation::UInt32 Read32(const Foundation::UInt32 offset) noexcept override
    {
        return Read(offset, 4U);
    }
    void Write8(const Foundation::UInt32 offset, const Foundation::UInt8 value) noexcept override
    {
        Write(offset, 1U, value);
    }
    void Write16(const Foundation::UInt32 offset, const Foundation::UInt16 value) noexcept override
    {
        Write(offset, 2U, value);
    }
    void Write32(const Foundation::UInt32 offset, const Foundation::UInt32 value) noexcept override
    {
        Write(offset, 4U, value);
    }
    void PublishDma() noexcept override
    {
        OrderDmaStorage();
    }
    void ObserveDma() noexcept override
    {
        OrderDmaStorage();
    }
    // LEGSUP is the low half of configuration dword 0xC0.
    [[nodiscard]] bool ReadConfig16(const Foundation::UInt32 offset, Foundation::UInt16& value) noexcept override
    {
        Foundation::UInt32 dword{0U};
        const bool read = ((offset & 3U) == 0U) && m_access.ReadConfiguration(offset, dword);
        value = static_cast<Foundation::UInt16>(dword & 0xFFFFU);
        return read;
    }
    [[nodiscard]] bool WriteConfig16(const Foundation::UInt32 offset, const Foundation::UInt16 value) noexcept override
    {
        return ((offset & 3U) == 0U) && m_access.WriteLegacySupport(offset, value);
    }

private:
    [[nodiscard]] Foundation::UInt32 Read(const Foundation::UInt32 offset, const Foundation::UInt64 width) noexcept
    {
        m_access.Pace();
        const Abi::SystemCallResponse read = m_client->PortIo(m_region, offset, width, 0U);
        if (read.Error != Abi::SystemCallError::None)
        {
            m_refused = true;
            return 0xFFFF'FFFFU;
        }
        return static_cast<Foundation::UInt32>(read.Value0);
    }
    void Write(const Foundation::UInt32 offset, const Foundation::UInt64 width, const Foundation::UInt32 value) noexcept
    {
        m_refused =
            m_refused || (m_client->PortIo(m_region, offset, width, 1U, value).Error != Abi::SystemCallError::None);
    }

    const SystemCallClient* m_client{nullptr};
    CapabilityHandle m_region{};
    UsbHostAccess m_access{};
    bool m_refused{false};
};
} // namespace Asteria::User::System

#endif // ASTERIA_USER_SYSTEM_USB_HOST_REGISTERS_HPP_INCLUDED
