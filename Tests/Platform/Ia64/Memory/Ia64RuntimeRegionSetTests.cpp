// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Platform/Ia64/Memory/Ia64RuntimeRegionSet.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask;
using Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask;
using Platform::Ia64::Memory::Ia64BootstrapPreferredPageShift;
using Platform::Ia64::Memory::Ia64BootstrapRegionRegisterWords;
using Platform::Ia64::Memory::Ia64RegionIdentifierAllocationReceipt;
using Platform::Ia64::Memory::Ia64RegionIdentifierAllocator;
using Platform::Ia64::Memory::Ia64RegionIdentifierAllocatorError;
using Platform::Ia64::Memory::Ia64RuntimeRegionCount;
using Platform::Ia64::Memory::Ia64RuntimeRegionSet;
using Platform::Ia64::Memory::Ia64RuntimeRegionSetError;
using Platform::Ia64::Memory::Ia64SharedBootstrapRegionIdentifier;
using Platform::Ia64::Memory::Ia64SharedBootstrapRegionRegister;
using Platform::Ia64::Memory::PalVirtualMemoryCapabilities;
using Platform::Ia64::Memory::TranslationCapabilities;

[[nodiscard]] constexpr TranslationCapabilities Translation(const Foundation::UInt8 regionIdentifierBits = 18U) noexcept
{
    // Deliberately unsupported unrelated values prove that this slice does not
    // repeat PAL translation, page-size, or memory-attribute validation.
    return TranslationCapabilities{
        .InsertablePageSizeMask = 0U,
        .PhysicalAddressBits = 0U,
        .ImplementedVirtualAddressMostSignificantBit = 0U,
        .RegionIdentifierBits = regionIdentifierBits,
        .ProtectionKeyBits = 0U,
        .MemoryAttributeMask = 0xFFU,
        .HardwareWalkerImplemented = false,
        .Reserved16 = 0U,
        .Reserved64 = 0U,
    };
}

[[nodiscard]] constexpr PalVirtualMemoryCapabilities
PalCapabilities(const Foundation::UInt8 regionIdentifierBits = 18U) noexcept
{
    return PalVirtualMemoryCapabilities{
        .Translation =
            TranslationCapabilities{
                .InsertablePageSizeMask = ArchitectedMinimumInsertablePageSizeMask,
                .PhysicalAddressBits = 48U,
                .ImplementedVirtualAddressMostSignificantBit = 53U,
                .RegionIdentifierBits = regionIdentifierBits,
                .ProtectionKeyBits = 24U,
                .MemoryAttributeMask = 0x11U,
                .HardwareWalkerImplemented = true,
                .Reserved16 = 0U,
                .Reserved64 = 0U,
            },
        .PurgePageSizeMask = ArchitectedMinimumPurgePageSizeMask,
        .InstructionTranslationRegisterCount = 8U,
        .DataTranslationRegisterCount = 8U,
        .ProtectionKeyRegisterCount = 16U,
        .MaximumConcurrentPurgesEncoding = 1U,
        .HashTagIdentifier = 0U,
        .UniqueTranslationCacheCount = 1U,
        .TranslationCacheLevelCount = 1U,
        .Reserved8 = 0U,
        .Reserved32 = 0U,
    };
}

[[nodiscard]] constexpr Foundation::UInt64 BootstrapWord(const Foundation::UInt32 regionIdentifier) noexcept
{
    return (static_cast<Foundation::UInt64>(regionIdentifier) << 8U) |
           (static_cast<Foundation::UInt64>(Ia64BootstrapPreferredPageShift) << 2U);
}

[[nodiscard]] constexpr Ia64BootstrapRegionRegisterWords BootstrapWords() noexcept
{
    Ia64BootstrapRegionRegisterWords words{};
    for (Foundation::Size index{0U}; index < Ia64RuntimeRegionCount; ++index)
    {
        words.RegionRegisters[index] =
            BootstrapWord(Ia64SharedBootstrapRegionIdentifier + static_cast<Foundation::UInt32>(index));
    }
    return words;
}

[[nodiscard]] bool IsUnbuilt(const Ia64RuntimeRegionSet& set) noexcept
{
    const auto result = set.GetRegionRegister(0U);
    return !result.HasValue() && (result.GetError() == Ia64RuntimeRegionSetError::RegionSetNotBuilt);
}

