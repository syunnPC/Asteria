// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableCapture.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Boot::Uefi::ConfigurationTable;
using Boot::Uefi::BootServices;
using Boot::Uefi::Status;
using Platform::Ia64::Firmware::MaximumSalSystemTableSnapshotBytes;
using Platform::Ia64::Firmware::SalSystemTableCapture;
using Platform::Ia64::Firmware::SalSystemTableCaptureError;
using Platform::Ia64::Firmware::SalSystemTableCaptureErrorKind;
using Platform::Ia64::Firmware::SalSystemTableGuid;
using Platform::Ia64::Firmware::SalSystemTableHeaderSize;
using Platform::Ia64::Firmware::SalSystemTableInformation;
using Platform::Ia64::Firmware::SalSystemTableLocateError;
using Platform::Ia64::Firmware::SalSystemTableParseError;

constexpr Foundation::Size PageSizeBytes{4096U};
constexpr Foundation::Size Ia64RuntimeAlignmentBytes{8192U};
constexpr Foundation::Size TableStorageSize{MaximumSalSystemTableSnapshotBytes + Ia64RuntimeAlignmentBytes};
constexpr Foundation::Size SnapshotTailBytes{16U};
constexpr Foundation::Size TableOffset{128U};
constexpr Foundation::Size EntrypointOffset{SalSystemTableHeaderSize};
constexpr Foundation::Size ValidTableLength{SalSystemTableHeaderSize + 48U};
constexpr Foundation::Size MemoryDescriptorSize{40U};
constexpr Foundation::UInt32 BootServicesDataType{4U};
constexpr Foundation::UInt64 WriteBackAttribute{Foundation::UInt64{1U} << 3U};

alignas(Ia64RuntimeAlignmentBytes) Foundation::UInt8 g_tableStorage[TableStorageSize];
alignas(8) Foundation::UInt8 g_snapshot[MaximumSalSystemTableSnapshotBytes + SnapshotTailBytes];
alignas(8) Foundation::UInt8 g_memoryMap[MemoryDescriptorSize];
ConfigurationTable g_configurationTable;
alignas(Ia64RuntimeAlignmentBytes) Foundation::UInt8 g_tableStorageInputSnapshot[TableStorageSize];
alignas(8) Foundation::UInt8 g_memoryMapInputSnapshot[MemoryDescriptorSize];
alignas(8) Foundation::UInt8 g_configurationTableInputSnapshot[sizeof(ConfigurationTable)];
alignas(8) Foundation::UInt8 g_destinationSnapshot[sizeof(g_snapshot)];

struct ConfigurationAliasBuffer
{
    ConfigurationTable Entry;
    Foundation::UInt8 Tail[ValidTableLength];
};

struct MemoryMapAliasBuffer
{
    alignas(8) Foundation::UInt8 Bytes[MemoryDescriptorSize + ValidTableLength];
};

enum class MemoryMapBehavior
{
    Success,
    Failure,
    OversizedSuccess,
};

ConfigurationAliasBuffer g_configurationAlias;
MemoryMapAliasBuffer g_memoryMapAlias;
BootServices g_bootServices{};
alignas(8) Foundation::UInt8 g_freshMemoryMap[MemoryDescriptorSize];
MemoryMapBehavior g_memoryMapBehavior{MemoryMapBehavior::Success};
Foundation::Size g_getMemoryMapCalls{0U};

static_assert(sizeof(ConfigurationAliasBuffer) >= ValidTableLength);
static_assert(sizeof(MemoryMapAliasBuffer) >= ValidTableLength);

void Fill(Foundation::UInt8* const bytes, const Foundation::Size count, const Foundation::UInt8 value) noexcept
{
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        bytes[index] = value;
    }
}

void CopyBytes(Foundation::UInt8* const destination, const Foundation::UInt8* const source,
               const Foundation::Size count) noexcept
{
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        destination[index] = source[index];
    }
}

