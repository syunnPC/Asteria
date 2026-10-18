// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <type_traits>

#include "Kernel/Device/AhciReadDmaExtCommandBuilder.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Kernel::Device::AhciReadDmaExtCommandBuilder;
using Kernel::Device::AhciReadDmaExtCommandError;
using Kernel::Device::AhciReadDmaExtCommandLayout;
using Kernel::Device::AhciReadDmaExtCommandPageWritableView;
using Kernel::Device::AhciReadDmaExtCommandRequest;
using Kernel::Device::AhciReadDmaExtDataBufferSpan;

constexpr Size PageSizeBytes{AhciReadDmaExtCommandBuilder::CommandPageSizeBytes};
constexpr UInt8 Sentinel{0xA5U};
constexpr UInt64 MaximumUInt64{~UInt64{0U}};

alignas(4096) UInt8 g_page[PageSizeBytes]{};
alignas(4096) UInt8 g_before[PageSizeBytes * 2U]{};
alignas(4096) UInt8 g_extendedPage[PageSizeBytes * 2U]{};
alignas(4096) UInt8 g_destination[8192U]{};

struct alignas(PageSizeBytes) RequestInCommandPageFixture
{
    AhciReadDmaExtCommandRequest Request;
    UInt8 PageRemainder[PageSizeBytes - sizeof(Request)];
};

struct alignas(PageSizeBytes) RequestCrossingCommandPageStartFixture
{
    UInt8 Prefix[PageSizeBytes - sizeof(AhciReadDmaExtCommandRequest) + 8U];
    AhciReadDmaExtCommandRequest Request;
    UInt8 PageRemainder[PageSizeBytes - 8U];
};

struct alignas(PageSizeBytes) RequestCrossingCommandPageEndFixture
{
    UInt8 PagePrefix[PageSizeBytes - 8U];
    AhciReadDmaExtCommandRequest Request;
};

struct alignas(PageSizeBytes) RequestAdjacentBeforeCommandPageFixture
{
    UInt8 Prefix[PageSizeBytes - sizeof(AhciReadDmaExtCommandRequest)];
    AhciReadDmaExtCommandRequest Request;
    UInt8 Page[PageSizeBytes];
};

struct alignas(PageSizeBytes) RequestAdjacentAfterCommandPageFixture
{
    UInt8 Page[PageSizeBytes];
    AhciReadDmaExtCommandRequest Request;
};

RequestInCommandPageFixture g_requestInCommandPage{};
RequestCrossingCommandPageStartFixture g_requestCrossingCommandPageStart{};
RequestCrossingCommandPageEndFixture g_requestCrossingCommandPageEnd{};
RequestAdjacentBeforeCommandPageFixture g_requestAdjacentBeforeCommandPage{};
RequestAdjacentAfterCommandPageFixture g_requestAdjacentAfterCommandPage{};

