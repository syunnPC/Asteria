// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/PciMessageSignaled.hpp"

namespace Asteria::Kernel::Device
{
namespace
{
constexpr Foundation::UInt16 CommandRegister{0x04U};
constexpr Foundation::UInt16 CapabilityPointer{0x34U};
constexpr Foundation::UInt32 CapabilityListStatus{Foundation::UInt32{1U} << 20U};
constexpr Foundation::UInt32 InterruptDisable{Foundation::UInt32{1U} << 10U};
constexpr Foundation::UInt8 MessageCapabilityId{0x05U};
constexpr Foundation::Size MaximumCapabilities{48U};
// Message Control is the capability dword's upper half.
constexpr Foundation::UInt32 MessageEnable{Foundation::UInt32{1U} << 16U};
constexpr Foundation::UInt32 MultipleMessageEnable{Foundation::UInt32{7U} << 20U};
constexpr Foundation::UInt32 WideAddress{Foundation::UInt32{1U} << 23U};
constexpr Foundation::UInt32 VectorMasking{Foundation::UInt32{1U} << 24U};

[[nodiscard]] bool Read(Hal::Device::IPciConfigurationSpace& reader, const PciFunctionAddress address,
                        const Foundation::UInt16 offset, Foundation::UInt32& value) noexcept
{
    return reader.Read32(address, offset, value) == Hal::Device::PciConfigurationReadError::None;
}

[[nodiscard]] bool Write(Hal::Device::IPciConfigurationWriter& writer, const PciFunctionAddress address,
                         const Foundation::UInt16 offset, const Foundation::UInt32 value) noexcept
{
    return writer.Write32(address, offset, value) == Hal::Device::PciConfigurationWriteError::None;
}

// Sets or clears the capability's enable bit, its multiple-message field
// zero; the dword's lower half (ID and next pointer) is read-only.
[[nodiscard]] bool SetEnable(Hal::Device::IPciConfigurationSpace& reader, Hal::Device::IPciConfigurationWriter& writer,
                             const PciFunctionAddress address, const PciMessageCapability& capability,
                             const bool enabled) noexcept
{
    Foundation::UInt32 header{0U};
    if (!Read(reader, address, capability.Offset, header))
    {
        return false;
    }
    header &= ~(MessageEnable | MultipleMessageEnable);
    return Write(writer, address, capability.Offset, enabled ? (header | MessageEnable) : header);
}
} // namespace

bool FindPciMessageCapability(Hal::Device::IPciConfigurationSpace& reader, const PciFunctionAddress address,
                              PciMessageCapability& capability) noexcept
{
    Foundation::UInt32 command{0U};
    Foundation::UInt32 pointer{0U};
    if (!Read(reader, address, CommandRegister, command) || ((command & CapabilityListStatus) == 0U) ||
        !Read(reader, address, CapabilityPointer, pointer))
    {
        return false;
    }
    Foundation::UInt16 offset = static_cast<Foundation::UInt16>(pointer & 0xFCU);
    for (Foundation::Size visited{0U}; (visited < MaximumCapabilities) && (offset >= 0x40U); ++visited)
    {
        Foundation::UInt32 header{0U};
        if (!Read(reader, address, offset, header))
        {
            return false;
        }
        if ((header & 0xFFU) == MessageCapabilityId)
        {
            capability = PciMessageCapability{.Offset = offset,
                                              .Wide = (header & WideAddress) != 0U,
                                              .PerVectorMask = (header & VectorMasking) != 0U,
                                              .Reserved = {}};
            return true;
        }
        offset = static_cast<Foundation::UInt16>((header >> 8U) & 0xFCU);
    }
    return false;
}

bool EnablePciMessages(Hal::Device::IPciConfigurationSpace& reader, Hal::Device::IPciConfigurationWriter& writer,
                       const PciFunctionAddress address, const PciMessageCapability& capability,
                       const Foundation::UInt64 messageAddress, const Foundation::UInt16 messageData) noexcept
{
    const auto at = [&capability](const Foundation::UInt16 offset) noexcept
    {
        return static_cast<Foundation::UInt16>(capability.Offset + offset);
    };
    const Foundation::UInt16 data = capability.Wide ? at(12U) : at(8U);
    bool written =
        SetEnable(reader, writer, address, capability, false) &&
        Write(writer, address, at(4U), static_cast<Foundation::UInt32>(messageAddress)) &&
        (!capability.Wide || Write(writer, address, at(8U), static_cast<Foundation::UInt32>(messageAddress >> 32U))) &&
        Write(writer, address, data, messageData) &&
        (!capability.PerVectorMask || Write(writer, address, static_cast<Foundation::UInt16>(data + 4U), 0U));
    Foundation::UInt32 command{0U};
    written = written && Read(reader, address, CommandRegister, command) &&
              Write(writer, address, CommandRegister, (command & 0xFFFFU) | InterruptDisable);
    return written && SetEnable(reader, writer, address, capability, true);
}

bool DisablePciMessages(Hal::Device::IPciConfigurationSpace& reader, Hal::Device::IPciConfigurationWriter& writer,
                        const PciFunctionAddress address, const PciMessageCapability& capability) noexcept
{
    return SetEnable(reader, writer, address, capability, false);
}
} // namespace Asteria::Kernel::Device
