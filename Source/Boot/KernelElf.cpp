// SPDX-License-Identifier: Apache-2.0

#include "Boot/KernelElf.hpp"

#include "Boot/BootInfo.hpp"
#include "Boot/KernelEntryAbi.hpp"
#include "Foundation/Compiler.hpp"

namespace Asteria::Boot
{
namespace
{
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::Size;
constexpr UInt64 Maximum{~UInt64{0U}};
constexpr UInt64 PageBytes{4096U};
constexpr UInt64 SlotMask{(UInt64{1U} << 41U) - 1U};
constexpr Size MaximumSections{64U};
constexpr Size MaximumPrograms{8U};
constexpr Size MaximumRelocations{65536U};
constexpr UInt64 PermissionFlags[]{5U, 4U, 6U};

[[nodiscard]] UInt64 Read(const UInt8* const bytes, const Size count) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < count; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (8U * index);
    }
    return value;
}

void Write(UInt8* const bytes, UInt64 value) noexcept
{
    for (Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<UInt8>(value & 0xFFU);
        value >>= 8U;
    }
}

[[nodiscard]] bool Within(const UInt64 offset, const UInt64 bytes, const UInt64 extent) noexcept
{
    return offset <= extent && bytes <= extent - offset;
}

[[nodiscard]] bool Overlaps(const UInt64 first, const UInt64 firstBytes, const UInt64 second,
                            const UInt64 secondBytes) noexcept
{
    return first < second + secondBytes && second < first + firstBytes;
}

[[nodiscard]] const UInt8* Section(const KernelElfPlan& plan, const UInt32 index) noexcept
{
    return plan.Source.Data + plan.SectionTableOffset + static_cast<UInt64>(index) * 64U;
}

[[nodiscard]] const UInt8* Program(const KernelElfPlan& plan, const UInt32 index) noexcept
{
    return plan.Source.Data + plan.ProgramTableOffset + static_cast<UInt64>(index) * 56U;
}

/// Returns a source borrow only if the entire range is backed by exactly one
/// load segment. No pointer is formed until file and virtual bounds are known.
[[nodiscard]] const UInt8* AtAddress(const KernelElfPlan& plan, const UInt64 address, const UInt64 bytes,
                                     const UInt32 flags) noexcept
{
    for (UInt32 index{0U}; index < plan.ProgramCount; ++index)
    {
        const UInt8* const program = Program(plan, index);
        if (Read(program, 4U) != 1U || Read(program + 4U, 4U) != flags)
        {
            continue;
        }
        const UInt64 start = Read(program + 16U, 8U);
        const UInt64 fileBytes = Read(program + 32U, 8U);
        if (address >= start && Within(address - start, bytes, fileBytes))
        {
            return plan.Source.Data + Read(program + 8U, 8U) + address - start;
        }
    }
    return nullptr;
}

[[nodiscard]] bool IsImageAddress(const KernelElfPlan& plan, const UInt64 address, const UInt64 bytes) noexcept
{
    return address >= plan.Image.PhysicalStart &&
           Within(address - plan.Image.PhysicalStart, bytes, plan.Image.SizeBytes);
}