Status ASTERIA_UEFI_API GetFreshMemoryMap(Foundation::Size* const memoryMapSize, void* const memoryMap,
                                          Foundation::Size* const memoryMapKey, Foundation::Size* const descriptorSize,
                                          Foundation::UInt32* const descriptorVersion) noexcept
{
    ++g_getMemoryMapCalls;
    if ((memoryMapSize == nullptr) || (memoryMap == nullptr) || (memoryMapKey == nullptr) ||
        (descriptorSize == nullptr) || (descriptorVersion == nullptr))
    {
        return Boot::Uefi::InvalidParameter;
    }
    if (g_memoryMapBehavior == MemoryMapBehavior::Failure)
    {
        return Boot::Uefi::DeviceError;
    }

    *memoryMapKey = 0x1234U;
    *descriptorSize = MemoryDescriptorSize;
    *descriptorVersion = 1U;
    if (g_memoryMapBehavior == MemoryMapBehavior::OversizedSuccess)
    {
        *memoryMapSize = sizeof(g_freshMemoryMap) + 1U;
        return Boot::Uefi::Success;
    }
    if (*memoryMapSize < sizeof(g_memoryMap))
    {
        *memoryMapSize = sizeof(g_memoryMap);
        return Boot::Uefi::BufferTooSmall;
    }

    CopyBytes(static_cast<Foundation::UInt8*>(memoryMap), g_memoryMap, sizeof(g_memoryMap));
    *memoryMapSize = sizeof(g_memoryMap);
    return Boot::Uefi::Success;
}

[[nodiscard]] bool AreBytesEqual(const Foundation::UInt8* const first, const Foundation::UInt8* const second,
                                 const Foundation::Size count) noexcept
{
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (first[index] != second[index])
        {
            return false;
        }
    }
    return true;
}

void WriteLittleEndian16(Foundation::UInt8* const bytes, const Foundation::UInt16 value) noexcept
{
    bytes[0] = static_cast<Foundation::UInt8>(value);
    bytes[1] = static_cast<Foundation::UInt8>(value >> 8U);
}

void WriteLittleEndian32(Foundation::UInt8* const bytes, const Foundation::UInt32 value) noexcept
{
    for (Foundation::UInt8 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> static_cast<Foundation::UInt8>(index * 8U));
    }
}

void WriteLittleEndian64(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::UInt8 index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> static_cast<Foundation::UInt8>(index * 8U));
    }
}

[[nodiscard]] Foundation::UInt64 TableStorageAddress() noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_tableStorage));
}

[[nodiscard]] Foundation::UInt8* TableBytes() noexcept
{
    return g_tableStorage + TableOffset;
}

void RepairChecksum() noexcept
{
    Foundation::UInt8* const table = TableBytes();
    table[12U] = 0U;
    Foundation::UInt8 checksum{0U};
    for (Foundation::Size index{0U}; index < ValidTableLength; ++index)
    {
        checksum = static_cast<Foundation::UInt8>(checksum + table[index]);
    }
    table[12U] = static_cast<Foundation::UInt8>(0U - checksum);
}

