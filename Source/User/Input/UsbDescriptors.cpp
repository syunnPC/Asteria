// SPDX-License-Identifier: Apache-2.0

#include "User/Input/UsbDescriptors.hpp"

#include "User/Input/UsbConfiguration.hpp"

namespace Asteria::User::Input
{
namespace
{
[[nodiscard]] UInt16 Little(const UInt8* const bytes) noexcept
{
    return static_cast<UInt16>(static_cast<UInt16>(bytes[0U]) | (static_cast<UInt16>(bytes[1U]) << 8U));
}
[[nodiscard]] SetupPacket Setup(const UInt8 type, const UInt8 request, const UInt16 value, const UInt16 index,
                                const UInt16 length) noexcept
{
    return {.Bytes = {type, request, static_cast<UInt8>(value & 255U), static_cast<UInt8>(value >> 8U),
                      static_cast<UInt8>(index & 255U), static_cast<UInt8>(index >> 8U),
                      static_cast<UInt8>(length & 255U), static_cast<UInt8>(length >> 8U)}};
}
} // namespace

bool UsbDescriptors::Device(const UInt8* const bytes, const Size length, DeviceDescriptor& output) noexcept
{
    if ((bytes == nullptr) || (length < 18U) || (bytes[0U] != 18U) || (bytes[1U] != 1U) || (bytes[17U] == 0U))
    {
        return false;
    }
    output = {.UsbVersion = Little(bytes + 2U),
              .Vendor = Little(bytes + 8U),
              .Product = Little(bytes + 10U),
              .DeviceClass = bytes[4U],
              .PacketSize0 = bytes[7U],
              .ConfigurationCount = bytes[17U]};
    return true;
}

bool UsbDescriptors::Configuration(const UInt8* const bytes, const Size length, ConfigurationHeader& output) noexcept
{
    if ((bytes == nullptr) || (length < 9U) || (bytes[0U] != 9U) || (bytes[1U] != 2U) || (bytes[4U] == 0U) ||
        (bytes[5U] == 0U))
    {
        return false;
    }
    const UInt16 total = Little(bytes + 2U);
    if ((total < 9U) || (total > MaximumConfigurationBytes))
    {
        return false;
    }
    output = {.TotalLength = total, .InterfaceCount = bytes[4U], .Value = bytes[5U]};
    return true;
}

bool UsbDescriptors::FindBoot(const UInt8* const bytes, const Size length, BootInterface& output) noexcept
{
    UsbConfiguration configuration{};
    if (UsbConfiguration::Parse(bytes, length, configuration) != DescriptorError::None)
    {
        return false;
    }
    for (Size i{0U}; i < configuration.InterfaceCount(); ++i)
    {
        InterfaceBinding binding{};
        if ((configuration.Bind(i, binding) == BindingError::None) &&
            ((binding.Class == InterfaceClass::BootKeyboard) || (binding.Class == InterfaceClass::BootMouse)))
        {
            output = {.Configuration = configuration.Header().Value,
                      .Number = binding.Number,
                      .Protocol =
                          binding.Class == InterfaceClass::BootKeyboard ? BootProtocol::Keyboard : BootProtocol::Mouse,
                      .ReportDescriptorLength = binding.ReportLength,
                      .Endpoint = binding.In};
            return true;
        }
    }
    return false;
}

bool UsbDescriptors::PacketSize0(const UInt8 encoded, const UsbSpeed speed, UInt16& output) noexcept
{
    switch (speed)
    {
    case UsbSpeed::Low:
        if (encoded != 8U)
        {
            return false;
        }
        output = 8U;
        return true;
    case UsbSpeed::Full:
        if ((encoded != 8U) && (encoded != 16U) && (encoded != 32U) && (encoded != 64U))
        {
            return false;
        }
        output = encoded;
        return true;
    case UsbSpeed::High:
        if (encoded != 64U)
        {
            return false;
        }
        output = 64U;
        return true;
    case UsbSpeed::Super:
        if (encoded != 9U)
        {
            return false;
        }
        output = 512U;
        return true;
    }
    return false;
}

SetupPacket UsbDescriptors::GetDescriptor(const DescriptorKind kind, const UInt8 index, const UInt16 length) noexcept
{
    UInt16 type = 0U;
    switch (kind)
    {
    case DescriptorKind::Device:
        type = 1U;
        break;
    case DescriptorKind::Configuration:
        type = 2U;
        break;
    }
    return Setup(0x80U, 6U, static_cast<UInt16>((type << 8U) | index), 0U, length);
}
SetupPacket UsbDescriptors::SetAddress(const UInt8 address) noexcept
{
    return Setup(0U, 5U, address, 0U, 0U);
}
SetupPacket UsbDescriptors::SetConfiguration(const UInt8 value) noexcept
{
    return Setup(0U, 9U, value, 0U, 0U);
}
SetupPacket UsbDescriptors::SetProtocol(const UInt8 interfaceNumber) noexcept
{
    return Setup(0x21U, 11U, 0U, interfaceNumber, 0U);
}
SetupPacket UsbDescriptors::SetIdle(const UInt8 interfaceNumber) noexcept
{
    return Setup(0x21U, 10U, 0U, interfaceNumber, 0U);
}
} // namespace Asteria::User::Input
