// SPDX-License-Identifier: Apache-2.0

#include "User/Input/UsbConfiguration.hpp"

namespace Asteria::User::Input
{
namespace
{
[[nodiscard]] UInt16 Little(const UInt8* const bytes) noexcept
{
    return static_cast<UInt16>(UInt16{bytes[0U]} | (UInt16{bytes[1U]} << 8U));
}
[[nodiscard]] DescriptorError Validate(const UInt8* const bytes, const Size total) noexcept
{
    Size interfaceAt{0U};
    Size endpointAt{0U};
    Size endpoints{0U};
    Size defaults{0U};
    bool hid{false};
    for (Size at{9U}; at < total; at += bytes[at])
    {
        if (total - at < 2U)
        {
            return DescriptorError::Truncated;
        }
        const Size count = bytes[at];
        const UInt8 type = bytes[at + 1U];
        if ((count < 2U) || (count > total - at))
        {
            return DescriptorError::Length;
        }
        if (type == 4U)
        {
            if (count != 9U)
            {
                return DescriptorError::Length;
            }
            if ((interfaceAt != 0U) && (endpoints != bytes[interfaceAt + 4U]))
            {
                return DescriptorError::EndpointCount;
            }
            bool hasDefault{false};
            for (Size previous{9U}; previous < at; previous += bytes[previous])
            {
                if ((bytes[previous + 1U] == 4U) && (bytes[previous + 2U] == bytes[at + 2U]))
                {
                    if (bytes[previous + 3U] == bytes[at + 3U])
                    {
                        return DescriptorError::DuplicateInterface;
                    }
                    hasDefault = hasDefault || (bytes[previous + 3U] == 0U);
                }
            }
            if (bytes[at + 3U] == 0U)
            {
                ++defaults;
            }
            else if (!hasDefault)
            {
                return DescriptorError::MissingDefault;
            }
            interfaceAt = at;
            endpointAt = 0U;
            endpoints = 0U;
            hid = false;
        }
        else if (type == 5U)
        {
            if (interfaceAt == 0U)
            {
                return DescriptorError::Orphan;
            }
            if ((count != 7U) && (count != 9U))
            {
                return DescriptorError::Length;
            }
            const UInt8 address = bytes[at + 2U];
            const UInt8 attributes = bytes[at + 3U];
            const UInt16 packet = Little(bytes + at + 4U);
            if (((address & 0x70U) != 0U) || ((address & 15U) == 0U) || ((attributes & 3U) == 0U) ||
                (((attributes & 3U) != 1U) && ((attributes & 0xFCU) != 0U)) || ((attributes & 0xC0U) != 0U) ||
                ((packet & 0xE000U) != 0U) || ((packet & 0x7FFU) == 0U) || ((packet & 0x7FFU) > 1024U) ||
                ((packet & 0x1800U) == 0x1800U) || (((attributes & 3U) == 2U) && ((packet & 0x1800U) != 0U)) ||
                ((((attributes & 3U) == 3U) || ((attributes & 3U) == 1U)) && (bytes[at + 6U] == 0U)))
            {
                return DescriptorError::Endpoint;
            }
            Size owner{0U};
            for (Size previous{9U}; previous < at; previous += bytes[previous])
            {
                if (bytes[previous + 1U] == 4U)
                {
                    owner = previous;
                }
                if ((bytes[previous + 1U] == 5U) && (bytes[previous + 2U] == address))
                {
                    // Endpoint addresses may be reused only by alternates of the same interface.
                    if ((owner == interfaceAt) || (bytes[owner + 2U] != bytes[interfaceAt + 2U]))
                    {
                        return DescriptorError::DuplicateEndpoint;
                    }
                }
            }
            endpointAt = at;
            ++endpoints;
        }
        else if ((type == 0x21U) && ((interfaceAt == 0U) || (bytes[interfaceAt + 5U] == 3U)))
        {
            if ((interfaceAt == 0U) || hid || (count < 9U) || (bytes[at + 5U] == 0U) ||
                (count != 6U + 3U * bytes[at + 5U]))
            {
                return DescriptorError::Hid;
            }
            bool report{false};
            for (Size entry{6U}; entry < count; entry += 3U)
            {
                if (Little(bytes + at + entry + 1U) == 0U)
                {
                    return DescriptorError::Hid;
                }
                if (bytes[at + entry] == 0x22U)
                {
                    if (report)
                    {
                        return DescriptorError::Hid;
                    }
                    report = true;
                }
            }
            hid = true;
        }
        else if (type == 0x30U)
        {
            if ((count != 6U) || (endpointAt == 0U) || (endpointAt + bytes[endpointAt] != at))
            {
                return DescriptorError::Companion;
            }
            const UInt8 attributes = bytes[at + 3U];
            const UInt8 transferType = bytes[endpointAt + 3U] & 3U;
            const UInt16 payload = Little(bytes + at + 4U);
            if ((bytes[at + 2U] > 15U) || ((transferType == 2U) && ((attributes > 16U) || (payload != 0U))) ||
                ((transferType == 3U) && ((attributes != 0U) || (payload == 0U))) ||
                ((transferType == 1U) &&
                 (((attributes & 0xFCU) != 0U) || ((attributes & 3U) == 3U) || (payload == 0U))))
            {
                return DescriptorError::Companion;
            }
            endpointAt = 0U;
        }
        else if (type == 0x0BU)
        {
            if ((count != 8U) || (bytes[at + 3U] == 0U) || (UInt16{bytes[at + 2U]} + bytes[at + 3U] > 256U))
            {
                return DescriptorError::Association;
            }
        }
        else if ((type == 1U) || (type == 2U))
        {
            return DescriptorError::Header;
        }
    }
    if ((interfaceAt == 0U) || (endpoints != bytes[interfaceAt + 4U]))
    {
        return DescriptorError::EndpointCount;
    }
    if (defaults != bytes[4U])
    {
        return DescriptorError::InterfaceCount;
    }
    return DescriptorError::None;
}
[[nodiscard]] DescriptorError Associations(const UInt8* const bytes, const Size total) noexcept
{
    for (Size at{9U}; at < total; at += bytes[at])
    {
        if (bytes[at + 1U] != 0x0BU)
        {
            continue;
        }
        const UInt16 first = bytes[at + 2U];
        const UInt16 end = static_cast<UInt16>(first + bytes[at + 3U]);
        for (UInt16 number{first}; number < end; ++number)
        {
            bool found{false};
            for (Size scan{9U}; scan < total; scan += bytes[scan])
            {
                if ((bytes[scan + 1U] == 4U) && (bytes[scan + 2U] == number) && (bytes[scan + 3U] == 0U))
                {
                    found = scan > at;
                }
                if ((scan < at) && (bytes[scan + 1U] == 0x0BU) && (number >= bytes[scan + 2U]) &&
                    (number < UInt16{bytes[scan + 2U]} + bytes[scan + 3U]))
                {
                    return DescriptorError::Association;
                }
            }
            if (!found)
            {
                return DescriptorError::Association;
            }
        }
    }
    return DescriptorError::None;
}
} // namespace

DescriptorError UsbConfiguration::Parse(const UInt8* const bytes, const Size length, UsbConfiguration& output) noexcept
{
    if (bytes == nullptr)
    {
        return DescriptorError::Null;
    }
    ConfigurationHeader header{};
    if (!UsbDescriptors::Configuration(bytes, length, header))
    {
        return DescriptorError::Header;
    }
    if (header.TotalLength > length)
    {
        return DescriptorError::Truncated;
    }
    const DescriptorError valid = Validate(bytes, header.TotalLength);
    if (valid != DescriptorError::None)
    {
        return valid;
    }
    const DescriptorError association = Associations(bytes, header.TotalLength);
    if (association != DescriptorError::None)
    {
        return association;
    }
    UsbConfiguration candidate{};
    candidate.m_bytes = bytes;
    candidate.m_header = header;
    for (Size at{9U}; at < header.TotalLength; at += bytes[at])
    {
        candidate.m_interfaces += bytes[at + 1U] == 4U ? 1U : 0U;
        candidate.m_associations += bytes[at + 1U] == 0x0BU ? 1U : 0U;
    }
    output = candidate;
    return DescriptorError::None;
}
bool UsbConfiguration::Interface(const Size index, InterfaceDescriptor& output) const noexcept
{
    if (index >= m_interfaces)
    {
        return false;
    }
    Size current{0U};
    for (Size at{9U}; at < m_header.TotalLength; at += m_bytes[at])
    {
        if ((m_bytes[at + 1U] != 4U) || (current++ != index))
        {
            continue;
        }
        InterfaceDescriptor candidate{.Number = m_bytes[at + 2U],
                                      .Alternate = m_bytes[at + 3U],
                                      .EndpointCount = m_bytes[at + 4U],
                                      .Class = m_bytes[at + 5U],
                                      .Subclass = m_bytes[at + 6U],
                                      .Protocol = m_bytes[at + 7U],
                                      .StringIndex = m_bytes[at + 8U],
                                      .Hid = {},
                                      .HasHid = false,
                                      .Offset = at,
                                      .End = m_header.TotalLength,
                                      .HidOffset = 0U};
        for (Size scan{at + 9U}; scan < m_header.TotalLength; scan += m_bytes[scan])
        {
            if (m_bytes[scan + 1U] == 4U)
            {
                candidate.End = scan;
                break;
            }
            if ((candidate.Class == 3U) && (m_bytes[scan + 1U] == 0x21U))
            {
                candidate.HasHid = true;
                candidate.HidOffset = scan;
                candidate.Hid = {.Version = Little(m_bytes + scan + 2U),
                                 .Country = m_bytes[scan + 4U],
                                 .DescriptorCount = m_bytes[scan + 5U],
                                 .ReportLength = 0U};
                for (Size entry{6U}; entry < m_bytes[scan]; entry += 3U)
                {
                    if (m_bytes[scan + entry] == 0x22U)
                    {
                        candidate.Hid.ReportLength = Little(m_bytes + scan + entry + 1U);
                    }
                }
            }
        }
        output = candidate;
        return true;
    }
    return false;
}
bool UsbConfiguration::Endpoint(const InterfaceDescriptor& interface, const Size index,
                                EndpointDescriptor& output) const noexcept
{
    // Reject forged/stale interface views before using their offsets.
    InterfaceDescriptor owner{};
    bool found{false};
    for (Size i{0U}; i < m_interfaces; ++i)
    {
        if (Interface(i, owner) && (owner.Offset == interface.Offset) && (owner.End == interface.End))
        {
            found = true;
            break;
        }
    }
    if (!found || (index >= owner.EndpointCount))
    {
        return false;
    }
    Size current{0U};
    for (Size at{owner.Offset + 9U}; at < owner.End; at += m_bytes[at])
    {
        if ((m_bytes[at + 1U] != 5U) || (current++ != index))
        {
            continue;
        }
        EndpointDescriptor candidate{.Address = m_bytes[at + 2U],
                                     .Attributes = m_bytes[at + 3U],
                                     .MaxPacketSize = Little(m_bytes + at + 4U),
                                     .Interval = m_bytes[at + 6U],
                                     .MaxBurst = 0U,
                                     .CompanionAttributes = 0U,
                                     .BytesPerInterval = 0U,
                                     .HasCompanion = false};
        const Size next = at + m_bytes[at];
        if ((next < owner.End) && (m_bytes[next + 1U] == 0x30U))
        {
            candidate.HasCompanion = true;
            candidate.MaxBurst = m_bytes[next + 2U];
            candidate.CompanionAttributes = m_bytes[next + 3U];
            candidate.BytesPerInterval = Little(m_bytes + next + 4U);
        }
        output = candidate;
        return true;
    }
    return false;
}
bool UsbConfiguration::HidEntry(const InterfaceDescriptor& interface, const Size index,
                                HidClassEntry& output) const noexcept
{
    for (Size i{0U}; i < m_interfaces; ++i)
    {
        InterfaceDescriptor owner{};
        if (Interface(i, owner) && (owner.Offset == interface.Offset) && (owner.End == interface.End))
        {
            if (!owner.HasHid || (index >= owner.Hid.DescriptorCount))
            {
                return false;
            }
            const Size at = owner.HidOffset + 6U + index * 3U;
            output = {.Type = m_bytes[at], .Length = Little(m_bytes + at + 1U)};
            return true;
        }
    }
    return false;
}
bool UsbConfiguration::Association(const Size index, InterfaceAssociation& output) const noexcept
{
    if (index >= m_associations)
    {
        return false;
    }
    Size current{0U};
    for (Size at{9U}; at < m_header.TotalLength; at += m_bytes[at])
    {
        if ((m_bytes[at + 1U] == 0x0BU) && (current++ == index))
        {
            output = {.First = m_bytes[at + 2U],
                      .Count = m_bytes[at + 3U],
                      .Class = m_bytes[at + 4U],
                      .Subclass = m_bytes[at + 5U],
                      .Protocol = m_bytes[at + 6U],
                      .StringIndex = m_bytes[at + 7U]};
            return true;
        }
    }
    return false;
}
BindingError UsbConfiguration::Bind(const Size index, InterfaceBinding& output) const noexcept
{
    InterfaceDescriptor interface{};
    if (!Interface(index, interface))
    {
        return BindingError::Unsupported;
    }
    if (interface.Alternate != 0U)
    {
        return BindingError::Alternate;
    }
    InterfaceClass kind{InterfaceClass::Unsupported};
    if ((interface.Class == 3U) && (interface.Subclass == 1U))
    {
        if (interface.Protocol == 1U)
        {
            kind = InterfaceClass::BootKeyboard;
        }
        else if (interface.Protocol == 2U)
        {
            kind = InterfaceClass::BootMouse;
        }
    }
    else if (interface.Class == 3U && interface.Subclass == 0U && interface.Protocol == 0U)
    {
        kind = InterfaceClass::ReportMouse;
    }
    else if ((interface.Class == 8U) && (interface.Subclass == 6U) && (interface.Protocol == 0x50U))
    {
        kind = InterfaceClass::MassStorage;
    }
    if (kind == InterfaceClass::Unsupported)
    {
        return BindingError::Unsupported;
    }
    if ((kind != InterfaceClass::MassStorage) && (!interface.HasHid || (interface.Hid.ReportLength == 0U)))
    {
        return BindingError::MissingHid;
    }
    InterfaceBinding candidate{
        .Class = kind, .Number = interface.Number, .In = {}, .Out = {}, .ReportLength = interface.Hid.ReportLength};
    for (Size i{0U}; i < interface.EndpointCount; ++i)
    {
        EndpointDescriptor endpoint{};
        if (!Endpoint(interface, i, endpoint))
        {
            return BindingError::MissingEndpoint;
        }
        const bool in = (endpoint.Address & 0x80U) != 0U;
        const UInt8 type = endpoint.Attributes & 3U;
        if (((kind == InterfaceClass::MassStorage) && (type == 2U)) ||
            ((kind != InterfaceClass::MassStorage) && (type == 3U) && in))
        {
            EndpointDescriptor& selected = in ? candidate.In : candidate.Out;
            if (selected.Address != 0U)
            {
                return BindingError::AmbiguousEndpoint;
            }
            selected = endpoint;
        }
    }
    if ((candidate.In.Address == 0U) || ((kind == InterfaceClass::MassStorage) && (candidate.Out.Address == 0U)))
    {
        return BindingError::MissingEndpoint;
    }
    const UInt16 minimum = kind == InterfaceClass::BootKeyboard ? 8U : kind == InterfaceClass::ReportMouse ? 1U : 3U;
    if ((kind != InterfaceClass::MassStorage) && ((candidate.In.MaxPacketSize & 0x7FFU) < minimum))
    {
        return BindingError::PacketSize;
    }
    output = candidate;
    return BindingError::None;
}
} // namespace Asteria::User::Input
