// SPDX-License-Identifier: Apache-2.0

#include "User/Loader/ExecutableFormats.hpp"

namespace Asteria::User::Loader
{
namespace
{
// Multiplication and division use constants that compile to shifts and adds.
constexpr Foundation::UInt64 PageBytes{4096U};

// ELF (the System V gABI): identification, the ELF64 header and program
// headers.
constexpr Foundation::Size ElfIdentityBytes{16U};
constexpr Foundation::Size Elf32HeaderBytes{52U};
constexpr Foundation::Size Elf64HeaderBytes{64U};
constexpr Foundation::Size ProgramHeaderBytes{56U};
constexpr Foundation::UInt8 ElfClass32{1U};
constexpr Foundation::UInt8 ElfClass64{2U};
constexpr Foundation::UInt8 ElfDataLittle{1U};
constexpr Foundation::UInt8 ElfDataBig{2U};
constexpr Foundation::UInt8 ElfCurrentVersion{1U};
constexpr Foundation::UInt8 ElfSystemVAbi{0U};
constexpr Foundation::UInt16 ElfExecutable{2U};
constexpr Foundation::UInt16 ElfShared{3U};
constexpr Foundation::UInt16 ElfMachineIa32{3U};
constexpr Foundation::UInt16 ElfMachineIa64{50U};
constexpr Foundation::UInt16 ElfMachineAmd64{62U};
constexpr Foundation::UInt16 ElfMachineAArch64{183U};
// EF_IA_64_ABI64: the LP64 ABI, which every IA-64 image here uses.
constexpr Foundation::UInt32 ElfIa64Abi64{0x10U};
constexpr Foundation::UInt32 LoadSegment{1U};
constexpr Foundation::UInt32 DynamicSegment{2U};
constexpr Foundation::UInt32 InterpreterSegment{3U};
constexpr Foundation::UInt32 ReadFlag{4U};
constexpr Foundation::UInt32 WriteFlag{2U};
constexpr Foundation::UInt32 ExecuteFlag{1U};
constexpr Foundation::UInt64 DescriptorBytes{16U};
constexpr Foundation::UInt64 BundleBytes{16U};

// PE (the Microsoft PE/COFF specification): the MZ stub's e_lfanew, the
// signature, the COFF header and the optional header's magic and subsystem.
constexpr Foundation::Size PeHeaderOffsetField{0x3CU};
constexpr Foundation::Size CoffHeaderBytes{20U};
constexpr Foundation::UInt16 PeMagic32{0x10BU};
constexpr Foundation::UInt16 PeMagic32Plus{0x20BU};
constexpr Foundation::Size PeSubsystemOffset{68U};
constexpr Foundation::UInt16 PeMachineIa32{0x14CU};
constexpr Foundation::UInt16 PeMachineIa64{0x200U};
constexpr Foundation::UInt16 PeMachineAmd64{0x8664U};
constexpr Foundation::UInt16 PeMachineAArch64{0xAA64U};

[[nodiscard]] Foundation::UInt64 Load(const Foundation::UInt8* const bytes, const Foundation::Size width,
                                      const bool big = false) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < width; ++index)
    {
        const Foundation::Size shift = big ? ((width - 1U - index) << 3U) : (index << 3U);
        value |= static_cast<Foundation::UInt64>(bytes[index]) << shift;
    }
    return value;
}

[[nodiscard]] ExecutableMachine ElfMachine(const Foundation::UInt64 code) noexcept
{
    switch (code)
    {
    case ElfMachineAmd64:
        return ExecutableMachine::Amd64;
    case ElfMachineIa64:
        return ExecutableMachine::Ia64;
    case ElfMachineIa32:
        return ExecutableMachine::Ia32;
    case ElfMachineAArch64:
        return ExecutableMachine::AArch64;
    default:
        return ExecutableMachine::Unknown;
    }
}

[[nodiscard]] ExecutableMachine PeMachine(const Foundation::UInt64 code) noexcept
{
    switch (code)
    {
    case PeMachineAmd64:
        return ExecutableMachine::Amd64;
    case PeMachineIa64:
        return ExecutableMachine::Ia64;
    case PeMachineIa32:
        return ExecutableMachine::Ia32;
    case PeMachineAArch64:
        return ExecutableMachine::AArch64;
    default:
        return ExecutableMachine::Unknown;
    }
}

// An ELF image of either class and byte order: its identification, type,
// machine and version. Only executables (ET_EXEC, ET_DYN) are recognized.
LoaderError ProbeElf(const Foundation::UInt8* const image, const Foundation::Size sizeBytes,
                     ExecutableIdentity& identity) noexcept
{
    if ((sizeBytes < ElfIdentityBytes) || (image[0U] != 0x7FU) || (image[1U] != 'E') || (image[2U] != 'L') ||
        (image[3U] != 'F'))
    {
        return LoaderError::UnsupportedFormat;
    }
    const Foundation::UInt8 elfClass = image[4U];
    const Foundation::UInt8 data = image[5U];
    if (((elfClass != ElfClass32) && (elfClass != ElfClass64)) || ((data != ElfDataLittle) && (data != ElfDataBig)) ||
        (image[6U] != ElfCurrentVersion) ||
        (sizeBytes < ((elfClass == ElfClass64) ? Elf64HeaderBytes : Elf32HeaderBytes)))
    {
        return LoaderError::Malformed;
    }
    const bool big = data == ElfDataBig;
    const Foundation::UInt64 type = Load(image + 16U, 2U, big);
    const Foundation::UInt64 machine = Load(image + 18U, 2U, big);
    if (Load(image + 20U, 4U, big) != ElfCurrentVersion)
    {
        return LoaderError::Malformed;
    }
    if ((type != ElfExecutable) && (type != ElfShared))
    {
        return LoaderError::UnsupportedFormat;
    }
    identity = ExecutableIdentity{
        .Format = (elfClass == ElfClass64) ? ExecutableFormat::Elf64 : ExecutableFormat::Elf32,
        .Machine = ElfMachine(machine),
        .ByteOrder = big ? ExecutableByteOrder::Big : ExecutableByteOrder::Little,
        .Abi = (image[7U] == ElfSystemVAbi) ? ExecutableAbi::Asteria : ExecutableAbi::Foreign,
        .Kind = (type == ElfExecutable) ? ExecutableKind::Fixed : ExecutableKind::Relocatable,
        .MachineCode = static_cast<Foundation::UInt16>(machine),
        .AbiCode = image[7U],
    };
    return LoaderError::None;
}

