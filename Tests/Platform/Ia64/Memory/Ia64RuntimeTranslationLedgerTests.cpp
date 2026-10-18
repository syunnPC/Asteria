// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Foundation/Types.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/AddressSpaceMappingRecord.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeTranslationEncoder.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeTranslationLedger.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::FrameKind;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::VirtualAddress;
using Platform::Ia64::Memory::EncodedIa64RuntimeTranslation;
using Platform::Ia64::Memory::Ia64RuntimeTranslationEncoder;
using Platform::Ia64::Memory::Ia64RuntimeTranslationEncodingError;
using Platform::Ia64::Memory::Ia64RuntimeTranslationLedger;
using Platform::Ia64::Memory::Ia64RuntimeTranslationLedgerError;
using Platform::Ia64::Memory::Ia64RuntimeTranslationLedgerFailure;
using Platform::Ia64::Memory::Ia64RuntimeTranslationLedgerWireRecord;
using Platform::Ia64::Memory::Ia64RuntimeTranslationLedgerWireStorage;
using Platform::Ia64::Memory::MaximumIa64RuntimeMappingPages;
using Platform::Ia64::Memory::TranslationCapabilities;

constexpr Foundation::UInt64 RegionFiveBase{0xA000'0000'0010'0000ULL};
constexpr Foundation::UInt64 RegionSixBase{0xC000'0000'0100'0000ULL};
constexpr Foundation::UInt64 PhysicalBase{0x0000'0000'0020'0000ULL};
constexpr Foundation::UInt64 PageSizeBytes{0x1000U};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};

static_assert(Kernel::Memory::MaximumRuntimeAddressSpaceMappings == 256U);
static_assert(sizeof(Ia64RuntimeTranslationLedgerWireRecord) == 24U);
static_assert(alignof(Ia64RuntimeTranslationLedgerWireRecord) == 8U);
static_assert(offsetof(Ia64RuntimeTranslationLedgerWireRecord, VirtualStart) == 0U);
static_assert(offsetof(Ia64RuntimeTranslationLedgerWireRecord, BaseTranslationWord) == 8U);
static_assert(offsetof(Ia64RuntimeTranslationLedgerWireRecord, PageCount) == 16U);
static_assert(sizeof(Ia64RuntimeTranslationLedgerWireStorage) == 6176U);
static_assert(alignof(Ia64RuntimeTranslationLedgerWireStorage) == 8U);
static_assert(offsetof(Ia64RuntimeTranslationLedgerWireStorage, Records) == 24U);
static_assert(offsetof(Ia64RuntimeTranslationLedgerWireStorage, PublicationWord) == 6168U);
static_assert(sizeof(Ia64RuntimeTranslationLedger) == 6176U);

[[nodiscard]] constexpr PagePermissions ReadOnly() noexcept
{
    return PagePermissions{PagePermission::Read};
}

[[nodiscard]] constexpr PagePermissions ReadWrite() noexcept
{
    return ReadOnly().With(PagePermission::Write);
}

[[nodiscard]] constexpr PagePermissions ReadExecute() noexcept
{
    return ReadOnly().With(PagePermission::Execute);
}

[[nodiscard]] constexpr TranslationCapabilities FullCapabilities() noexcept
{
    return TranslationCapabilities{
        .InsertablePageSizeMask = Foundation::UInt64{1U} << 12U,
        .PhysicalAddressBits = 50U,
        .ImplementedVirtualAddressMostSignificantBit = 60U,
        .RegionIdentifierBits = 24U,
        .ProtectionKeyBits = 24U,
        .MemoryAttributeMask = static_cast<Foundation::UInt8>(
            (Foundation::UInt8{1U} << 0U) | (Foundation::UInt8{1U} << 4U) | (Foundation::UInt8{1U} << 5U) |
            (Foundation::UInt8{1U} << 6U) | (Foundation::UInt8{1U} << 7U)),
        .HardwareWalkerImplemented = true,
        .Reserved16 = 0U,
        .Reserved64 = 0U,
    };
}

[[nodiscard]] constexpr AddressSpaceMappingRecord
MakePlan(const Foundation::UInt64 virtualStart = RegionFiveBase, const Foundation::UInt64 physicalStart = PhysicalBase,
         const Foundation::UInt64 pages = 1U, const PagePermissions permissions = ReadOnly(),
         const MemoryCachePolicy cachePolicy = MemoryCachePolicy::WriteBack,
         const FrameKind kind = FrameKind::Ram) noexcept
{
    return AddressSpaceMappingRecord{
        .MemoryObject = {},
        .DeviceRegion = {},
        .Frame = {},
        .VirtualStart = VirtualAddress{virtualStart},
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Pages = PageCount{pages},
        .MemoryObjectOffsetPages = 0U,
        .Permissions = permissions,
        .CachePolicy = cachePolicy,
        .Kind = kind,
        .Commitment = {},
        .RuntimeMapping = true,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr bool Succeeded(const Ia64RuntimeTranslationLedgerFailure failure) noexcept
{
    return (failure.Code == Ia64RuntimeTranslationLedgerError::None) && (failure.Reserved[0] == 0U) &&
           (failure.Reserved[1] == 0U) && (failure.Reserved[2] == 0U) &&
           (failure.EncodingError == Ia64RuntimeTranslationEncodingError::None);
}

[[nodiscard]] constexpr bool
IsFailure(const Ia64RuntimeTranslationLedgerFailure failure, const Ia64RuntimeTranslationLedgerError code,
          const Ia64RuntimeTranslationEncodingError encodingError = Ia64RuntimeTranslationEncodingError::None) noexcept
{
    return (failure.Code == code) && (failure.Reserved[0] == 0U) && (failure.Reserved[1] == 0U) &&
           (failure.Reserved[2] == 0U) && (failure.EncodingError == encodingError);
}

[[nodiscard]] constexpr bool IsExact(const EncodedIa64RuntimeTranslation& actual,
                                     const EncodedIa64RuntimeTranslation& expected) noexcept
{
    return (actual.TranslationWord == expected.TranslationWord) &&
           (actual.TranslationInsertion == expected.TranslationInsertion) &&
           (actual.FaultingAddress == expected.FaultingAddress);
}

void TestConstantInitialization(TestContext& runner) noexcept
{
    constinit static Ia64RuntimeTranslationLedger ledger;
    ASTERIA_EXPECT(runner, "constinit zero storage begins uninitialized without a constructor walker",
                   IsFailure(ledger.Append(MakePlan()), Ia64RuntimeTranslationLedgerError::NotInitialized));
    ASTERIA_EXPECT(runner, "constinit zero storage transitions to initialized-empty",
                   Succeeded(ledger.Initialize(FullCapabilities())));
    ASTERIA_EXPECT(runner, "constinit ledger publishes its first encoded record", Succeeded(ledger.Append(MakePlan())));
    ASTERIA_EXPECT(runner, "constinit ledger returns its first published record",
                   ledger.Lookup(RegionFiveBase + 0xFFFU).HasValue());
}

void TestInitializationAndUninitializedUse(TestContext& runner) noexcept
{
    Ia64RuntimeTranslationLedger ledger;
    ASTERIA_EXPECT(runner, "append before initialization is rejected",
                   IsFailure(ledger.Append(MakePlan()), Ia64RuntimeTranslationLedgerError::NotInitialized));
    const auto uninitializedLookup = ledger.Lookup(RegionFiveBase);
    ASTERIA_EXPECT(runner, "lookup before initialization is rejected", !uninitializedLookup.HasValue(),
                   IsFailure(uninitializedLookup.GetError(), Ia64RuntimeTranslationLedgerError::NotInitialized));

    TranslationCapabilities malformed = FullCapabilities();
    malformed.PhysicalAddressBits = 31U;
    ASTERIA_EXPECT(runner, "malformed capabilities propagate the encoder error",
                   IsFailure(ledger.Initialize(malformed), Ia64RuntimeTranslationLedgerError::EncodingRejected,
                             Ia64RuntimeTranslationEncodingError::InvalidCapabilities));

    malformed = FullCapabilities();
    malformed.InsertablePageSizeMask = 0U;
    ASTERIA_EXPECT(runner, "missing 4 KiB insertion support propagates exactly",
                   IsFailure(ledger.Initialize(malformed), Ia64RuntimeTranslationLedgerError::EncodingRejected,
                             Ia64RuntimeTranslationEncodingError::UnsupportedPageSize));

    ASTERIA_EXPECT(runner, "failed initialization is atomic and retryable",
                   Succeeded(ledger.Initialize(FullCapabilities())));
    ASTERIA_EXPECT(
        runner, "successful initialization cannot be repeated",
        IsFailure(ledger.Initialize(FullCapabilities()), Ia64RuntimeTranslationLedgerError::AlreadyInitialized));
}

void TestExactReconstructionAndTopPage(TestContext& runner) noexcept
{
    Ia64RuntimeTranslationLedger ledger;
    ASTERIA_EXPECT(runner, "exact-reconstruction ledger initializes", Succeeded(ledger.Initialize(FullCapabilities())));

    const AddressSpaceMappingRecord plan =
        MakePlan(RegionFiveBase, PhysicalBase, 3U, ReadWrite(), MemoryCachePolicy::Uncached);
    ASTERIA_EXPECT(runner, "three-page mapping appends", Succeeded(ledger.Append(plan)));

    const auto expectedFirst = Ia64RuntimeTranslationEncoder::EncodePage(plan, 0U, FullCapabilities());
    const auto expectedLast = Ia64RuntimeTranslationEncoder::EncodePage(plan, 2U, FullCapabilities());
    const auto actualFirst = ledger.Lookup(RegionFiveBase + 0x321U);
    const auto actualLast = ledger.Lookup(RegionFiveBase + (2U * PageSizeBytes) + 0xABCU);
    ASTERIA_EXPECT(runner, "offset within first page reconstructs its exact aligned result", expectedFirst.HasValue(),
                   actualFirst.HasValue(), IsExact(actualFirst.GetValue(), expectedFirst.GetValue()));
    ASTERIA_EXPECT(runner, "offset within last page reconstructs its exact aligned result", expectedLast.HasValue(),
                   actualLast.HasValue(), IsExact(actualLast.GetValue(), expectedLast.GetValue()));

    constexpr Foundation::UInt64 maximumVirtualStart{RegionFiveBase + 0x0100'0000U};
    const AddressSpaceMappingRecord maximumPlan =
        MakePlan(maximumVirtualStart, PhysicalBase + 0x0100'0000U, MaximumIa64RuntimeMappingPages, ReadOnly());
    ASTERIA_EXPECT(runner, "maximum 512-page plan appends", Succeeded(ledger.Append(maximumPlan)));
    const Foundation::UInt64 maximumLastIndex = MaximumIa64RuntimeMappingPages - 1U;
    const auto expectedMaximumLast =
        Ia64RuntimeTranslationEncoder::EncodePage(maximumPlan, maximumLastIndex, FullCapabilities());
    const auto actualMaximumLast =
        ledger.Lookup(maximumVirtualStart + (maximumLastIndex * PageSizeBytes) + PageOffsetMask);
    ASTERIA_EXPECT(runner, "maximum plan last page reconstructs exactly from the compact record",
                   expectedMaximumLast.HasValue(), actualMaximumLast.HasValue(),
                   IsExact(actualMaximumLast.GetValue(), expectedMaximumLast.GetValue()));

    constexpr Foundation::UInt64 topPage{0xFFFF'FFFF'FFFF'F000ULL};
    const AddressSpaceMappingRecord topPlan = MakePlan(topPage, PhysicalBase + 0x8000U, 1U, ReadExecute());
    ASTERIA_EXPECT(runner, "UINT64 top page appends without an exclusive-end overflow",
                   Succeeded(ledger.Append(topPlan)));
    const auto expectedTop = Ia64RuntimeTranslationEncoder::EncodePage(topPlan, 0U, FullCapabilities());
    const auto actualTop = ledger.Lookup(0xFFFF'FFFF'FFFF'FFFFULL);
    ASTERIA_EXPECT(runner, "UINT64_MAX raw fault address reconstructs the aligned top page exactly",
                   expectedTop.HasValue(), actualTop.HasValue(), IsExact(actualTop.GetValue(), expectedTop.GetValue()));

    const auto before = ledger.Lookup(RegionFiveBase - 1U);
    const auto after = ledger.Lookup(RegionFiveBase + (3U * PageSizeBytes));
    ASTERIA_EXPECT(runner, "raw address in the page immediately before is missing", !before.HasValue(),
                   IsFailure(before.GetError(), Ia64RuntimeTranslationLedgerError::MappingNotFound));
    ASTERIA_EXPECT(runner, "address at the adjacent page after the mapping is missing", !after.HasValue(),
                   IsFailure(after.GetError(), Ia64RuntimeTranslationLedgerError::MappingNotFound));
}

void TestDataAndInstructionPermissions(TestContext& runner) noexcept
{
    Ia64RuntimeTranslationLedger ledger;
    ASTERIA_EXPECT(runner, "permission ledger initializes", Succeeded(ledger.Initialize(FullCapabilities())));

    const AddressSpaceMappingRecord readOnly = MakePlan(RegionFiveBase, PhysicalBase, 1U, ReadOnly());
    const AddressSpaceMappingRecord readWrite =
        MakePlan(RegionFiveBase + 0x4000U, PhysicalBase + 0x4000U, 1U, ReadWrite());
    const AddressSpaceMappingRecord readExecute =
        MakePlan(RegionFiveBase + 0x8000U, PhysicalBase + 0x8000U, 1U, ReadExecute());
    ASTERIA_EXPECT(runner, "R mapping appends", Succeeded(ledger.Append(readOnly)));
    ASTERIA_EXPECT(runner, "RW mapping appends", Succeeded(ledger.Append(readWrite)));
    ASTERIA_EXPECT(runner, "RX mapping appends", Succeeded(ledger.Append(readExecute)));

    const AddressSpaceMappingRecord plans[]{readOnly, readWrite, readExecute};
    for (const AddressSpaceMappingRecord& plan : plans)
    {
        const auto expected = Ia64RuntimeTranslationEncoder::EncodePage(plan, 0U, FullCapabilities());
        const auto data = ledger.Lookup(plan.VirtualStart.GetValue() + 0x5A5U);
        ASTERIA_EXPECT(runner, "raw-address lookup preserves each exact encoded AR word", expected.HasValue(),
                       data.HasValue(), IsExact(data.GetValue(), expected.GetValue()));
    }

    const auto readDataSide = ledger.Lookup(readOnly.VirtualStart.GetValue() + 1U);
    const auto readInstructionSide = ledger.Lookup(readOnly.VirtualStart.GetValue() + 0xFFFU);
    ASTERIA_EXPECT(runner, "non-executable AR0 mapping is returned identically for either miss side",
                   readDataSide.HasValue(), readInstructionSide.HasValue(),
                   IsExact(readDataSide.GetValue(), readInstructionSide.GetValue()));

    const auto missing = ledger.Lookup(RegionFiveBase + 0xC000U);
    ASTERIA_EXPECT(runner, "missing aligned mapping is rejected", !missing.HasValue(),
                   IsFailure(missing.GetError(), Ia64RuntimeTranslationLedgerError::MappingNotFound));
}

void TestOverlapBoundaries(TestContext& runner) noexcept
{
    Ia64RuntimeTranslationLedger ledger;
    ASTERIA_EXPECT(runner, "overlap ledger initializes", Succeeded(ledger.Initialize(FullCapabilities())));
    ASTERIA_EXPECT(runner, "overlap baseline appends",
                   Succeeded(ledger.Append(MakePlan(RegionFiveBase, PhysicalBase, 3U))));
    ASTERIA_EXPECT(runner, "mapping ending one page before is accepted",
                   Succeeded(ledger.Append(MakePlan(RegionFiveBase - PageSizeBytes, PhysicalBase + 0x10000U, 1U))));
    ASTERIA_EXPECT(
        runner, "mapping starting one page after is accepted",
        Succeeded(ledger.Append(MakePlan(RegionFiveBase + (3U * PageSizeBytes), PhysicalBase + 0x11000U, 1U))));

    const AddressSpaceMappingRecord overlaps[]{
        MakePlan(RegionFiveBase, PhysicalBase + 0x20000U, 1U),
        MakePlan(RegionFiveBase - (2U * PageSizeBytes), PhysicalBase + 0x21000U, 3U),
        MakePlan(RegionFiveBase + (2U * PageSizeBytes), PhysicalBase + 0x22000U, 2U),
        MakePlan(RegionFiveBase - (2U * PageSizeBytes), PhysicalBase + 0x23000U, 7U),
    };
    for (const AddressSpaceMappingRecord& plan : overlaps)
    {
        ASTERIA_EXPECT(runner, "each inclusive overlap shape is rejected",
                       IsFailure(ledger.Append(plan), Ia64RuntimeTranslationLedgerError::OverlappingVirtualRange));
    }
}

void TestEncoderFailuresRemainAtomic(TestContext& runner) noexcept
{
    Ia64RuntimeTranslationLedger ledger;
    ASTERIA_EXPECT(runner, "encoder-failure ledger initializes", Succeeded(ledger.Initialize(FullCapabilities())));

    AddressSpaceMappingRecord malformed = MakePlan();
    malformed.Reserved[0] = 1U;
    ASTERIA_EXPECT(runner, "malformed plan error propagates exactly",
                   IsFailure(ledger.Append(malformed), Ia64RuntimeTranslationLedgerError::EncodingRejected,
                             Ia64RuntimeTranslationEncodingError::InvalidMappingPlan));
    ASTERIA_EXPECT(runner, "oversized mapping error propagates exactly",
                   IsFailure(ledger.Append(MakePlan(RegionFiveBase, PhysicalBase, MaximumIa64RuntimeMappingPages + 1U)),
                             Ia64RuntimeTranslationLedgerError::EncodingRejected,
                             Ia64RuntimeTranslationEncodingError::MappingTooLarge));
    ASTERIA_EXPECT(runner, "permission error propagates exactly",
                   IsFailure(ledger.Append(MakePlan(RegionFiveBase, PhysicalBase, 1U, PagePermissions{})),
                             Ia64RuntimeTranslationLedgerError::EncodingRejected,
                             Ia64RuntimeTranslationEncodingError::UnsupportedPermissions));
    ASTERIA_EXPECT(runner, "cache-policy error propagates exactly",
                   IsFailure(ledger.Append(MakePlan(RegionFiveBase, PhysicalBase, 1U, ReadOnly(),
                                                    MemoryCachePolicy::WriteThrough)),
                             Ia64RuntimeTranslationLedgerError::EncodingRejected,
                             Ia64RuntimeTranslationEncodingError::UnsupportedCachePolicy));
    ASTERIA_EXPECT(runner, "device-execution error propagates exactly",
                   IsFailure(ledger.Append(MakePlan(RegionFiveBase, PhysicalBase, 1U, ReadExecute(),
                                                    MemoryCachePolicy::Uncached, FrameKind::Device)),
                             Ia64RuntimeTranslationLedgerError::EncodingRejected,
                             Ia64RuntimeTranslationEncodingError::DeviceExecutionDenied));
    ASTERIA_EXPECT(
        runner, "invalid full-plan physical tail propagates exactly",
        IsFailure(ledger.Append(MakePlan(RegionFiveBase, (Foundation::UInt64{1U} << 50U) - PageSizeBytes, 2U)),
                  Ia64RuntimeTranslationLedgerError::EncodingRejected,
                  Ia64RuntimeTranslationEncodingError::PhysicalAddressOutOfRange));

    ASTERIA_EXPECT(runner, "all encoder failures leave the first record slot retryable",
                   Succeeded(ledger.Append(MakePlan())));
    const auto lookup = ledger.Lookup(RegionFiveBase);
    ASTERIA_EXPECT(runner, "retry after encoder failures publishes a usable record", lookup.HasValue());
}

void TestFixedCapacityAndFailureAtomicity(TestContext& runner) noexcept
{
    Ia64RuntimeTranslationLedger ledger;
    ASTERIA_EXPECT(runner, "capacity ledger initializes", Succeeded(ledger.Initialize(FullCapabilities())));

    for (Foundation::Size index{0U}; index < Kernel::Memory::MaximumRuntimeAddressSpaceMappings; ++index)
    {
        const Foundation::UInt64 offset = static_cast<Foundation::UInt64>(index) * 2U * PageSizeBytes;
        const Foundation::UInt64 physicalOffset = static_cast<Foundation::UInt64>(index) * PageSizeBytes;
        const Ia64RuntimeTranslationLedgerFailure append =
            ledger.Append(MakePlan(RegionSixBase + offset, PhysicalBase + physicalOffset, 1U, ReadWrite()));
        ASTERIA_EXPECT(runner, "each of exactly 256 records appends", Succeeded(append));

        if (index == 0U)
        {
            ASTERIA_EXPECT(runner, "failed overlap does not consume a record slot",
                           IsFailure(ledger.Append(MakePlan(RegionSixBase, PhysicalBase + 0x400000U, 1U)),
                                     Ia64RuntimeTranslationLedgerError::OverlappingVirtualRange));
        }
    }

    const Foundation::UInt64 exhaustedOffset =
        static_cast<Foundation::UInt64>(Kernel::Memory::MaximumRuntimeAddressSpaceMappings) * 2U * PageSizeBytes;
    ASTERIA_EXPECT(runner, "record 257 is rejected by the exact fixed capacity",
                   IsFailure(ledger.Append(MakePlan(RegionSixBase + exhaustedOffset, PhysicalBase + 0x500000U, 1U)),
                             Ia64RuntimeTranslationLedgerError::CapacityExhausted));

    const Foundation::UInt64 lastIndex =
        static_cast<Foundation::UInt64>(Kernel::Memory::MaximumRuntimeAddressSpaceMappings - 1U);
    const auto last = ledger.Lookup(RegionSixBase + (lastIndex * 2U * PageSizeBytes));
    ASTERIA_EXPECT(runner, "last record remains readable after capacity exhaustion", last.HasValue(),
                   last.GetValue().FaultingAddress == RegionSixBase + (lastIndex * 2U * PageSizeBytes));
}

void TestTombstones(TestContext& runner) noexcept
{
    Ia64RuntimeTranslationLedger uninitialized;
    ASTERIA_EXPECT(runner, "an uninitialized ledger removes nothing",
                   IsFailure(uninitialized.Remove(MakePlan()), Ia64RuntimeTranslationLedgerError::NotInitialized));

    Ia64RuntimeTranslationLedger ledger;
    const AddressSpaceMappingRecord first = MakePlan(RegionFiveBase, PhysicalBase, 3U, ReadWrite());
    const AddressSpaceMappingRecord second = MakePlan(RegionFiveBase + 0x10000U, PhysicalBase + 0x10000U, 1U);
    ASTERIA_EXPECT(runner, "tombstone ledger initializes and appends two records",
                   Succeeded(ledger.Initialize(FullCapabilities())), Succeeded(ledger.Append(first)),
                   Succeeded(ledger.Append(second)));

    AddressSpaceMappingRecord malformed = first;
    malformed.Reserved[0] = 1U;
    ASTERIA_EXPECT(runner, "a removal Append could not accept is rejected by the encoder",
                   IsFailure(ledger.Remove(malformed), Ia64RuntimeTranslationLedgerError::EncodingRejected,
                             Ia64RuntimeTranslationEncodingError::InvalidMappingPlan));
    ASTERIA_EXPECT(runner, "only the exact record is removed",
                   IsFailure(ledger.Remove(MakePlan(RegionFiveBase, PhysicalBase, 2U, ReadWrite())),
                             Ia64RuntimeTranslationLedgerError::MappingNotFound),
                   IsFailure(ledger.Remove(MakePlan(RegionFiveBase, PhysicalBase + PageSizeBytes, 3U, ReadWrite())),
                             Ia64RuntimeTranslationLedgerError::MappingNotFound),
                   IsFailure(ledger.Remove(MakePlan(RegionFiveBase, PhysicalBase, 3U)),
                             Ia64RuntimeTranslationLedgerError::MappingNotFound),
                   IsFailure(ledger.Remove(MakePlan(RegionFiveBase + PageSizeBytes, PhysicalBase + PageSizeBytes, 2U,
                                                    ReadWrite())),
                             Ia64RuntimeTranslationLedgerError::MappingNotFound),
                   ledger.Lookup(RegionFiveBase + (2U * PageSizeBytes)).HasValue());

    ASTERIA_EXPECT(runner, "removal tombstones the record in place without republishing",
                   Succeeded(ledger.Remove(first)), ledger.GetWireStorage().Records[0].PageCount == 0U,
                   ledger.GetWireStorage().Records[0].VirtualStart == RegionFiveBase,
                   ledger.GetWireStorage().PublicationWord.LoadAcquire() == 3U);
    for (Foundation::UInt64 page{0U}; page < 3U; ++page)
    {
        const auto lookup = ledger.Lookup(RegionFiveBase + (page * PageSizeBytes) + 0x123U);
        ASTERIA_EXPECT(runner, "no page of a tombstoned record is found",
                       !lookup.HasValue() &&
                           IsFailure(lookup.GetError(), Ia64RuntimeTranslationLedgerError::MappingNotFound));
    }
    ASTERIA_EXPECT(runner, "the other record survives and a tombstone is not removed twice",
                   ledger.Lookup(RegionFiveBase + 0x10000U).HasValue(),
                   IsFailure(ledger.Remove(first), Ia64RuntimeTranslationLedgerError::MappingNotFound));

    // A tombstone overlaps nothing, including a mapping that starts below it
    // and covers it; acknowledged unmap allows the tombstoned slot to be reused.
    const AddressSpaceMappingRecord covering =
        MakePlan(RegionFiveBase - (2U * PageSizeBytes), PhysicalBase + 0x40000U, 6U, ReadWrite());
    ASTERIA_EXPECT(runner, "a removed range maps again in the tombstoned slot", Succeeded(ledger.Append(covering)),
                   ledger.GetWireStorage().PublicationWord.LoadAcquire() == 3U,
                   ledger.GetWireStorage().Records[0].PageCount == 6U);
    const auto remapped = ledger.Lookup(RegionFiveBase);
    const auto expected = Ia64RuntimeTranslationEncoder::EncodePage(covering, 2U, FullCapabilities());
    ASTERIA_EXPECT(runner, "the remapped page resolves to the new record", remapped.HasValue(), expected.HasValue(),
                   IsExact(remapped.GetValue(), expected.GetValue()));
    ASTERIA_EXPECT(runner, "the live records still exclude overlap",
                   IsFailure(ledger.Append(MakePlan(RegionFiveBase + (3U * PageSizeBytes), PhysicalBase, 1U)),
                             Ia64RuntimeTranslationLedgerError::OverlappingVirtualRange));
}

void TestTombstonesReuseCapacity(TestContext& runner) noexcept
{
    Ia64RuntimeTranslationLedger ledger;
    ASTERIA_EXPECT(runner, "lifetime-capacity ledger initializes", Succeeded(ledger.Initialize(FullCapabilities())));
    Foundation::UInt64 failures{0U};
    for (Foundation::Size index{0U}; index < 1024U; ++index)
    {
        const AddressSpaceMappingRecord plan = MakePlan(RegionSixBase, PhysicalBase, 1U, ReadWrite());
        if (!Succeeded(ledger.Append(plan)) || !Succeeded(ledger.Remove(plan)))
        {
            ++failures;
        }
    }
    ASTERIA_EXPECT(runner, "a ledger reuses a slot for more than 256 acknowledged unmaps", failures == 0U,
                   Succeeded(ledger.Append(MakePlan(RegionSixBase, PhysicalBase, 1U, ReadWrite()))),
                   ledger.GetWireStorage().PublicationWord.LoadAcquire() == 2U);
}

void TestInterleavedReuseReader(TestContext& runner)
{
    Ia64RuntimeTranslationLedger ledger;
    const auto old = MakePlan(RegionFiveBase, PhysicalBase, 1U, ReadWrite());
    const auto replacement = MakePlan(RegionSixBase, PhysicalBase + 0x10000U, 2U, ReadWrite());
    const bool ready = Succeeded(ledger.Initialize(FullCapabilities())) && Succeeded(ledger.Append(old)) &&
                       Succeeded(ledger.Remove(old));
    // The old refill has acknowledged before reuse. Interleave a new reader
    // with each writer word while the publication prefix remains unchanged.
    auto& record = const_cast<Ia64RuntimeTranslationLedgerWireRecord&>(ledger.GetWireStorage().Records[0U]);
    const auto encoded = Ia64RuntimeTranslationEncoder::EncodePage(replacement, 0U, FullCapabilities());
    std::atomic<unsigned int> phase{0U};
    std::atomic<unsigned int> observed{0U};
    bool whole{true};
    std::thread reader{[&]() noexcept
                       {
                           for (unsigned int stage{1U}; stage <= 3U; ++stage)
                           {
                               while (phase.load(std::memory_order_acquire) < stage)
                               {
                                   std::this_thread::yield();
                               }
                               const auto lookup = ledger.Lookup(RegionSixBase + PageSizeBytes);
                               if (stage < 3U)
                               {
                                   whole = whole && !lookup.HasValue();
                               }
                               else
                               {
                                   const auto expected =
                                       Ia64RuntimeTranslationEncoder::EncodePage(replacement, 1U, FullCapabilities());
                                   whole = whole && lookup.HasValue() && expected.HasValue() &&
                                           IsExact(lookup.GetValue(), expected.GetValue());
                               }
                               observed.store(stage, std::memory_order_release);
                           }
                       }};
    record.VirtualStart = replacement.VirtualStart.GetValue();
    phase.store(1U, std::memory_order_release);
    while (observed.load(std::memory_order_acquire) != 1U)
    {
        std::this_thread::yield();
    }
    record.BaseTranslationWord = encoded.GetValue().TranslationWord;
    phase.store(2U, std::memory_order_release);
    while (observed.load(std::memory_order_acquire) != 2U)
    {
        std::this_thread::yield();
    }
    __atomic_store_n(&record.PageCount, replacement.Pages.GetValue(), __ATOMIC_RELEASE);
    phase.store(3U, std::memory_order_release);
    reader.join();
    ASTERIA_EXPECT(runner, "an interleaved refill sees zero until PageCount publishes the replacement payload", ready,
                   encoded.HasValue(), whole, ledger.GetWireStorage().PublicationWord.LoadAcquire() == 2U);
}

void CheckConcurrentLookup(const Ia64RuntimeTranslationLedger& ledger, const Foundation::UInt64 index,
                           const bool requirePublished, std::atomic<Foundation::UInt64>& failures) noexcept
{
    const Foundation::UInt64 virtualAddress = RegionSixBase + (index * 2U * PageSizeBytes);
    const Foundation::UInt64 physicalAddress = PhysicalBase + (index * PageSizeBytes);
    const auto lookup = ledger.Lookup(virtualAddress + 0xA5AU);
    if (!lookup.HasValue())
    {
        if (requirePublished || !IsFailure(lookup.GetError(), Ia64RuntimeTranslationLedgerError::MappingNotFound))
        {
            static_cast<void>(failures.fetch_add(1U));
        }
        return;
    }

    const auto expected = Ia64RuntimeTranslationEncoder::EncodePage(
        MakePlan(virtualAddress, physicalAddress, 1U, ReadWrite()), 0U, FullCapabilities());
    if (!expected.HasValue() || !IsExact(lookup.GetValue(), expected.GetValue()))
    {
        static_cast<void>(failures.fetch_add(1U));
    }
}

void TestConcurrentPublication(TestContext& runner)
{
    Ia64RuntimeTranslationLedger ledger;
    ASTERIA_EXPECT(runner, "concurrent ledger initializes", Succeeded(ledger.Initialize(FullCapabilities())));

    constexpr Foundation::UInt64 readerCount{3U};
    constexpr Foundation::UInt64 recordCount{
        static_cast<Foundation::UInt64>(Kernel::Memory::MaximumRuntimeAddressSpaceMappings)};
    std::atomic<Foundation::UInt64> readersReady{0U};
    std::atomic<bool> start{false};
    std::atomic<bool> writerDone{false};
    std::atomic<Foundation::UInt64> failures{0U};

    const auto reader = [&]() noexcept
    {
        static_cast<void>(readersReady.fetch_add(1U, std::memory_order_release));
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }

        Foundation::UInt64 index{0U};
        while (!writerDone.load(std::memory_order_acquire))
        {
            CheckConcurrentLookup(ledger, index, false, failures);
            index = (index + 1U) % recordCount;
        }
        for (index = 0U; index < recordCount; ++index)
        {
            CheckConcurrentLookup(ledger, index, true, failures);
        }
    };

    std::thread readerOne{reader};
    std::thread readerTwo{reader};
    std::thread readerThree{reader};
    while (readersReady.load(std::memory_order_acquire) != readerCount)
    {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);

    for (Foundation::UInt64 index{0U}; index < recordCount; ++index)
    {
        const Ia64RuntimeTranslationLedgerFailure append = ledger.Append(MakePlan(
            RegionSixBase + (index * 2U * PageSizeBytes), PhysicalBase + (index * PageSizeBytes), 1U, ReadWrite()));
        if (!Succeeded(append))
        {
            static_cast<void>(failures.fetch_add(1U));
        }
    }
    writerDone.store(true, std::memory_order_release);

    readerOne.join();
    readerTwo.join();
    readerThree.join();
    ASTERIA_EXPECT(runner, "release/acquire publication exposes only complete immutable records",
                   failures.load(std::memory_order_acquire) == 0U);
}

void TestConcurrentRemoval(TestContext& runner)
{
    Ia64RuntimeTranslationLedger ledger;
    constexpr Foundation::UInt64 recordCount{64U};
    Foundation::UInt64 appendFailures{0U};
    ASTERIA_EXPECT(runner, "concurrent-removal ledger initializes", Succeeded(ledger.Initialize(FullCapabilities())));
    for (Foundation::UInt64 index{0U}; index < recordCount; ++index)
    {
        if (!Succeeded(ledger.Append(MakePlan(RegionSixBase + (index * 2U * PageSizeBytes),
                                              PhysicalBase + (index * PageSizeBytes), 1U, ReadWrite()))))
        {
            ++appendFailures;
        }
    }

    std::atomic<Foundation::UInt64> readersReady{0U};
    std::atomic<bool> start{false};
    std::atomic<bool> writerDone{false};
    std::atomic<Foundation::UInt64> failures{0U};
    const auto reader = [&]() noexcept
    {
        static_cast<void>(readersReady.fetch_add(1U, std::memory_order_release));
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        Foundation::UInt64 index{0U};
        while (!writerDone.load(std::memory_order_acquire))
        {
            CheckConcurrentLookup(ledger, index, false, failures);
            index = (index + 1U) % recordCount;
        }
        for (index = 0U; index < recordCount; ++index)
        {
            if (ledger.Lookup(RegionSixBase + (index * 2U * PageSizeBytes)).HasValue())
            {
                static_cast<void>(failures.fetch_add(1U));
            }
        }
    };

    std::thread readerOne{reader};
    std::thread readerTwo{reader};
    while (readersReady.load(std::memory_order_acquire) != 2U)
    {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (Foundation::UInt64 index{0U}; index < recordCount; ++index)
    {
        if (!Succeeded(ledger.Remove(MakePlan(RegionSixBase + (index * 2U * PageSizeBytes),
                                              PhysicalBase + (index * PageSizeBytes), 1U, ReadWrite()))))
        {
            static_cast<void>(failures.fetch_add(1U));
        }
    }
    writerDone.store(true, std::memory_order_release);
    readerOne.join();
    readerTwo.join();
    ASTERIA_EXPECT(runner, "readers see a record whole or tombstoned, and none after removal", appendFailures == 0U,
                   failures.load(std::memory_order_acquire) == 0U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64RuntimeTranslationLedgerTests"};
    Asteria::Test::TestConstantInitialization(runner);
    Asteria::Test::TestInitializationAndUninitializedUse(runner);
    Asteria::Test::TestExactReconstructionAndTopPage(runner);
    Asteria::Test::TestDataAndInstructionPermissions(runner);
    Asteria::Test::TestOverlapBoundaries(runner);
    Asteria::Test::TestEncoderFailuresRemainAtomic(runner);
    Asteria::Test::TestFixedCapacityAndFailureAtomicity(runner);
    Asteria::Test::TestTombstones(runner);
    Asteria::Test::TestTombstonesReuseCapacity(runner);
    Asteria::Test::TestInterleavedReuseReader(runner);
    Asteria::Test::TestConcurrentPublication(runner);
    Asteria::Test::TestConcurrentRemoval(runner);
    return runner.Finish();
}