void ResetFixture() noexcept
{
    Fill(g_tableStorage, sizeof(g_tableStorage), 0xA5U);
    Fill(g_snapshot, sizeof(g_snapshot), 0x5AU);
    Fill(g_memoryMap, sizeof(g_memoryMap), 0U);

    Foundation::UInt8* const table = TableBytes();
    Fill(table, ValidTableLength, 0U);
    table[0U] = static_cast<Foundation::UInt8>('S');
    table[1U] = static_cast<Foundation::UInt8>('S');
    table[2U] = static_cast<Foundation::UInt8>('T');
    table[3U] = static_cast<Foundation::UInt8>('_');
    WriteLittleEndian32(table + 4U, static_cast<Foundation::UInt32>(ValidTableLength));
    WriteLittleEndian16(table + 8U, 0x0340U);
    WriteLittleEndian16(table + 10U, 1U);
    WriteLittleEndian16(table + 20U, 0x0100U);
    WriteLittleEndian16(table + 22U, 0x0100U);
    table[24U] = static_cast<Foundation::UInt8>('Q');
    table[25U] = static_cast<Foundation::UInt8>('E');
    table[26U] = static_cast<Foundation::UInt8>('M');
    table[27U] = static_cast<Foundation::UInt8>('U');
    table[EntrypointOffset] = 0U;
    WriteLittleEndian64(table + EntrypointOffset + 8U, 0x0010'0000U);
    WriteLittleEndian64(table + EntrypointOffset + 16U, 0x0020'0000U);
    WriteLittleEndian64(table + EntrypointOffset + 24U, 0x0030'0000U);
    RepairChecksum();

    WriteLittleEndian32(g_memoryMap, BootServicesDataType);
    WriteLittleEndian64(g_memoryMap + 8U, TableStorageAddress());
    WriteLittleEndian64(g_memoryMap + 24U, static_cast<Foundation::UInt64>(TableStorageSize / PageSizeBytes));
    WriteLittleEndian64(g_memoryMap + 32U, WriteBackAttribute);

    g_configurationTable = ConfigurationTable{
        .VendorGuid = SalSystemTableGuid,
        .VendorTable = TableBytes(),
    };
    g_bootServices = BootServices{};
    g_bootServices.GetMemoryMapFunction = GetFreshMemoryMap;
    Fill(g_freshMemoryMap, sizeof(g_freshMemoryMap), 0xC3U);
    g_memoryMapBehavior = MemoryMapBehavior::Success;
    g_getMemoryMapCalls = 0U;
}

[[nodiscard]] auto Capture(const Foundation::Size capacity = sizeof(g_snapshot)) noexcept
{
    return SalSystemTableCapture::Capture(&g_configurationTable, 1U, g_memoryMap, sizeof(g_memoryMap),
                                          sizeof(g_memoryMap), 1U, g_snapshot, capacity);
}

[[nodiscard]] bool IsExpectedInformation(const SalSystemTableInformation& information) noexcept
{
    return (information.PalProcedurePhysicalAddress == 0x0010'0000U) &&
           (information.SalProcedurePhysicalAddress == 0x0020'0000U) &&
           (information.SalGlobalDataPhysicalAddress == 0x0030'0000U) &&
           (information.TotalTableLength == ValidTableLength) && (information.EntryCount == 1U) &&
           (information.SalRevision == 0x0340U) && !information.HasPlatformFeatures &&
           !information.HasTranslationRegister && !information.HasPurgeCoherenceInformation &&
           !information.HasApWakeVector;
}

[[nodiscard]] bool IsError(const SalSystemTableCaptureError& error, const SalSystemTableCaptureErrorKind kind,
                           const SalSystemTableLocateError locationError,
                           const SalSystemTableParseError parseError) noexcept
{
    return (error.Kind == kind) && (error.LocationError == locationError) && (error.ParseError == parseError);
}

[[nodiscard]] bool IsSnapshotPrefixScrubbed(const Foundation::Size scrubbedBytes) noexcept
{
    for (Foundation::Size index{0U}; index < scrubbedBytes; ++index)
    {
        if (g_snapshot[index] != 0U)
        {
            return false;
        }
    }
    for (Foundation::Size index{scrubbedBytes}; index < sizeof(g_snapshot); ++index)
    {
        if (g_snapshot[index] != 0x5AU)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsSnapshotScrubbed() noexcept
{
    return IsSnapshotPrefixScrubbed(MaximumSalSystemTableSnapshotBytes);
}

void SnapshotInputs() noexcept
{
    CopyBytes(g_tableStorageInputSnapshot, g_tableStorage, sizeof(g_tableStorage));
    CopyBytes(g_memoryMapInputSnapshot, g_memoryMap, sizeof(g_memoryMap));
    CopyBytes(g_configurationTableInputSnapshot, reinterpret_cast<const Foundation::UInt8*>(&g_configurationTable),
              sizeof(g_configurationTable));
}

void SnapshotDestination() noexcept
{
    CopyBytes(g_destinationSnapshot, g_snapshot, sizeof(g_snapshot));
}

void TestSuccessfulOwnedCapture(TestContext& runner) noexcept
{
    ResetFixture();
    SnapshotInputs();
    const auto result = Capture();

    ASTERIA_EXPECT(runner, "valid table is copied and parsed into inert information", result.HasValue(),
                   IsExpectedInformation(result.GetValue()));
    ASTERIA_EXPECT(runner, "capture does not modify firmware table storage",
                   AreBytesEqual(g_tableStorage, g_tableStorageInputSnapshot, sizeof(g_tableStorage)));
    ASTERIA_EXPECT(runner, "capture does not modify the UEFI memory map",
                   AreBytesEqual(g_memoryMap, g_memoryMapInputSnapshot, sizeof(g_memoryMap)));
    ASTERIA_EXPECT(runner, "capture does not modify the UEFI configuration entry",
                   AreBytesEqual(reinterpret_cast<const Foundation::UInt8*>(&g_configurationTable),
                                 g_configurationTableInputSnapshot, sizeof(g_configurationTable)));

    bool declaredBytesCopied{true};
    for (Foundation::Size index{0U}; index < ValidTableLength; ++index)
    {
        declaredBytesCopied = declaredBytesCopied && (g_snapshot[index] == TableBytes()[index]);
    }
    ASTERIA_EXPECT(runner, "declared SAL bytes are owned by snapshot", declaredBytesCopied);
    bool tailUntouched{true};
    for (Foundation::Size index{MaximumSalSystemTableSnapshotBytes}; index < sizeof(g_snapshot); ++index)
    {
        tailUntouched = tailUntouched && (g_snapshot[index] == 0x5AU);
    }
    ASTERIA_EXPECT(runner, "copy work is capped at the 64-KiB bootstrap bound",
                   g_snapshot[MaximumSalSystemTableSnapshotBytes - 1U] == 0xA5U, tailUntouched);

    WriteLittleEndian64(TableBytes() + EntrypointOffset + 8U, 0x0040'0000U);
    ASTERIA_EXPECT(runner, "returned information and snapshot do not alias firmware storage", result.HasValue(),
                   result.GetValue().PalProcedurePhysicalAddress == 0x0010'0000U,
                   g_snapshot[EntrypointOffset + 10U] == 0x10U, TableBytes()[EntrypointOffset + 10U] == 0x40U);
}

void TestCapacityBoundaries(TestContext& runner) noexcept
{
    ResetFixture();
    const auto exact = Capture(ValidTableLength);
    ASTERIA_EXPECT(runner, "exact declared table capacity succeeds", exact.HasValue(),
                   IsExpectedInformation(exact.GetValue()));

    ResetFixture();
    const auto truncated = Capture(ValidTableLength - 1U);
    ASTERIA_EXPECT(runner, "one-byte-short snapshot reports the exact parser failure", !truncated.HasValue(),
                   IsError(truncated.GetError(), SalSystemTableCaptureErrorKind::ParseFailed,
                           SalSystemTableLocateError::None, SalSystemTableParseError::TruncatedTable));

    ResetFixture();
    const auto headerOnly = Capture(SalSystemTableHeaderSize);
    ASTERIA_EXPECT(runner, "header-only snapshot cannot hide a declared descriptor stream", !headerOnly.HasValue(),
                   IsError(headerOnly.GetError(), SalSystemTableCaptureErrorKind::ParseFailed,
                           SalSystemTableLocateError::None, SalSystemTableParseError::TruncatedTable));

    ResetFixture();
    SnapshotDestination();
    const auto tooSmall = Capture(SalSystemTableHeaderSize - 1U);
    ASTERIA_EXPECT(runner, "sub-header destination is rejected before firmware inputs", !tooSmall.HasValue(),
                   IsError(tooSmall.GetError(), SalSystemTableCaptureErrorKind::SnapshotBufferTooSmall,
                           SalSystemTableLocateError::None, SalSystemTableParseError::None));
    ASTERIA_EXPECT(runner, "sub-header destination failure leaves all snapshot bytes untouched",
                   AreBytesEqual(g_snapshot, g_destinationSnapshot, sizeof(g_snapshot)));

    ResetFixture();
    const auto nullBuffer = SalSystemTableCapture::Capture(nullptr, 0U, nullptr, 0U, 0U, 0U, nullptr, 0U);
    ASTERIA_EXPECT(runner, "null destination has deterministic first failure priority", !nullBuffer.HasValue(),
                   IsError(nullBuffer.GetError(), SalSystemTableCaptureErrorKind::InvalidSnapshotBuffer,
                           SalSystemTableLocateError::None, SalSystemTableParseError::None));
}

void TestOverlappingDestination(TestContext& runner) noexcept
{
    ResetFixture();
    Foundation::UInt8* const overlappingDestination = TableBytes() + 16U;
    const auto result =
        SalSystemTableCapture::Capture(&g_configurationTable, 1U, g_memoryMap, sizeof(g_memoryMap), sizeof(g_memoryMap),
                                       1U, overlappingDestination, ValidTableLength);
    ASTERIA_EXPECT(runner, "partially overlapping destination retains one owned valid table", result.HasValue(),
                   IsExpectedInformation(result.GetValue()),
                   overlappingDestination[0U] == static_cast<Foundation::UInt8>('S'),
                   overlappingDestination[3U] == static_cast<Foundation::UInt8>('_'));

    ResetFixture();
    g_configurationAlias.Entry = g_configurationTable;
    Fill(g_configurationAlias.Tail, sizeof(g_configurationAlias.Tail), 0x3CU);
    auto* const configurationDestination = reinterpret_cast<Foundation::UInt8*>(&g_configurationAlias.Entry);
    const auto configurationAlias =
        SalSystemTableCapture::Capture(&g_configurationAlias.Entry, 1U, g_memoryMap, sizeof(g_memoryMap),
                                       sizeof(g_memoryMap), 1U, configurationDestination, ValidTableLength);
    ASTERIA_EXPECT(runner, "configuration input may be overwritten only after location completes",
                   configurationAlias.HasValue(), IsExpectedInformation(configurationAlias.GetValue()),
                   configurationDestination[0U] == static_cast<Foundation::UInt8>('S'));

    ResetFixture();
    CopyBytes(g_memoryMapAlias.Bytes, g_memoryMap, sizeof(g_memoryMap));
    Fill(g_memoryMapAlias.Bytes + sizeof(g_memoryMap), sizeof(g_memoryMapAlias.Bytes) - sizeof(g_memoryMap), 0xC3U);
    const auto memoryMapAlias =
        SalSystemTableCapture::Capture(&g_configurationTable, 1U, g_memoryMapAlias.Bytes, sizeof(g_memoryMap),
                                       sizeof(g_memoryMap), 1U, g_memoryMapAlias.Bytes, ValidTableLength);
    ASTERIA_EXPECT(runner, "memory-map input may be overwritten only after location completes",
                   memoryMapAlias.HasValue(), IsExpectedInformation(memoryMapAlias.GetValue()),
                   g_memoryMapAlias.Bytes[0U] == static_cast<Foundation::UInt8>('S'));
}

void TestBootServicesCaptureAndScrub(TestContext& runner) noexcept
{
    ResetFixture();
    const auto success =
        SalSystemTableCapture::CaptureFromBootServices(g_bootServices, &g_configurationTable, 1U, g_freshMemoryMap,
                                                       sizeof(g_freshMemoryMap), g_snapshot, sizeof(g_snapshot));
    ASTERIA_EXPECT(runner, "fresh boot-services capture scrubs and verifies its complete snapshot", success.HasValue(),
                   IsExpectedInformation(success.GetValue()), g_getMemoryMapCalls == 1U, IsSnapshotScrubbed());

    ResetFixture();
    TableBytes()[12U] ^= 1U;
    const auto parseFailure =
        SalSystemTableCapture::CaptureFromBootServices(g_bootServices, &g_configurationTable, 1U, g_freshMemoryMap,
                                                       sizeof(g_freshMemoryMap), g_snapshot, sizeof(g_snapshot));
    ASTERIA_EXPECT(runner, "parser failure still scrubs the complete boot snapshot", !parseFailure.HasValue(),
                   IsError(parseFailure.GetError(), SalSystemTableCaptureErrorKind::ParseFailed,
                           SalSystemTableLocateError::None, SalSystemTableParseError::InvalidChecksum),
                   g_getMemoryMapCalls == 1U, IsSnapshotScrubbed());

    ResetFixture();
    g_memoryMapBehavior = MemoryMapBehavior::Failure;
    const auto mapFailure =
        SalSystemTableCapture::CaptureFromBootServices(g_bootServices, &g_configurationTable, 1U, g_freshMemoryMap,
                                                       sizeof(g_freshMemoryMap), g_snapshot, sizeof(g_snapshot));
    ASTERIA_EXPECT(runner, "failed fresh memory-map acquisition scrubs stale snapshot bytes", !mapFailure.HasValue(),
                   IsError(mapFailure.GetError(), SalSystemTableCaptureErrorKind::MemoryMapFailed,
                           SalSystemTableLocateError::None, SalSystemTableParseError::None),
                   g_getMemoryMapCalls == 1U, IsSnapshotScrubbed());

    ResetFixture();
    g_memoryMapBehavior = MemoryMapBehavior::OversizedSuccess;
    const auto oversizedMap =
        SalSystemTableCapture::CaptureFromBootServices(g_bootServices, &g_configurationTable, 1U, g_freshMemoryMap,
                                                       sizeof(g_freshMemoryMap), g_snapshot, sizeof(g_snapshot));
    ASTERIA_EXPECT(runner, "successful oversized memory-map report is rejected before capture",
                   !oversizedMap.HasValue(),
                   IsError(oversizedMap.GetError(), SalSystemTableCaptureErrorKind::MemoryMapTooLarge,
                           SalSystemTableLocateError::None, SalSystemTableParseError::None),
                   g_getMemoryMapCalls == 1U, IsSnapshotScrubbed());

    ResetFixture();
    g_bootServices.GetMemoryMapFunction = nullptr;
    const auto missingCallback =
        SalSystemTableCapture::CaptureFromBootServices(g_bootServices, &g_configurationTable, 1U, g_freshMemoryMap,
                                                       sizeof(g_freshMemoryMap), g_snapshot, sizeof(g_snapshot));
    ASTERIA_EXPECT(runner, "missing memory-map callback fails before firmware access and scrubs",
                   !missingCallback.HasValue(),
                   IsError(missingCallback.GetError(), SalSystemTableCaptureErrorKind::MissingGetMemoryMap,
                           SalSystemTableLocateError::None, SalSystemTableParseError::None),
                   g_getMemoryMapCalls == 0U, IsSnapshotScrubbed());

    ResetFixture();
    const auto nullMap = SalSystemTableCapture::CaptureFromBootServices(
        g_bootServices, &g_configurationTable, 1U, nullptr, sizeof(g_freshMemoryMap), g_snapshot, sizeof(g_snapshot));
    ASTERIA_EXPECT(runner, "null fresh memory-map buffer has deterministic priority and scrubs", !nullMap.HasValue(),
                   IsError(nullMap.GetError(), SalSystemTableCaptureErrorKind::InvalidMemoryMapBuffer,
                           SalSystemTableLocateError::None, SalSystemTableParseError::None),
                   g_getMemoryMapCalls == 0U, IsSnapshotScrubbed());

    ResetFixture();
    const auto zeroMapCapacity = SalSystemTableCapture::CaptureFromBootServices(
        g_bootServices, &g_configurationTable, 1U, g_freshMemoryMap, 0U, g_snapshot, sizeof(g_snapshot));
    ASTERIA_EXPECT(runner, "zero fresh memory-map capacity fails before callback and scrubs",
                   !zeroMapCapacity.HasValue(),
                   IsError(zeroMapCapacity.GetError(), SalSystemTableCaptureErrorKind::InvalidMemoryMapBuffer,
                           SalSystemTableLocateError::None, SalSystemTableParseError::None),
                   g_getMemoryMapCalls == 0U, IsSnapshotScrubbed());

    ResetFixture();
    const auto shortSnapshot = SalSystemTableCapture::CaptureFromBootServices(
        g_bootServices, &g_configurationTable, 1U, g_freshMemoryMap, sizeof(g_freshMemoryMap), g_snapshot,
        SalSystemTableHeaderSize - 1U);
    ASTERIA_EXPECT(runner, "short boot snapshot is scrubbed before its first-priority failure",
                   !shortSnapshot.HasValue(),
                   IsError(shortSnapshot.GetError(), SalSystemTableCaptureErrorKind::SnapshotBufferTooSmall,
                           SalSystemTableLocateError::None, SalSystemTableParseError::None),
                   g_getMemoryMapCalls == 0U, IsSnapshotPrefixScrubbed(SalSystemTableHeaderSize - 1U));

    ResetFixture();
    const auto nullSnapshot = SalSystemTableCapture::CaptureFromBootServices(
        g_bootServices, &g_configurationTable, 1U, g_freshMemoryMap, sizeof(g_freshMemoryMap), nullptr, 0U);
    ASTERIA_EXPECT(runner, "null boot snapshot fails before every firmware or memory access", !nullSnapshot.HasValue(),
                   IsError(nullSnapshot.GetError(), SalSystemTableCaptureErrorKind::InvalidSnapshotBuffer,
                           SalSystemTableLocateError::None, SalSystemTableParseError::None),
                   g_getMemoryMapCalls == 0U);
}

void TestLocationFailures(TestContext& runner) noexcept
{
    ResetFixture();
    g_configurationTable.VendorGuid.Data1 = 0U;
    SnapshotDestination();
    const auto missing = Capture();
    ASTERIA_EXPECT(runner, "locator failure retains its exact typed cause", !missing.HasValue(),
                   IsError(missing.GetError(), SalSystemTableCaptureErrorKind::LocationFailed,
                           SalSystemTableLocateError::SalSystemTableNotFound, SalSystemTableParseError::None));
    ASTERIA_EXPECT(runner, "location failure leaves snapshot storage untouched",
                   AreBytesEqual(g_snapshot, g_destinationSnapshot, sizeof(g_snapshot)));

    ResetFixture();
    const auto invalidMap = SalSystemTableCapture::Capture(&g_configurationTable, 1U, g_memoryMap, sizeof(g_memoryMap),
                                                           sizeof(g_memoryMap), 0U, g_snapshot, sizeof(g_snapshot));
    ASTERIA_EXPECT(runner, "memory-map framing failure remains distinguishable from parsing", !invalidMap.HasValue(),
                   IsError(invalidMap.GetError(), SalSystemTableCaptureErrorKind::LocationFailed,
                           SalSystemTableLocateError::InvalidMemoryMap, SalSystemTableParseError::None));
}

void TestParseFailures(TestContext& runner) noexcept
{
    ResetFixture();
    TableBytes()[12U] ^= 1U;
    const auto checksum = Capture();
    ASTERIA_EXPECT(runner, "checksum failure is reported from the owned snapshot parser", !checksum.HasValue(),
                   IsError(checksum.GetError(), SalSystemTableCaptureErrorKind::ParseFailed,
                           SalSystemTableLocateError::None, SalSystemTableParseError::InvalidChecksum));
    ASTERIA_EXPECT(runner, "parse failure still operates on the copied bytes", g_snapshot[12U] == TableBytes()[12U]);

    ResetFixture();
    TableBytes()[0U] = static_cast<Foundation::UInt8>('X');
    const auto signature = Capture();
    ASTERIA_EXPECT(runner, "invalid signature remains a typed parser failure", !signature.HasValue(),
                   IsError(signature.GetError(), SalSystemTableCaptureErrorKind::ParseFailed,
                           SalSystemTableLocateError::None, SalSystemTableParseError::InvalidSignature));

    ResetFixture();
    WriteLittleEndian32(TableBytes() + 4U, static_cast<Foundation::UInt32>(MaximumSalSystemTableSnapshotBytes + 1U));
    WriteLittleEndian16(TableBytes() + 10U, 1'365U);
    const auto overLimit = Capture();
    ASTERIA_EXPECT(runner, "declared table beyond the 64-KiB bound fails closed", !overLimit.HasValue(),
                   IsError(overLimit.GetError(), SalSystemTableCaptureErrorKind::ParseFailed,
                           SalSystemTableLocateError::None, SalSystemTableParseError::TruncatedTable));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"SalSystemTableCaptureTests"};
    Asteria::Test::TestSuccessfulOwnedCapture(runner);
    Asteria::Test::TestCapacityBoundaries(runner);
    Asteria::Test::TestOverlappingDestination(runner);
    Asteria::Test::TestBootServicesCaptureAndScrub(runner);
    Asteria::Test::TestLocationFailures(runner);
    Asteria::Test::TestParseFailures(runner);
    return runner.Finish();
}