// Decodes one PT_LOAD into segment under rules; load false for a PT_LOAD
// with no memory bytes, which maps nothing (a script's.bss segment where
// an image has none).
[[nodiscard]] LoaderError DecodeSegment(const Foundation::UInt8* const header, const Foundation::Size imageBytes,
                                        const MachineRules& rules, const bool relocatable, bool& load,
                                        ExecutableSegment& segment) noexcept
{
    const auto flags = static_cast<Foundation::UInt32>(Load(header + 4U, 4U));
    const Foundation::UInt64 offset = Load(header + 8U, 8U);
    const Foundation::UInt64 address = Load(header + 16U, 8U);
    const Foundation::UInt64 fileBytes = Load(header + 32U, 8U);
    const Foundation::UInt64 memoryBytes = Load(header + 40U, 8U);
    const Foundation::UInt64 pageOffset = address % PageBytes;
    const Foundation::UInt64 alignment = Load(header + 48U, 8U);
    if ((alignment > 1U) &&
        (((alignment & (alignment - 1U)) != 0U) || ((address & (alignment - 1U)) != (offset & (alignment - 1U)))))
    {
        return LoaderError::Malformed;
    }
    load = memoryBytes != 0U;
    if (!load)
    {
        return (fileBytes == 0U) ? LoaderError::None : LoaderError::Malformed;
    }
    const bool writable = (flags & WriteFlag) != 0U;
    const bool executable = (flags & ExecuteFlag) != 0U;
    // A segment with no file bytes has no file offset to check: a linker
    // may place it past the file's end.
    const bool zeroFill = fileBytes == 0U;
    if ((fileBytes > memoryBytes) ||
        (!zeroFill &&
         ((offset > imageBytes) || (fileBytes > (imageBytes - offset)) || ((offset % PageBytes) != pageOffset))) ||
        (!relocatable && (address < rules.UserStart)) || (address >= rules.UserLimit) ||
        (memoryBytes > (rules.UserLimit - address)) ||
        (memoryBytes > (~Foundation::UInt64{0U} - pageOffset - PageBytes + 1U)))
    {
        return LoaderError::Malformed;
    }
    if (((flags & ReadFlag) == 0U) || (writable && executable) || (writable && !zeroFill && !rules.WritableFileBytes))
    {
        return LoaderError::UnsupportedFormat;
    }
    const Foundation::UInt64 pages = (pageOffset + memoryBytes + PageBytes - 1U) / PageBytes;
    if (pages * PageBytes > ~Foundation::UInt64{0U} - (address - pageOffset))
    {
        return LoaderError::Malformed;
    }
    if (pages > rules.MaximumSegmentPages)
    {
        return LoaderError::ImageTooLarge;
    }
    segment = ExecutableSegment{
        .VirtualStart = address - pageOffset,
        .Pages = pages,
        .PageOffset = pageOffset,
        .FileOffset = zeroFill ? 0U : offset,
        .FileBytes = fileBytes,
        .Writable = writable,
        .Executable = executable,
        .MemoryBytes = memoryBytes,
    };
    return LoaderError::None;
}

// Adds a disjoint segment within the rules' count; fixed component policy
// also retains its single writable segment restriction.
[[nodiscard]] LoaderError AddSegment(const ExecutableSegment& segment, const MachineRules& rules,
                                     ExecutableImage& output) noexcept
{
    if ((output.SegmentCount >= rules.MaximumSegments) || (output.SegmentCount >= MaximumExecutableSegments))
    {
        return LoaderError::ImageTooLarge;
    }
    const Foundation::UInt64 end = segment.VirtualStart + (segment.Pages * PageBytes);
    for (Foundation::Size other{0U}; other < output.SegmentCount; ++other)
    {
        const ExecutableSegment& earlier = output.Segments[other];
        if ((segment.VirtualStart < (earlier.VirtualStart + (earlier.Pages * PageBytes))) &&
            (earlier.VirtualStart < end))
        {
            return LoaderError::Malformed;
        }
        if (segment.Writable && earlier.Writable && (output.Identity.Kind == ExecutableKind::Fixed))
        {
            return LoaderError::UnsupportedFormat;
        }
    }
    output.Segments[output.SegmentCount] = segment;
    ++output.SegmentCount;
    return LoaderError::None;
}

[[nodiscard]] bool IsExecutableAt(const ExecutableImage& image, const Foundation::UInt64 address) noexcept
{
    for (Foundation::Size index{0U}; index < image.SegmentCount; ++index)
    {
        const ExecutableSegment& segment = image.Segments[index];
        if (segment.Executable && (address >= segment.VirtualStart) &&
            ((address - segment.VirtualStart) < (segment.Pages * PageBytes)))
        {
            return true;
        }
    }
    return false;
}

// The ELF entry names a {ip, gp} descriptor: reads it from the segment
// whose file bytes hold it, and requires ip to be a bundle in executable
// text.
[[nodiscard]] bool ReadEntryDescriptor(const Foundation::UInt8* const image, ExecutableImage& output) noexcept
{
    const Foundation::UInt64 descriptor = output.Entry;
    if ((descriptor % sizeof(Foundation::UInt64)) != 0U)
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < output.SegmentCount; ++index)
    {
        const ExecutableSegment& segment = output.Segments[index];
        const Foundation::UInt64 fileStart = segment.VirtualStart + segment.PageOffset;
        if ((descriptor < fileStart) || ((descriptor - fileStart) > segment.FileBytes) ||
            ((segment.FileBytes - (descriptor - fileStart)) < DescriptorBytes))
        {
            continue;
        }
        const Foundation::UInt8* const bytes = image + segment.FileOffset + (descriptor - fileStart);
        output.Entry = Load(bytes, 8U);
        output.GlobalPointer = Load(bytes + 8U, 8U);
        return ((output.Entry % BundleBytes) == 0U) && IsExecutableAt(output, output.Entry);
    }
    return false;
}

constexpr Foundation::UInt64 MaximumDynamicEntries{4096U};
constexpr Foundation::UInt64 MaximumRelocations{65536U};
constexpr Foundation::UInt32 Amd64Relative{8U};
constexpr Foundation::UInt32 Ia64Relative{0x6FU};
constexpr Foundation::UInt32 Ia64Direct{0x27U};
constexpr Foundation::UInt32 Ia64FunctionPointer{0x47U};
constexpr Foundation::UInt32 Ia64Descriptor{0x81U};

