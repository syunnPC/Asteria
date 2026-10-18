// SPDX-License-Identifier: Apache-2.0
#include "User/Input/UsbDescriptors.hpp"

#include <cstring>
#include <initializer_list>

#include "Support/TestContext.hpp"
#include "User/Input/ProtocolFixtures.hpp"

int main()
{
    namespace In = Asteria::User::Input;
    namespace Fx = Asteria::Test::Input;
    Asteria::TestSupport::TestContext context{"UsbDescriptors"};
    In::DeviceDescriptor device{};
    In::BootInterface boot{};
    ASTERIA_EXPECT(context, "synthetic usb-kbd device", In::UsbDescriptors::Device(Fx::KeyboardDevice, 18U, device),
                   device.Vendor == 0xDEADU, device.Product == 1U, device.PacketSize0 == 8U);
    ASTERIA_EXPECT(context, "synthetic usb-mouse device", In::UsbDescriptors::Device(Fx::MouseDevice, 18U, device));
    ASTERIA_EXPECT(context, "synthetic keyboard interface",
                   In::UsbDescriptors::FindBoot(Fx::KeyboardConfiguration, 34U, boot),
                   boot.Protocol == In::BootProtocol::Keyboard, boot.Configuration == 1U, boot.Number == 0U,
                   boot.Endpoint.Address == 0x81U, boot.Endpoint.MaxPacketSize == 8U, boot.Endpoint.Interval == 10U,
                   boot.ReportDescriptorLength == 64U);
    ASTERIA_EXPECT(context, "synthetic mouse interface",
                   In::UsbDescriptors::FindBoot(Fx::MouseConfiguration, 34U, boot),
                   boot.Protocol == In::BootProtocol::Mouse, boot.Endpoint.MaxPacketSize == 4U,
                   boot.ReportDescriptorLength == 32U);
    In::UInt8 bytes[In::MaximumConfigurationBytes + 1U]{};
    const In::BootInterface before = boot;
    for (In::Size length = 0U; length < 34U; ++length)
    {
        ASTERIA_EXPECT(context, "all truncations reject atomically",
                       !In::UsbDescriptors::FindBoot(Fx::KeyboardConfiguration, length, boot),
                       boot.Protocol == before.Protocol, boot.Endpoint.MaxPacketSize == before.Endpoint.MaxPacketSize);
    }
    ASTERIA_EXPECT(context, "null rejects", !In::UsbDescriptors::FindBoot(nullptr, 34U, boot),
                   !In::UsbDescriptors::Device(nullptr, 18U, device));
    for (const In::Size at : {9U, 18U, 27U})
    {
        for (const In::UInt8 bad : {In::UInt8{0U}, In::UInt8{1U}, In::UInt8{0xFFU}})
        {
            std::memcpy(bytes, Fx::KeyboardConfiguration, 34U);
            bytes[at] = bad;
            ASTERIA_EXPECT(context, "invalid bLength", !In::UsbDescriptors::FindBoot(bytes, 34U, boot));
        }
    }
    std::memcpy(bytes, Fx::KeyboardConfiguration, 34U);
    bytes[2U] = 1U;
    bytes[3U] = 4U;
    ASTERIA_EXPECT(context, "wTotalLength bound despite enough bytes",
                   !In::UsbDescriptors::FindBoot(bytes, sizeof(bytes), boot));
    In::ConfigurationHeader header{};
    ASTERIA_EXPECT(context, "header also enforces fixed bound", !In::UsbDescriptors::Configuration(bytes, 9U, header));
    std::memcpy(bytes, Fx::KeyboardConfiguration, 34U);
    bytes[2U] = 35U;
    bytes[34U] = 2U;
    ASTERIA_EXPECT(context, "one trailing byte", !In::UsbDescriptors::FindBoot(bytes, 35U, boot));
    bytes[2U] = 36U;
    bytes[35U] = 0x99U;
    ASTERIA_EXPECT(context, "unknown descriptor skipped", In::UsbDescriptors::FindBoot(bytes, 36U, boot));
    bytes[27U + 2U] = 1U;
    ASTERIA_EXPECT(context, "OUT is not IN", !In::UsbDescriptors::FindBoot(bytes, 36U, boot));
    std::memcpy(bytes, Fx::KeyboardConfiguration, 34U);
    bytes[27U + 3U] = 2U;
    ASTERIA_EXPECT(context, "bulk is not interrupt", !In::UsbDescriptors::FindBoot(bytes, 34U, boot));
    bytes[27U + 3U] = 3U;
    bytes[27U + 4U] = 7U;
    ASTERIA_EXPECT(context, "keyboard packet at least 8", !In::UsbDescriptors::FindBoot(bytes, 34U, boot));
    std::memcpy(bytes, Fx::KeyboardConfiguration, 34U);
    bytes[16U] = 0xFFU;
    ASTERIA_EXPECT(context, "unknown interface protocol", !In::UsbDescriptors::FindBoot(bytes, 34U, boot));
    std::memcpy(bytes, Fx::KeyboardConfiguration, 34U);
    bytes[23U] = 2U;
    ASTERIA_EXPECT(context, "HID subordinate lengths checked", !In::UsbDescriptors::FindBoot(bytes, 34U, boot));
    std::memcpy(bytes, Fx::KeyboardConfiguration, 34U);
    bytes[13U] = 2U;
    ASTERIA_EXPECT(context, "endpoint count mismatch", !In::UsbDescriptors::FindBoot(bytes, 34U, boot));
    // A nonboot interface followed by a boot mouse; unrelated endpoint cannot attach.
    std::memcpy(bytes, Fx::KeyboardConfiguration, 34U);
    bytes[2U] = 59U;
    bytes[4U] = 2U;
    bytes[14U] = 0xFFU;
    std::memcpy(bytes + 34U, Fx::MouseConfiguration + 9U, 25U);
    bytes[36U] = 1U;
    bytes[54U] = 0x82U;
    ASTERIA_EXPECT(context, "interface-scoped endpoint", In::UsbDescriptors::FindBoot(bytes, 59U, boot),
                   boot.Number == 1U, boot.Protocol == In::BootProtocol::Mouse, boot.Endpoint.MaxPacketSize == 4U);
    // Maximum-sized valid configuration, padded with bounded unknown descriptors.
    std::memcpy(bytes, Fx::KeyboardConfiguration, 34U);
    bytes[2U] = 0U;
    bytes[3U] = 4U;
    for (In::Size at = 34U; at < 1024U; at += 2U)
    {
        bytes[at] = 2U;
        bytes[at + 1U] = 0x99U;
    }
    ASTERIA_EXPECT(context, "exact bound accepted", In::UsbDescriptors::FindBoot(bytes, 1024U, boot));
    bytes[1023U] = 2U;
    bytes[1022U] = 3U;
    ASTERIA_EXPECT(context, "descriptor cannot exceed total", !In::UsbDescriptors::FindBoot(bytes, 1024U, boot));
    // synthetic usb-kbd2/usb-mouse2 high-speed differences: bcdUSB=0200,
    // bMaxPacketSize0=64 and interrupt bInterval=7 (dev-hid.c).
    std::memcpy(bytes, Fx::KeyboardDevice, 18U);
    bytes[3U] = 2U;
    bytes[7U] = 64U;
    ASTERIA_EXPECT(context, "synthetic high-speed device", In::UsbDescriptors::Device(bytes, 18U, device),
                   device.UsbVersion == 0x0200U, device.PacketSize0 == 64U);
    std::memcpy(bytes, Fx::MouseConfiguration, 34U);
    bytes[33U] = 7U;
    ASTERIA_EXPECT(context, "synthetic high-speed mouse endpoint", In::UsbDescriptors::FindBoot(bytes, 34U, boot),
                   boot.Endpoint.Interval == 7U, boot.Endpoint.MaxPacketSize == 4U);
    // Synthetic SuperSpeed companion, kept intact for the xHCI endpoint context.
    std::memcpy(bytes, Fx::KeyboardConfiguration, 34U);
    bytes[2U] = 40U;
    bytes[33U] = 4U;
    constexpr In::UInt8 companion[]{6U, 0x30U, 2U, 0U, 24U, 0U};
    std::memcpy(bytes + 34U, companion, 6U);
    ASTERIA_EXPECT(context, "SuperSpeed endpoint companion", In::UsbDescriptors::FindBoot(bytes, 40U, boot),
                   boot.Endpoint.HasCompanion, boot.Endpoint.MaxBurst == 2U, boot.Endpoint.BytesPerInterval == 24U);
    In::UInt16 packet = 0U;
    ASTERIA_EXPECT(context, "speed-specific endpoint zero",
                   In::UsbDescriptors::PacketSize0(8U, In::UsbSpeed::Low, packet), packet == 8U,
                   !In::UsbDescriptors::PacketSize0(16U, In::UsbSpeed::Low, packet),
                   In::UsbDescriptors::PacketSize0(64U, In::UsbSpeed::High, packet), packet == 64U,
                   In::UsbDescriptors::PacketSize0(9U, In::UsbSpeed::Super, packet), packet == 512U,
                   !In::UsbDescriptors::PacketSize0(64U, In::UsbSpeed::Super, packet));
    constexpr In::UInt8 expected[6U][8U]{
        {0x80U, 6U, 0U, 1U, 0U, 0U, 18U, 0U}, {0x80U, 6U, 2U, 2U, 0U, 0U, 0x34U, 0x12U},
        {0U, 5U, 7U, 0U, 0U, 0U, 0U, 0U},     {0U, 9U, 1U, 0U, 0U, 0U, 0U, 0U},
        {0x21U, 11U, 0U, 0U, 3U, 0U, 0U, 0U}, {0x21U, 10U, 0U, 0U, 3U, 0U, 0U, 0U},
    };
    const In::SetupPacket setups[]{In::UsbDescriptors::GetDescriptor(In::DescriptorKind::Device, 0U, 18U),
                                   In::UsbDescriptors::GetDescriptor(In::DescriptorKind::Configuration, 2U, 0x1234U),
                                   In::UsbDescriptors::SetAddress(7U),
                                   In::UsbDescriptors::SetConfiguration(1U),
                                   In::UsbDescriptors::SetProtocol(3U),
                                   In::UsbDescriptors::SetIdle(3U)};
    for (In::Size i = 0U; i < 6U; ++i)
    {
        ASTERIA_EXPECT(context, "setup bytes", std::memcmp(setups[i].Bytes, expected[i], 8U) == 0);
    }
    return context.Finish();
}