static_assert(std::is_aggregate_v<AhciReadDmaExtCommandPageWritableView>);
static_assert(std::is_standard_layout_v<AhciReadDmaExtCommandPageWritableView>);
static_assert(std::is_trivially_copyable_v<AhciReadDmaExtCommandPageWritableView>);
static_assert(std::is_aggregate_v<AhciReadDmaExtDataBufferSpan>);
static_assert(std::is_standard_layout_v<AhciReadDmaExtDataBufferSpan>);
static_assert(std::is_trivially_copyable_v<AhciReadDmaExtDataBufferSpan>);
static_assert(std::is_aggregate_v<AhciReadDmaExtCommandRequest>);
static_assert(std::is_standard_layout_v<AhciReadDmaExtCommandRequest>);
static_assert(std::is_trivially_copyable_v<AhciReadDmaExtCommandRequest>);
static_assert(std::is_aggregate_v<AhciReadDmaExtCommandLayout>);
static_assert(std::is_standard_layout_v<AhciReadDmaExtCommandLayout>);
static_assert(std::is_trivially_copyable_v<AhciReadDmaExtCommandLayout>);
static_assert(sizeof(AhciReadDmaExtCommandError) == 1U);
static_assert(sizeof(AhciReadDmaExtCommandPageWritableView) == 24U);
static_assert(sizeof(AhciReadDmaExtDataBufferSpan) == 16U);
static_assert(sizeof(AhciReadDmaExtCommandRequest) == 80U);
static_assert(sizeof(AhciReadDmaExtCommandLayout) == 40U);
static_assert(AhciReadDmaExtCommandBuilder::CommandListOffsetBytes == 0U);
static_assert(AhciReadDmaExtCommandBuilder::CommandListSizeBytes == 1024U);
static_assert(AhciReadDmaExtCommandBuilder::ReceivedFisOffsetBytes == 0x400U);
static_assert(AhciReadDmaExtCommandBuilder::ReceivedFisSizeBytes == 256U);
static_assert(AhciReadDmaExtCommandBuilder::CommandTableOffsetBytes == 0x500U);
static_assert(AhciReadDmaExtCommandBuilder::CommandTableSizeBytes == 144U);
static_assert(AhciReadDmaExtCommandBuilder::MaximumPrdtTransferSizeBytes == 4U * 1024U * 1024U);
static_assert(AhciReadDmaExtCommandBuilder::MaximumSectorCount == 65'536U);
static_assert(sizeof(RequestInCommandPageFixture) == PageSizeBytes);
static_assert(sizeof(RequestCrossingCommandPageStartFixture) == PageSizeBytes * 2U);
static_assert(sizeof(RequestCrossingCommandPageEndFixture) == PageSizeBytes * 2U);
static_assert(sizeof(RequestAdjacentBeforeCommandPageFixture) == PageSizeBytes * 2U);
static_assert(sizeof(RequestAdjacentAfterCommandPageFixture) == PageSizeBytes * 2U);
static_assert(offsetof(RequestCrossingCommandPageStartFixture, Request) ==
              PageSizeBytes - sizeof(AhciReadDmaExtCommandRequest) + 8U);
static_assert(offsetof(RequestCrossingCommandPageEndFixture, Request) == PageSizeBytes - 8U);
static_assert(offsetof(RequestAdjacentBeforeCommandPageFixture, Page) == PageSizeBytes);
static_assert(offsetof(RequestAdjacentAfterCommandPageFixture, Request) == PageSizeBytes);

void FillBytes(UInt8* const bytes, const Size sizeBytes, const UInt8 value) noexcept
{
    for (Size index{0U}; index < sizeBytes; ++index)
    {
        bytes[index] = value;
    }
}

void CopyBytes(UInt8* const destination, const UInt8* const source, const Size sizeBytes) noexcept
{
    for (Size index{0U}; index < sizeBytes; ++index)
    {
        destination[index] = source[index];
    }
}

[[nodiscard]] bool BytesEqual(const UInt8* const first, const UInt8* const second, const Size sizeBytes) noexcept
{
    for (Size index{0U}; index < sizeBytes; ++index)
    {
        if (first[index] != second[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool RegionHasValue(const UInt8* const bytes, const Size offsetBytes, const Size sizeBytes,
                                  const UInt8 value) noexcept
{
    for (Size index{0U}; index < sizeBytes; ++index)
    {
        if (bytes[offsetBytes + index] != value)
        {
            return false;
        }
    }
    return true;
}

void WriteLittleEndian16(UInt8* const bytes, const UInt16 value) noexcept
{
    bytes[0U] = static_cast<UInt8>(value);
    bytes[1U] = static_cast<UInt8>(value >> 8U);
}

void WriteLittleEndian32(UInt8* const bytes, const UInt32 value) noexcept
{
    for (UInt32 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

void WriteAddress(UInt8* const bytes, const UInt64 address) noexcept
{
    WriteLittleEndian32(bytes, static_cast<UInt32>(address));
    WriteLittleEndian32(bytes + 4U, static_cast<UInt32>(address >> 32U));
}

[[nodiscard]] UInt16 ReadLittleEndian16(const UInt8* const bytes) noexcept
{
    return static_cast<UInt16>(static_cast<UInt16>(bytes[0U]) |
                               static_cast<UInt16>(static_cast<UInt16>(bytes[1U]) << 8U));
}

[[nodiscard]] UInt32 ReadLittleEndian32(const UInt8* const bytes) noexcept
{
    UInt32 value{0U};
    for (UInt32 index{0U}; index < 4U; ++index)
    {
        value |= static_cast<UInt32>(bytes[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] UInt64 ReadAddress(const UInt8* const bytes) noexcept
{
    return static_cast<UInt64>(ReadLittleEndian32(bytes)) |
           (static_cast<UInt64>(ReadLittleEndian32(bytes + 4U)) << 32U);
}

[[nodiscard]] AhciReadDmaExtCommandRequest MakeRequest() noexcept
{
    return AhciReadDmaExtCommandRequest{
        .CommandPage =
            AhciReadDmaExtCommandPageWritableView{
                .Data = g_page,
                .SizeBytes = PageSizeBytes,
                .PhysicalAddress = 0x0010'0000U,
            },
        .DataBuffer =
            AhciReadDmaExtDataBufferSpan{
                .PhysicalAddress = 0x0020'0000U,
                .SizeBytes = 4096U,
            },
        .DeviceSectorCount = 0x0001'0000U,
        .StartingLba = 0x1234U,
        .LogicalSectorSizeBytes = 512U,
        .SectorCount = 8U,
        .Supports64BitAddressing = false,
        .Supports48BitAddressing = true,
        .Reserved = {},
    };
}

void BuildExpectedImage(UInt8* const expected, const AhciReadDmaExtCommandRequest& request,
                        const UInt8 untouchedValue) noexcept
{
    FillBytes(expected, PageSizeBytes, untouchedValue);
    FillBytes(expected + AhciReadDmaExtCommandBuilder::CommandListOffsetBytes,
              AhciReadDmaExtCommandBuilder::CommandListSizeBytes, 0U);
    FillBytes(expected + AhciReadDmaExtCommandBuilder::ReceivedFisOffsetBytes,
              AhciReadDmaExtCommandBuilder::ReceivedFisSizeBytes, 0U);
    FillBytes(expected + AhciReadDmaExtCommandBuilder::CommandTableOffsetBytes,
              AhciReadDmaExtCommandBuilder::CommandTableSizeBytes, 0U);

    WriteLittleEndian32(expected, 0x0001'0005U);
    WriteAddress(expected + 8U,
                 request.CommandPage.PhysicalAddress + AhciReadDmaExtCommandBuilder::CommandTableOffsetBytes);
    const Size commandFisOffset = AhciReadDmaExtCommandBuilder::CommandTableOffsetBytes;
    expected[commandFisOffset + 0U] = 0x27U;
    expected[commandFisOffset + 1U] = 0x80U;
    expected[commandFisOffset + 2U] = 0x25U;
    expected[commandFisOffset + 4U] = static_cast<UInt8>(request.StartingLba);
    expected[commandFisOffset + 5U] = static_cast<UInt8>(request.StartingLba >> 8U);
    expected[commandFisOffset + 6U] = static_cast<UInt8>(request.StartingLba >> 16U);
    expected[commandFisOffset + 7U] = 0x40U;
    expected[commandFisOffset + 8U] = static_cast<UInt8>(request.StartingLba >> 24U);
    expected[commandFisOffset + 9U] = static_cast<UInt8>(request.StartingLba >> 32U);
    expected[commandFisOffset + 10U] = static_cast<UInt8>(request.StartingLba >> 40U);
    WriteLittleEndian16(expected + commandFisOffset + 12U, static_cast<UInt16>(request.SectorCount));

    const Size prdtOffset = commandFisOffset + 128U;
    WriteAddress(expected + prdtOffset, request.DataBuffer.PhysicalAddress);
    WriteLittleEndian32(expected + prdtOffset + 12U, static_cast<UInt32>(request.DataBuffer.SizeBytes - 1U));
}

void CheckRejectedWithoutMutation(TestContext& runner, const AhciReadDmaExtCommandRequest& request,
                                  const AhciReadDmaExtCommandError expectedError, UInt8* const checkedStorage,
                                  const Size checkedSizeBytes, const char* const description) noexcept
{
    FillBytes(checkedStorage, checkedSizeBytes, 0x6DU);
    CopyBytes(g_before, checkedStorage, checkedSizeBytes);
    const auto result = AhciReadDmaExtCommandBuilder::Build(request);
    ASTERIA_EXPECT(runner, description, !result.HasValue(), result.GetError() == expectedError,
                   BytesEqual(checkedStorage, g_before, checkedSizeBytes));
}

void CheckPreparedStorageRejectedWithoutMutation(TestContext& runner, const AhciReadDmaExtCommandRequest& request,
                                                 const AhciReadDmaExtCommandError expectedError,
                                                 UInt8* const checkedStorage, const Size checkedSizeBytes,
                                                 const char* const description) noexcept
{
    CopyBytes(g_before, checkedStorage, checkedSizeBytes);
    const auto result = AhciReadDmaExtCommandBuilder::Build(request);
    ASTERIA_EXPECT(runner, description, !result.HasValue(), result.GetError() == expectedError,
                   BytesEqual(checkedStorage, g_before, checkedSizeBytes));
}

void TestExactWireImageAndLayout(TestContext& runner) noexcept
{
    AhciReadDmaExtCommandRequest request = MakeRequest();
    request.CommandPage.PhysicalAddress = 0x1234'5678'9ABC'D000ULL;
    request.DataBuffer.PhysicalAddress = 0x0FED'CBA9'8765'4000ULL;
    request.StartingLba = 0x1234'5678'9ABCULL;
    request.SectorCount = 0x0201U;
    request.DataBuffer.SizeBytes = 0x0004'0200U;
    request.DeviceSectorCount = 0x0001'0000'0000'0000ULL;
    request.Supports64BitAddressing = true;
    FillBytes(g_page, PageSizeBytes, Sentinel);

    const auto result = AhciReadDmaExtCommandBuilder::Build(request);
    ASTERIA_EXPECT(runner, "64-bit controller accepts the exact READ DMA EXT image", result.HasValue());
    if (!result.HasValue())
    {
        return;
    }
    const AhciReadDmaExtCommandLayout& layout = result.GetValue();
    ASTERIA_EXPECT(runner, "layout reports CLB, FB, CTBA, destination, and transfer extent",
                   layout.CommandListPhysicalAddress == 0x1234'5678'9ABC'D000ULL,
                   layout.ReceivedFisPhysicalAddress == 0x1234'5678'9ABC'D400ULL,
                   layout.CommandTablePhysicalAddress == 0x1234'5678'9ABC'D500ULL,
                   layout.DataBufferPhysicalAddress == request.DataBuffer.PhysicalAddress,
                   layout.TransferSizeBytes == request.DataBuffer.SizeBytes);

    BuildExpectedImage(g_before, request, Sentinel);
    ASTERIA_EXPECT(runner, "all command-page bytes match the canonical fixed layout",
                   BytesEqual(g_page, g_before, PageSizeBytes));
    ASTERIA_EXPECT(runner, "slot-0 header encodes CFL=5, PRDTL=1, zero PRDBC, and W/A/P clear",
                   ReadLittleEndian32(g_page) == 0x0001'0005U, (ReadLittleEndian32(g_page) & 0x0000'00E0U) == 0U,
                   ReadLittleEndian32(g_page + 4U) == 0U);
    ASTERIA_EXPECT(runner, "slot-0 command-table address encodes both 32-bit halves",
                   ReadAddress(g_page + 8U) == layout.CommandTablePhysicalAddress);
    ASTERIA_EXPECT(runner, "H2D register FIS encodes opcode and 48-bit LBA in ATA byte order", g_page[0x500U] == 0x27U,
                   g_page[0x501U] == 0x80U, g_page[0x502U] == 0x25U, g_page[0x504U] == 0xBCU, g_page[0x505U] == 0x9AU,
                   g_page[0x506U] == 0x78U, g_page[0x507U] == 0x40U, g_page[0x508U] == 0x56U, g_page[0x509U] == 0x34U,
                   g_page[0x50AU] == 0x12U);
    ASTERIA_EXPECT(runner, "H2D register FIS encodes the 16-bit sector count little-endian",
                   ReadLittleEndian16(g_page + 0x50CU) == 0x0201U);
    const UInt32 prdtDescription = ReadLittleEndian32(g_page + 0x58CU);
    ASTERIA_EXPECT(runner, "PRDT encodes external DBA, zero DW2, odd zero-based DBC, and no reserved or IOC bits",
                   ReadAddress(g_page + 0x580U) == request.DataBuffer.PhysicalAddress,
                   ReadLittleEndian32(g_page + 0x588U) == 0U, prdtDescription == 0x0004'01FFU,
                   (prdtDescription & 1U) != 0U, (prdtDescription & 0xFFC0'0000U) == 0U);
    ASTERIA_EXPECT(runner, "success leaves every byte outside active command regions untouched",
                   RegionHasValue(g_page, 0x590U, PageSizeBytes - 0x590U, Sentinel));
}

void TestExternalDestinationRemainsUntouched(TestContext& runner) noexcept
{
    AhciReadDmaExtCommandRequest request = MakeRequest();
    request.CommandPage.PhysicalAddress = 0U;
    request.DataBuffer.PhysicalAddress = reinterpret_cast<UInt64>(g_destination);
    request.DataBuffer.SizeBytes = sizeof(g_destination);
    request.LogicalSectorSizeBytes = 4096U;
    request.SectorCount = 2U;
    request.Supports64BitAddressing = true;
    FillBytes(g_page, PageSizeBytes, Sentinel);
    FillBytes(g_destination, sizeof(g_destination), 0x3CU);

    const auto result = AhciReadDmaExtCommandBuilder::Build(request);
    ASTERIA_EXPECT(runner, "4096-byte sectors use the external PRDT destination without touching it", result.HasValue(),
                   RegionHasValue(g_destination, 0U, sizeof(g_destination), 0x3CU),
                   ReadAddress(g_page + 0x580U) == reinterpret_cast<UInt64>(g_destination),
                   ReadLittleEndian32(g_page + 0x58CU) == 8191U);
}

void TestAddressAndTransferBoundaries(TestContext& runner) noexcept
{
    AhciReadDmaExtCommandRequest request = MakeRequest();
    request.CommandPage.PhysicalAddress = 0xFFFF'F000ULL;
    request.DataBuffer.PhysicalAddress = 0U;
    request.DataBuffer.SizeBytes = 512U;
    request.SectorCount = 1U;
    FillBytes(g_page, PageSizeBytes, Sentinel);
    const auto last32BitPage = AhciReadDmaExtCommandBuilder::Build(request);
    ASTERIA_EXPECT(runner, "32-bit controller accepts the final complete command page", last32BitPage.HasValue(),
                   ReadLittleEndian32(g_page + 0x00CU) == 0U);

    request.CommandPage.PhysicalAddress = 0U;
    request.DataBuffer.PhysicalAddress = 0xFFFF'FE00ULL;
    FillBytes(g_page, PageSizeBytes, Sentinel);
    const auto last32BitData = AhciReadDmaExtCommandBuilder::Build(request);
    ASTERIA_EXPECT(runner, "32-bit controller accepts a destination ending at 4 GiB minus one",
                   last32BitData.HasValue(), ReadLittleEndian32(g_page + 0x584U) == 0U);

    request.CommandPage.PhysicalAddress = 0xFFFF'FFFF'FFFF'F000ULL;
    request.DataBuffer.PhysicalAddress = 0U;
    request.Supports64BitAddressing = true;
    FillBytes(g_page, PageSizeBytes, Sentinel);
    const auto last64BitPage = AhciReadDmaExtCommandBuilder::Build(request);
    ASTERIA_EXPECT(runner, "64-bit controller accepts the final non-wrapping command page", last64BitPage.HasValue(),
                   ReadLittleEndian32(g_page + 0x00CU) == 0xFFFF'FFFFU);

    request.CommandPage.PhysicalAddress = 0U;
    request.DataBuffer.PhysicalAddress = 0xFFFF'FFFF'FFFF'FE00ULL;
    FillBytes(g_page, PageSizeBytes, Sentinel);
    const auto last64BitData = AhciReadDmaExtCommandBuilder::Build(request);
    ASTERIA_EXPECT(runner, "64-bit controller accepts a destination ending at UInt64 maximum", last64BitData.HasValue(),
                   ReadLittleEndian32(g_page + 0x584U) == 0xFFFF'FFFFU);

    request = MakeRequest();
    request.StartingLba = AhciReadDmaExtCommandBuilder::MaximumLba;
    request.DeviceSectorCount = 0x0001'0000'0000'0000ULL;
    request.SectorCount = 1U;
    request.DataBuffer.SizeBytes = 512U;
    FillBytes(g_page, PageSizeBytes, Sentinel);
    const auto lastLba = AhciReadDmaExtCommandBuilder::Build(request);
    ASTERIA_EXPECT(runner, "final 48-bit LBA is accepted and encoded without truncation", lastLba.HasValue(),
                   RegionHasValue(g_page, 0x504U, 3U, 0xFFU), RegionHasValue(g_page, 0x508U, 3U, 0xFFU));

    request = MakeRequest();
    request.SectorCount = 8192U;
    request.DataBuffer.SizeBytes = 4U * 1024U * 1024U;
    request.DeviceSectorCount = 16'384U;
    request.StartingLba = 0U;
    FillBytes(g_page, PageSizeBytes, Sentinel);
    const auto maximum512Transfer = AhciReadDmaExtCommandBuilder::Build(request);
    ASTERIA_EXPECT(runner, "one PRDT accepts exactly 4 MiB using 512-byte sectors", maximum512Transfer.HasValue(),
                   ReadLittleEndian32(g_page + 0x58CU) == 0x003F'FFFFU);

    request.LogicalSectorSizeBytes = 4096U;
    request.SectorCount = 1024U;
    request.DataBuffer.SizeBytes = 4U * 1024U * 1024U;
    FillBytes(g_page, PageSizeBytes, Sentinel);
    const auto maximum4096Transfer = AhciReadDmaExtCommandBuilder::Build(request);
    ASTERIA_EXPECT(runner, "one PRDT accepts exactly 4 MiB using 4096-byte sectors", maximum4096Transfer.HasValue(),
                   ReadLittleEndian16(g_page + 0x50CU) == 1024U);

    request = MakeRequest();
    request.LogicalSectorSizeBytes = 514U;
    request.SectorCount = 1U;
    request.DataBuffer.SizeBytes = 514U;
    FillBytes(g_page, PageSizeBytes, Sentinel);
    const auto extendedLogicalSector = AhciReadDmaExtCommandBuilder::Build(request);
    ASTERIA_EXPECT(runner, "smallest ATA extended logical sector size is accepted", extendedLogicalSector.HasValue(),
                   ReadLittleEndian32(g_page + 0x58CU) == 513U);

    request.LogicalSectorSizeBytes = 4U * 1024U * 1024U;
    request.DataBuffer.SizeBytes = 4U * 1024U * 1024U;
    FillBytes(g_page, PageSizeBytes, Sentinel);
    const auto largestSingleSector = AhciReadDmaExtCommandBuilder::Build(request);
    ASTERIA_EXPECT(runner, "largest even logical sector fitting one PRDT is accepted", largestSingleSector.HasValue(),
                   ReadLittleEndian32(g_page + 0x58CU) == 0x003F'FFFFU);

    request = MakeRequest();
    request.CommandPage.PhysicalAddress = 0x0020'0000U;
    request.DataBuffer.PhysicalAddress = 0x0020'1000U;
    FillBytes(g_page, PageSizeBytes, Sentinel);
    const auto adjacentAfter = AhciReadDmaExtCommandBuilder::Build(request);
    ASTERIA_EXPECT(runner, "destination beginning after the command page is non-overlapping", adjacentAfter.HasValue());

    request.CommandPage.PhysicalAddress = 0x0020'1000U;
    request.DataBuffer.PhysicalAddress = 0x0020'0000U;
    FillBytes(g_page, PageSizeBytes, Sentinel);
    const auto adjacentBefore = AhciReadDmaExtCommandBuilder::Build(request);
    ASTERIA_EXPECT(runner, "destination ending before the command page is non-overlapping", adjacentBefore.HasValue());
}

void TestCommandPageRejections(TestContext& runner) noexcept
{
    AhciReadDmaExtCommandRequest request = MakeRequest();
    request.CommandPage.Data = nullptr;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::NullCommandPage, g_page, PageSizeBytes,
                                 "null command page is rejected without mutation");

    request = MakeRequest();
    request.CommandPage.SizeBytes = PageSizeBytes - 1U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::InvalidCommandPageSize, g_page,
                                 PageSizeBytes, "short command page is rejected without mutation");

    request = MakeRequest();
    request.CommandPage.Data = g_extendedPage;
    request.CommandPage.SizeBytes = PageSizeBytes + 1U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::InvalidCommandPageSize, g_extendedPage,
                                 PageSizeBytes + 1U, "oversized command page is rejected without mutation");

    request = MakeRequest();
    request.CommandPage.Data = g_extendedPage + 1U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::InvalidCommandPageVirtualAlignment,
                                 g_extendedPage + 1U, PageSizeBytes,
                                 "misaligned virtual command page is rejected without mutation");

    request = MakeRequest();
    request.CommandPage.PhysicalAddress = MaximumUInt64;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::CommandPagePhysicalAddressWrap, g_page,
                                 PageSizeBytes, "wrapping command-page physical extent is rejected without mutation");

    request = MakeRequest();
    request.CommandPage.PhysicalAddress = 0x0010'0001U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::InvalidCommandPagePhysicalAlignment,
                                 g_page, PageSizeBytes,
                                 "misaligned physical command page is rejected without mutation");
}

void TestRequestAndCommandPageVirtualDisjointness(TestContext& runner) noexcept
{
    UInt8* const requestInPageBytes = reinterpret_cast<UInt8*>(&g_requestInCommandPage);
    FillBytes(requestInPageBytes, sizeof(g_requestInCommandPage), 0x6DU);
    g_requestInCommandPage.Request = MakeRequest();
    g_requestInCommandPage.Request.CommandPage.Data = requestInPageBytes;
    CheckPreparedStorageRejectedWithoutMutation(runner, g_requestInCommandPage.Request,
                                                AhciReadDmaExtCommandError::CommandPageOverlapsRequest,
                                                requestInPageBytes, sizeof(g_requestInCommandPage),
                                                "request at command-page start is rejected before any page write");

    UInt8* const crossingStartBytes = reinterpret_cast<UInt8*>(&g_requestCrossingCommandPageStart);
    UInt8* const pageCrossingRequestEnd = crossingStartBytes + PageSizeBytes;
    FillBytes(crossingStartBytes, sizeof(g_requestCrossingCommandPageStart), 0x6DU);
    g_requestCrossingCommandPageStart.Request = MakeRequest();
    g_requestCrossingCommandPageStart.Request.CommandPage.Data = pageCrossingRequestEnd;
    CheckPreparedStorageRejectedWithoutMutation(
        runner, g_requestCrossingCommandPageStart.Request, AhciReadDmaExtCommandError::CommandPageOverlapsRequest,
        crossingStartBytes, sizeof(g_requestCrossingCommandPageStart),
        "eight-byte request overlap at command-page start is rejected without mutation");

    UInt8* const crossingEndBytes = reinterpret_cast<UInt8*>(&g_requestCrossingCommandPageEnd);
    FillBytes(crossingEndBytes, sizeof(g_requestCrossingCommandPageEnd), 0x6DU);
    g_requestCrossingCommandPageEnd.Request = MakeRequest();
    g_requestCrossingCommandPageEnd.Request.CommandPage.Data = crossingEndBytes;
    CheckPreparedStorageRejectedWithoutMutation(
        runner, g_requestCrossingCommandPageEnd.Request, AhciReadDmaExtCommandError::CommandPageOverlapsRequest,
        crossingEndBytes, sizeof(g_requestCrossingCommandPageEnd),
        "eight-byte request overlap at command-page end is rejected without mutation");

    FillBytes(g_requestAdjacentBeforeCommandPage.Page, PageSizeBytes, Sentinel);
    g_requestAdjacentBeforeCommandPage.Request = MakeRequest();
    g_requestAdjacentBeforeCommandPage.Request.CommandPage.Data = g_requestAdjacentBeforeCommandPage.Page;
    const AhciReadDmaExtCommandRequest adjacentBeforeSnapshot = g_requestAdjacentBeforeCommandPage.Request;
    const auto adjacentBefore = AhciReadDmaExtCommandBuilder::Build(g_requestAdjacentBeforeCommandPage.Request);
    ASTERIA_EXPECT(runner, "request ending at command-page start remains unchanged and is accepted",
                   adjacentBefore.HasValue(),
                   BytesEqual(reinterpret_cast<const UInt8*>(&g_requestAdjacentBeforeCommandPage.Request),
                              reinterpret_cast<const UInt8*>(&adjacentBeforeSnapshot), sizeof(adjacentBeforeSnapshot)));

    FillBytes(g_requestAdjacentAfterCommandPage.Page, PageSizeBytes, Sentinel);
    g_requestAdjacentAfterCommandPage.Request = MakeRequest();
    g_requestAdjacentAfterCommandPage.Request.CommandPage.Data = g_requestAdjacentAfterCommandPage.Page;
    const AhciReadDmaExtCommandRequest adjacentAfterSnapshot = g_requestAdjacentAfterCommandPage.Request;
    const auto adjacentAfter = AhciReadDmaExtCommandBuilder::Build(g_requestAdjacentAfterCommandPage.Request);
    ASTERIA_EXPECT(runner, "request starting at command-page end remains unchanged and is accepted",
                   adjacentAfter.HasValue(),
                   BytesEqual(reinterpret_cast<const UInt8*>(&g_requestAdjacentAfterCommandPage.Request),
                              reinterpret_cast<const UInt8*>(&adjacentAfterSnapshot), sizeof(adjacentAfterSnapshot)));
}

void TestTransferAndLbaRejections(TestContext& runner) noexcept
{
    AhciReadDmaExtCommandRequest request = MakeRequest();
    request.SectorCount = 0U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::ZeroSectorCount, g_page, PageSizeBytes,
                                 "zero sector count is rejected without mutation");

    request = MakeRequest();
    request.SectorCount = AhciReadDmaExtCommandBuilder::MaximumSectorCount;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::TransferExceedsPrdtEntry, g_page,
                                 PageSizeBytes, "semantic 65,536-sector count reaches the one-PRDT limit");

    request = MakeRequest();
    request.SectorCount = AhciReadDmaExtCommandBuilder::MaximumSectorCount + 1U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::SectorCountNotEncodable, g_page,
                                 PageSizeBytes, "first sector count above ATA semantics is rejected without mutation");

    request = MakeRequest();
    request.Supports48BitAddressing = false;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::DeviceDoesNotSupport48BitAddressing,
                                 g_page, PageSizeBytes,
                                 "device without 48-bit addressing is rejected without mutation");

    request = MakeRequest();
    request.LogicalSectorSizeBytes = 510U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::UnsupportedLogicalSectorSize, g_page,
                                 PageSizeBytes, "logical sector below 512 bytes is rejected without mutation");

    request = MakeRequest();
    request.LogicalSectorSizeBytes = 513U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::UnsupportedLogicalSectorSize, g_page,
                                 PageSizeBytes, "odd logical sector size is rejected without mutation");

    request = MakeRequest();
    request.SectorCount = AhciReadDmaExtCommandBuilder::MaximumSectorCount;
    request.LogicalSectorSizeBytes = MaximumUInt64 - 1U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::TransferByteCountOverflow, g_page,
                                 PageSizeBytes, "transfer-byte multiplication overflow is rejected without mutation");

    request = MakeRequest();
    request.SectorCount = 8193U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::TransferExceedsPrdtEntry, g_page,
                                 PageSizeBytes, "512-byte transfer above 4 MiB is rejected without mutation");

    request = MakeRequest();
    request.LogicalSectorSizeBytes = 4096U;
    request.SectorCount = 1025U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::TransferExceedsPrdtEntry, g_page,
                                 PageSizeBytes, "4096-byte transfer above 4 MiB is rejected without mutation");

    request = MakeRequest();
    request.LogicalSectorSizeBytes = (4U * 1024U * 1024U) + 2U;
    request.SectorCount = 1U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::TransferExceedsPrdtEntry, g_page,
                                 PageSizeBytes, "single extended sector above 4 MiB is rejected without mutation");

    request = MakeRequest();
    request.StartingLba = AhciReadDmaExtCommandBuilder::MaximumLba + 1U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::LbaNotEncodable, g_page, PageSizeBytes,
                                 "LBA wider than 48 bits is rejected without mutation");

    request = MakeRequest();
    request.StartingLba = AhciReadDmaExtCommandBuilder::MaximumLba;
    request.SectorCount = 2U;
    request.DataBuffer.SizeBytes = 1024U;
    request.DeviceSectorCount = 0x0001'0000'0000'0000ULL;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::LbaNotEncodable, g_page, PageSizeBytes,
                                 "transfer crossing the 48-bit LBA limit is rejected without mutation");

    request = MakeRequest();
    request.DeviceSectorCount = request.StartingLba;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::TransferOutsideDeviceCapacity, g_page,
                                 PageSizeBytes, "LBA equal to device capacity is rejected without mutation");

    request = MakeRequest();
    request.DeviceSectorCount = request.StartingLba + 7U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::TransferOutsideDeviceCapacity, g_page,
                                 PageSizeBytes, "transfer crossing device capacity is rejected without mutation");

    request = MakeRequest();
    request.DeviceSectorCount = 0U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::TransferOutsideDeviceCapacity, g_page,
                                 PageSizeBytes, "empty device capacity is rejected without mutation");
}

