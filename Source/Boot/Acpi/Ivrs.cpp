// SPDX-License-Identifier: Apache-2.0

#include "Boot/Acpi/Ivrs.hpp"

namespace Asteria::Boot::Acpi
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt64;

[[nodiscard]] UInt64 Read(const UInt8* bytes, const Size count) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < count; ++index)
    {
        value |= UInt64{bytes[index]} << (index * 8U);
    }
    return value;
}

void Append(IvrsTopology& output, const IvrsDevice& device) noexcept
{
    if (output.DeviceCount < MaximumIvrsDevices)
    {
        output.Devices[output.DeviceCount] = device;
    }
    ++output.DeviceCount;
}

void Devices(const UInt8* bytes, const Size length, const Size header, IvrsTopology& output) noexcept
{
    IvrsDevice pending{};
    bool range{false};
    for (Size offset{header}; offset < length;)
    {
        const UInt8* entry = bytes + offset;
        const Size remaining = length - offset;
        const UInt8 type = entry[0U];
        const Size size = (type == 0xF0U) ? ((remaining < 22U) ? 22U : 22U + entry[21U]) : (Size{4U} << (type >> 6U));
        if (size > remaining)
        {
            ++output.Malformed;
            break;
        }
        if (type == 0xF0U)
        {
            ++output.SkippedHid;
            offset += size;
            continue;
        }
        IvrsDevice device{.ExtendedFlags = 0U,
                          .First = static_cast<UInt16>(Read(entry + 1U, 2U)),
                          .Last = static_cast<UInt16>(Read(entry + 1U, 2U)),
                          .Alias = 0U,
                          .Flags = entry[3U],
                          .Type = type,
                          .Handle = 0U,
                          .Variety = 0U,
                          .Reserved = 0U};
        if ((type == 0x42U) || (type == 0x43U))
        {
            device.Alias = static_cast<UInt16>(Read(entry + 5U, 2U));
        }
        if ((type == 0x46U) || (type == 0x47U))
        {
            device.ExtendedFlags = static_cast<Foundation::UInt32>(Read(entry + 4U, 4U));
        }
        switch (type)
        {
        case 0x00U:
            break;
        case 0x01U:
            device.First = 0U;
            device.Last = 0xFFFFU;
            Append(output, device);
            break;
        case 0x02U:
        case 0x42U:
        case 0x46U:
            Append(output, device);
            break;
        case 0x03U:
        case 0x43U:
        case 0x47U:
            if (range)
            {
                ++output.Malformed;
            }
            pending = device;
            range = true;
            break;
        case 0x04U:
            if (!range || (device.Last < pending.First))
            {
                ++output.Malformed;
            }
            else
            {
                pending.Last = device.Last;
                Append(output, pending);
            }
            range = false;
            break;
        case 0x48U:
            device.First = static_cast<UInt16>(Read(entry + 5U, 2U));
            device.Last = device.First;
            device.Handle = entry[4U];
            device.Variety = entry[7U];
            if ((device.Variety == 1U) || (device.Variety == 2U))
            {
                Append(output, device);
            }
            else
            {
                ++output.Malformed;
            }
            break;
        default:
            ++output.Malformed;
            break;
        }
        offset += size;
    }
    if (range)
    {
        ++output.Malformed;
    }
}

enum class MemoryBlock : UInt8
{
    Retained,
    Inert,
    Malformed,
};

// One IVMD block as ParseIvrs judges it; a Retained or Malformed block with a
// valid range fills block.
[[nodiscard]] MemoryBlock DecodeMemory(const UInt8* bytes, const Size length, IvrsMemory& block) noexcept
{
    if (length < 32U)
    {
        return MemoryBlock::Malformed;
    }
    const UInt64 rawBase = Read(bytes + 16U, 8U);
    const UInt64 rawSize = Read(bytes + 24U, 8U);
    const UInt8 flags = bytes[1U];
    const bool inert = ((flags & 9U) == 0U) || (((flags & 8U) == 0U) && ((flags & 6U) == 0U));
    // AMD 48882 rev 3.09 section 5.2.2.2, table 109: start is a byte
    // address; length is in bytes and may be rounded to a 4-Kbyte boundary.
    // Cover the complete firmware interval with its enclosing pages: never
    // move the first requested byte upward or round length independently.
    if ((rawSize == 0U) || ((rawSize - 1U) > (~UInt64{0U} - rawBase)))
    {
        return inert ? MemoryBlock::Inert : MemoryBlock::Malformed;
    }
    const UInt16 first = (bytes[0U] == 0x20U) ? UInt16{0U} : static_cast<UInt16>(Read(bytes + 4U, 2U));
    const UInt16 last = (bytes[0U] == 0x20U)
                            ? UInt16{0xFFFFU}
                            : ((bytes[0U] == 0x21U) ? first : static_cast<UInt16>(Read(bytes + 6U, 2U)));
    block = {.Base = rawBase & ~UInt64{0xFFFU},
             .Limit = (rawBase + (rawSize - 1U)) | UInt64{0xFFFU},
             .First = first,
             .Last = last,
             .Segment = static_cast<UInt16>(Read(bytes + 8U, 2U)),
             .Flags = flags,
             .Type = bytes[0U]};
    if (inert)
    {
        return MemoryBlock::Inert;
    }
    if ((first > last) || ((flags & 0xF0U) != 0U) || ((flags & 9U) == 0U) ||
        (((flags & 1U) != 0U) && ((flags & 6U) == 0U)))
    {
        return MemoryBlock::Malformed;
    }
    return MemoryBlock::Retained;
}

