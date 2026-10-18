// SPDX-License-Identifier: Apache-2.0
// Fuzz harness for the PAL virtual-memory decoder
// (PalVirtualMemoryDecoder::Decode, the decoder's only entry point).
//
// Input: twelve little-endian 64-bit words (missing bytes read as zero), the
// Status and Result1-3 registers of PAL_VM_SUMMARY, PAL_VM_PAGE_SIZE and
// PAL_MEM_ATTRIB in that order.
//
// Properties checked for every input:
// - Decoding is deterministic.
// - A failed PAL call is reported as that call's failure.
// - Decode accepts exactly the register sets that satisfy the documented
//   profile: successful calls, zero reserved fields, the address, protection
//   key, translation-register and cache-topology ranges, the architected
//   page sizes with insertable sizes purgeable, and write-back plus uncached
//   memory attributes.
// - Accepted capabilities re-encode to exactly the input registers (field
//   positions per the Itanium PAL specification), so nothing is dropped.
// - Setting an input-selected reserved bit of an accepted register set is
//   rejected as the corresponding reserved-result error.

#include "Platform/Ia64/Memory/PalVirtualMemoryDecoder.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Foundation/Types.hpp"

namespace
{
using Asteria::Foundation::Int64;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::UInt8;
using Asteria::Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask;
using Asteria::Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask;
using Asteria::Platform::Ia64::Memory::PalStaticCallResult;
using Asteria::Platform::Ia64::Memory::PalVirtualMemoryCapabilities;
using Asteria::Platform::Ia64::Memory::PalVirtualMemoryDecodeError;
using Asteria::Platform::Ia64::Memory::PalVirtualMemoryDecoder;

constexpr Size RegisterSetCount{3U};
constexpr Size InputBytes{RegisterSetCount * 4U * 8U};

struct RegisterSets
{
    PalStaticCallResult Registers[RegisterSetCount];
};

[[noreturn]] void Violation(const char* const property)
{
    static_cast<void>(std::fprintf(stderr, "FUZZ PROPERTY VIOLATED: %s\n", property));
    std::abort();
}

[[nodiscard]] UInt64 Field(const UInt64 value, const UInt32 shift, const UInt32 bits) noexcept
{
    return (value >> shift) & ((UInt64{1U} << bits) - 1U);
}

[[nodiscard]] bool InRange(const UInt64 value, const UInt64 minimum, const UInt64 maximum) noexcept
{
    return (value >= minimum) && (value <= maximum);
}

// The acceptance profile, restated from the PAL register layouts.
[[nodiscard]] bool IsAcceptable(const RegisterSets& sets) noexcept
{
    const PalStaticCallResult& summary = sets.Registers[0U];
    const PalStaticCallResult& pages = sets.Registers[1U];
    const PalStaticCallResult& attributes = sets.Registers[2U];
    const UInt64 information = summary.Result1;
    const UInt64 addressing = summary.Result2;
    const UInt64 regionBits = Field(addressing, 8U, 8U);
    const UInt64 keyBits = Field(information, 8U, 8U);
    const UInt64 levels = Field(information, 56U, 8U);
    const UInt64 attributeMask = attributes.Result1;
    return (summary.Status == 0) && (pages.Status == 0) && (attributes.Status == 0) &&
           (Field(addressing, 32U, 32U) == 0U) && (summary.Result3 == 0U) && (pages.Result3 == 0U) &&
           (attributes.Result2 == 0U) && (attributes.Result3 == 0U) && InRange(Field(information, 1U, 7U), 32U, 63U) &&
           InRange(Field(addressing, 0U, 8U), 50U, 60U) && InRange(regionBits, 18U, 24U) &&
           InRange(keyBits, regionBits, 24U) && (Field(information, 16U, 8U) >= 15U) &&
           (Field(information, 32U, 8U) >= 7U) && (Field(information, 40U, 8U) >= 7U) && (levels != 0U) &&
           InRange(Field(information, 48U, 8U), 1U, 2U * levels) &&
           ((pages.Result1 & ArchitectedMinimumInsertablePageSizeMask) == ArchitectedMinimumInsertablePageSizeMask) &&
           ((pages.Result2 & ArchitectedMinimumPurgePageSizeMask) == ArchitectedMinimumPurgePageSizeMask) &&
           (((pages.Result1 | pages.Result2) & 0xFFFU) == 0U) && ((pages.Result1 & ~pages.Result2) == 0U) &&
           ((attributeMask & ~UInt64{0xF1U}) == 0U) && ((attributeMask & 0x11U) == 0x11U);
}

// Re-encodes accepted capabilities into the PAL return registers.
[[nodiscard]] RegisterSets Encode(const PalVirtualMemoryCapabilities& capabilities) noexcept
{
    const auto& translation = capabilities.Translation;
    RegisterSets sets{};
    sets.Registers[0U].Result1 =
        (translation.HardwareWalkerImplemented ? 1U : 0U) | (UInt64{translation.PhysicalAddressBits} << 1U) |
        (UInt64{translation.ProtectionKeyBits} << 8U) | (UInt64{capabilities.ProtectionKeyRegisterCount - 1U} << 16U) |
        (UInt64{capabilities.HashTagIdentifier} << 24U) |
        (UInt64{capabilities.DataTranslationRegisterCount - 1U} << 32U) |
        (UInt64{capabilities.InstructionTranslationRegisterCount - 1U} << 40U) |
        (UInt64{capabilities.UniqueTranslationCacheCount} << 48U) |
        (UInt64{capabilities.TranslationCacheLevelCount} << 56U);
    sets.Registers[0U].Result2 = UInt64{translation.ImplementedVirtualAddressMostSignificantBit} |
                                 (UInt64{translation.RegionIdentifierBits} << 8U) |
                                 (UInt64{capabilities.MaximumConcurrentPurgesEncoding} << 16U);
    sets.Registers[1U].Result1 = translation.InsertablePageSizeMask;
    sets.Registers[1U].Result2 = capabilities.PurgePageSizeMask;
    sets.Registers[2U].Result1 = translation.MemoryAttributeMask;
    return sets;
}

// Result1-3 of one register set by number.
[[nodiscard]] UInt64& ResultRegister(PalStaticCallResult& registers, const Size number) noexcept
{
    return number == 1U ? registers.Result1 : (number == 2U ? registers.Result2 : registers.Result3);
}

[[nodiscard]] PalVirtualMemoryDecodeError DecodeError(const RegisterSets& sets) noexcept
{
    return PalVirtualMemoryDecoder::Decode(sets.Registers[0U], sets.Registers[1U], sets.Registers[2U]).GetError();
}

constinit UInt8 g_seedInput[InputBytes]{};

void EmitSets(void (*const emit)(const std::uint8_t* data, std::size_t size), const RegisterSets& sets) noexcept
{
    for (Size set{0U}; set < RegisterSetCount; ++set)
    {
        const PalStaticCallResult& registers = sets.Registers[set];
        const UInt64 words[4U]{static_cast<UInt64>(registers.Status), registers.Result1, registers.Result2,
                               registers.Result3};
        for (Size word{0U}; word < 4U; ++word)
        {
            for (Size byte{0U}; byte < 8U; ++byte)
            {
                g_seedInput[(((set * 4U) + word) * 8U) + byte] = static_cast<UInt8>(words[word] >> (byte * 8U));
            }
        }
    }
    emit(g_seedInput, InputBytes);
}

[[nodiscard]] RegisterSets MakeSets(const UInt64 information, const UInt64 addressing, const UInt64 insertable,
                                    const UInt64 purgeable, const UInt64 attributes) noexcept
{
    RegisterSets sets{};
    sets.Registers[0U].Result1 = information;
    sets.Registers[0U].Result2 = addressing;
    sets.Registers[1U].Result1 = insertable;
    sets.Registers[1U].Result2 = purgeable;
    sets.Registers[2U].Result1 = attributes;
    return sets;
}

// PAL_VM_SUMMARY vm_info_1: walker, physical bits, key bits, key registers - 1,
// hash tag, DTRs - 1, ITRs - 1, unique caches, cache levels.
[[nodiscard]] constexpr UInt64 Information(const UInt64 walker, const UInt64 physicalBits, const UInt64 keyBits,
                                           const UInt64 keyRegisters, const UInt64 translationRegisters,
                                           const UInt64 uniqueCaches, const UInt64 levels) noexcept
{
    return walker | (physicalBits << 1U) | (keyBits << 8U) | ((keyRegisters - 1U) << 16U) | (UInt64{0x5AU} << 24U) |
           ((translationRegisters - 1U) << 32U) | ((translationRegisters - 1U) << 40U) | (uniqueCaches << 48U) |
           (levels << 56U);
}
} // namespace

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    constexpr UInt64 Minimum{ArchitectedMinimumInsertablePageSizeMask};
    constexpr UInt64 MinimumPurge{ArchitectedMinimumPurgePageSizeMask};
    constexpr UInt64 Typical{Information(1U, 50U, 24U, 16U, 64U, 4U, 2U)};
    constexpr UInt64 TypicalAddressing{60U | (24U << 8U) | (1U << 16U)};
    constexpr UInt64 LargePage{UInt64{1U} << 30U};
    const RegisterSets typical =
        MakeSets(Typical, TypicalAddressing, Minimum | LargePage, MinimumPurge | LargePage, 0xF1U);
    EmitSets(emit, typical);
    const UInt64 smallest{Information(0U, 32U, 18U, 16U, 8U, 2U, 1U)};
    EmitSets(emit, MakeSets(smallest, 50U | (18U << 8U), Minimum, MinimumPurge, 0x11U));
    EmitSets(emit, MakeSets(Information(1U, 63U, 24U, 256U, 256U, 255U, 255U), 60U | (24U << 8U) | (0xFFFFU << 16U),
                            ~UInt64{0xFFFU}, ~UInt64{0xFFFU}, 0xF1U));
    // Near misses of individual rules.
    for (Size set{0U}; set < RegisterSetCount; ++set)
    {
        RegisterSets failed = typical;
        failed.Registers[set].Status = -1;
        EmitSets(emit, failed);
        RegisterSets reserved = typical;
        reserved.Registers[set].Result3 = 1U;
        EmitSets(emit, reserved);
    }
    const UInt64 keyBelowRegion{Information(1U, 50U, 20U, 16U, 64U, 4U, 2U)};
    const UInt64 tooManyCaches{Information(1U, 50U, 24U, 16U, 64U, 5U, 2U)};
    EmitSets(emit, MakeSets(keyBelowRegion, TypicalAddressing, Minimum, MinimumPurge, 0xF1U));
    EmitSets(emit, MakeSets(tooManyCaches, TypicalAddressing, Minimum, MinimumPurge, 0xF1U));
    EmitSets(emit, MakeSets(Typical, TypicalAddressing, Minimum & ~(UInt64{1U} << 16U), MinimumPurge, 0xF1U));
    EmitSets(emit, MakeSets(Typical, TypicalAddressing, Minimum | (UInt64{1U} << 34U), MinimumPurge, 0xF1U));
    EmitSets(emit, MakeSets(Typical, TypicalAddressing, Minimum, MinimumPurge | (UInt64{1U} << 11U), 0xF1U));
    EmitSets(emit, MakeSets(Typical, TypicalAddressing, Minimum, MinimumPurge, 0xE1U));
    EmitSets(emit, MakeSets(Typical, TypicalAddressing, Minimum, MinimumPurge, 0xF3U));
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    UInt8 bytes[InputBytes]{};
    if (size != 0U)
    {
        std::memcpy(bytes, data, size < InputBytes ? size : InputBytes);
    }
    RegisterSets sets{};
    for (Size set{0U}; set < RegisterSetCount; ++set)
    {
        UInt64 words[4U]{};
        for (Size word{0U}; word < 4U; ++word)
        {
            for (Size byte{0U}; byte < 8U; ++byte)
            {
                words[word] |= static_cast<UInt64>(bytes[(((set * 4U) + word) * 8U) + byte]) << (byte * 8U);
            }
        }
        sets.Registers[set] = PalStaticCallResult{static_cast<Int64>(words[0U]), words[1U], words[2U], words[3U]};
    }

    const auto result = PalVirtualMemoryDecoder::Decode(sets.Registers[0U], sets.Registers[1U], sets.Registers[2U]);
    const auto repeated = PalVirtualMemoryDecoder::Decode(sets.Registers[0U], sets.Registers[1U], sets.Registers[2U]);
    // PalVirtualMemoryCapabilities has no padding (pinned by static_assert).
    if ((result.HasValue() != repeated.HasValue()) ||
        (result.HasValue()
             ? (std::memcmp(&result.GetValue(), &repeated.GetValue(), sizeof(PalVirtualMemoryCapabilities)) != 0)
             : (result.GetError() != repeated.GetError())))
    {
        Violation("decoding is deterministic");
    }
    constexpr PalVirtualMemoryDecodeError CallFailures[RegisterSetCount]{
        PalVirtualMemoryDecodeError::SummaryCallFailed,
        PalVirtualMemoryDecodeError::PageSizeCallFailed,
        PalVirtualMemoryDecodeError::MemoryAttributeCallFailed,
    };
    for (Size set{0U}; set < RegisterSetCount; ++set)
    {
        if (sets.Registers[set].Status != 0)
        {
            if (result.HasValue() || (result.GetError() != CallFailures[set]))
            {
                Violation("a failed PAL call is reported as that call's failure");
            }
            return 0;
        }
    }
    if (result.HasValue() != IsAcceptable(sets))
    {
        Violation("Decode accepts exactly the register sets of the documented profile");
    }
    if (!result.HasValue())
    {
        return 0;
    }
    const auto& capabilities = result.GetValue();
    const RegisterSets encoded = Encode(capabilities);
    if ((std::memcmp(&encoded, &sets, sizeof(sets)) != 0) || (capabilities.Translation.Reserved16 != 0U) ||
        (capabilities.Translation.Reserved64 != 0U) || (capabilities.Reserved8 != 0U) ||
        (capabilities.Reserved32 != 0U))
    {
        Violation("accepted capabilities re-encode to exactly the input registers");
    }

    // Reserved fields: summary Result2 bits 32-63 and Result3, page-size
    // Result3, and memory-attribute Result1 bits 8-63, Result2 and Result3.
    struct ReservedBit
    {
        Size Set;
        Size Result;
        UInt32 FirstBit;
        PalVirtualMemoryDecodeError Error;
    };
    constexpr ReservedBit ReservedBits[]{
        {0U, 2U, 32U, PalVirtualMemoryDecodeError::SummaryReservedResult},
        {0U, 3U, 0U, PalVirtualMemoryDecodeError::SummaryReservedResult},
        {1U, 3U, 0U, PalVirtualMemoryDecodeError::PageSizeReservedResult},
        {2U, 1U, 8U, PalVirtualMemoryDecodeError::MemoryAttributeReservedResult},
        {2U, 2U, 0U, PalVirtualMemoryDecodeError::MemoryAttributeReservedResult},
        {2U, 3U, 0U, PalVirtualMemoryDecodeError::MemoryAttributeReservedResult},
    };
    UInt64 hash{0U};
    for (Size index{0U}; index < InputBytes; ++index)
    {
        hash = (hash * 31U) + bytes[index];
    }
    const ReservedBit& reserved = ReservedBits[hash % (sizeof(ReservedBits) / sizeof(ReservedBits[0U]))];
    const UInt32 bit = reserved.FirstBit + static_cast<UInt32>((hash >> 8U) % (64U - reserved.FirstBit));
    RegisterSets perturbed = sets;
    ResultRegister(perturbed.Registers[reserved.Set], reserved.Result) |= UInt64{1U} << bit;
    if (DecodeError(perturbed) != reserved.Error)
    {
        Violation("a reserved bit of an accepted register set is rejected as a reserved result");
    }
    return 0;
}