[[nodiscard]] bool Matches(const Ia64RuntimeRegionSet& set,
                           const Foundation::UInt64 (&expected)[Ia64RuntimeRegionCount]) noexcept
{
    for (Foundation::Size index{0U}; index < Ia64RuntimeRegionCount; ++index)
    {
        const auto actual = set.GetRegionRegister(index);
        if (!actual.HasValue() || (actual.GetValue() != expected[index]))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool AllocateFirstReceipt(Ia64RegionIdentifierAllocator& allocator,
                                        Ia64RegionIdentifierAllocationReceipt& receipt,
                                        const Foundation::UInt8 regionIdentifierBits = 18U) noexcept
{
    if (allocator.Initialize(PalCapabilities(regionIdentifierBits)) != Ia64RegionIdentifierAllocatorError::None)
    {
        return false;
    }
    const auto result = allocator.Allocate();
    if (!result.HasValue())
    {
        return false;
    }
    receipt = result.GetValue();
    return true;
}

[[nodiscard]] bool AdoptionRejected(const Ia64BootstrapRegionRegisterWords& words,
                                    const TranslationCapabilities& capabilities,
                                    const Ia64RuntimeRegionSetError expected) noexcept
{
    Ia64RuntimeRegionSet output;
    return (Ia64RuntimeRegionSet::AdoptBootstrap(words, capabilities, output) == expected) && IsUnbuilt(output);
}

void TestExactBootstrapAdoption(TestContext& runner) noexcept
{
    Ia64RuntimeRegionSet output;
    const auto defaultLookup = output.GetRegionRegister(0U);
    ASTERIA_EXPECT(runner, "a default region set exposes no zero-word sentinel as a valid RR",
                   !defaultLookup.HasValue(), defaultLookup.GetError() == Ia64RuntimeRegionSetError::RegionSetNotBuilt);

    const Ia64BootstrapRegionRegisterWords bootstrap{BootstrapWords()};
    const Ia64RuntimeRegionSetError error = Ia64RuntimeRegionSet::AdoptBootstrap(bootstrap, Translation(), output);
    ASTERIA_EXPECT(runner, "bootstrap adoption preserves exactly RIDs 0x2000 through 0x2007 with PS 12 and VE off",
                   error == Ia64RuntimeRegionSetError::None, Matches(output, bootstrap.RegionRegisters));

    const auto upperBound = output.GetRegionRegister(Ia64RuntimeRegionCount);
    const auto extremeBound = output.GetRegionRegister(~Foundation::Size{0U});
    ASTERIA_EXPECT(runner, "a built set bounds RR lookup to region numbers zero through seven", !upperBound.HasValue(),
                   upperBound.GetError() == Ia64RuntimeRegionSetError::RegionIndexOutOfBounds, !extremeBound.HasValue(),
                   extremeBound.GetError() == Ia64RuntimeRegionSetError::RegionIndexOutOfBounds);

    Ia64BootstrapRegionRegisterWords replacement{bootstrap};
    replacement.RegionRegisters[7U] ^= Foundation::UInt64{1U} << 8U;
    ASTERIA_EXPECT(runner, "a built immutable set cannot be replaced",
                   Ia64RuntimeRegionSet::AdoptBootstrap(replacement, Translation(), output) ==
                       Ia64RuntimeRegionSetError::OutputAlreadyBuilt,
                   Matches(output, bootstrap.RegionRegisters));
}

void TestRejectedBootstrapValues(TestContext& runner) noexcept
{
    Ia64BootstrapRegionRegisterWords invalid{BootstrapWords()};
    invalid.RegionRegisters[1U] |= Foundation::UInt64{1U} << 1U;
    ASTERIA_EXPECT(runner, "bootstrap adoption rejects reserved low RR bits atomically",
                   AdoptionRejected(invalid, Translation(), Ia64RuntimeRegionSetError::InvalidBootstrapRegionRegister));

    invalid = BootstrapWords();
    invalid.RegionRegisters[2U] |= Foundation::UInt64{1U} << 32U;
    ASTERIA_EXPECT(runner, "bootstrap adoption rejects reserved high RR bits atomically",
                   AdoptionRejected(invalid, Translation(), Ia64RuntimeRegionSetError::InvalidBootstrapRegionRegister));

    invalid = BootstrapWords();
    invalid.RegionRegisters[3U] |= 1U;
    ASTERIA_EXPECT(
        runner, "bootstrap adoption rejects enabled VHPT walking atomically",
        AdoptionRejected(invalid, Translation(), Ia64RuntimeRegionSetError::UnsupportedBootstrapRegionRegister));

    invalid = BootstrapWords();
    invalid.RegionRegisters[4U] = (static_cast<Foundation::UInt64>(0x2004U) << 8U) | (13U << 2U);
    ASTERIA_EXPECT(
        runner, "bootstrap adoption rejects a non-4-KiB preferred page size atomically",
        AdoptionRejected(invalid, Translation(), Ia64RuntimeRegionSetError::UnsupportedBootstrapRegionRegister));

    invalid = BootstrapWords();
    invalid.RegionRegisters[5U] = BootstrapWord(0x40000U);
    ASTERIA_EXPECT(
        runner, "bootstrap adoption rejects an RID outside the admitted width atomically",
        AdoptionRejected(invalid, Translation(18U), Ia64RuntimeRegionSetError::UnrepresentableRegionIdentifier));

    invalid = BootstrapWords();
    invalid.RegionRegisters[0U] = BootstrapWord(0x2001U);
    ASTERIA_EXPECT(runner, "bootstrap adoption rejects a mismatched shared RR0 anchor atomically",
                   AdoptionRejected(invalid, Translation(), Ia64RuntimeRegionSetError::SharedBootstrapAnchorMismatch));

    invalid = BootstrapWords();
    invalid.RegionRegisters[6U] = BootstrapWord(0x2007U);
    ASTERIA_EXPECT(runner, "bootstrap adoption rejects any non-anchor RID sequence mismatch atomically",
                   AdoptionRejected(invalid, Translation(), Ia64RuntimeRegionSetError::BootstrapRegionSetMismatch));

    TranslationCapabilities invalidCapabilities{Translation(17U)};
    ASTERIA_EXPECT(
        runner, "a seventeen-bit RID width is rejected atomically",
        AdoptionRejected(BootstrapWords(), invalidCapabilities, Ia64RuntimeRegionSetError::InvalidCapabilities));
    invalidCapabilities = Translation(25U);
    ASTERIA_EXPECT(
        runner, "a twenty-five-bit RID width is rejected atomically",
        AdoptionRejected(BootstrapWords(), invalidCapabilities, Ia64RuntimeRegionSetError::InvalidCapabilities));
    invalidCapabilities = Translation();
    invalidCapabilities.Reserved16 = 1U;
    ASTERIA_EXPECT(
        runner, "a retained sixteen-bit reserved capability field is rejected atomically",
        AdoptionRejected(BootstrapWords(), invalidCapabilities, Ia64RuntimeRegionSetError::InvalidCapabilities));
    invalidCapabilities = Translation();
    invalidCapabilities.Reserved64 = 1U;
    ASTERIA_EXPECT(
        runner, "a retained sixty-four-bit reserved capability field is rejected atomically",
        AdoptionRejected(BootstrapWords(), invalidCapabilities, Ia64RuntimeRegionSetError::InvalidCapabilities));
}

void TestExactRuntimeConstruction(TestContext& runner) noexcept
{
    Ia64RegionIdentifierAllocator allocator;
    Ia64RegionIdentifierAllocationReceipt firstReceipt;
    const bool firstAllocated = AllocateFirstReceipt(allocator, firstReceipt);
    Ia64RuntimeRegionSet first;
    const Ia64RuntimeRegionSetError firstError = Ia64RuntimeRegionSet::Construct(firstReceipt, Translation(), first);

    Foundation::UInt64 expectedFirst[Ia64RuntimeRegionCount]{};
    expectedFirst[0U] = Ia64SharedBootstrapRegionRegister;
    for (Foundation::Size index{1U}; index < Ia64RuntimeRegionCount; ++index)
    {
        expectedFirst[index] = (static_cast<Foundation::UInt64>(0x2008U + index) << 8U) | 0x30U;
    }
    bool candidateZeroUnused{true};
    constexpr Foundation::UInt64 candidateZeroWord{(Foundation::UInt64{0x2008U} << 8U) | 0x30U};
    for (Foundation::Size index{0U}; index < Ia64RuntimeRegionCount; ++index)
    {
        const auto word = first.GetRegionRegister(index);
        candidateZeroUnused = candidateZeroUnused && word.HasValue() && (word.GetValue() != candidateZeroWord);
    }
    ASTERIA_EXPECT(runner, "runtime construction keeps exact RR0 and uses only candidates one through seven",
                   firstAllocated, firstError == Ia64RuntimeRegionSetError::None, Matches(first, expectedFirst),
                   candidateZeroUnused);

    const auto secondResult = allocator.Allocate();
    Ia64RegionIdentifierAllocationReceipt secondReceipt;
    if (secondResult.HasValue())
    {
        secondReceipt = secondResult.GetValue();
    }
    Ia64RuntimeRegionSet second;
    const Ia64RuntimeRegionSetError secondError = Ia64RuntimeRegionSet::Construct(secondReceipt, Translation(), second);
    Foundation::UInt64 expectedSecond[Ia64RuntimeRegionCount]{};
    expectedSecond[0U] = Ia64SharedBootstrapRegionRegister;
    bool supportedControlFields{true};
    for (Foundation::Size index{1U}; index < Ia64RuntimeRegionCount; ++index)
    {
        expectedSecond[index] = (static_cast<Foundation::UInt64>(0x2010U + index) << 8U) | 0x30U;
        const auto word = second.GetRegionRegister(index);
        supportedControlFields = supportedControlFields && word.HasValue() && ((word.GetValue() & 0xFFU) == 0x30U);
    }
    ASTERIA_EXPECT(runner, "a later receipt yields private RIDs with PS 12 and VHPT off", secondResult.HasValue(),
                   secondError == Ia64RuntimeRegionSetError::None, Matches(second, expectedSecond),
                   supportedControlFields);

    ASTERIA_EXPECT(runner, "runtime construction cannot overwrite an immutable built set",
                   Ia64RuntimeRegionSet::Construct(secondReceipt, Translation(), first) ==
                       Ia64RuntimeRegionSetError::OutputAlreadyBuilt,
                   Matches(first, expectedFirst));
}

void TestRuntimeFailuresAreAtomic(TestContext& runner) noexcept
{
    Ia64RegionIdentifierAllocator allocator;
    Ia64RegionIdentifierAllocationReceipt receipt;
    const bool allocated = AllocateFirstReceipt(allocator, receipt);

    const Ia64RegionIdentifierAllocationReceipt invalidReceipt{};
    Ia64RuntimeRegionSet invalidReceiptOutput;
    ASTERIA_EXPECT(runner, "a default receipt is rejected with output unmodified",
                   Ia64RuntimeRegionSet::Construct(invalidReceipt, Translation(), invalidReceiptOutput) ==
                       Ia64RuntimeRegionSetError::InvalidReceipt,
                   IsUnbuilt(invalidReceiptOutput));

    TranslationCapabilities invalidCapabilities{Translation()};
    invalidCapabilities.Reserved64 = 1U;
    Ia64RuntimeRegionSet capabilityOutput;
    ASTERIA_EXPECT(runner, "runtime construction rejects reserved capability state atomically", allocated,
                   Ia64RuntimeRegionSet::Construct(receipt, invalidCapabilities, capabilityOutput) ==
                       Ia64RuntimeRegionSetError::InvalidCapabilities,
                   IsUnbuilt(capabilityOutput));
}

void TestReceiptRepresentabilityMismatch(TestContext& runner) noexcept
{
    constexpr Foundation::UInt32 FirstUnrepresentableBase{0x40000U};
    constexpr Foundation::UInt32 FirstAllocatedBase{0x2008U};
    constexpr Foundation::UInt32 ReceiptStride{8U};
    constexpr Foundation::UInt32 AllocationIndex{(FirstUnrepresentableBase - FirstAllocatedBase) / ReceiptStride};

    Ia64RegionIdentifierAllocator allocator;
    bool allocated = allocator.Initialize(PalCapabilities(24U)) == Ia64RegionIdentifierAllocatorError::None;
    Ia64RegionIdentifierAllocationReceipt receipt;
    for (Foundation::UInt32 index{0U}; index <= AllocationIndex; ++index)
    {
        const auto result = allocator.Allocate();
        if (!result.HasValue())
        {
            allocated = false;
            continue;
        }
        receipt = result.GetValue();
    }

    const auto firstCandidate = receipt.GetRegionIdentifier(0U);
    Ia64RuntimeRegionSet output;
    ASTERIA_EXPECT(runner, "a valid wider receipt cannot escape the admitted runtime RID width", allocated,
                   firstCandidate.HasValue(), firstCandidate.GetValue() == FirstUnrepresentableBase,
                   Ia64RuntimeRegionSet::Construct(receipt, Translation(18U), output) ==
                       Ia64RuntimeRegionSetError::UnrepresentableRegionIdentifier,
                   IsUnbuilt(output));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64RuntimeRegionSetTests"};
    Asteria::Test::TestExactBootstrapAdoption(runner);
    Asteria::Test::TestRejectedBootstrapValues(runner);
    Asteria::Test::TestExactRuntimeConstruction(runner);
    Asteria::Test::TestRuntimeFailuresAreAtomic(runner);
    Asteria::Test::TestReceiptRepresentabilityMismatch(runner);
    return runner.Finish();
}