void TestDataBufferAndOverlapRejections(TestContext& runner) noexcept
{
    AhciReadDmaExtCommandRequest request = MakeRequest();
    request.DataBuffer.SizeBytes -= 1U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::InvalidDataBufferSize, g_page,
                                 PageSizeBytes, "short destination span is rejected without command-page mutation");

    request = MakeRequest();
    request.DataBuffer.SizeBytes += 1U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::InvalidDataBufferSize, g_page,
                                 PageSizeBytes, "oversized destination span is rejected without command-page mutation");

    request = MakeRequest();
    request.DataBuffer.PhysicalAddress = MaximumUInt64 - 4094U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::DataBufferPhysicalAddressWrap, g_page,
                                 PageSizeBytes,
                                 "wrapping destination extent is rejected without command-page mutation");

    request = MakeRequest();
    request.DataBuffer.PhysicalAddress = 0x0020'0001U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::InvalidDataBufferPhysicalAlignment,
                                 g_page, PageSizeBytes,
                                 "misaligned destination address is rejected without command-page mutation");

    request = MakeRequest();
    request.CommandPage.PhysicalAddress = 0x0000'0001'0000'0000ULL;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::ControllerAddressWidthExceeded, g_page,
                                 PageSizeBytes, "32-bit controller rejects a command page at 4 GiB without mutation");

    request = MakeRequest();
    request.DataBuffer.PhysicalAddress = 0xFFFF'F002ULL;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::ControllerAddressWidthExceeded, g_page,
                                 PageSizeBytes,
                                 "32-bit controller rejects a destination crossing 4 GiB without mutation");

    request = MakeRequest();
    request.DataBuffer.PhysicalAddress = request.CommandPage.PhysicalAddress;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::CommandPageOverlapsDataBuffer, g_page,
                                 PageSizeBytes, "destination at command-page start is rejected without mutation");

    request = MakeRequest();
    request.DataBuffer.PhysicalAddress = request.CommandPage.PhysicalAddress - 2U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::CommandPageOverlapsDataBuffer, g_page,
                                 PageSizeBytes,
                                 "destination crossing into command-page start is rejected without mutation");

    request = MakeRequest();
    request.DataBuffer.PhysicalAddress = request.CommandPage.PhysicalAddress + PageSizeBytes - 2U;
    CheckRejectedWithoutMutation(runner, request, AhciReadDmaExtCommandError::CommandPageOverlapsDataBuffer, g_page,
                                 PageSizeBytes, "destination crossing command-page end is rejected without mutation");
}

