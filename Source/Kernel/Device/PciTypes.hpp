// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_PCI_TYPES_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_PCI_TYPES_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Device
{
/// Identifies one PCI function in a segment:bus:device.function namespace.
class PciFunctionAddress final
{
public:
    constexpr PciFunctionAddress() noexcept = default;

    constexpr PciFunctionAddress(const Foundation::UInt16 segment, const Foundation::UInt8 bus,
                                 const Foundation::UInt8 device, const Foundation::UInt8 function) noexcept
        : m_segment{segment}, m_bus{bus}, m_device{device}, m_function{function}
    {
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_device <= MaximumDevice) && (m_function <= MaximumFunction);
    }

    [[nodiscard]] constexpr Foundation::UInt16 GetSegment() const noexcept
    {
        return m_segment;
    }

    [[nodiscard]] constexpr Foundation::UInt8 GetBus() const noexcept
    {
        return m_bus;
    }

    [[nodiscard]] constexpr Foundation::UInt8 GetDevice() const noexcept
    {
        return m_device;
    }

    [[nodiscard]] constexpr Foundation::UInt8 GetFunction() const noexcept
    {
        return m_function;
    }

    /// Returns the conventional bus/device/function requester identifier.
    /// Preconditions:
    /// - IsValid is true.
    [[nodiscard]] constexpr Foundation::UInt16 GetRequesterId() const noexcept
    {
        return static_cast<Foundation::UInt16>((static_cast<Foundation::UInt16>(m_bus) << 8U) |
                                               (static_cast<Foundation::UInt16>(m_device) << 3U) |
                                               static_cast<Foundation::UInt16>(m_function));
    }

    [[nodiscard]] constexpr bool IsEqualTo(const PciFunctionAddress other) const noexcept
    {
        return (m_segment == other.m_segment) && (m_bus == other.m_bus) && (m_device == other.m_device) &&
               (m_function == other.m_function);
    }

    static constexpr Foundation::UInt8 MaximumDevice{31U};
    static constexpr Foundation::UInt8 MaximumFunction{7U};

private:
    Foundation::UInt16 m_segment{0U};
    Foundation::UInt8 m_bus{0U};
    Foundation::UInt8 m_device{0xFFU};
    Foundation::UInt8 m_function{0xFFU};
    [[maybe_unused]] Foundation::UInt8 m_reserved{0U};
};

struct PciBusRange
{
    Foundation::UInt16 Segment;
    Foundation::UInt8 FirstBus;
    Foundation::UInt8 LastBus;
    Foundation::UInt8 Reserved[4];
};

struct PciBusRangeView
{
    const PciBusRange* Data;
    Foundation::Size Count;
};

struct PciFunctionDescriptor
{
    PciFunctionAddress Address;
    Foundation::UInt16 VendorId;
    Foundation::UInt16 DeviceId;
    Foundation::UInt8 RevisionId;
    Foundation::UInt8 ProgrammingInterface;
    Foundation::UInt8 Subclass;
    Foundation::UInt8 ClassCode;
    Foundation::UInt8 HeaderLayout;
    bool Multifunction;
};

static_assert(sizeof(PciFunctionAddress) == 6U);
static_assert(alignof(PciFunctionAddress) == 2U);
static_assert(sizeof(PciBusRange) == 8U);
static_assert(alignof(PciBusRange) == 2U);
static_assert(sizeof(PciBusRangeView) == 16U);
static_assert(alignof(PciBusRangeView) == 8U);
static_assert(sizeof(PciFunctionDescriptor) == 16U);
static_assert(alignof(PciFunctionDescriptor) == 2U);
static_assert(PciFunctionAddress{0xFFFFU, 0xFFU, 31U, 7U}.IsValid());
static_assert(PciFunctionAddress{0U, 0xABU, 0x1FU, 7U}.GetRequesterId() == 0xABFFU);
static_assert(!PciFunctionAddress{}.IsValid());
static_assert(!PciFunctionAddress{0U, 0U, 32U, 0U}.IsValid());
static_assert(!PciFunctionAddress{0U, 0U, 0U, 8U}.IsValid());
} // Namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_PCI_TYPES_HPP_INCLUDED
