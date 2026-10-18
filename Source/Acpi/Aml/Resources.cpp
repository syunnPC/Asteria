// SPDX-License-Identifier: Apache-2.0

#include "Acpi/Aml/Resources.hpp"

namespace Asteria::Acpi::Aml
{
namespace
{
UInt64 Little(const UInt8* const bytes, const Size width) noexcept
{
    UInt64 value{0U};
    for (Size i{0U}; i < width; ++i)
    {
        value |= static_cast<UInt64>(bytes[i]) << (i * 8U);
    }
    return value;
}
Reason Source(const Bytes payload, const Size offset, Resource& resource) noexcept
{
    if (offset == payload.Length)
    {
        return Reason::None;
    }
    if (offset > payload.Length || payload.Length - offset < 2U)
    {
        return Reason::Truncated;
    }
    resource.SourceIndex = payload.Data[offset];
    Size end{offset + 1U};
    while (end < payload.Length && payload.Data[end] != 0U)
    {
        ++end;
    }
    if (end + 1U != payload.Length)
    {
        return Reason::Malformed;
    }
    resource.Source = {payload.Data + offset + 1U, end - offset - 1U};
    return Reason::None;
}
Reason Address(const UInt8 tag, const Bytes bytes, Resource& resource) noexcept
{
    const Size width{tag == 0x88U ? 2U : tag == 0x87U ? 4U : 8U};
    const bool extended{tag == 0x8BU};
    const Size offset{extended ? 5U : 3U};
    const Size minimum{offset + width * 5U + (extended ? 8U : 0U)};
    if (bytes.Length < minimum || (extended && bytes.Length != minimum))
    {
        return Reason::Truncated;
    }
    resource.Kind = tag == 0x88U   ? ResourceKind::WordAddress
                    : tag == 0x87U ? ResourceKind::DWordAddress
                    : extended     ? ResourceKind::ExtendedAddress
                                   : ResourceKind::QWordAddress;
    resource.Space = bytes.Data[0U];
    resource.Flags = static_cast<UInt16>(bytes.Data[1U] | (static_cast<UInt16>(bytes.Data[2U]) << 8U));
    resource.Alignment = Little(bytes.Data + offset, width);
    resource.Minimum = Little(bytes.Data + offset + width, width);
    resource.Maximum = Little(bytes.Data + offset + width * 2U, width);
    resource.Translation = Little(bytes.Data + offset + width * 3U, width);
    resource.Length = Little(bytes.Data + offset + width * 4U, width);
    if (extended)
    {
        if (bytes.Data[3U] != 1U || bytes.Data[4U] != 0U)
        {
            return Reason::Malformed;
        }
        resource.Attributes = Little(bytes.Data + offset + 40U, 8U);
        return Reason::None;
    }
    return Source(bytes, minimum, resource);
}
Reason Small(const UInt8 tag, const Bytes bytes, Resource& resource) noexcept
{
    switch (tag)
    {
    case 4U:
        if (bytes.Length != 2U && bytes.Length != 3U)
        {
            return Reason::Malformed;
        }
        resource.Kind = ResourceKind::Irq;
        resource.Interrupt = static_cast<UInt32>(Little(bytes.Data, 2U));
        resource.Flags = bytes.Length == 3U ? bytes.Data[2U] : 1U;
        return Reason::None;
    case 5U:
        if (bytes.Length != 2U)
        {
            return Reason::Malformed;
        }
        resource.Kind = ResourceKind::Dma;
        resource.Interrupt = bytes.Data[0U];
        resource.Flags = bytes.Data[1U];
        return Reason::None;
    case 6U:
    case 7U:
        if ((tag == 6U && bytes.Length > 1U) || (tag == 7U && bytes.Length != 0U))
        {
            return Reason::Malformed;
        }
        resource.Kind = ResourceKind::Dependent;
        resource.Flags = bytes.Length == 0U ? 0U : bytes.Data[0U];
        return Reason::None;
    case 8U:
        if (bytes.Length != 7U)
        {
            return Reason::Malformed;
        }
        resource.Kind = ResourceKind::Io;
        resource.Flags = bytes.Data[0U];
        resource.Minimum = Little(bytes.Data + 1U, 2U);
        resource.Maximum = Little(bytes.Data + 3U, 2U);
        resource.Alignment = bytes.Data[5U];
        resource.Length = bytes.Data[6U];
        return Reason::None;
    case 9U:
        if (bytes.Length != 3U)
        {
            return Reason::Malformed;
        }
        resource.Kind = ResourceKind::FixedIo;
        resource.Minimum = Little(bytes.Data, 2U);
        resource.Maximum = resource.Minimum;
        resource.Length = bytes.Data[2U];
        return Reason::None;
    case 14U:
        resource.Kind = ResourceKind::Vendor;
        return Reason::None;
    case 15U:
        resource.Kind = ResourceKind::End;
        return bytes.Length == 1U ? Reason::None : Reason::Malformed;
    default:
        return Reason::Unsupported;
    }
}
Reason Large(const UInt8 tag, const Bytes bytes, Resource& resource) noexcept
{
    switch (tag)
    {
    case 0x81U:
    case 0x85U:
    case 0x86U:
    {
        const Size width{tag == 0x81U ? 2U : 4U};
        const bool fixed{tag == 0x86U};
        const Size expected{fixed ? 9U : 1U + width * 4U};
        if (bytes.Length != expected)
        {
            return Reason::Malformed;
        }
        resource.Kind = tag == 0x81U ? ResourceKind::Memory24
                        : fixed      ? ResourceKind::FixedMemory32
                                     : ResourceKind::Memory32;
        resource.Flags = bytes.Data[0U];
        resource.Minimum = Little(bytes.Data + 1U, width);
        resource.Maximum = fixed ? resource.Minimum : Little(bytes.Data + 1U + width, width);
        resource.Alignment = fixed ? 0U : Little(bytes.Data + 1U + width * 2U, width);
        resource.Length = Little(bytes.Data + (fixed ? 5U : 1U + width * 3U), width);
        if (tag == 0x81U)
        {
            resource.Minimum <<= 8U;
            resource.Maximum <<= 8U;
            if (resource.Alignment == 0U)
            {
                resource.Alignment = 65536U;
            }
            resource.Length <<= 8U;
        }
        return Reason::None;
    }
    case 0x84U:
        resource.Kind = ResourceKind::Vendor;
        return Reason::None;
    case 0x87U:
    case 0x88U:
    case 0x8AU:
    case 0x8BU:
        return Address(tag, bytes, resource);
    case 0x89U:
        if (bytes.Length < 2U || bytes.Data[1U] == 0U || bytes.Data[1U] > (bytes.Length - 2U) / 4U)
        {
            return Reason::Truncated;
        }
        resource.Kind = ResourceKind::ExtendedIrq;
        resource.Flags = bytes.Data[0U];
        resource.Length = bytes.Data[1U];
        resource.Interrupt = static_cast<UInt32>(Little(bytes.Data + 2U, 4U));
        return Source(bytes, 2U + Size{bytes.Data[1U]} * 4U, resource);
    case 0x8CU:
        if (bytes.Length < 20U)
        {
            return Reason::Truncated;
        }
        resource.Kind = ResourceKind::Gpio;
        return Reason::None;
    case 0x8EU:
        if (bytes.Length < 9U)
        {
            return Reason::Truncated;
        }
        resource.Kind = bytes.Data[2U] == 1U   ? ResourceKind::I2c
                        : bytes.Data[2U] == 2U ? ResourceKind::Spi
                        : bytes.Data[2U] == 3U ? ResourceKind::Uart
                                               : ResourceKind::Skipped;
        return Reason::None;
    case 0x8DU:
    case 0x8FU:
    case 0x90U:
    case 0x91U:
    case 0x92U:
        resource.Kind = ResourceKind::Pin;
        return bytes.Length == 0U ? Reason::Truncated : Reason::None;
    default:
        return Reason::Unsupported;
    }
}
} // namespace

bool ResourceDecoder::Decode(const Bytes bytes, Resources& output) noexcept
{
    output.Count = 0U;
    output.Error = Reason::None;
    output.ErrorOffset = 0U;
    if (bytes.Data == nullptr || bytes.Length == 0U || bytes.Length > MaximumObjectBytes)
    {
        output.Error = Reason::Malformed;
        return false;
    }
    Size cursor{0U};
    UInt8 checksum{0U};
    while (cursor < bytes.Length)
    {
        const Size begin{cursor};
        const UInt8 tag{bytes.Data[cursor++]};
        Size length{tag & 7U};
        const bool large{(tag & 0x80U) != 0U};
        if (large)
        {
            if (bytes.Length - cursor < 2U)
            {
                output.Error = Reason::Truncated;
                break;
            }
            length = static_cast<Size>(Little(bytes.Data + cursor, 2U));
            cursor += 2U;
        }
        if (length > bytes.Length - cursor)
        {
            output.Error = Reason::Truncated;
            break;
        }
        if (output.Count >= MaximumResources)
        {
            output.Error = Reason::Capacity;
            break;
        }
        Resource& resource{output.Items[output.Count]};
        resource = {};
        resource.Raw = {bytes.Data + begin, cursor + length - begin};
        const Bytes payload{bytes.Data + cursor, length};
        output.Error = large ? Large(tag, payload, resource) : Small(static_cast<UInt8>(tag >> 3U), payload, resource);
        if (output.Error != Reason::None)
        {
            break;
        }
        ++output.Count;
        cursor += length;
        for (Size i{begin}; i < cursor; ++i)
        {
            checksum = static_cast<UInt8>((UInt32{checksum} + bytes.Data[i]) & 0xFFU);
        }
        if (resource.Kind == ResourceKind::End)
        {
            if (payload.Data[0U] != 0U && checksum != 0U)
            {
                output.Error = Reason::Checksum;
            }
            else if (cursor != bytes.Length)
            {
                // AML BufferSize may append zero padding beyond EndTag.
                for (Size i{cursor}; i < bytes.Length; ++i)
                {
                    if (bytes.Data[i] != 0U)
                    {
                        output.Error = Reason::Malformed;
                        break;
                    }
                }
            }
            output.ErrorOffset = begin;
            return output.Error == Reason::None;
        }
        output.ErrorOffset = begin;
    }
    if (output.Error == Reason::None)
    {
        output.Error = Reason::Truncated;
    }
    output.ErrorOffset = cursor;
    return false;
}

const char* ResourceText(const ResourceKind kind) noexcept
{
    switch (kind)
    {
    case ResourceKind::Io:
        return "IO";
    case ResourceKind::FixedIo:
        return "FixedIO";
    case ResourceKind::Irq:
        return "IRQ";
    case ResourceKind::Dma:
        return "DMA";
    case ResourceKind::ExtendedIrq:
        return "ExtendedIRQ";
    case ResourceKind::Memory24:
        return "Memory24";
    case ResourceKind::Memory32:
        return "Memory32";
    case ResourceKind::FixedMemory32:
        return "Memory32Fixed";
    case ResourceKind::WordAddress:
        return "WordAddress";
    case ResourceKind::DWordAddress:
        return "DWordAddress";
    case ResourceKind::QWordAddress:
        return "QWordAddress";
    case ResourceKind::ExtendedAddress:
        return "ExtendedAddress";
    case ResourceKind::Vendor:
        return "Vendor";
    case ResourceKind::End:
        return "End";
    case ResourceKind::Gpio:
        return "GPIO (skipped)";
    case ResourceKind::I2c:
        return "I2C (skipped)";
    case ResourceKind::Spi:
        return "SPI (skipped)";
    case ResourceKind::Uart:
        return "UART (skipped)";
    case ResourceKind::Pin:
        return "Pin (skipped)";
    case ResourceKind::Dependent:
        return "Dependent";
    case ResourceKind::Skipped:
        return "Connection (skipped)";
    }
    return "invalid-resource";
}
} // namespace Asteria::Acpi::Aml