[[nodiscard]] bool Contains(const Foundation::UInt64 start, const Foundation::UInt64 bytes,
                            const Foundation::UInt64 address, const Foundation::UInt64 count) noexcept
{
    return (address >= start) && ((address - start) <= bytes) && (count <= (bytes - (address - start)));
}

[[nodiscard]] bool FileAt(const ExecutableImage& description, const Foundation::UInt64 address,
                          const Foundation::UInt64 count, Foundation::UInt64& offset) noexcept
{
    for (Foundation::Size index{0U}; index < description.SegmentCount; ++index)
    {
        const ExecutableSegment& segment = description.Segments[index];
        const Foundation::UInt64 start = segment.VirtualStart + segment.PageOffset;
        if (Contains(start, segment.FileBytes, address, count))
        {
            offset = segment.FileOffset + (address - start);
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool MemoryAt(const ExecutableImage& description, const Foundation::UInt64 address,
                            const Foundation::UInt64 count, Foundation::Size& index) noexcept
{
    for (index = 0U; index < description.SegmentCount; ++index)
    {
        const ExecutableSegment& segment = description.Segments[index];
        if (Contains(segment.VirtualStart + segment.PageOffset, segment.MemoryBytes, address, count))
        {
            return true;
        }
    }
    return false;
}

struct DynamicValues
{
    Foundation::UInt64 Rela{};
    Foundation::UInt64 RelaBytes{};
    Foundation::UInt64 RelaEntry{};
    Foundation::UInt64 Plt{};
    Foundation::UInt64 PltBytes{};
    Foundation::UInt64 PltKind{};
    Foundation::UInt64 SymbolEntry{};
    Foundation::UInt64 Seen{};
    bool Now{};
};

// Tags are validated as integers; unknown tags cannot enable unimplemented
// linking, constructors, symbol versions, RELR, audit hooks or TLS behavior.
[[nodiscard]] LoaderError DynamicTag(const Foundation::UInt8* const image, const Foundation::UInt64 tag,
                                     const Foundation::UInt64 value, DynamicValues& values,
                                     ExecutableImage& output) noexcept
{
    Foundation::UInt64 offset{0U};
    Foundation::UInt64* destination{nullptr};
    Foundation::UInt64 bit{0U};
    switch (tag)
    {
    case 7U:
        destination = &values.Rela;
        bit = 1U;
        break;
    case 8U:
        destination = &values.RelaBytes;
        bit = 2U;
        break;
    case 9U:
        destination = &values.RelaEntry;
        bit = 4U;
        break;
    case 2U:
        destination = &values.PltBytes;
        bit = 8U;
        break;
    case 20U:
        destination = &values.PltKind;
        bit = 16U;
        break;
    case 23U:
        destination = &values.Plt;
        bit = 32U;
        break;
    case 6U:
        destination = &output.Relocations.SymbolAddress;
        bit = 64U;
        break;
    case 5U:
        destination = &output.Relocations.StringAddress;
        bit = 128U;
        break;
    case 10U:
        destination = &output.Relocations.StringBytes;
        bit = 256U;
        break;
    case 11U:
        destination = &values.SymbolEntry;
        bit = 512U;
        break;
    case 0x6FFF'FFF9U:
        destination = &output.Relocations.RelativeCount;
        bit = 1024U;
        break;
    case 3U:
        if (!FileAt(output, value, 8U, offset))
        {
            return LoaderError::Malformed;
        }
        if (output.Identity.Machine == ExecutableMachine::Ia64)
        {
            destination = &output.GlobalPointer;
            bit = 2048U;
        }
        break;
    case 30U:
        if ((value & ~Foundation::UInt64{8U}) != 0U)
        {
            return LoaderError::UnsupportedFormat;
        }
        values.Now = values.Now || ((value & 8U) != 0U);
        bit = 4096U;
        break;
    case 0x6FFF'FFFBU:
        if ((value & ~Foundation::UInt64{0x0800'0001U}) != 0U)
        {
            return LoaderError::UnsupportedFormat;
        }
        values.Now = values.Now || ((value & 1U) != 0U);
        bit = 8192U;
        break;
    case 24U: // DT_BIND_NOW
        values.Now = true;
        bit = 16384U;
        break;
    case 21U: // DT_DEBUG is never filled by this out-of-process loader.
        return (value == 0U) ? LoaderError::None : LoaderError::UnsupportedFormat;
    case 4U: // Unused SysV hash; validate its entire declared extent.
        if (!FileAt(output, value, 8U, offset) ||
            !FileAt(output, value, 8U + 4U * (Load(image + offset, 4U) + Load(image + offset + 4U, 4U)), offset))
        {
            return LoaderError::Malformed;
        }
        break;
    case 0x6FFF'FEF5U: // Unused GNU hash: header, bloom and bucket arrays.
        if (!FileAt(output, value, 16U, offset) ||
            !FileAt(output, value, 16U + 8U * Load(image + offset + 8U, 4U) + 4U * Load(image + offset, 4U), offset))
        {
            return LoaderError::Malformed;
        }
        break;
    case 0x7000'0000U: // DT_IA_64_PLT_RESERVE, three reserved GOT words.
        if ((output.Identity.Machine != ExecutableMachine::Ia64) || !FileAt(output, value, 24U, offset))
        {
            return LoaderError::Malformed;
        }
        break;
    case 1U:  // DT_NEEDED
    case 17U: // DT_REL
    case 18U: // DT_RELSZ
    case 19U: // DT_RELENT
    case 22U: // DT_TEXTREL
    default:
        return LoaderError::UnsupportedFormat;
    }
    if ((values.Seen & bit) != 0U)
    {
        return LoaderError::Malformed;
    }
    values.Seen |= bit;
    if (destination != nullptr)
    {
        *destination = value;
    }
    return LoaderError::None;
}

[[nodiscard]] LoaderError FinishDynamic(const DynamicValues& values, ExecutableImage& output) noexcept
{
    if (((values.Seen & 7U) != 0U) && (((values.Seen & 7U) != 7U) || (values.RelaEntry != 24U)))
    {
        return LoaderError::Malformed;
    }
    if (((values.Seen & 56U) != 0U) && ((values.Seen & 56U) != 56U))
    {
        return LoaderError::Malformed;
    }
    if (((values.Seen & 56U) != 0U) && ((values.PltKind != 7U) || ((values.PltBytes != 0U) && !values.Now)))
    {
        return LoaderError::UnsupportedFormat;
    }
    if ((values.RelaBytes % 24U != 0U) || (values.PltBytes % 24U != 0U) ||
        (values.RelaBytes / 24U > MaximumRelocations) ||
        (values.PltBytes / 24U > MaximumRelocations - values.RelaBytes / 24U) ||
        (output.Relocations.RelativeCount > values.RelaBytes / 24U))
    {
        return LoaderError::Malformed;
    }
    Foundation::UInt64 offset{0U};
    if (((values.Seen & 7U) != 0U) && !FileAt(output, values.Rela, values.RelaBytes, offset))
    {
        return LoaderError::Malformed;
    }
    output.Relocations.FileOffset = offset;
    output.Relocations.Bytes = values.RelaBytes;
    if (((values.Seen & 56U) != 0U) && !FileAt(output, values.Plt, values.PltBytes, offset))
    {
        return LoaderError::Malformed;
    }
    output.Relocations.PltFileOffset = (values.PltBytes == 0U) ? 0U : offset;
    output.Relocations.PltBytes = values.PltBytes;
    if ((values.PltBytes != 0U) && (values.RelaBytes != 0U) &&
        (output.Relocations.FileOffset < offset + values.PltBytes) &&
        (offset < output.Relocations.FileOffset + values.RelaBytes))
    {
        return LoaderError::Malformed;
    }
    if (((values.Seen & 576U) != 0U) && (((values.Seen & 576U) != 576U) || (values.SymbolEntry != 24U) ||
                                         !FileAt(output, output.Relocations.SymbolAddress, 24U, offset)))
    {
        return LoaderError::Malformed;
    }
    if (((values.Seen & 384U) != 0U) &&
        (((values.Seen & 384U) != 384U) ||
         !FileAt(output, output.Relocations.StringAddress, output.Relocations.StringBytes, offset)))
    {
        return LoaderError::Malformed;
    }
    output.Relocations.Kind = 7U;
    return LoaderError::None;
}

[[nodiscard]] LoaderError ParseDynamic(const Foundation::UInt8* const image, const Foundation::UInt8* const header,
                                       const Foundation::Size sizeBytes, ExecutableImage& output) noexcept
{
    const Foundation::UInt64 offset = Load(header + 8U, 8U);
    const Foundation::UInt64 address = Load(header + 16U, 8U);
    const Foundation::UInt64 bytes = Load(header + 32U, 8U);
    const Foundation::UInt64 memory = Load(header + 40U, 8U);
    Foundation::UInt64 mapped{0U};
    if ((bytes == 0U) || (bytes % 16U != 0U) || (bytes / 16U > MaximumDynamicEntries) || (bytes > memory) ||
        (offset > sizeBytes) || (bytes > sizeBytes - offset) || !FileAt(output, address, memory, mapped) ||
        (mapped != offset))
    {
        return LoaderError::Malformed;
    }
    DynamicValues values{};
    for (Foundation::UInt64 index{0U}; index < bytes; index += 16U)
    {
        const Foundation::UInt64 tag = Load(image + offset + index, 8U);
        if (tag == 0U)
        {
            return FinishDynamic(values, output);
        }
        const LoaderError error = DynamicTag(image, tag, Load(image + offset + index + 8U, 8U), values, output);
        if (error != LoaderError::None)
        {
            return error;
        }
    }
    return LoaderError::Malformed; // Missing DT_NULL inside PT_DYNAMIC.
}

struct RelocationWord
{
    Foundation::UInt64 Address{};
    Foundation::UInt64 Value{};
    Foundation::UInt64 Symbol{};
    Foundation::UInt64 Code{};
    Foundation::UInt32 Type{};
    Foundation::Size Segment{};
    Foundation::Size Bytes{8U};
};

[[nodiscard]] bool AddSigned(const Foundation::UInt64 value, const Foundation::UInt64 addend,
                             Foundation::UInt64& result) noexcept
{
    // Decode two's-complement addends without an out-of-range signed conversion.
    if ((addend >> 63U) == 0U)
    {
        if (addend > ~Foundation::UInt64{0U} - value)
        {
            return false;
        }
        result = value + addend;
        return true;
    }
    const Foundation::UInt64 magnitude = (~addend) + 1U;
    if (magnitude > value)
    {
        return false;
    }
    result = value - magnitude;
    return true;
}

[[nodiscard]] bool ReadLocalSymbol(const Foundation::UInt8* const image, const ExecutableImage& description,
                                   const Foundation::UInt64 number, Foundation::UInt64& value,
                                   const bool function) noexcept
{
    const ExecutableRelocations& tables = description.Relocations;
    Foundation::UInt64 offset{0U};
    if ((number == 0U) || (tables.SymbolAddress == 0U) || (number > MaximumRelocations) ||
        (number * 24U > ~Foundation::UInt64{0U} - tables.SymbolAddress) ||
        !FileAt(description, tables.SymbolAddress + number * 24U, 24U, offset))
    {
        return false;
    }
    const Foundation::UInt8* const symbol = image + offset;
    const Foundation::UInt64 section = Load(symbol + 6U, 2U);
    const Foundation::UInt8 binding = symbol[4U] >> 4U;
    const Foundation::UInt8 visibility = symbol[5U] & 3U;
    // Only defined, local/non-preemptible symbols; no ABS, COMMON, TLS or IFUNC.
    if ((section == 0U) || (section >= 0xFF00U) || (binding > 1U) ||
        ((binding != 0U) && (visibility != 2U) && (visibility != 3U)) || ((symbol[4U] & 15U) > 3U) ||
        (function && ((symbol[4U] & 15U) != 2U)))
    {
        return false;
    }
    const Foundation::UInt64 name = Load(symbol, 4U);
    if ((name >= tables.StringBytes) ||
        !FileAt(description, tables.StringAddress + name, tables.StringBytes - name, offset))
    {
        return false;
    }
    bool terminated{false};
    for (Foundation::UInt64 index{0U}; index < tables.StringBytes - name; ++index)
    {
        if (image[offset + index] == 0U)
        {
            terminated = true;
            break;
        }
    }
    value = Load(symbol + 8U, 8U);
    Foundation::Size segment{0U};
    return terminated && MemoryAt(description, value, 1U, segment) &&
           (!function || (((value % BundleBytes) == 0U) && description.Segments[segment].Executable));
}

[[nodiscard]] LoaderError DecodeRelocation(const Foundation::UInt8* const image, const ExecutableImage& description,
                                           const Foundation::UInt64 offset, const Foundation::UInt64 base,
                                           RelocationWord& word, const bool describing = false) noexcept
{
    word.Address = Load(image + offset, 8U);
    const Foundation::UInt64 info = Load(image + offset + 8U, 8U);
    word.Type = static_cast<Foundation::UInt32>(info & 0xFFFF'FFFFU);
    word.Symbol = info >> 32U;
    const Foundation::UInt64 addend = Load(image + offset + 16U, 8U);
    const bool amd64 = description.Identity.Machine == ExecutableMachine::Amd64;
    const bool relative = word.Type == (amd64 ? Amd64Relative : Ia64Relative);
    const bool descriptor = !amd64 && (word.Type == Ia64Descriptor);
    const bool pointer = !amd64 && (word.Type == Ia64FunctionPointer);
    const bool direct = !amd64 && (word.Type == Ia64Direct);
    if (!relative && !descriptor && !pointer && !direct)
    {
        return LoaderError::UnsupportedFormat;
    }
    word.Bytes = descriptor ? 16U : 8U;
    if ((word.Address % 8U != 0U) || !MemoryAt(description, word.Address, word.Bytes, word.Segment))
    {
        return LoaderError::Malformed;
    }
    const ExecutableSegment& segment = description.Segments[word.Segment];
    if (segment.Executable ||
        (!segment.Writable && !Contains(description.Relro.Start, description.Relro.Bytes, word.Address, word.Bytes)))
    {
        return LoaderError::UnsupportedFormat;
    }
    Foundation::UInt64 symbol{0U};
    if (relative || (descriptor && (word.Symbol == 0U)))
    {
        if (word.Symbol != 0U)
        {
            return LoaderError::Malformed;
        }
    }
    else if (!ReadLocalSymbol(image, description, word.Symbol, symbol, descriptor || pointer))
    {
        return LoaderError::UnsupportedFormat;
    }
    if (describing && !descriptor && !pointer)
    {
        // Link-time values use ELF's modulo-2^64 representation. Actual load
        // Addresses are checked with signed arithmetic at the chosen base.
        word.Value = symbol + addend;
    }
    else if ((symbol > ~Foundation::UInt64{0U} - base) || !AddSigned(base + symbol, addend, word.Value))
    {
        return LoaderError::Malformed;
    }
    word.Code = word.Value;
    if (descriptor || pointer)
    {
        Foundation::Size codeSegment{0U};
        if ((word.Code < base) || (word.Code % BundleBytes != 0U) ||
            !MemoryAt(description, word.Code - base, 1U, codeSegment) ||
            !description.Segments[codeSegment].Executable || (description.GlobalPointer == 0U))
        {
            return LoaderError::Malformed;
        }
    }
    return LoaderError::None;
}

[[nodiscard]] Foundation::UInt64 RelocationOffset(const ExecutableImage& description,
                                                  const Foundation::UInt64 number) noexcept
{
    const Foundation::UInt64 ordinary = description.Relocations.Bytes / 24U;
    return (number < ordinary) ? description.Relocations.FileOffset + number * 24U
                               : description.Relocations.PltFileOffset + (number - ordinary) * 24U;
}

[[nodiscard]] LoaderError ValidateRelocations(const Foundation::UInt8* const image, const ExecutableImage& description,
                                              Foundation::UInt32& type) noexcept
{
    const Foundation::UInt64 count = (description.Relocations.Bytes + description.Relocations.PltBytes) / 24U;
    for (Foundation::UInt64 index{0U}; index < count; ++index)
    {
        RelocationWord word{};
        const LoaderError error =
            DecodeRelocation(image, description, RelocationOffset(description, index), 0U, word, true);
        type = word.Type;
        if (error != LoaderError::None)
        {
            return error;
        }
        if ((index < description.Relocations.RelativeCount) &&
            (word.Type != ((description.Identity.Machine == ExecutableMachine::Amd64) ? Amd64Relative : Ia64Relative)))
        {
            return LoaderError::Malformed;
        }
    }
    type = 0U;
    return LoaderError::None;
}

// Entry descriptors may contain linker-script QUAD values, REL64LSB words,
// or a GNU ld IPLTLSB initializer. No destination read callback is needed.
[[nodiscard]] bool DescribeDynamicEntry(const Foundation::UInt8* const image, const MachineRules& rules,
                                        ExecutableImage& output) noexcept
{
    if (!rules.EntryDescriptor)
    {
        Foundation::Size segment{0U};
        return MemoryAt(output, output.Entry, 1U, segment) && output.Segments[segment].Executable;
    }
    output.EntryDescriptor = output.Entry;
    Foundation::UInt64 offset{0U};
    if ((output.EntryDescriptor % 8U != 0U) || !FileAt(output, output.EntryDescriptor, 16U, offset))
    {
        return false;
    }
    output.Entry = Load(image + offset, 8U);
    Foundation::UInt64 descriptorGp = Load(image + offset + 8U, 8U);
    const Foundation::UInt64 count = (output.Relocations.Bytes + output.Relocations.PltBytes) / 24U;
    for (Foundation::UInt64 index{0U}; index < count; ++index)
    {
        RelocationWord word{};
        if (DecodeRelocation(image, output, RelocationOffset(output, index), 0U, word, true) != LoaderError::None)
        {
            return false;
        }
        if (word.Address == output.EntryDescriptor)
        {
            output.Entry = word.Value;
            if (word.Type == Ia64Descriptor)
            {
                descriptorGp = output.GlobalPointer;
            }
        }
        if (word.Address == output.EntryDescriptor + 8U)
        {
            descriptorGp = word.Value;
        }
    }
    Foundation::Size segment{0U};
    return ((descriptorGp == 0U) || (descriptorGp == output.GlobalPointer)) && (output.Entry % BundleBytes == 0U) &&
           MemoryAt(output, output.Entry, 1U, segment) && output.Segments[segment].Executable &&
           MemoryAt(output, output.GlobalPointer, 1U, segment);
}

// Walks the program headers of a little-endian ELF64 executable: every
// PT_LOAD becomes a segment; PT_INTERP stays refused. ET_DYN alone admits
// PT_DYNAMIC, empty PT_TLS and recorded PT_GNU_RELRO. The entry must be
// executable code, or name a descriptor of it under the rules.
[[gnu::noinline]] LoaderError WalkElf64(const Foundation::UInt8* const image, const Foundation::Size sizeBytes,
                                        const MachineRules& rules, ExecutableImage& output) noexcept
{
    const Foundation::UInt64 headers = Load(image + 32U, 8U);
    const Foundation::UInt64 headerBytes = Load(image + 54U, 2U);
    const Foundation::UInt64 count = Load(image + 56U, 2U);
    // Count has 16 bits, so its product with the header size cannot overflow.
    if ((headerBytes != ProgramHeaderBytes) || (headers > sizeBytes) ||
        ((count * ProgramHeaderBytes) > (sizeBytes - headers)))
    {
        return LoaderError::Malformed;
    }
    const bool relocatable = output.Identity.Kind == ExecutableKind::Relocatable;
    const Foundation::UInt8* dynamic{nullptr};
    const Foundation::UInt8* relro{nullptr};
    output.UserStart = rules.UserStart;
    output.UserLimit = rules.UserLimit;
    for (Foundation::UInt64 index{0U}; index < count; ++index)
    {
        const Foundation::UInt8* const header = image + headers + (index * ProgramHeaderBytes);
        const auto type = static_cast<Foundation::UInt32>(Load(header, 4U));
        if ((type == InterpreterSegment) || ((type == DynamicSegment) && !relocatable))
        {
            return LoaderError::UnsupportedFormat;
        }
        if (relocatable && (type == DynamicSegment))
        {
            if (dynamic != nullptr)
            {
                return LoaderError::Malformed;
            }
            dynamic = header;
        }
        if (relocatable && (type == 0x6474'E552U))
        {
            if (relro != nullptr)
            {
                return LoaderError::Malformed;
            }
            relro = header;
        }
        if (relocatable && (type == 7U) && ((Load(header + 32U, 8U) != 0U) || (Load(header + 40U, 8U) != 0U)))
        {
            return LoaderError::UnsupportedFormat;
        }
        if (type != LoadSegment)
        {
            continue;
        }
        bool load{false};
        ExecutableSegment segment{};
        LoaderError error = DecodeSegment(header, sizeBytes, rules, relocatable, load, segment);
        if ((error == LoaderError::None) && load)
        {
            error = AddSegment(segment, rules, output);
            const Foundation::UInt64 alignment = Load(header + 48U, 8U);
            if (alignment > output.Alignment)
            {
                output.Alignment = alignment;
            }
        }
        if (error != LoaderError::None)
        {
            return error;
        }
    }
    if (output.SegmentCount == 0U)
    {
        return LoaderError::Malformed;
    }
    Foundation::UInt64 start{~Foundation::UInt64{0U}};
    Foundation::UInt64 end{0U};
    for (Foundation::Size index{0U}; index < output.SegmentCount; ++index)
    {
        const ExecutableSegment& segment = output.Segments[index];
        if (segment.VirtualStart < start)
        {
            start = segment.VirtualStart;
        }
        const Foundation::UInt64 segmentEnd = segment.VirtualStart + segment.Pages * PageBytes;
        if (segmentEnd > end)
        {
            end = segmentEnd;
        }
    }
    output.Span = ExecutableRange{.Start = start, .Bytes = end - start};
    output.Entry = Load(image + 24U, 8U);
    if (relocatable)
    {
        if (dynamic == nullptr)
        {
            return LoaderError::Malformed;
        }
        if (relro != nullptr)
        {
            output.Relro = ExecutableRange{.Start = Load(relro + 16U, 8U), .Bytes = Load(relro + 40U, 8U)};
            Foundation::Size segment{0U};
            Foundation::UInt64 offset{0U};
            const Foundation::UInt64 fileBytes = Load(relro + 32U, 8U);
            if (!MemoryAt(output, output.Relro.Start, output.Relro.Bytes, segment) ||
                (fileBytes > output.Relro.Bytes) || !FileAt(output, output.Relro.Start, fileBytes, offset) ||
                (offset != Load(relro + 8U, 8U)) || output.Segments[segment].Executable)
            {
                return LoaderError::Malformed;
            }
        }
        LoaderError error = ParseDynamic(image, dynamic, sizeBytes, output);
        Foundation::UInt32 type{0U};
        if (error == LoaderError::None)
        {
            error = ValidateRelocations(image, output, type);
        }
        if (error != LoaderError::None)
        {
            output.RejectedRelocationType = (error == LoaderError::UnsupportedFormat) ? type : 0U;
            return error;
        }
        return DescribeDynamicEntry(image, rules, output) ? LoaderError::None : LoaderError::Malformed;
    }
    const bool entered =
        rules.EntryDescriptor ? ReadEntryDescriptor(image, output) : IsExecutableAt(output, output.Entry);
    return entered ? LoaderError::None : LoaderError::Malformed;
}

LoaderError ParseElf(const Foundation::UInt8* const image, const Foundation::Size sizeBytes,
                     const ExecutableIdentity& identity, const MachineRules& rules, ExecutableImage& output) noexcept
{
    // Little-endian ELF64, fixed or opt-in static PIE; on IA-64 only LP64.
    if ((identity.Format != ExecutableFormat::Elf64) || (identity.ByteOrder != ExecutableByteOrder::Little) ||
        ((identity.Kind == ExecutableKind::Relocatable) && !rules.PositionIndependentImages) ||
        ((identity.Machine == ExecutableMachine::Ia64) && ((Load(image + 48U, 4U) & ElfIa64Abi64) == 0U)))
    {
        return LoaderError::UnsupportedFormat;
    }
    return WalkElf64(image, sizeBytes, rules, output);
}

// A PE image: the MZ stub's e_lfanew, inside the image, names the PE
// Signature, the COFF header and an optional header of PE32 or PE32+.
LoaderError ProbePe(const Foundation::UInt8* const image, const Foundation::Size sizeBytes,
                    ExecutableIdentity& identity) noexcept
{
    if ((sizeBytes < (PeHeaderOffsetField + 4U)) || (image[0U] != 'M') || (image[1U] != 'Z'))
    {
        return LoaderError::UnsupportedFormat;
    }
    const Foundation::UInt64 header = Load(image + PeHeaderOffsetField, 4U);
    if ((header > sizeBytes) || ((sizeBytes - header) < (4U + CoffHeaderBytes + 2U)) || (image[header] != 'P') ||
        (image[header + 1U] != 'E') || (image[header + 2U] != 0U) || (image[header + 3U] != 0U))
    {
        return LoaderError::UnsupportedFormat;
    }
    const Foundation::UInt8* const coff = image + header + 4U;
    const Foundation::UInt64 machine = Load(coff, 2U);
    const Foundation::UInt64 optionalBytes = Load(coff + 16U, 2U);
    const Foundation::UInt64 magic = Load(coff + CoffHeaderBytes, 2U);
    const Foundation::UInt64 optionalStart = header + 4U + CoffHeaderBytes;
    if (((magic != PeMagic32) && (magic != PeMagic32Plus)) || (optionalBytes < (PeSubsystemOffset + 2U)) ||
        (optionalBytes > (sizeBytes - optionalStart)))
    {
        return LoaderError::Malformed;
    }
    identity = ExecutableIdentity{
        .Format = (magic == PeMagic32Plus) ? ExecutableFormat::Pe32Plus : ExecutableFormat::Pe32,
        .Machine = PeMachine(machine),
        .ByteOrder = ExecutableByteOrder::Little,
        // Every PE subsystem is another system's interface.
        .Abi = ExecutableAbi::Foreign,
        .Kind = ExecutableKind::Fixed,
        .MachineCode = static_cast<Foundation::UInt16>(machine),
        .AbiCode = static_cast<Foundation::UInt16>(Load(image + optionalStart + PeSubsystemOffset, 2U)),
    };
    return LoaderError::None;
}

// PE loading is unsupported: an identified PE image is refused.
LoaderError ParsePe(const Foundation::UInt8*, const Foundation::Size, const ExecutableIdentity&, const MachineRules&,
                    ExecutableImage&) noexcept
{
    return LoaderError::UnsupportedFormat;
}

[[nodiscard]] const FormatHandler* FindHandler(const ExecutableFormat format) noexcept;

// The formats in the order they are probed.
constexpr FormatHandler Handlers[]{
    {.Format = ExecutableFormat::Elf64, .Probe = ProbeElf, .Parse = ParseElf},
    {.Format = ExecutableFormat::Pe32Plus, .Probe = ProbePe, .Parse = ParsePe},
};

const FormatHandler* FindHandler(const ExecutableFormat format) noexcept
{
    switch (format)
    {
    case ExecutableFormat::Elf64:
    case ExecutableFormat::Elf32:
        return &Handlers[0U];
    case ExecutableFormat::Pe32Plus:
    case ExecutableFormat::Pe32:
        return &Handlers[1U];
    case ExecutableFormat::Unknown:
        break;
    }
    return nullptr;
}

void Clear(ExecutableImage& output) noexcept
{
    output.Identity = ExecutableIdentity{};
    output.Entry = 0U;
    output.GlobalPointer = 0U;
    output.SegmentCount = 0U;
    output.Relocations = ExecutableRelocations{};
    output.Span = ExecutableRange{};
    output.Alignment = PageBytes;
    output.Relro = ExecutableRange{};
    output.EntryDescriptor = 0U;
    output.UserStart = 0U;
    output.UserLimit = 0U;
    output.RejectedRelocationType = 0U;
}
} // Namespace

LoaderError IdentifyExecutable(const Foundation::UInt8* const image, const Foundation::Size sizeBytes,
                               ExecutableIdentity& identity) noexcept
{
    if (image == nullptr)
    {
        return LoaderError::Malformed;
    }
    for (const FormatHandler& handler : Handlers)
    {
        const LoaderError probed = handler.Probe(image, sizeBytes, identity);
        if (probed != LoaderError::UnsupportedFormat)
        {
            return probed;
        }
    }
    return LoaderError::UnsupportedFormat;
}

LoaderError DescribeExecutable(const Foundation::UInt8* const image, const Foundation::Size sizeBytes,
                               const MachineRules& rules, ExecutableImage& output) noexcept
{
    Clear(output);
    if ((rules.UserStart < PageBytes) || (rules.UserLimit <= rules.UserStart))
    {
        return LoaderError::Malformed;
    }
    LoaderError error = IdentifyExecutable(image, sizeBytes, output.Identity);
    const FormatHandler* const handler = FindHandler(output.Identity.Format);
    if ((error == LoaderError::None) && (output.Identity.Machine != rules.Machine))
    {
        error = LoaderError::ForeignMachineUnsupported;
    }
    else if ((error == LoaderError::None) && (output.Identity.Abi != ExecutableAbi::Asteria))
    {
        error = LoaderError::UnsupportedFormat;
    }
    else if (error == LoaderError::None)
    {
        error = (handler != nullptr) ? handler->Parse(image, sizeBytes, output.Identity, rules, output)
                                     : LoaderError::UnsupportedFormat;
    }
    if (error != LoaderError::None)
    {
        const ExecutableIdentity identity = output.Identity;
        const Foundation::UInt32 type = output.RejectedRelocationType;
        Clear(output);
        output.Identity = identity;
        output.RejectedRelocationType = type;
    }
    return error;
}
namespace
{
[[nodiscard]] bool DescriptorFor(const Foundation::UInt8* const image, const ExecutableImage& description,
                                 const Foundation::UInt64 base, const RelocationWriter& writer, RelocationWord& word,
                                 bool& external) noexcept
{
    external = false;
    const Foundation::UInt64 count = (description.Relocations.Bytes + description.Relocations.PltBytes) / 24U;
    Foundation::UInt64 slot{count};
    for (Foundation::UInt64 index{0U}; index < count; ++index)
    {
        RelocationWord candidate{};
        if (DecodeRelocation(image, description, RelocationOffset(description, index), base, candidate) !=
            LoaderError::None)
        {
            return false;
        }
        if ((candidate.Type == Ia64Descriptor) && (candidate.Code == word.Code))
        {
            word.Value = base + candidate.Address;
            return true;
        }
        if ((candidate.Type == Ia64FunctionPointer) && (candidate.Code == word.Code) && (index < slot))
        {
            slot = index;
        }
    }
    // The first FPTR relocation for this entry owns its slot. This preserves
    // pointer equality across aliases and permits distinct symbol + addend entries.
    word.Symbol = slot;
    const Foundation::UInt64 offset = slot * 16U; // Count is capped at MaximumRelocations.
    if ((writer.DescriptorAddress % 16U != 0U) || (offset > writer.DescriptorBytes) ||
        (16U > writer.DescriptorBytes - offset))
    {
        return false;
    }
    word.Value = writer.DescriptorAddress + offset;
    external = true;
    return true;
}

[[nodiscard]] bool WriteWords(const RelocationWriter& writer, const Foundation::Size segment,
                              const Foundation::UInt64 offset, const Foundation::UInt64 first,
                              const Foundation::UInt64 second, const Foundation::Size count) noexcept
{
    Foundation::UInt8 bytes[16U]{};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>((first >> (index * 8U)) & 0xFFU);
        bytes[index + 8U] = static_cast<Foundation::UInt8>((second >> (index * 8U)) & 0xFFU);
    }
    return writer.Write(writer.Context, segment, offset, bytes, count);
}

[[nodiscard]] bool ValidBase(const ExecutableImage& description, const Foundation::UInt64 base,
                             const RelocationWriter& writer) noexcept
{
    if ((description.SegmentCount == 0U) || (description.SegmentCount > MaximumExecutableSegments) ||
        (description.Alignment < PageBytes) || ((description.Alignment & (description.Alignment - 1U)) != 0U) ||
        ((base & (description.Alignment - 1U)) != 0U) || (description.Span.Start > ~Foundation::UInt64{0U} - base))
    {
        return false;
    }
    const Foundation::UInt64 start = base + description.Span.Start;
    if ((start < description.UserStart) || (start >= description.UserLimit) ||
        (description.Span.Bytes > description.UserLimit - start))
    {
        return false;
    }
    if (writer.DescriptorBytes == 0U)
    {
        return true;
    }
    if ((writer.DescriptorAddress % 16U != 0U) || (writer.DescriptorAddress < description.UserStart) ||
        (writer.DescriptorAddress >= description.UserLimit) ||
        (writer.DescriptorBytes > description.UserLimit - writer.DescriptorAddress))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < description.SegmentCount; ++index)
    {
        const ExecutableSegment& segment = description.Segments[index];
        if ((writer.DescriptorAddress < base + segment.VirtualStart + segment.Pages * PageBytes) &&
            (base + segment.VirtualStart < writer.DescriptorAddress + writer.DescriptorBytes))
        {
            return false;
        }
    }
    return true;
}
} // Namespace

RelocationResult ApplyExecutableRelocations(const Foundation::UInt8* const image, const Foundation::Size sizeBytes,
                                            const ExecutableImage& description, const Foundation::UInt64 base,
                                            const RelocationWriter& writer) noexcept
{
    RelocationResult result{.Error = LoaderError::Malformed};
    if ((image == nullptr) || (writer.Write == nullptr) || (description.Identity.Format != ExecutableFormat::Elf64) ||
        (description.Identity.Kind != ExecutableKind::Relocatable) ||
        ((description.Identity.Machine != ExecutableMachine::Amd64) &&
         (description.Identity.Machine != ExecutableMachine::Ia64)) ||
        !ValidBase(description, base, writer))
    {
        return result;
    }
    const ExecutableRelocations& tables = description.Relocations;
    if ((tables.Kind != 7U) || (tables.FileOffset > sizeBytes) || (tables.Bytes > sizeBytes - tables.FileOffset) ||
        (tables.PltFileOffset > sizeBytes) || (tables.PltBytes > sizeBytes - tables.PltFileOffset) ||
        (tables.Bytes % 24U != 0U) || (tables.PltBytes % 24U != 0U) || (tables.Bytes / 24U > MaximumRelocations) ||
        (tables.PltBytes / 24U > MaximumRelocations - tables.Bytes / 24U))
    {
        return result;
    }
    if (description.EntryDescriptor != 0U)
    {
        Foundation::Size segment{0U};
        if (!MemoryAt(description, description.EntryDescriptor, 16U, segment) ||
            description.Segments[segment].Executable ||
            (!description.Segments[segment].Writable &&
             !Contains(description.Relro.Start, description.Relro.Bytes, description.EntryDescriptor, 16U)))
        {
            return result;
        }
    }
    // Two bounded passes: arithmetic, symbol, permission and descriptor errors
    // are discovered before the callback sees any bytes. Callback failure is
    // the only non-atomic failure; discard the staged image in that case.
    const Foundation::UInt64 count = (tables.Bytes + tables.PltBytes) / 24U;
    for (Foundation::Size pass{0U}; pass < 2U; ++pass)
    {
        for (Foundation::UInt64 index{0U}; index < count; ++index)
        {
            RelocationWord word{};
            result.Error = DecodeRelocation(image, description, RelocationOffset(description, index), base, word);
            result.Type = (result.Error == LoaderError::UnsupportedFormat) ? word.Type : 0U;
            if (result.Error != LoaderError::None)
            {
                return result;
            }
            bool external{false};
            if ((word.Type == Ia64FunctionPointer) && !DescriptorFor(image, description, base, writer, word, external))
            {
                return RelocationResult{.Error = LoaderError::ImageTooLarge};
            }
            if (pass == 0U)
            {
                continue;
            }
            if (external && !WriteWords(writer, description.SegmentCount, word.Symbol * 16U, word.Code,
                                        base + description.GlobalPointer, 16U))
            {
                return RelocationResult{.Error = LoaderError::Denied};
            }
            if (!WriteWords(writer, word.Segment, word.Address - description.Segments[word.Segment].VirtualStart,
                            word.Value, base + description.GlobalPointer, word.Bytes))
            {
                return RelocationResult{.Error = LoaderError::Denied};
            }
        }
    }
    if (description.EntryDescriptor != 0U)
    {
        Foundation::Size segment{0U};
        if (!MemoryAt(description, description.EntryDescriptor, 16U, segment) ||
            (!description.Segments[segment].Writable &&
             !Contains(description.Relro.Start, description.Relro.Bytes, description.EntryDescriptor, 16U)))
        {
            return RelocationResult{.Error = LoaderError::Malformed};
        }
        if (!WriteWords(writer, segment, description.EntryDescriptor - description.Segments[segment].VirtualStart,
                        base + description.Entry, base + description.GlobalPointer, 16U))
        {
            return RelocationResult{.Error = LoaderError::Denied};
        }
    }
    return RelocationResult{
        .Error = LoaderError::None,
        .Entry = base + description.Entry,
        .GlobalPointer = (description.GlobalPointer == 0U) ? 0U : base + description.GlobalPointer,
        .EntryDescriptor = (description.EntryDescriptor == 0U) ? 0U : base + description.EntryDescriptor,
    };
}

} // Namespace Asteria::User::Loader