void TestRepeatBuildClearsOnlyActiveRegions(TestContext& runner) noexcept
{
    AhciReadDmaExtCommandRequest request = MakeRequest();
    FillBytes(g_page, PageSizeBytes, Sentinel);
    const auto first = AhciReadDmaExtCommandBuilder::Build(request);
    ASTERIA_EXPECT(runner, "first READ DMA EXT command build succeeds", first.HasValue());
    if (!first.HasValue())
    {
        return;
    }
    g_page[0x004U] = 0xFFU;
    g_page[0x020U] = 0xFFU;
    g_page[0x400U] = 0xFFU;
    g_page[0x510U] = 0xFFU;
    g_page[0x588U] = 0xFFU;
    g_page[0x590U] = 0x3CU;
    g_page[0xFFFU] = 0x7EU;

    const auto second = AhciReadDmaExtCommandBuilder::Build(request);
    BuildExpectedImage(g_before, request, Sentinel);
    g_before[0x590U] = 0x3CU;
    g_before[0xFFFU] = 0x7EU;
    ASTERIA_EXPECT(runner, "repeat build clears stale active state while preserving inactive bytes", second.HasValue(),
                   BytesEqual(g_page, g_before, PageSizeBytes),
                   second.GetValue().TransferSizeBytes == first.GetValue().TransferSizeBytes);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"AhciReadDmaExtCommandBuilderTests"};
    Asteria::Test::TestExactWireImageAndLayout(runner);
    Asteria::Test::TestExternalDestinationRemainsUntouched(runner);
    Asteria::Test::TestAddressAndTransferBoundaries(runner);
    Asteria::Test::TestCommandPageRejections(runner);
    Asteria::Test::TestRequestAndCommandPageVirtualDisjointness(runner);
    Asteria::Test::TestTransferAndLbaRejections(runner);
    Asteria::Test::TestDataBufferAndOverlapRejections(runner);
    Asteria::Test::TestRepeatBuildClearsOnlyActiveRegions(runner);
    return runner.Finish();
}
