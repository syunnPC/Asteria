// SPDX-License-Identifier: Apache-2.0

#include "User/Usb/Scsi.hpp"

namespace Asteria::User::Usb
{
namespace
{
UInt64 Big(const UInt8* bytes, Size count) noexcept
{
    UInt64 result{0U};
    for (Size i{0U}; i < count; ++i)
    {
        result = (result << 8U) | bytes[i];
    }
    return result;
}
void Put(UInt8* bytes, UInt32 value, Size count) noexcept
{
    for (Size i{0U}; i < count; ++i)
    {
        bytes[count - i - 1U] = static_cast<UInt8>((value >> (8U * i)) & 255U);
    }
}
ScsiCommand Command(UInt8 opcode, UInt8 length, UInt32 transfer) noexcept
{
    ScsiCommand result{};
    result.Bytes[0U] = opcode;
    result.Length = length;
    result.TransferLength = transfer;
    return result;
}
SenseStatus Classify(UInt8 key, UInt8 asc, UInt8 ascq) noexcept
{
    if (key == 0U)
    {
        return SenseStatus::Good;
    }
    if (key == 2U)
    {
        if ((asc == 4U) && (ascq == 1U))
        {
            return SenseStatus::BecomingReady;
        }
        if (asc == 0x3AU)
        {
            return SenseStatus::NoMedium;
        }
        return SenseStatus::NotReady;
    }
    if (key == 6U)
    {
        if (asc == 0x28U)
        {
            return SenseStatus::MediumChanged;
        }
        if (asc == 0x29U)
        {
            return SenseStatus::Reset;
        }
    }
    if (key == 5U)
    {
        return SenseStatus::IllegalRequest;
    }
    if (key == 7U)
    {
        return SenseStatus::WriteProtected;
    }
    if (key == 3U)
    {
        return SenseStatus::MediumError;
    }
    return SenseStatus::Error;
}
}
bool Scsi::DecodeSense(const UInt8* bytes, Size length, Sense& output) noexcept
{
    if ((bytes == nullptr) || (length < 8U))
    {
        return false;
    }
    const UInt8 format = bytes[0U] & 0x7FU;
    if (static_cast<Size>(bytes[7U]) > length - 8U)
    {
        return false;
    }
    Sense result{};
    if ((format == 0x70U) || (format == 0x71U))
    {
        if ((length < 14U) || (bytes[7U] < 6U))
        {
            return false;
        }
        result.Key = bytes[2U] & 15U;
        result.Asc = bytes[12U];
        result.Ascq = bytes[13U];
    }
    else if ((format == 0x72U) || (format == 0x73U))
    {
        result.Key = bytes[1U] & 15U;
        result.Asc = bytes[2U];
        result.Ascq = bytes[3U];
        // Validate descriptor boundaries, though v0 consumes no descriptors.
        const Size end = 8U + bytes[7U];
        for (Size at{8U}; at < end;)
        {
            if ((end - at < 2U) || (static_cast<Size>(bytes[at + 1U]) > end - at - 2U))
            {
                return false;
            }
            at += 2U + bytes[at + 1U];
        }
    }
    else
    {
        return false;
    }
    if ((format == 0x70U) || (format == 0x72U))
    {
        result.Status = Classify(result.Key, result.Asc, result.Ascq);
    }
    output = result;
    return true;
}
bool Scsi::DecodeInquiry(const UInt8* bytes, Size length, Inquiry& output) noexcept
{
    if ((bytes == nullptr) || (length < 36U) || (bytes[4U] < 31U) || ((bytes[0U] & 0xE0U) != 0U))
    {
        return false;
    }
    Inquiry result{};
    result.PeripheralType = bytes[0U] & 31U;
    result.Removable = (bytes[1U] & 0x80U) != 0U;
    for (Size i{0U}; i < 24U; ++i)
    {
        const UInt8 value = bytes[8U + i];
        if ((value < 32U) || (value > 126U))
        {
            return false;
        }
        if (i < 8U)
        {
            result.Vendor[i] = static_cast<char>(value);
        }
        else
        {
            result.Product[i - 8U] = static_cast<char>(value);
        }
    }
    output = result;
    return true;
}
bool Scsi::Capacity10(const UInt8* bytes, Size length, Capacity& output) noexcept
{
    if ((bytes == nullptr) || (length < 8U))
    {
        return false;
    }
    const UInt64 last = Big(bytes, 4U);
    const UInt32 size = static_cast<UInt32>(Big(bytes + 4U, 4U));
    if (size == 0U)
    {
        return false;
    }
    output = {.BlockCount = last + 1U, .BlockSize = size, .Saturated = last == 0xFFFFFFFFU};
    return true;
}
bool Scsi::Capacity16(const UInt8* bytes, Size length, Capacity& output) noexcept
{
    if ((bytes == nullptr) || (length < 32U))
    {
        return false;
    }
    const UInt64 last = Big(bytes, 8U);
    const UInt32 size = static_cast<UInt32>(Big(bytes + 8U, 4U));
    if ((last == ~UInt64{0U}) || (size == 0U))
    {
        return false;
    }
    output = {.BlockCount = last + 1U, .BlockSize = size, .Saturated = false};
    return true;
}
bool Scsi::ModeSense(const UInt8* bytes, Size length, bool ten, bool& readOnly) noexcept
{
    const Size header = ten ? 8U : 4U;
    if ((bytes == nullptr) || (length < header))
    {
        return false;
    }
    const UInt64 total = ten ? Big(bytes, 2U) + 2U : static_cast<UInt64>(bytes[0U]) + 1U;
    const UInt64 descriptors = ten ? Big(bytes + 6U, 2U) : bytes[3U];
    // MODE SENSE may truncate pages to allocation length; header and advertised
    // descriptor region must still lie in its declared response.
    if ((total < header) || (descriptors > total - header))
    {
        return false;
    }
    readOnly = (bytes[ten ? 3U : 2U] & 0x80U) != 0U;
    return true;
}
ScsiCommand Scsi::InquiryCommand() noexcept
{
    ScsiCommand c = Command(0x12U, 6U, 36U);
    c.Bytes[4U] = 36U;
    return c;
}
ScsiCommand Scsi::ReadyCommand() noexcept
{
    return Command(0U, 6U, 0U);
}
ScsiCommand Scsi::SenseCommand() noexcept
{
    ScsiCommand c = Command(3U, 6U, 64U);
    c.Bytes[4U] = 64U;
    return c;
}
ScsiCommand Scsi::CapacityCommand(bool sixteen) noexcept
{
    ScsiCommand c = Command(sixteen ? 0x9EU : 0x25U, sixteen ? 16U : 10U, sixteen ? 32U : 8U);
    if (sixteen)
    {
        c.Bytes[1U] = 0x10U;
        c.Bytes[13U] = 32U;
    }
    return c;
}
ScsiCommand Scsi::ModeCommand(bool ten) noexcept
{
    ScsiCommand c = Command(ten ? 0x5AU : 0x1AU, ten ? 10U : 6U, 64U);
    c.Bytes[1U] = 8U;    // DBD: no block descriptors needed.
    c.Bytes[2U] = 0x3FU; // Current values, all pages.
    c.Bytes[ten ? 8U : 4U] = 64U;
    return c;
}
ScsiCommand Scsi::FlushCommand() noexcept
{
    return Command(0x35U, 10U, 0U);
}
bool Scsi::RangeCommand(bool write, UInt64 lba, UInt32 blocks, FrameBuffer buffer, UInt32 maximumBytes,
                        ScsiCommand& output) noexcept
{
    if ((blocks == 0U) || (blocks > 128U) || (lba > 0xFFFFFFFFU) || (blocks - 1U > 0xFFFFFFFFU - lba) ||
        (blocks * 512U > maximumBytes) || (blocks * 512U > buffer.Length) ||
        (buffer.Offset > ~UInt64{0U} - blocks * 512U))
    {
        return false;
    }
    ScsiCommand c = Command(write ? 0x2AU : 0x28U, 10U, blocks * 512U);
    c.In = !write;
    c.UseFrame = true;
    c.Buffer = buffer;
    Put(c.Bytes + 2U, static_cast<UInt32>(lba), 4U);
    Put(c.Bytes + 7U, blocks, 2U);
    output = c;
    return true;
}
}
