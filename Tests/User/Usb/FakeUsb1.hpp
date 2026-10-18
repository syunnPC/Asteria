// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_USB_FAKEUSB1_HPP_INCLUDED
#define ASTERIA_TESTS_USER_USB_FAKEUSB1_HPP_INCLUDED

#include "User/Usb/Usb1.hpp"

#include <cstring>

#include "User/Input/ProtocolFixtures.hpp"

namespace Asteria::Test::Usb
{
namespace U = User::Usb;
namespace In = User::Input;
namespace Fx = Test::Input;
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
enum class WireFault : UInt8
{
    None,
    Nak,
    Stall,
    Crc,
    Babble,
    Buffer,
    Stuff,
    Toggle,
};
struct WireResult
{
    WireFault Fault;
    Size Length;
};
// Shared USB device protocol, independent of either controller's state.
class Usb1Wire final
{
public:
    void Reset() noexcept
    {
        Address = 0U;
        Enabled = true;
        ReportToggle = false;
        SecondToggle = false;
        Halted = false;
        m_control = false;
    }
    [[nodiscard]] WireResult Packet(const UInt32 pid, const UInt32 address, const UInt32 endpoint, const bool toggle,
                                    const bool low, UInt8* const bytes, const Size maximum, const UInt64 frame,
                                    const UInt32 packet) noexcept
    {
        if (!Connected || !Enabled || (address != Address))
        {
            return {.Fault = WireFault::Crc, .Length = 0U};
        }
        Good = Good && (low == Low);
        if ((endpoint == 0U) && HoldControl)
        {
            return {.Fault = WireFault::Nak, .Length = 0U};
        }
        if (endpoint != 0U)
        {
            ++Attempts;
            LastAttempt = frame;
            if ((Fault != WireFault::None) || Halted)
            {
                const WireFault fault = Halted ? WireFault::Stall : Fault;
                if (fault == WireFault::Stall)
                {
                    Halted = true;
                }
                return {.Fault = fault, .Length = 0U};
            }
            // Endpoint 2 is a second interrupt IN endpoint, a composite
            // device's mouse, with its own DATA toggle.
            bool& expected = endpoint == 2U ? SecondToggle : ReportToggle;
            Good = Good && (pid == 0x69U) && ((endpoint == 1U) || (endpoint == 2U)) && (toggle == expected);
            expected = !expected;
            const bool mouse = Mouse || (endpoint == 2U);
            const UInt8* const report = mouse ? Fx::UsbMouseReport : Fx::UsbKeyboardReport;
            const Size length = mouse ? 4U : 8U;
            const Size actual = maximum < length ? maximum : length;
            if (bytes != nullptr)
            {
                std::memcpy(bytes, report, actual);
            }
            ++Reports;
            return {.Fault = WireFault::None, .Length = actual};
        }
        if (pid == 0x2DU)
        {
            Good = Good && (maximum == 8U) && !toggle && (bytes != nullptr);
            if (bytes == nullptr)
            {
                return {.Fault = WireFault::Buffer, .Length = 0U};
            }
            std::memcpy(m_setup.Bytes, bytes, 8U);
            m_control = true;
            m_offset = 0U;
            m_expected = true;
            m_input = (bytes[0U] & 0x80U) != 0U;
            m_length = Size{bytes[6U]} | (Size{bytes[7U]} << 8U);
            ++Setups;
            return {.Fault = WireFault::None, .Length = 8U};
        }
        Good = Good && m_control;
        if (maximum == 0U)
        {
            Good = Good && toggle && (pid == (m_input ? 0xE1U : 0x69U));
            m_control = false;
            ++Statuses;
            if (m_setup.Bytes[1U] == 5U)
            {
                Address = m_setup.Bytes[2U];
            }
            if ((m_setup.Bytes[0U] == 2U) && (m_setup.Bytes[1U] == 1U))
            {
                Halted = false;
                if (m_setup.Bytes[4U] == 0x82U)
                {
                    SecondToggle = false;
                }
                else
                {
                    ReportToggle = false;
                }
            }
            return {.Fault = WireFault::None, .Length = 0U};
        }
        Good = Good && (toggle == m_expected) && (pid == (m_input ? 0x69U : 0xE1U));
        const UInt8* source{nullptr};
        Size available{m_length};
        if (m_input && (m_setup.Bytes[1U] == 6U))
        {
            if (m_setup.Bytes[3U] == 1U)
            {
                source = Mouse ? Fx::MouseDevice : Fx::KeyboardDevice;
                available = 18U;
            }
            else
            {
                source = Mouse ? Fx::MouseConfiguration : Fx::KeyboardConfiguration;
                available = 34U;
            }
        }
        if (available > m_length)
        {
            available = m_length;
        }
        const Size remaining = m_offset < available ? available - m_offset : 0U;
        const Size length = maximum < remaining ? maximum : remaining;
        if (m_input && (source != nullptr) && (bytes != nullptr))
        {
            std::memcpy(bytes, source + m_offset, length);
        }
        if (m_input && (source == nullptr) && (bytes != nullptr))
        {
            for (Size i{0U}; i < length; ++i)
            {
                bytes[i] = static_cast<UInt8>((m_offset + i) & 0xFFU);
            }
        }
        m_offset += length;
        const Size packets = length == 0U ? 1U : (length + packet - 1U) / packet;
        if ((packets & 1U) != 0U)
        {
            m_expected = !m_expected;
        }
        return {.Fault = WireFault::None, .Length = length};
    }
    bool Connected{true};
    bool Enabled{false};
    bool Low{false};
    bool Mouse{false};
    bool Good{true};
    bool Halted{false};
    bool HoldControl{false};
    bool ReportToggle{false};
    bool SecondToggle{false};
    UInt32 Address{0U};
    UInt32 Setups{0U};
    UInt32 Statuses{0U};
    UInt32 Reports{0U};
    UInt32 Attempts{0U};
    UInt64 LastAttempt{0U};
    WireFault Fault{WireFault::None};

private:
    In::SetupPacket m_setup{};
    Size m_offset{0U};
    Size m_length{0U};
    bool m_expected{true};
    bool m_control{false};
    bool m_input{false};
};
class Usb1Mappings
{
public:
    void Map(const U::DmaMemory memory) noexcept
    {
        if (m_count < 4U)
        {
            m_mappings[m_count++] = memory;
        }
        else
        {
            Good = false;
        }
    }
    [[nodiscard]] UInt8* Translate(const UInt64 address, const Size bytes) noexcept
    {
        for (Size i{0U}; i < m_count; ++i)
        {
            const U::DmaMemory& map = m_mappings[i];
            if ((address >= map.Address) && ((address - map.Address) <= map.Bytes) &&
                (bytes <= map.Bytes - (address - map.Address)))
            {
                return map.Cpu + (address - map.Address);
            }
        }
        Good = false;
        return nullptr;
    }
    [[nodiscard]] Usb1Wire* Device(const UInt32 address) noexcept
    {
        for (auto& device : Devices)
        {
            if (device.Connected && device.Enabled && (device.Address == address))
            {
                return &device;
            }
        }
        return nullptr;
    }
    bool Good{true};
    Usb1Wire Devices[2U]{};
    UInt64 Frames{0U};
    UInt32 Publishes{0U};
    UInt32 Observes{0U};

private:
    U::DmaMemory m_mappings[4U]{};
    Size m_count{0U};
};
} // namespace Asteria::Test::Usb
#endif
