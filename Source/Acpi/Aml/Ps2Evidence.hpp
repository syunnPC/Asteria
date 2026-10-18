// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ACPI_AML_PS2_EVIDENCE_HPP_INCLUDED
#define ASTERIA_ACPI_AML_PS2_EVIDENCE_HPP_INCLUDED

#include "Acpi/Aml/Facts.hpp"

namespace Asteria::Acpi::Aml
{
enum class Ps2DeviceKind : UInt8
{
    Any,
    Keyboard,
    Mouse,
};
enum class Ps2Condition : UInt8
{
    Qualified,
    Identifier,
    Status,
    ResourcesUnknown,
    ResourceKind,
    Ports,
    Interrupt,
};
struct Ps2Declaration
{
    UInt32 Identifier{0U};
    Ps2Condition Condition{Ps2Condition::Identifier};
    bool Candidate{false};
    bool Mouse{false};
    bool StaUnknown{false};
    /// The signaling of the first descriptor naming the required IRQ (ACPI
    /// 6.5 6.4.2.1 _HE/_LL, 6.4.3.6); edge, active-high is the IRQ default.
    bool InterruptLevel{false};
    bool InterruptActiveLow{false};
};
/// ACPI 6.5 6.1/6.3/6.4 declaration policy, AML boot facts. Pure, bounded, allocation-free.
/// Resource spans must be decoder-produced.
class Ps2Evidence final
{
public:
    [[nodiscard]] static Ps2Declaration Derive(const Library& library, const DeviceFacts& device,
                                               const Ps2DeviceKind kind = Ps2DeviceKind::Any) noexcept
    {
        Ps2Declaration result{};
        const Identifiers* const sets[]{&device.Hid, &device.Cid};
        for (const Identifiers* ids : sets)
        {
            for (Size index{0U}; index < ids->Count; ++index)
            {
                const Identifier& id = ids->Items[index];
                const Bytes bytes = library.Data(id.Value);
                const char* text = id.Eisa[0U] != '\0' ? id.Eisa : nullptr;
                char normalized[8U]{};
                if (text == nullptr && bytes.Length == 7U)
                {
                    for (Size byte{0U}; byte < 7U; ++byte)
                    {
                        normalized[byte] = static_cast<char>(bytes.Data[byte]);
                    }
                    text = normalized;
                }
                if (text == nullptr || text[0U] != 'P' || text[1U] != 'N' || text[2U] != 'P')
                {
                    continue;
                }
                UInt32 number{0U};
                bool valid{true};
                for (Size digit{3U}; digit < 7U; ++digit)
                {
                    const char value = text[digit];
                    if (value >= '0' && value <= '9')
                    {
                        number = (number << 4U) | static_cast<UInt32>(value - '0');
                    }
                    else if (value >= 'A' && value <= 'F')
                    {
                        number = (number << 4U) | static_cast<UInt32>(value - 'A' + 10);
                    }
                    else
                    {
                        valid = false;
                    }
                }
                if (!valid)
                {
                    continue;
                }
                const bool keyboardFamily = (number & 0xFF00U) == 0x0300U;
                const bool mouseFamily = (number & 0xFF00U) == 0x0F00U;
                result.Candidate = result.Candidate || (keyboardFamily && kind != Ps2DeviceKind::Mouse) ||
                                   (mouseFamily && kind != Ps2DeviceKind::Keyboard);
                const bool keyboardId = (number >= 0x0300U && number <= 0x030BU) || number == 0x0320U;
                const bool mouseId = number == 0x0F03U || number == 0x0F13U;
                if (result.Identifier == 0U &&
                    ((keyboardId && kind != Ps2DeviceKind::Mouse) || (mouseId && kind != Ps2DeviceKind::Keyboard)))
                {
                    result.Identifier = number;
                    result.Mouse = number == 0x0F03U || number == 0x0F13U;
                }
            }
        }
        if (result.Identifier == 0U)
        {
            return result;
        }
        result.StaUnknown = device.Sta == Presence::Unknown;
        if (device.Sta == Presence::Absent)
        {
            result.Condition = Ps2Condition::Status;
            return result;
        }
        if (device.Crs.Error != Reason::None)
        {
            result.Condition = Ps2Condition::ResourcesUnknown;
            return result;
        }
        bool data{false};
        bool command{false};
        bool interrupt{false};
        const UInt32 required = result.Mouse ? 12U : 1U;
        for (Size index{0U}; index < device.Crs.Count; ++index)
        {
            const Resource& resource = device.Crs.Items[index];
            switch (resource.Kind)
            {
            case ResourceKind::Io:
            case ResourceKind::FixedIo:
                if (resource.Minimum != resource.Maximum || resource.Length != 1U ||
                    (resource.Minimum != 0x60U && resource.Minimum != 0x64U))
                {
                    result.Condition = Ps2Condition::Ports;
                    return result;
                }
                data = data || resource.Minimum == 0x60U;
                command = command || resource.Minimum == 0x64U;
                break;
            case ResourceKind::Irq:
                if (!interrupt && (resource.Interrupt & (UInt32{1U} << required)) != 0U)
                {
                    interrupt = true;
                    result.InterruptLevel = (resource.Flags & 1U) == 0U;
                    result.InterruptActiveLow = (resource.Flags & 8U) != 0U;
                }
                break;
            case ResourceKind::ExtendedIrq:
                for (Size irq{0U}; irq < resource.Length; ++irq)
                {
                    UInt32 number{0U};
                    for (Size byte{0U}; byte < 4U; ++byte)
                    {
                        number |= UInt32{resource.Raw.Data[5U + irq * 4U + byte]} << (byte * 8U);
                    }
                    if (!interrupt && number == required)
                    {
                        interrupt = true;
                        result.InterruptLevel = (resource.Flags & 2U) == 0U;
                        result.InterruptActiveLow = (resource.Flags & 4U) != 0U;
                    }
                }
                break;
            case ResourceKind::Vendor:
            case ResourceKind::End:
                break;
            default:
                result.Condition = Ps2Condition::ResourceKind;
                return result;
            }
        }
        result.Condition = interrupt ? Ps2Condition::Qualified : Ps2Condition::Interrupt;
        if (!result.Mouse && (!data || !command))
        {
            result.Condition = Ps2Condition::Ports;
        }
        return result;
    }
    [[nodiscard]] static const char* ConditionText(const Ps2Condition condition) noexcept
    {
        switch (condition)
        {
        case Ps2Condition::Qualified:
            return "qualified";
        case Ps2Condition::Identifier:
            return "identifier";
        case Ps2Condition::Status:
            return "status-absent-or-disabled";
        case Ps2Condition::ResourcesUnknown:
            return "resources-unknown";
        case Ps2Condition::ResourceKind:
            return "resource-kind";
        case Ps2Condition::Ports:
            return "ports";
        case Ps2Condition::Interrupt:
            return "interrupt";
        }
        return "identifier";
    }
};
}
#endif