[[nodiscard]] bool IsCode(const KernelElfPlan& plan, const UInt64 address) noexcept
{
    return (plan.Architecture != BootArchitectureIa64 || (address & 15U) == 0U) &&
           AtAddress(plan, address, 1U, 5U) != nullptr;
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool Header(const Executable::Elf::Elf64ByteView source, const UInt32 architecture,
                                                 KernelElfPlan& plan) noexcept
{
    if (source.Data == nullptr || source.SizeBytes < 64U || source.SizeBytes > BootFileMaximumBytes ||
        reinterpret_cast<Foundation::UIntPointer>(source.Data) > Maximum - source.SizeBytes)
    {
        return false;
    }
    const UInt8* const header = source.Data;
    const UInt64 machine = architecture == BootArchitectureAmd64 ? 62U : 50U;
    if (Read(header, 4U) != 0x464C457FU || header[4U] != 2U || header[5U] != 1U || header[6U] != 1U ||
        header[7U] != 0U || header[8U] != 0U || Read(header + 9U, 7U) != 0U || Read(header + 16U, 2U) != 2U ||
        Read(header + 18U, 2U) != machine || Read(header + 20U, 4U) != 1U || Read(header + 52U, 2U) != 64U ||
        Read(header + 54U, 2U) != 56U || Read(header + 58U, 2U) != 64U ||
        Read(header + 48U, 4U) != (architecture == BootArchitectureAmd64 ? 0U : 0x10U))
    {
        return false;
    }
    plan.Source = source;
    plan.Architecture = architecture;
    plan.ProgramTableOffset = Read(header + 32U, 8U);
    plan.SectionTableOffset = Read(header + 40U, 8U);
    plan.ProgramCount = static_cast<UInt16>(Read(header + 56U, 2U));
    plan.SectionCount = static_cast<UInt16>(Read(header + 60U, 2U));
    return plan.ProgramCount > 0U && plan.ProgramCount <= MaximumPrograms && plan.SectionCount > 0U &&
           plan.SectionCount <= MaximumSections && Read(header + 62U, 2U) < plan.SectionCount &&
           plan.ProgramTableOffset >= 64U && plan.SectionTableOffset >= 64U && (plan.ProgramTableOffset & 7U) == 0U &&
           (plan.SectionTableOffset & 7U) == 0U &&
           Within(plan.ProgramTableOffset, static_cast<UInt64>(plan.ProgramCount) * 56U, source.SizeBytes) &&
           Within(plan.SectionTableOffset, static_cast<UInt64>(plan.SectionCount) * 64U, source.SizeBytes) &&
           !Overlaps(plan.ProgramTableOffset, static_cast<UInt64>(plan.ProgramCount) * 56U, plan.SectionTableOffset,
                     static_cast<UInt64>(plan.SectionCount) * 64U);
}

[[nodiscard]] bool ReadNote(KernelElfPlan& plan) noexcept
{
    const UInt8* note{nullptr};
    for (UInt32 index{1U}; index < plan.SectionCount; ++index)
    {
        const UInt8* const section = Section(plan, index);
        if (Read(section + 4U, 4U) != 7U)
        {
            continue;
        }
        if (note != nullptr || Read(section + 8U, 8U) != 2U ||
            !Within(Read(section + 24U, 8U), Read(section + 32U, 8U), plan.Source.SizeBytes) ||
            Read(section + 32U, 8U) != PageBytes)
        {
            return false;
        }
        plan.NoteOffset = Read(section + 24U, 8U);
        note = plan.Source.Data + plan.NoteOffset;
    }
    if (note == nullptr || Read(note, 4U) != 8U || Read(note + 4U, 4U) != KernelElfNoteDescriptorBytes ||
        Read(note + 8U, 4U) != KernelElfNoteType || Read(note + 12U, 8U) != 0x0041495245545341ULL)
    {
        return false;
    }
    for (Size offset{20U + KernelElfNoteDescriptorBytes}; offset < PageBytes; ++offset)
    {
        if (note[offset] != 0U)
        {
            return false;
        }
    }
    const UInt8* const data = note + 20U;
    if (Read(data, 4U) != KernelEntryAbiVersion || Read(data + 4U, 4U) != BootInfoVersion ||
        Read(data + 8U, 4U) != plan.Architecture || Read(data + 88U, 8U) != 0U)
    {
        return false;
    }
    plan.Image.PhysicalStart = Read(data + 16U, 8U);
    plan.Image.SizeBytes = Read(data + 24U, 8U);
    plan.Image.AlignmentBytes = Read(data + 32U, 8U);
    plan.Image.Placement = 2U;
    plan.Image.SectionCount = static_cast<UInt32>(Read(data + 12U, 4U));
    if (plan.Image.SectionCount != (plan.Architecture == BootArchitectureAmd64 ? 7U : 6U) ||
        plan.Image.AlignmentBytes != (plan.Architecture == BootArchitectureAmd64 ? 0x200000U : 0x1000000U) ||
        plan.Image.PhysicalStart == 0U || (plan.Image.PhysicalStart & (plan.Image.AlignmentBytes - 1U)) != 0U ||
        plan.Image.SizeBytes == 0U || plan.Image.SizeBytes > 0x1000000U || (plan.Image.SizeBytes & 0xFFFU) != 0U ||
        plan.Image.PhysicalStart > Maximum - plan.Image.SizeBytes)
    {
        return false;
    }
    plan.PrepareRva = Read(data + 40U, 8U);
    plan.EnterRva = Read(data + 48U, 8U);
    plan.QueryRva = Read(data + 56U, 8U);
    plan.GpRva = Read(data + 64U, 8U);
    plan.DescriptorRva = Read(data + 72U, 8U);
    plan.DescriptorBytes = Read(data + 80U, 8U);
    for (Size index{0U}; index < BootImageMaximumSections; ++index)
    {
        const UInt8* const item = data + 96U + index * 24U;
        const UInt64 rva = Read(item, 8U);
        const UInt64 pages = Read(item + 8U, 8U);
        const UInt32 purpose = static_cast<UInt32>(Read(item + 16U, 4U));
        if (Read(item + 20U, 4U) != 0U || purpose > 2U || pages > plan.Image.SizeBytes / PageBytes ||
            !Within(rva, pages * PageBytes, plan.Image.SizeBytes) ||
            (index < plan.Image.SectionCount && ((rva & 0xFFFU) != 0U || pages == 0U)) ||
            (index >= plan.Image.SectionCount && (rva != 0U || pages != 0U || purpose != 0U)))
        {
            return false;
        }
        plan.Image.Sections[index] = {index < plan.Image.SectionCount ? plan.Image.PhysicalStart + rva : 0U, pages,
                                      purpose, 0U};
    }
    for (Size index{0U}; index < KernelElfReservationCount; ++index)
    {
        const UInt8* const item = data + 264U + index * 32U;
        plan.Reservations[index] = {Read(item, 8U), Read(item + 8U, 8U), Read(item + 16U, 8U),
                                    static_cast<UInt32>(Read(item + 24U, 4U)),
                                    static_cast<UInt32>(Read(item + 28U, 4U))};
    }
    return true;
}

[[nodiscard]] bool Reservations(const KernelElfPlan& plan) noexcept
{
    for (Size index{0U}; index < KernelElfReservationCount; ++index)
    {
        const auto& reservation = plan.Reservations[index];
        if (plan.Architecture == BootArchitectureIa64 && index == 2U)
        {
            if (reservation.PageCount != 0U || reservation.AlignmentPages != 0U || reservation.MaximumAddress != 0U ||
                reservation.Kind != 0U || reservation.Flags != 0U)
            {
                return false;
            }
            continue;
        }
        if (reservation.PageCount == 0U || reservation.PageCount > 65536U || reservation.AlignmentPages == 0U ||
            reservation.AlignmentPages > 65536U ||
            (reservation.AlignmentPages & (reservation.AlignmentPages - 1U)) != 0U ||
            reservation.MaximumAddress < reservation.PageCount * PageBytes - 1U)
        {
            return false;
        }
        if (plan.Architecture == BootArchitectureAmd64)
        {
            if (reservation.Kind != index || reservation.Flags != 0U ||
                (index == 1U && reservation.MaximumAddress > 0xFFFFFFFFU) ||
                (index == 2U && reservation.MaximumAddress > 0xFFFFFU))
            {
                return false;
            }
        }
        else if ((index == 0U && (reservation.Kind != 0U || reservation.Flags != 1U ||
                                  reservation.PageCount >= plan.Reservations[1U].PageCount)) ||
                 (index == 1U && (reservation.Kind != 3U || reservation.Flags != 2U ||
                                  reservation.PageCount != reservation.AlignmentPages)) ||
                 (index == 2U && (reservation.Kind != 0U || reservation.Flags != 1U ||
                                  reservation.PageCount != plan.Reservations[0U].PageCount)))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool Segments(const KernelElfPlan& plan) noexcept
{
    UInt64 previousEnd = plan.Image.PhysicalStart;
    Size ownership{0U};
    UInt64 ownershipStart{0U};
    for (UInt32 index{0U}; index < plan.ProgramCount; ++index)
    {
        const UInt8* const program = Program(plan, index);
        const UInt64 type = Read(program, 4U);
        if (type != 1U)
        {
            return false;
        }
        const UInt64 start = Read(program + 16U, 8U);
        const UInt64 bytes = Read(program + 40U, 8U);
        const UInt64 fileOffset = Read(program + 8U, 8U);
        const UInt64 fileBytes = Read(program + 32U, 8U);
        const UInt64 flags = Read(program + 4U, 4U);
        const UInt64 alignment = Read(program + 48U, 8U);
        if (bytes == 0U || start < previousEnd || (start & 0xFFFU) != 0U || (bytes & 0xFFFU) != 0U ||
            Read(program + 24U, 8U) != start || !IsImageAddress(plan, start, bytes) || fileBytes > bytes ||
            !Within(fileOffset, fileBytes, plan.Source.SizeBytes) || (flags != 4U && flags != 5U && flags != 6U) ||
            alignment < PageBytes || alignment > plan.Image.AlignmentBytes || (alignment & (alignment - 1U)) != 0U ||
            (fileOffset & (alignment - 1U)) != (start & (alignment - 1U)))
        {
            return false;
        }
        if (plan.Architecture == BootArchitectureIa64 &&
            (plan.ProgramCount != 3U || start - plan.Image.PhysicalStart != (index + 1U) * 0x400000U ||
             bytes != 0x400000U || flags != PermissionFlags[index]))
        {
            return false;
        }
        ownershipStart = start;
        while (ownership < plan.Image.SectionCount && ownershipStart < start + bytes)
        {
            const auto& section = plan.Image.Sections[ownership];
            const UInt64 expected = PermissionFlags[section.Purpose];
            if (section.PhysicalStart != ownershipStart || expected != flags ||
                section.PageCount * PageBytes > start + bytes - ownershipStart)
            {
                return false;
            }
            ownershipStart += section.PageCount * PageBytes;
            ++ownership;
        }
        if (ownershipStart != start + bytes)
        {
            return false;
        }
        previousEnd = start + bytes;
    }
    if (ownership != plan.Image.SectionCount || previousEnd != plan.Image.PhysicalStart + plan.Image.SizeBytes)
    {
        return false;
    }
    const UInt64 entries[]{plan.PrepareRva, plan.EnterRva, plan.QueryRva};
    for (const UInt64 entry : entries)
    {
        if (!Within(entry, 1U, plan.Image.SizeBytes))
        {
            return false;
        }
        if (plan.Architecture == BootArchitectureAmd64)
        {
            if (!IsCode(plan, plan.Image.PhysicalStart + entry))
            {
                return false;
            }
        }
        else if ((entry & 15U) != 0U || entry < plan.DescriptorRva ||
                 !Within(entry - plan.DescriptorRva, 16U, plan.DescriptorBytes))
        {
            return false;
        }
    }
    return Read(plan.Source.Data + 24U, 8U) == plan.Image.PhysicalStart + plan.EnterRva;
}

[[nodiscard]] bool Sections(const KernelElfPlan& plan) noexcept
{
    for (Size offset{0U}; offset < 64U; ++offset)
    {
        if (Section(plan, 0U)[offset] != 0U)
        {
            return false;
        }
    }
    for (UInt32 index{1U}; index < plan.SectionCount; ++index)
    {
        const UInt8* const section = Section(plan, index);
        const UInt64 type = Read(section + 4U, 4U);
        const UInt64 flags = Read(section + 8U, 8U);
        const UInt64 address = Read(section + 16U, 8U);
        const UInt64 bytes = Read(section + 32U, 8U);
        if ((flags & ~UInt64{0x47U}) != 0U || (flags & 5U) == 5U ||
            (type != 1U && type != 2U && type != 3U && type != 4U && type != 7U && type != 8U) ||
            (type != 8U && !Within(Read(section + 24U, 8U), bytes, plan.Source.SizeBytes)))
        {
            return false;
        }
        if ((flags & 2U) != 0U)
        {
            if (bytes == 0U || !IsImageAddress(plan, address, bytes))
            {
                return false;
            }
            bool contained{false};
            for (UInt32 programIndex{0U}; programIndex < plan.ProgramCount; ++programIndex)
            {
                const UInt8* const program = Program(plan, programIndex);
                const UInt64 start = Read(program + 16U, 8U);
                const UInt64 extent = Read(program + 40U, 8U);
                UInt64 expected{4U};
                if ((flags & 4U) != 0U)
                {
                    expected = 5U;
                }
                else if ((flags & 1U) != 0U)
                {
                    expected = 6U;
                }
                if (address >= start && Within(address - start, bytes, extent) && Read(program + 4U, 4U) == expected)
                {
                    contained = type == 8U || (Within(address - start, bytes, Read(program + 32U, 8U)) &&
                                               Read(section + 24U, 8U) == Read(program + 8U, 8U) + address - start);
                }
            }
            if (!contained)
            {
                return false;
            }
            for (UInt32 other{1U}; other < index; ++other)
            {
                const UInt8* const prior = Section(plan, other);
                if ((Read(prior + 8U, 8U) & 2U) != 0U &&
                    Overlaps(address, bytes, Read(prior + 16U, 8U), Read(prior + 32U, 8U)))
                {
                    return false;
                }
            }
        }
        else if (address != 0U || (flags & ~UInt64{0x40U}) != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] UInt64 Immediate(const UInt8* const bundle) noexcept
{
    const UInt64 low = Read(bundle, 8U);
    const UInt64 high = Read(bundle + 8U, 8U);
    const UInt64 slot1 = ((low >> 46U) | (high << 18U)) & SlotMask;
    const UInt64 slot2 = high >> 23U;
    return ((slot2 >> 13U) & 0x7FU) | (((slot2 >> 27U) & 0x1FFU) << 7U) | (((slot2 >> 22U) & 0x1FU) << 16U) |
           (((slot2 >> 21U) & 1U) << 21U) | (slot1 << 22U) | (((slot2 >> 36U) & 1U) << 63U);
}

void SetImmediate(UInt8* const bundle, const UInt64 value) noexcept
{
    const UInt64 low = Read(bundle, 8U);
    const UInt64 high = Read(bundle + 8U, 8U);
    const UInt64 slot1 = (value >> 22U) & SlotMask;
    UInt64 slot2 = high >> 23U;
    slot2 &= ~((UInt64{1U} << 36U) | (UInt64{0x1FFU} << 27U) | (UInt64{0x1FU} << 22U) | (UInt64{1U} << 21U) |
               (UInt64{0x7FU} << 13U));
    slot2 |= (((value >> 63U) & 1U) << 36U) | (((value >> 7U) & 0x1FFU) << 27U) | (((value >> 16U) & 0x1FU) << 22U) |
             (((value >> 21U) & 1U) << 21U) | ((value & 0x7FU) << 13U);
    Write(bundle, (low & ((UInt64{1U} << 46U) - 1U)) | (slot1 << 46U));
    Write(bundle + 8U, (slot1 >> 18U) | (slot2 << 23U));
}

[[nodiscard]] bool Descriptors(const KernelElfPlan& plan, UInt8* const destination, const UInt64 delta) noexcept
{
    if (plan.Architecture == BootArchitectureAmd64)
    {
        return plan.GpRva == 0U && plan.DescriptorRva == 0U && plan.DescriptorBytes == 0U;
    }
    if ((plan.GpRva & 7U) != 0U || !Within(plan.GpRva, 1U, plan.Image.SizeBytes) || plan.DescriptorBytes == 0U ||
        (plan.DescriptorBytes & 15U) != 0U || (plan.DescriptorRva & 15U) != 0U ||
        !Within(plan.DescriptorRva, plan.DescriptorBytes, plan.Image.SizeBytes))
    {
        return false;
    }
    const UInt8* const pairs = AtAddress(plan, plan.Image.PhysicalStart + plan.DescriptorRva, plan.DescriptorBytes, 4U);
    if (pairs == nullptr)
    {
        return false;
    }
    for (UInt64 offset{0U}; offset < plan.DescriptorBytes; offset += 16U)
    {
        const UInt64 code = Read(pairs + offset, 8U);
        const UInt64 gp = Read(pairs + offset + 8U, 8U);
        if (!IsCode(plan, code) || gp != plan.Image.PhysicalStart + plan.GpRva)
        {
            return false;
        }
        if (destination != nullptr)
        {
            // Addresses and the load delta use modulo 2^64 arithmetic. Bounds
            // on both linked and placed images prove the results do not wrap.
            Write(destination + plan.DescriptorRva + offset, code + delta);
            Write(destination + plan.DescriptorRva + offset + 8U, gp + delta);
        }
    }
    return true;
}

[[nodiscard]] bool SymbolValue(const KernelElfPlan& plan, const UInt8* const relocationSection, const UInt64 number,
                               UInt64& value) noexcept
{
    const UInt64 link = Read(relocationSection + 40U, 4U);
    if (link >= plan.SectionCount)
    {
        return false;
    }
    const UInt8* const table = Section(plan, static_cast<UInt32>(link));
    const UInt64 bytes = Read(table + 32U, 8U);
    if (Read(table + 4U, 4U) != 2U || Read(table + 56U, 8U) != 24U || bytes % 24U != 0U || number >= bytes / 24U)
    {
        return false;
    }
    const UInt8* const symbol = plan.Source.Data + Read(table + 24U, 8U) + number * 24U;
    const UInt64 sectionIndex = Read(symbol + 6U, 2U);
    value = Read(symbol + 8U, 8U);
    if (!IsImageAddress(plan, value, 0U) || sectionIndex == 0U ||
        (sectionIndex != 0xFFF1U && (sectionIndex >= plan.SectionCount ||
                                     (Read(Section(plan, static_cast<UInt32>(sectionIndex)) + 8U, 8U) & 2U) == 0U)))
    {
        return false;
    }
    return true;
}

[[nodiscard]] bool InvariantInstruction(const KernelElfPlan& plan, const UInt64 address, const UInt64 kind,
                                        const UInt64 value, const UInt8* const target) noexcept
{
    const UInt64 slot = address & 15U;
    const UInt64 bundle = address & ~UInt64{15U};
    const UInt64 start = Read(target + 16U, 8U);
    if (slot > 2U || (Read(target + 8U, 8U) & 4U) == 0U || bundle < start ||
        !Within(bundle - start, 16U, Read(target + 32U, 8U)) || !IsImageAddress(plan, value, 0U))
    {
        return false;
    }
    const UInt8* const bytes = plan.Source.Data + Read(target + 24U, 8U) + bundle - start;
    const UInt64 low = Read(bytes, 8U);
    const UInt64 high = Read(bytes + 8U, 8U);
    UInt64 instruction{high >> 23U};
    if (slot == 0U)
    {
        instruction = (low >> 5U) & SlotMask;
    }
    else if (slot == 1U)
    {
        instruction = ((low >> 46U) | (high << 18U)) & SlotMask;
    }
    const UInt64 displacement = kind == 0x49U ? value - bundle : value - plan.Image.PhysicalStart - plan.GpRva;
    if (kind == 0x2BU)
    {
        return slot == 1U && (bytes[0U] & 0x1EU) == 4U && (high >> 60U) == 6U && Immediate(bytes) == displacement;
    }
    if (kind == 0x2AU)
    {
        UInt64 encoded = ((instruction >> 13U) & 0x7FU) | (((instruction >> 27U) & 0x1FFU) << 7U) |
                         (((instruction >> 22U) & 0x1FU) << 16U) | (((instruction >> 36U) & 1U) << 21U);
        if ((encoded & (UInt64{1U} << 21U)) != 0U)
        {
            encoded |= ~((UInt64{1U} << 22U) - 1U);
        }
        return encoded == displacement;
    }
    UInt64 encoded = ((instruction >> 13U) & 0xFFFFFU) | (((instruction >> 36U) & 1U) << 20U);
    if ((encoded & (UInt64{1U} << 20U)) != 0U)
    {
        encoded |= ~((UInt64{1U} << 21U) - 1U);
    }
    return IsCode(plan, value) && (encoded << 4U) == displacement;
}

struct Fixup
{
    UInt64 Address;
    UInt64 Value;
    UInt64 OldValue;
    UInt64 Bytes;
    UInt64 Kind;
    UInt64 Symbol;
};

[[nodiscard]] bool DecodeFixup(const KernelElfPlan& plan, const UInt8* const section, const UInt8* const relocation,
                               Fixup& fixup) noexcept
{
    const UInt64 information = Read(relocation + 8U, 8U);
    fixup.Kind = information & 0xFFFFFFFFU;
    fixup.Address = Read(relocation, 8U);
    const UInt64 addend = Read(relocation + 16U, 8U);
    fixup.Bytes = 0U;
    if (plan.Architecture == BootArchitectureAmd64 && fixup.Kind == 8U)
    {
        if ((information >> 32U) != 0U)
        {
            return false;
        }
        fixup.Value = addend;
    }
    else
    {
        if (!SymbolValue(plan, section, information >> 32U, fixup.Symbol))
        {
            return false;
        }
        // ELF signed addends are decoded as two's-complement bits. Addition
        // is modulo 2^64; the resulting identity address is checked below.
        fixup.Value = fixup.Symbol + addend;
    }
    const UInt64 targetIndex = Read(section + 44U, 4U);
    if (targetIndex == 0U || targetIndex >= plan.SectionCount)
    {
        return false;
    }
    const UInt8* const target = Section(plan, static_cast<UInt32>(targetIndex));
    const UInt64 targetStart = Read(target + 16U, 8U);
    const UInt64 targetBytes = Read(target + 32U, 8U);
    const UInt64 flags = Read(target + 8U, 8U);
    if ((flags & 2U) == 0U || Read(target + 4U, 4U) != 1U)
    {
        return false;
    }
    if (plan.Architecture == BootArchitectureAmd64)
    {
        if (fixup.Kind == 1U || fixup.Kind == 8U)
        {
            fixup.Bytes = 8U;
        }
        else if (fixup.Kind == 2U || fixup.Kind == 4U)
        {
            if (fixup.Address < targetStart || !Within(fixup.Address - targetStart, 4U, targetBytes) ||
                (flags & 4U) == 0U)
            {
                return false;
            }
            const UInt8* const site = plan.Source.Data + Read(target + 24U, 8U) + fixup.Address - targetStart;
            const UInt64 displacement = fixup.Value - fixup.Address;
            const UInt64 encoded = Read(site, 4U);
            return (displacement <= 0x7FFFFFFFU || displacement >= 0xFFFFFFFF80000000ULL) &&
                   encoded == (displacement & 0xFFFFFFFFU);
        }
        else
        {
            return false;
        }
    }
    else if (fixup.Kind == 0x27U || fixup.Kind == 0x47U)
    {
        fixup.Bytes = 8U;
    }
    else if (fixup.Kind == 0x81U)
    {
        fixup.Bytes = 16U;
    }
    else if (fixup.Kind == 0x23U)
    {
        if ((fixup.Address & 15U) != 1U || (flags & 4U) == 0U)
        {
            return false;
        }
        fixup.Address &= ~UInt64{15U};
        fixup.Bytes = 16U;
    }
    else if (fixup.Kind == 0x49U || fixup.Kind == 0x2AU || fixup.Kind == 0x2BU)
    {
        return InvariantInstruction(plan, fixup.Address, fixup.Kind, fixup.Value, target);
    }
    else
    {
        return false;
    }
    if (fixup.Address < targetStart || !Within(fixup.Address - targetStart, fixup.Bytes, targetBytes) ||
        !IsImageAddress(plan, fixup.Value, 0U) ||
        (plan.Architecture == BootArchitectureIa64 && fixup.Kind != 0x23U && (fixup.Address & 7U) != 0U) ||
        (plan.Architecture == BootArchitectureIa64 &&
         Overlaps(fixup.Address - plan.Image.PhysicalStart, fixup.Bytes, plan.DescriptorRva, plan.DescriptorBytes)))
    {
        return false;
    }
    const UInt8* const site = plan.Source.Data + Read(target + 24U, 8U) + fixup.Address - targetStart;
    fixup.OldValue = fixup.Kind == 0x23U ? Immediate(site) : Read(site, 8U);
    if (plan.Architecture == BootArchitectureIa64 && fixup.Kind == 0x23U &&
        (((site[0U] & 0x1EU) != 4U) || Read(site + 8U, 8U) >> 60U != 6U))
    {
        return false;
    }
    if (plan.Architecture == BootArchitectureIa64 && fixup.Kind == 0x47U)
    {
        const UInt64 descriptor = fixup.OldValue;
        if ((descriptor & 15U) != 0U || descriptor < plan.Image.PhysicalStart + plan.DescriptorRva ||
            !Within(descriptor - plan.Image.PhysicalStart - plan.DescriptorRva, 16U, plan.DescriptorBytes))
        {
            return false;
        }
        const UInt8* const pair = AtAddress(plan, descriptor, 16U, 4U);
        return pair != nullptr && Read(pair, 8U) == fixup.Value &&
               Read(pair + 8U, 8U) == plan.Image.PhysicalStart + plan.GpRva;
    }
    return fixup.OldValue == fixup.Value &&
           (plan.Architecture != BootArchitectureIa64 || fixup.Kind != 0x81U ||
            (IsCode(plan, fixup.Value) && Read(site + 8U, 8U) == plan.Image.PhysicalStart + plan.GpRva));
}

[[nodiscard]] bool Relocations(const KernelElfPlan& plan, UInt8* const destination, const UInt64 delta) noexcept
{
    Size total{0U};
    for (UInt32 sectionIndex{1U}; sectionIndex < plan.SectionCount; ++sectionIndex)
    {
        const UInt8* const section = Section(plan, sectionIndex);
        if (Read(section + 4U, 4U) != 4U)
        {
            continue;
        }
        const UInt64 bytes = Read(section + 32U, 8U);
        const UInt64 target = Read(section + 44U, 4U);
        if (Read(section + 56U, 8U) != 24U || bytes % 24U != 0U || bytes / 24U > MaximumRelocations - total)
        {
            return false;
        }
        total += static_cast<Size>(bytes / 24U);
        for (UInt32 priorIndex{1U}; priorIndex < sectionIndex; ++priorIndex)
        {
            const UInt8* const prior = Section(plan, priorIndex);
            if (Read(prior + 4U, 4U) == 4U && Read(prior + 44U, 4U) == target)
            {
                return false;
            }
        }
        UInt64 previousEnd{0U};
        const UInt8* const table = plan.Source.Data + Read(section + 24U, 8U);
        for (UInt64 offset{0U}; offset < bytes; offset += 24U)
        {
            Fixup fixup{};
            if (!DecodeFixup(plan, section, table + offset, fixup))
            {
                return false;
            }
            if (fixup.Bytes == 0U)
            {
                continue;
            }
            if (fixup.Address < previousEnd)
            {
                return false;
            }
            previousEnd = fixup.Address + fixup.Bytes;
            if (destination != nullptr)
            {
                UInt8* const site = destination + fixup.Address - plan.Image.PhysicalStart;
                const UInt64 value = fixup.Kind == 0x47U ? fixup.OldValue : fixup.Value;
                if (plan.Architecture == BootArchitectureIa64 && fixup.Kind == 0x23U)
                {
                    SetImmediate(site, value + delta);
                }
                else
                {
                    Write(site, value + delta);
                    if (plan.Architecture == BootArchitectureIa64 && fixup.Kind == 0x81U)
                    {
                        Write(site + 8U, plan.Image.PhysicalStart + plan.GpRva + delta);
                    }
                }
            }
        }
    }
    return total != 0U;
}

[[nodiscard]] ASTERIA_NOINLINE_NOIPA KernelElfError Validate(KernelElfPlan& plan) noexcept
{
    if (!ReadNote(plan))
    {
        return KernelElfError::InvalidNote;
    }
    if (!Reservations(plan))
    {
        return KernelElfError::InvalidReservations;
    }
    if (!Segments(plan))
    {
        return KernelElfError::InvalidSegments;
    }
    if (!Sections(plan))
    {
        return KernelElfError::InvalidSections;
    }
    if (!Descriptors(plan, nullptr, 0U))
    {
        return KernelElfError::InvalidDescriptors;
    }
    if (!Relocations(plan, nullptr, 0U))
    {
        return KernelElfError::InvalidRelocations;
    }
    return KernelElfError::None;
}

void CopySegments(const KernelElfPlan& plan, UInt8* const destination) noexcept
{
    for (UInt64 offset{0U}; offset < plan.Image.SizeBytes; ++offset)
    {
        destination[offset] = 0U;
    }
    for (UInt32 index{0U}; index < plan.ProgramCount; ++index)
    {
        const UInt8* const program = Program(plan, index);
        const UInt64 rva = Read(program + 16U, 8U) - plan.Image.PhysicalStart;
        const UInt64 bytes = Read(program + 32U, 8U);
        const UInt8* const source = plan.Source.Data + Read(program + 8U, 8U);
        for (UInt64 offset{0U}; offset < bytes; ++offset)
        {
            destination[rva + offset] = source[offset];
        }
    }
}
} // namespace

ASTERIA_NOINLINE_NOIPA KernelElfError KernelElf::Parse(const Executable::Elf::Elf64ByteView source,
                                                       const UInt32 architecture, KernelElfPlan& output) noexcept
{
    if (architecture != BootArchitectureAmd64 && architecture != BootArchitectureIa64)
    {
        return KernelElfError::InvalidHeader;
    }
    const UInt64 sourceAddress = reinterpret_cast<Foundation::UIntPointer>(source.Data);
    const UInt64 outputAddress = reinterpret_cast<Foundation::UIntPointer>(&output);
    if (sourceAddress > Maximum - source.SizeBytes || outputAddress > Maximum - sizeof(output) ||
        Overlaps(sourceAddress, source.SizeBytes, outputAddress, sizeof(output)))
    {
        return KernelElfError::InvalidSource;
    }
    KernelElfPlan plan{};
    if (!Header(source, architecture, plan))
    {
        return KernelElfError::InvalidHeader;
    }
    const KernelElfError error = Validate(plan);
    if (error == KernelElfError::None)
    {
        output = plan;
    }
    return error;
}

KernelElfError KernelElf::Place(const KernelElfPlan& plan, void* const destination,
                                const Size destinationBytes) noexcept
{
    // Revalidation prevents a forged/stale plan from bypassing preflight.
    KernelElfPlan checked{};
    const KernelElfError error = Parse(plan.Source, plan.Architecture, checked);
    if (error != KernelElfError::None)
    {
        return error;
    }
    const UInt64 address = reinterpret_cast<Foundation::UIntPointer>(destination);
    const UInt64 sourceAddress = reinterpret_cast<Foundation::UIntPointer>(checked.Source.Data);
    const UInt64 planAddress = reinterpret_cast<Foundation::UIntPointer>(&plan);
    if (address == 0U || (address & (checked.Image.AlignmentBytes - 1U)) != 0U ||
        destinationBytes < checked.Image.SizeBytes || address > Maximum - destinationBytes ||
        Overlaps(address, checked.Image.SizeBytes, sourceAddress, checked.Source.SizeBytes) ||
        Overlaps(address, checked.Image.SizeBytes, planAddress, sizeof(plan)))
    {
        return KernelElfError::InvalidDestination;
    }
    auto* const bytes = static_cast<UInt8*>(destination);
    CopySegments(checked, bytes);
    // Uniform placement uses an intentional modulo-2^64 delta. Both image
    // extents were checked, so every rebased address remains in the destination.
    const UInt64 delta = address - checked.Image.PhysicalStart;
    static_cast<void>(Relocations(checked, bytes, delta));
    static_cast<void>(Descriptors(checked, bytes, delta));
    return KernelElfError::None;
}
} // namespace Asteria::Boot
