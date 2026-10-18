// SPDX-License-Identifier: Apache-2.0
// Fuzz harness for the SAL System Table parser (SalSystemTableParser::Parse).
//
// Input: flags (1 byte), then the table bytes. The table is copied so that it
// ends exactly at an unmapped guard page, and availableBytes covers exactly
// the copied bytes.
//   flags bit 0: repair the header checksum before parsing
//
// Properties checked for every input:
// - A null table is rejected as NullBuffer.
// - Parse reads nothing past bytes + availableBytes (the guard page faults).
// - Parsing is deterministic.
// - An accepted table re-validates independently: signature, length bounds,
//   zero checksum over TotalTableLength, supported BCD revision, and the
//   published fields equal a reference walk of the descriptors (order, counts,
//   presence flags and values, entrypoint and translation constraints).
// - Only the declared table matters: parsing with availableBytes equal to
//   TotalTableLength gives the same result.
// - The checksum covers the whole table: incrementing its last byte or an
//   input-selected byte outside its length field makes an accepted table
//   rejected.

#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/mman.h>

#include "Foundation/Types.hpp"

namespace
{
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt16;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UIntPointer;
using Asteria::Platform::Ia64::Firmware::SalSystemTableHeaderSize;
using Asteria::Platform::Ia64::Firmware::SalSystemTableInformation;
using Asteria::Platform::Ia64::Firmware::SalSystemTableParseError;
using Asteria::Platform::Ia64::Firmware::SalSystemTableParser;

constexpr Size PageBytes{4096U};
constexpr UIntPointer BufferAddress{0x4160'0000U};
constexpr UInt8 RepairChecksumFlag{1U << 0U};
constexpr Size DescriptorSizes[]{48U, 32U, 16U, 32U, 16U, 16U};
constexpr UInt64 PhysicalAddressTopBit{UInt64{1U} << 63U};

constinit UInt8* g_page{nullptr};
constinit UInt8 g_scratch[PageBytes]{};
constinit UInt8 g_seed[PageBytes]{};
constinit UInt8 g_seedInput[PageBytes]{};

[[noreturn]] void Violation(const char* const property)
{
    static_cast<void>(std::fprintf(stderr, "FUZZ PROPERTY VIOLATED: %s\n", property));
    std::abort();
}

[[nodiscard]] UInt64 ReadLittleEndian(const UInt8* const bytes, const Size byteCount) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < byteCount; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

void WriteLittleEndian(UInt8* const bytes, const UInt64 value, const Size byteCount) noexcept
{
    for (Size index{0U}; index < byteCount; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

[[nodiscard]] UInt8 Sum(const UInt8* const bytes, const Size byteCount) noexcept
{
    UInt8 sum{0U};
    for (Size index{0U}; index < byteCount; ++index)
    {
        sum = static_cast<UInt8>(sum + bytes[index]);
    }
    return sum;
}

// One readable page followed by a page that is unmapped again, so a read past
// the table end faults in the GCC replay build as well.
[[nodiscard]] UInt8* GetPage() noexcept
{
    if (g_page == nullptr)
    {
        void* const hint = reinterpret_cast<void*>(BufferAddress);
        void* const memory = mmap(hint, 2U * PageBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if ((memory != hint) || (munmap(static_cast<UInt8*>(memory) + PageBytes, PageBytes) != 0))
        {
            static_cast<void>(std::fprintf(stderr, "FAIL: cannot map the guarded SAL table page\n"));
            std::abort();
        }
        g_page = static_cast<UInt8*>(memory);
    }
    return g_page;
}

[[nodiscard]] bool IsBcd(const UInt16 value) noexcept
{
    return ((value & 0x0FU) <= 9U) && (((value >> 4U) & 0x0FU) <= 9U) && (((value >> 8U) & 0x0FU) <= 9U) &&
           ((value >> 12U) <= 9U);
}

// Re-validates an accepted table from its bytes alone.
void CheckAccepted(const UInt8* const table, const Size availableBytes,
                   const SalSystemTableInformation& information) noexcept
{
    const Size length = information.TotalTableLength;
    const UInt16 revision = information.SalRevision;
    const UInt32 major = static_cast<UInt32>(revision) >> 8U;
    const UInt32 minor = revision & 0xFFU;
    if ((std::memcmp(table, "SST_", 4U) != 0) || (length != ReadLittleEndian(table + 4U, 4U)) ||
        (length > availableBytes) || (information.EntryCount != ReadLittleEndian(table + 10U, 2U)) ||
        (length < (SalSystemTableHeaderSize + (16U * information.EntryCount))) ||
        (length > (SalSystemTableHeaderSize + (48U * information.EntryCount))) || (Sum(table, length) != 0U))
    {
        Violation("an accepted table has the signature, a bounded length, and a zero checksum");
    }
    if ((revision != ReadLittleEndian(table + 8U, 2U)) || !IsBcd(revision) || !IsBcd(information.SalAVersion) ||
        !IsBcd(information.SalBVersion) || !(((major == 2U) && (minor >= 0x80U)) || (major == 3U)) ||
        (information.SalAVersion != ReadLittleEndian(table + 20U, 2U)) ||
        (information.SalBVersion != ReadLittleEndian(table + 22U, 2U)) || (information.Reserved8 != 0U))
    {
        Violation("an accepted table has a supported BCD revision and published versions");
    }

    Size counts[6U]{};
    Size firstOffsets[6U]{};
    Size offset{SalSystemTableHeaderSize};
    UInt8 previousType{0U};
    for (Size index{0U}; index < information.EntryCount; ++index)
    {
        const UInt8 type = offset < length ? table[offset] : UInt8{0xFFU};
        if ((type > 5U) || (type < previousType) || ((offset + DescriptorSizes[type]) > length))
        {
            Violation("the descriptors of an accepted table are known, ordered, and inside the table");
        }
        firstOffsets[type] = counts[type] == 0U ? offset : firstOffsets[type];
        ++counts[type];
        previousType = type;
        offset += DescriptorSizes[type];
    }
    if ((offset != length) || (counts[0U] != 1U) || (counts[2U] > 1U) || (counts[3U] > 1U) || (counts[4U] > 1U) ||
        (counts[5U] > 1U) || (information.MemoryDescriptorCount != counts[1U]) ||
        (information.HasPlatformFeatures != (counts[2U] == 1U)) ||
        (information.HasTranslationRegister != (counts[3U] == 1U)) ||
        (information.HasPurgeCoherenceInformation != (counts[4U] == 1U)) ||
        (information.HasApWakeVector != (counts[5U] == 1U)))
    {
        Violation("descriptor counts and presence flags match a reference walk");
    }

    const UInt8* const entrypoint = table + firstOffsets[0U];
    const UInt64 pal = information.PalProcedurePhysicalAddress;
    const UInt64 sal = information.SalProcedurePhysicalAddress;
    const UInt64 globalData = information.SalGlobalDataPhysicalAddress;
    if ((pal != ReadLittleEndian(entrypoint + 8U, 8U)) || (sal != ReadLittleEndian(entrypoint + 16U, 8U)) ||
        (globalData != ReadLittleEndian(entrypoint + 24U, 8U)) || (pal == 0U) || (sal == 0U) || (globalData == 0U) ||
        ((pal & 0x0FU) != 0U) || ((sal & 0x0FU) != 0U) || (((pal | sal | globalData) & PhysicalAddressTopBit) != 0U))
    {
        Violation("the entrypoint descriptor publishes aligned physical procedure addresses");
    }
    const UInt8* const features = table + firstOffsets[2U];
    if (information.PlatformFeatures != (information.HasPlatformFeatures ? features[1U] : 0U))
    {
        Violation("platform features equal the descriptor byte");
    }
    const UInt8* const translation = table + firstOffsets[3U];
    const UInt64 pageSize = information.TranslationRegisterEncodedPageSize;
    const UInt64 pageShift = (pageSize >> 2U) & 0x3FU;
    const UInt64 offsetMask = pageShift >= 61U ? (UInt64{1U} << 61U) - 1U : (UInt64{1U} << pageShift) - 1U;
    if (information.HasTranslationRegister
            ? ((information.TranslationRegisterVirtualAddress != ReadLittleEndian(translation + 8U, 8U)) ||
               (pageSize != ReadLittleEndian(translation + 16U, 8U)) || ((pageSize & ~UInt64{0xFCU}) != 0U) ||
               (pageShift < 12U) || ((information.TranslationRegisterVirtualAddress & offsetMask) != 0U))
            : ((information.TranslationRegisterVirtualAddress != 0U) || (pageSize != 0U)))
    {
        Violation("the translation register is a page-aligned mapping of a supported size");
    }
    const UInt8* const purge = table + firstOffsets[4U];
    const UInt64 coherenceAddress = information.CoherenceDomainInformationPhysicalAddress;
    if (information.HasPurgeCoherenceInformation
            ? ((information.CoherenceDomainCount != ReadLittleEndian(purge + 4U, 4U)) ||
               (coherenceAddress != ReadLittleEndian(purge + 8U, 8U)) || (information.CoherenceDomainCount == 0U) ||
               (coherenceAddress == 0U) || ((coherenceAddress & 7U) != 0U) ||
               ((coherenceAddress & PhysicalAddressTopBit) != 0U))
            : ((information.CoherenceDomainCount != 0U) || (coherenceAddress != 0U)))
    {
        Violation("purge coherence information is a nonzero domain count at an aligned address");
    }
    const UInt8* const apWake = table + firstOffsets[5U];
    if (information.HasApWakeVector ? ((information.ApWakeVector != ReadLittleEndian(apWake + 8U, 8U)) ||
                                       (information.ApWakeVector < 0x10U) || (information.ApWakeVector > 0xFFU))
                                    : (information.ApWakeVector != 0U))
    {
        Violation("the AP wake vector is an external interrupt vector");
    }
}

// Seed tables: a header followed by descriptors, finished with its checksum.
class SeedTable final
{
public:
    explicit SeedTable(const UInt16 revision) noexcept
    {
        std::memset(g_seed, 0, sizeof(g_seed));
        std::memcpy(g_seed, "SST_", 4U);
        WriteLittleEndian(g_seed + 8U, revision, 2U);
        WriteLittleEndian(g_seed + 20U, 0x0100U, 2U);
        WriteLittleEndian(g_seed + 22U, 0x0100U, 2U);
        std::memcpy(g_seed + 24U, "ASTERIA", 7U);
        std::memcpy(g_seed + 56U, "IA-64 FUZZ SEED", 15U);
    }

    SeedTable& Add(const UInt8 type, const UInt64 first, const UInt64 second, const UInt64 third) noexcept
    {
        UInt8* const descriptor = g_seed + m_length;
        descriptor[0U] = type;
        switch (type)
        {
        case 0U:
            WriteLittleEndian(descriptor + 8U, first, 8U);
            WriteLittleEndian(descriptor + 16U, second, 8U);
            WriteLittleEndian(descriptor + 24U, third, 8U);
            break;
        case 2U:
            descriptor[1U] = static_cast<UInt8>(first);
            break;
        case 3U:
            WriteLittleEndian(descriptor + 8U, first, 8U);
            WriteLittleEndian(descriptor + 16U, second, 8U);
            break;
        case 4U:
            WriteLittleEndian(descriptor + 4U, second, 4U);
            WriteLittleEndian(descriptor + 8U, first, 8U);
            break;
        case 5U:
            WriteLittleEndian(descriptor + 8U, first, 8U);
            break;
        default:
            WriteLittleEndian(descriptor + 8U, first, 8U);
            break;
        }
        m_length += type <= 5U ? DescriptorSizes[type] : 16U;
        ++m_entries;
        return *this;
    }

    void Emit(void (*const emit)(const std::uint8_t* data, std::size_t size), const UInt8 flags,
              const Size extraLength = 0U) noexcept
    {
        UInt8* const input = g_seedInput;
        WriteLittleEndian(g_seed + 4U, m_length + extraLength, 4U);
        WriteLittleEndian(g_seed + 10U, m_entries, 2U);
        g_seed[12U] = 0U;
        g_seed[12U] = static_cast<UInt8>(0U - Sum(g_seed, m_length + extraLength));
        input[0U] = flags;
        std::memcpy(input + 1U, g_seed, m_length + extraLength);
        emit(input, 1U + m_length + extraLength);
    }

private:
    Size m_length{SalSystemTableHeaderSize};
    UInt16 m_entries{0U};
};

constexpr UInt64 PalProcedure{0x0000'0000'FFF8'0000U};
constexpr UInt64 SalProcedure{0x0000'0000'FFF9'0010U};
constexpr UInt64 SalGlobalData{0x0000'0000'FFFA'0008U};
constexpr UInt64 TranslationAddress{0xE000'0000'0400'0000U};
constexpr UInt64 TranslationPageSize{26U << 2U};
constexpr UInt64 CoherenceAddress{0x0000'0000'7F00'0000U};

SeedTable& AddEntrypoint(SeedTable& table) noexcept
{
    return table.Add(0U, PalProcedure, SalProcedure, SalGlobalData);
}
} // namespace

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    // Every descriptor kind on a revision 3.40 table, also in repair mode.
    constexpr UInt8 FullTableFlags[]{0U, RepairChecksumFlag};
    for (const UInt8 flags : FullTableFlags)
    {
        SeedTable table{0x0340U};
        AddEntrypoint(table)
            .Add(1U, 0x0000'0000'FFF0'0000U, 0U, 0U)
            .Add(1U, 0x0000'0000'FFE0'0000U, 0U, 0U)
            .Add(2U, 0x0FU, 0U, 0U)
            .Add(3U, TranslationAddress, TranslationPageSize, 0U)
            .Add(4U, CoherenceAddress, 2U, 0U)
            .Add(5U, 0xF0U, 0U, 0U)
            .Emit(emit, flags);
    }
    SeedTable minimal{0x0280U};
    AddEntrypoint(minimal).Emit(emit, 0U);
    SeedTable revision300{0x0300U};
    AddEntrypoint(revision300).Add(2U, 0x07U, 0U, 0U).Add(5U, 0x10U, 0U, 0U).Emit(emit, 0U);
    // Near misses, each rejected by one specific rule.
    SeedTable unordered{0x0340U};
    unordered.Add(1U, 0x0000'0000'FFF0'0000U, 0U, 0U);
    AddEntrypoint(unordered).Emit(emit, 0U);
    SeedTable duplicate{0x0340U};
    AddEntrypoint(duplicate).Add(3U, TranslationAddress, TranslationPageSize, 0U).Add(3U, 0U, 0x30U, 0U).Emit(emit, 0U);
    SeedTable lowVector{0x0340U};
    AddEntrypoint(lowVector).Add(5U, 0x0FU, 0U, 0U).Emit(emit, 0U);
    SeedTable misaligned{0x0340U};
    AddEntrypoint(misaligned).Add(3U, TranslationAddress + 0x1000U, TranslationPageSize, 0U).Emit(emit, 0U);
    SeedTable newFeatures{0x0300U};
    AddEntrypoint(newFeatures).Add(2U, 0x08U, 0U, 0U).Emit(emit, 0U);
    SeedTable unknown{0x0340U};
    AddEntrypoint(unknown).Add(6U, 0U, 0U, 0U).Emit(emit, 0U);
    SeedTable trailing{0x0340U};
    AddEntrypoint(trailing).Add(1U, 0x0000'0000'FFF0'0000U, 0U, 0U).Emit(emit, 0U, 16U);
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    if ((size == 0U) || (size > PageBytes))
    {
        return 0;
    }
    const Size tableBytes = size - 1U;
    UInt8* const table = GetPage() + (PageBytes - tableBytes);
    std::memcpy(table, data + 1U, tableBytes);
    if (((data[0U] & RepairChecksumFlag) != 0U) && (tableBytes > 12U))
    {
        const Size declared = static_cast<Size>(ReadLittleEndian(table + 4U, 4U));
        const Size covered = declared < tableBytes ? declared : tableBytes;
        table[12U] = 0U;
        table[12U] = static_cast<UInt8>(0U - Sum(table, covered));
    }

    const auto absent = SalSystemTableParser::Parse(nullptr, tableBytes);
    if (absent.HasValue() || (absent.GetError() != SalSystemTableParseError::NullBuffer))
    {
        Violation("a null table is rejected as NullBuffer");
    }
    const auto result = SalSystemTableParser::Parse(table, tableBytes);
    const auto repeated = SalSystemTableParser::Parse(table, tableBytes);
    // SalSystemTableInformation has no padding (pinned by static_assert).
    if ((result.HasValue() != repeated.HasValue()) ||
        (result.HasValue()
             ? (std::memcmp(&result.GetValue(), &repeated.GetValue(), sizeof(SalSystemTableInformation)) != 0)
             : (result.GetError() != repeated.GetError())))
    {
        Violation("parsing is deterministic");
    }
    if (!result.HasValue())
    {
        return 0;
    }
    const SalSystemTableInformation& information = result.GetValue();
    CheckAccepted(table, tableBytes, information);

    const auto exact = SalSystemTableParser::Parse(table, information.TotalTableLength);
    if (!exact.HasValue() || (std::memcmp(&exact.GetValue(), &information, sizeof(information)) != 0))
    {
        Violation("only the declared table determines the result");
    }

    // The last byte and one input-dependent byte outside the length field.
    Size index = (Sum(data, size) + size) % information.TotalTableLength;
    index = ((index >= 4U) && (index < 8U)) ? 12U : index;
    const Size changedBytes[]{index, information.TotalTableLength - 1U};
    for (const Size changed : changedBytes)
    {
        std::memcpy(g_scratch, table, information.TotalTableLength);
        ++g_scratch[changed];
        if (SalSystemTableParser::Parse(g_scratch, information.TotalTableLength).HasValue())
        {
            Violation("the checksum covers every byte of the table");
        }
    }
    return 0;
}