void Memory(const UInt8* bytes, const Size length, IvrsTopology& output) noexcept
{
    IvrsMemory block{};
    switch (DecodeMemory(bytes, length, block))
    {
    case MemoryBlock::Inert:
        return;
    case MemoryBlock::Malformed:
        ++output.Malformed;
        return;
    case MemoryBlock::Retained:
        break;
    }
    if (output.MemoryCount < MaximumIvrsMemory)
    {
        output.Memory[output.MemoryCount] = block;
    }
    ++output.MemoryCount;
}
} // namespace

void CollectIvrsExclusions(const UInt8* const bytes, const Size length, BootDmaExclusions& output) noexcept
{
    if ((bytes == nullptr) || (length < 48U) || (length > 65536U))
    {
        return;
    }
    for (Size offset{48U}; offset < length;)
    {
        if (length - offset < 4U)
        {
            return;
        }
        const UInt8* const block = bytes + offset;
        const Size size = static_cast<Size>(Read(block + 2U, 2U));
        if ((size < 4U) || (size > length - offset))
        {
            return;
        }
        offset += size;
        if ((block[0U] < 0x20U) || (block[0U] > 0x22U))
        {
            continue;
        }
        ++output.DeclaredCount;
        // The rounding and the checks ParseIvrs applies: an inert or invalid
        // block is still a range firmware declared, when it has one.
        IvrsMemory range{};
        const MemoryBlock judged = DecodeMemory(block, size, range);
        if ((range.Limit == 0U) || (output.Count >= BootDmaExclusionMaximumCount))
        {
            continue;
        }
        output.Ranges[output.Count] = BootDmaExclusion{
            .Base = range.Base,
            .Limit = range.Limit,
            .Segment = range.Segment,
            .Kind = BootDmaExclusionIvmd,
            .Malformed = static_cast<UInt8>((judged == MemoryBlock::Retained) ? 0U : 1U),
            .Reserved = 0U,
        };
        ++output.Count;
    }
}

bool ParseIvrs(const UInt8* const bytes, const Size length, IvrsTopology& output) noexcept
{
    output = {};
    if ((bytes == nullptr) || (length < 48U) || (length > 65536U) || (bytes[0U] != 'I') || (bytes[1U] != 'V') ||
        (bytes[2U] != 'R') || (bytes[3U] != 'S') || (Read(bytes + 4U, 4U) != length) || (bytes[8U] == 0U))
    {
        return false;
    }
    UInt8 checksum{0U};
    for (Size index{0U}; index < length; ++index)
    {
        checksum = static_cast<UInt8>(checksum + bytes[index]);
    }
    if (checksum != 0U)
    {
        return false;
    }
    // Validate framing before publishing anything.
    for (Size offset{48U}; offset < length;)
    {
        if (length - offset < 4U)
        {
            return false;
        }
        const Size size = static_cast<Size>(Read(bytes + offset + 2U, 2U));
        if ((size < 4U) || (size > length - offset))
        {
            return false;
        }
        offset += size;
    }
    Foundation::UInt32 imageMalformed{0U};
    Foundation::UInt32 imageHid{0U};
    output.Information = static_cast<Foundation::UInt32>(Read(bytes + 36U, 4U));
    for (Size offset{48U}; offset < length;)
    {
        const UInt8* block = bytes + offset;
        const Size size = static_cast<Size>(Read(block + 2U, 2U));
        const UInt8 type = block[0U];
        if ((type == 0x10U) || (type == 0x11U) || (type == 0x40U))
        {
            const Size header = (type == 0x10U) ? 24U : 40U;
            if (size < header)
            {
                ++output.Malformed;
                offset += size;
                continue;
            }
            const UInt64 base = Read(block + 8U, 8U);
            const UInt16 segment = static_cast<UInt16>(Read(block + 16U, 2U));
            const UInt16 device = static_cast<UInt16>(Read(block + 4U, 2U));
            if ((output.UnitCount != 0U) &&
                ((output.RegisterBase != base) || (output.Segment != segment) || (output.IommuDevice != device)))
            {
                ++output.UnitCount;
            }
            else if ((output.UnitCount != 0U) && (type == output.Type))
            {
                ++output.Malformed;
            }
            else if ((output.UnitCount == 0U) || (type > output.Type))
            {
                if (output.UnitCount == 0U)
                {
                    output.UnitCount = 1U;
                }
                output.RegisterBase = base;
                output.Segment = segment;
                output.IommuDevice = device;
                output.CapabilityOffset = static_cast<UInt16>(Read(block + 6U, 2U));
                output.IommuInformation = static_cast<UInt16>(Read(block + 18U, 2U));
                output.Type = type;
                output.Flags = block[1U];
                output.ExtendedFeatures = (header == 40U) ? Read(block + 24U, 8U) : 0U;
                output.ExtendedFeatures2 = (header == 40U) ? Read(block + 32U, 8U) : 0U;
                output.DeviceCount = 0U;
                output.Malformed -= imageMalformed;
                output.SkippedHid -= imageHid;
                const Foundation::UInt32 before = output.Malformed;
                const Foundation::UInt32 hidBefore = output.SkippedHid;
                Devices(block, size, header, output);
                imageMalformed = output.Malformed - before;
                imageHid = output.SkippedHid - hidBefore;
            }
        }
        else if ((type >= 0x20U) && (type <= 0x22U))
        {
            Memory(block, size, output);
        }
        offset += size;
    }
    return true;
}
} // namespace Asteria::Boot::Acpi
