// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_INPUT_USBDESCRIPTORS_HPP_INCLUDED
#define ASTERIA_USER_INPUT_USBDESCRIPTORS_HPP_INCLUDED

#include "User/Input/InputEvent.hpp"

namespace Asteria::User::Input
{
inline constexpr Size MaximumConfigurationBytes{1024U};
inline constexpr Size MaximumInterruptBytes{1024U};
enum class UsbSpeed : UInt8
{
    Low,
    Full,
    High,
    Super
};
enum class BootProtocol : UInt8
{
    Keyboard = 1U,
    Mouse = 2U
};
enum class DescriptorKind : UInt8
{
    Device = 1U,
    Configuration = 2U
};

struct DeviceDescriptor
{
    UInt16 UsbVersion;
    UInt16 Vendor;
    UInt16 Product;
    UInt8 DeviceClass;
    UInt8 PacketSize0;
    UInt8 ConfigurationCount;
};
struct ConfigurationHeader
{
    UInt16 TotalLength;
    UInt8 InterfaceCount;
    UInt8 Value;
};
/// Raw endpoint fields, including high-speed transaction bits. The adapter
/// builds endpoint contexts/EDs/QHs from these, the port speed and EP0 size.
struct EndpointDescriptor
{
    UInt8 Address;
    UInt8 Attributes;
    UInt16 MaxPacketSize;
    UInt8 Interval;
    UInt8 MaxBurst;
    UInt8 CompanionAttributes;
    UInt16 BytesPerInterval;
    bool HasCompanion;
};
struct BootInterface
{
    UInt8 Configuration;
    UInt8 Number;
    BootProtocol Protocol;
    UInt16 ReportDescriptorLength;
    EndpointDescriptor Endpoint;
};

/// Explicit byte representation of the eight-byte little-endian setup packet.
struct SetupPacket
{
    UInt8 Bytes[8U];
};
static_assert(sizeof(SetupPacket) == 8U);
static_assert(alignof(SetupPacket) == 1U);

/// All parsers are bounded, allocation-free and failure-atomic. Null inputs
/// reject. Extra buffer bytes beyond wTotalLength are not part of the set.
class UsbDescriptors final
{
public:
    [[nodiscard]] static bool Device(const UInt8* bytes, Size length, DeviceDescriptor& output) noexcept;
    [[nodiscard]] static bool Configuration(const UInt8* bytes, Size length, ConfigurationHeader& output) noexcept;
    [[nodiscard]] static bool FindBoot(const UInt8* bytes, Size length, BootInterface& output) noexcept;
    [[nodiscard]] static bool PacketSize0(UInt8 encoded, UsbSpeed speed, UInt16& output) noexcept;
    [[nodiscard]] static SetupPacket GetDescriptor(DescriptorKind kind, UInt8 index, UInt16 length) noexcept;
    [[nodiscard]] static SetupPacket SetAddress(UInt8 address) noexcept;
    [[nodiscard]] static SetupPacket SetConfiguration(UInt8 value) noexcept;
    [[nodiscard]] static SetupPacket SetProtocol(UInt8 interfaceNumber) noexcept;
    [[nodiscard]] static SetupPacket SetIdle(UInt8 interfaceNumber) noexcept;
};
} // namespace Asteria::User::Input

#endif
