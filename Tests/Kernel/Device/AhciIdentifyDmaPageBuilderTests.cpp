// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <type_traits>

#include "Kernel/Device/AhciIdentifyDmaPageBuilder.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt32;
using Foundation::UInt64;
using Kernel::Device::AhciDmaPageWritableView;
using Kernel::Device::AhciIdentifyDmaPageBuilder;
using Kernel::Device::AhciIdentifyDmaPageError;
using Kernel::Device::AhciIdentifyDmaPageLayout;

constexpr Size PageSizeBytes{AhciIdentifyDmaPageBuilder::PageSizeBytes};
constexpr UInt8 Sentinel{0xA5U};
constexpr UInt64 MaximumUInt64{~UInt64{0U}};

alignas(4096) UInt8 g_page[PageSizeBytes]{};
alignas(4096) UInt8 g_before[PageSizeBytes]{};
alignas(4096) UInt8 g_extendedPage[PageSizeBytes * 2U]{};

static_assert(std::is_aggregate_v<AhciDmaPageWritableView>);
static_assert(std::is_standard_layout_v<AhciDmaPageWritableView>);
static_assert(std::is_trivially_copyable_v<AhciDmaPageWritableView>);
static_assert(sizeof(AhciDmaPageWritableView) == 24U);
static_assert(alignof(AhciDmaPageWritableView) == 8U);
static_assert(std::is_aggregate_v<AhciIdentifyDmaPageLayout>);
static_assert(std::is_standard_layout_v<AhciIdentifyDmaPageLayout>);
static_assert(std::is_trivially_copyable_v<AhciIdentifyDmaPageLayout>);
static_assert(sizeof(AhciIdentifyDmaPageLayout) == 32U);
static_assert(alignof(AhciIdentifyDmaPageLayout) == 8U);
static_assert(sizeof(AhciIdentifyDmaPageError) == 1U);
static_assert(AhciIdentifyDmaPageBuilder::PageSizeBytes == 4096U);
static_assert(AhciIdentifyDmaPageBuilder::CommandListOffsetBytes == 0x000U);
static_assert(AhciIdentifyDmaPageBuilder::CommandListSizeBytes == 1024U);
static_assert(AhciIdentifyDmaPageBuilder::ReceivedFisOffsetBytes == 0x400U);
static_assert(AhciIdentifyDmaPageBuilder::ReceivedFisSizeBytes == 256U);
static_assert(AhciIdentifyDmaPageBuilder::CommandTableOffsetBytes == 0x500U);
static_assert(AhciIdentifyDmaPageBuilder::CommandTableSizeBytes == 144U);
static_assert(AhciIdentifyDmaPageBuilder::IdentifyDataOffsetBytes == 0x600U);
static_assert(AhciIdentifyDmaPageBuilder::IdentifyDataSizeBytes == 512U);

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

void WriteLittleEndian32(UInt8* const bytes, const UInt32 value) noexcept
{
    for (UInt32 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

void WriteAddress(UInt8* const bytes, const UInt64 address) noexcept
{
    WriteLittleEndian32(bytes, static_cast<UInt32>(address & 0xFFFF'FFFFULL));
    WriteLittleEndian32(bytes + 4U, static_cast<UInt32>(address >> 32U));
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

void BuildExpectedImage(UInt8* const expected, const UInt64 physicalAddress, const UInt8 untouchedValue) noexcept
{
    FillBytes(expected, PageSizeBytes, untouchedValue);
    FillBytes(expected + AhciIdentifyDmaPageBuilder::CommandListOffsetBytes,
              AhciIdentifyDmaPageBuilder::CommandListSizeBytes, 0U);
    FillBytes(expected + AhciIdentifyDmaPageBuilder::ReceivedFisOffsetBytes,
              AhciIdentifyDmaPageBuilder::ReceivedFisSizeBytes, 0U);
    FillBytes(expected + AhciIdentifyDmaPageBuilder::CommandTableOffsetBytes,
              AhciIdentifyDmaPageBuilder::CommandTableSizeBytes, 0U);
    FillBytes(expected + AhciIdentifyDmaPageBuilder::IdentifyDataOffsetBytes,
              AhciIdentifyDmaPageBuilder::IdentifyDataSizeBytes, 0U);

    WriteLittleEndian32(expected + AhciIdentifyDmaPageBuilder::CommandListOffsetBytes, 0x0001'0005U);
    WriteAddress(expected + AhciIdentifyDmaPageBuilder::CommandListOffsetBytes + 8U,
                 physicalAddress + AhciIdentifyDmaPageBuilder::CommandTableOffsetBytes);

    const Size commandFisOffset = AhciIdentifyDmaPageBuilder::CommandTableOffsetBytes;
    expected[commandFisOffset + 0U] = 0x27U;
    expected[commandFisOffset + 1U] = 0x80U;
    expected[commandFisOffset + 2U] = 0xECU;

    const Size prdtOffset = commandFisOffset + 128U;
    WriteAddress(expected + prdtOffset, physicalAddress + AhciIdentifyDmaPageBuilder::IdentifyDataOffsetBytes);
    WriteLittleEndian32(expected + prdtOffset + 12U, 511U);
}

[[nodiscard]] AhciDmaPageWritableView MakePageView(UInt8* const bytes, const Size sizeBytes,
                                                   const UInt64 physicalAddress) noexcept
{
    return AhciDmaPageWritableView{
        .Data = bytes,
        .SizeBytes = sizeBytes,
        .PhysicalAddress = physicalAddress,
    };
}

void CheckRejectedWithoutMutation(TestContext& runner, const AhciDmaPageWritableView page,
                                  const bool supports64BitAddressing, const AhciIdentifyDmaPageError expectedError,
                                  UInt8* const checkedStorage, const char* const description) noexcept
{
    FillBytes(checkedStorage, PageSizeBytes, 0x6DU);
    CopyBytes(g_before, checkedStorage, PageSizeBytes);

    const auto result = AhciIdentifyDmaPageBuilder::Build(page, supports64BitAddressing);
    ASTERIA_EXPECT(runner, description, !result.HasValue(), result.GetError() == expectedError);
    ASTERIA_EXPECT(runner, "rejection leaves all 4096 checked bytes unchanged",
                   BytesEqual(checkedStorage, g_before, PageSizeBytes));
}

void TestExactWireImageAndLayout(TestContext& runner) noexcept
{
    constexpr UInt64 physicalAddress{0x1234'5678'9ABC'D000ULL};
    FillBytes(g_page, PageSizeBytes, Sentinel);

    const auto result = AhciIdentifyDmaPageBuilder::Build(MakePageView(g_page, PageSizeBytes, physicalAddress), true);
    ASTERIA_EXPECT(runner, "64-bit controller accepts high DMA page", result.HasValue());
    if (!result.HasValue())
    {
        return;
    }

    const AhciIdentifyDmaPageLayout& layout = result.GetValue();
    ASTERIA_EXPECT(runner, "layout reports command-list physical address",
                   layout.CommandListPhysicalAddress == physicalAddress);
    ASTERIA_EXPECT(runner, "layout reports received-FIS physical address",
                   layout.ReceivedFisPhysicalAddress == physicalAddress + 0x400U);
    ASTERIA_EXPECT(runner, "layout reports command-table physical address",
                   layout.CommandTablePhysicalAddress == physicalAddress + 0x500U);
    ASTERIA_EXPECT(runner, "layout reports IDENTIFY-buffer physical address",
                   layout.IdentifyDataPhysicalAddress == physicalAddress + 0x600U);

    BuildExpectedImage(g_before, physicalAddress, Sentinel);
    ASTERIA_EXPECT(runner, "all 4096 output bytes match the canonical fixed layout",
                   BytesEqual(g_page, g_before, PageSizeBytes));
    ASTERIA_EXPECT(runner, "slot-0 header encodes CFL=5, read direction, and PRDTL=1",
                   ReadLittleEndian32(g_page + 0x000U) == 0x0001'0005U);
    ASTERIA_EXPECT(runner, "slot-0 PRDBC starts at zero", ReadLittleEndian32(g_page + 0x004U) == 0U);
    ASTERIA_EXPECT(runner, "slot-0 CTBA low half is little-endian",
                   ReadLittleEndian32(g_page + 0x008U) == 0x9ABC'D500U);
    ASTERIA_EXPECT(runner, "slot-0 CTBA upper half is little-endian",
                   ReadLittleEndian32(g_page + 0x00CU) == 0x1234'5678U);
    ASTERIA_EXPECT(runner, "reserved slot-0 bytes and unused command headers are zero",
                   RegionHasValue(g_page, 0x010U, 0x3F0U, 0U));
    ASTERIA_EXPECT(runner, "received-FIS buffer is fresh and zeroed", RegionHasValue(g_page, 0x400U, 0x100U, 0U));
    ASTERIA_EXPECT(runner, "Register H2D FIS carries type, command flag, and IDENTIFY opcode", g_page[0x500U] == 0x27U,
                   g_page[0x501U] == 0x80U, g_page[0x502U] == 0xECU);
    ASTERIA_EXPECT(runner, "PRDT data-base low half is little-endian",
                   ReadLittleEndian32(g_page + 0x580U) == 0x9ABC'D600U);
    ASTERIA_EXPECT(runner, "PRDT data-base upper half is little-endian",
                   ReadLittleEndian32(g_page + 0x584U) == 0x1234'5678U);
    ASTERIA_EXPECT(runner, "PRDT reserved dword is zero", ReadLittleEndian32(g_page + 0x588U) == 0U);
    ASTERIA_EXPECT(runner, "PRDT DBC encodes exactly 512 bytes with interrupts disabled",
                   ReadLittleEndian32(g_page + 0x58CU) == 511U);
    ASTERIA_EXPECT(runner, "alignment gap after command table remains untouched",
                   RegionHasValue(g_page, 0x590U, 0x70U, Sentinel));
    ASTERIA_EXPECT(runner, "IDENTIFY data buffer is cleared for a fresh transfer",
                   RegionHasValue(g_page, 0x600U, 0x200U, 0U));
    ASTERIA_EXPECT(runner, "unused page tail remains untouched", RegionHasValue(g_page, 0x800U, 0x800U, Sentinel));
}

void TestThirtyTwoBitAddressingBoundaries(TestContext& runner) noexcept
{
    constexpr UInt64 last32BitPage{0x0000'0000'FFFF'F000ULL};
    FillBytes(g_page, PageSizeBytes, 0xC3U);
    const auto boundary = AhciIdentifyDmaPageBuilder::Build(MakePageView(g_page, PageSizeBytes, last32BitPage), false);
    ASTERIA_EXPECT(runner, "32-bit controller accepts the final complete 4-KiB page", boundary.HasValue());
    if (boundary.HasValue())
    {
        ASTERIA_EXPECT(runner, "32-bit boundary layout keeps IDENTIFY data address representable",
                       boundary.GetValue().IdentifyDataPhysicalAddress == 0x0000'0000'FFFF'F600ULL);
        ASTERIA_EXPECT(runner, "32-bit boundary CTBA upper half is zero", ReadLittleEndian32(g_page + 0x00CU) == 0U);
        ASTERIA_EXPECT(runner, "32-bit boundary PRDT upper half is zero", ReadLittleEndian32(g_page + 0x584U) == 0U);
    }

    CheckRejectedWithoutMutation(runner, MakePageView(g_page, PageSizeBytes, 0x0000'0001'0000'0000ULL), false,
                                 AhciIdentifyDmaPageError::ControllerAddressWidthExceeded, g_page,
                                 "32-bit controller rejects a page beginning at 4 GiB");
}

void TestZeroAndMaximumAddressBoundaries(TestContext& runner) noexcept
{
    FillBytes(g_page, PageSizeBytes, 0x87U);
    const auto zero = AhciIdentifyDmaPageBuilder::Build(MakePageView(g_page, PageSizeBytes, 0U), false);
    ASTERIA_EXPECT(runner, "physical page zero is accepted when caller owns it", zero.HasValue());
    if (zero.HasValue())
    {
        ASTERIA_EXPECT(runner, "zero-page layout derives the fixed physical offsets",
                       zero.GetValue().CommandListPhysicalAddress == 0U,
                       zero.GetValue().CommandTablePhysicalAddress == 0x500U,
                       zero.GetValue().IdentifyDataPhysicalAddress == 0x600U);
    }

    constexpr UInt64 last64BitPage{0xFFFF'FFFF'FFFF'F000ULL};
    FillBytes(g_page, PageSizeBytes, 0x91U);
    const auto maximum = AhciIdentifyDmaPageBuilder::Build(MakePageView(g_page, PageSizeBytes, last64BitPage), true);
    ASTERIA_EXPECT(runner, "64-bit controller accepts the final non-wrapping page", maximum.HasValue());
    if (maximum.HasValue())
    {
        ASTERIA_EXPECT(runner, "maximum-page address arithmetic remains exact",
                       maximum.GetValue().IdentifyDataPhysicalAddress == 0xFFFF'FFFF'FFFF'F600ULL);
        ASTERIA_EXPECT(runner, "maximum-page CTBA high half is all ones",
                       ReadLittleEndian32(g_page + 0x00CU) == 0xFFFF'FFFFU);
    }

    CheckRejectedWithoutMutation(runner, MakePageView(g_page, PageSizeBytes, MaximumUInt64), true,
                                 AhciIdentifyDmaPageError::PhysicalAddressWrap, g_page,
                                 "page range that wraps the 64-bit physical address space is rejected");
}

void TestViewAndAlignmentFailures(TestContext& runner) noexcept
{
    CheckRejectedWithoutMutation(runner, MakePageView(nullptr, PageSizeBytes, 0x0020'0000U), true,
                                 AhciIdentifyDmaPageError::NullBuffer, g_page, "null writable view is rejected");
    CheckRejectedWithoutMutation(runner, MakePageView(g_page, PageSizeBytes - 1U, 0x0020'0000U), true,
                                 AhciIdentifyDmaPageError::InvalidBufferSize, g_page,
                                 "short writable view is rejected");
    CheckRejectedWithoutMutation(runner, MakePageView(g_extendedPage, PageSizeBytes + 1U, 0x0020'0000U), true,
                                 AhciIdentifyDmaPageError::InvalidBufferSize, g_extendedPage,
                                 "oversized writable view is rejected");
    CheckRejectedWithoutMutation(runner, MakePageView(g_extendedPage + 1U, PageSizeBytes, 0x0020'0000U), true,
                                 AhciIdentifyDmaPageError::InvalidVirtualAlignment, g_extendedPage + 1U,
                                 "misaligned virtual page is rejected");
    CheckRejectedWithoutMutation(runner, MakePageView(g_page, PageSizeBytes, 0x0020'0001U), true,
                                 AhciIdentifyDmaPageError::InvalidPhysicalAlignment, g_page,
                                 "misaligned physical page is rejected");
}

void TestRepeatBuildClearsActiveState(TestContext& runner) noexcept
{
    constexpr UInt64 physicalAddress{0x0000'0000'0100'0000ULL};
    FillBytes(g_page, PageSizeBytes, Sentinel);
    const AhciDmaPageWritableView page = MakePageView(g_page, PageSizeBytes, physicalAddress);
    const auto first = AhciIdentifyDmaPageBuilder::Build(page, false);
    ASTERIA_EXPECT(runner, "first repeatability build succeeds", first.HasValue());
    if (!first.HasValue())
    {
        return;
    }

    g_page[0x004U] = 0xFFU;
    g_page[0x010U] = 0xFFU;
    g_page[0x020U] = 0xFFU;
    g_page[0x400U] = 0xFFU;
    g_page[0x510U] = 0xFFU;
    g_page[0x540U] = 0xFFU;
    g_page[0x588U] = 0xFFU;
    g_page[0x600U] = 0xFFU;
    g_page[0x590U] = 0x3CU;
    g_page[0x800U] = 0x7EU;

    const auto second = AhciIdentifyDmaPageBuilder::Build(page, false);
    ASTERIA_EXPECT(runner, "second build over simulated completion succeeds", second.HasValue());
    if (!second.HasValue())
    {
        return;
    }

    BuildExpectedImage(g_before, physicalAddress, Sentinel);
    g_before[0x590U] = 0x3CU;
    g_before[0x800U] = 0x7EU;
    ASTERIA_EXPECT(runner, "repeat build clears stale active state and preserves unused bytes",
                   BytesEqual(g_page, g_before, PageSizeBytes));
    ASTERIA_EXPECT(runner, "repeat build returns the same typed layout",
                   second.GetValue().CommandTablePhysicalAddress == first.GetValue().CommandTablePhysicalAddress,
                   second.GetValue().IdentifyDataPhysicalAddress == first.GetValue().IdentifyDataPhysicalAddress);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"AhciIdentifyDmaPageBuilderTests"};
    Asteria::Test::TestExactWireImageAndLayout(runner);
    Asteria::Test::TestThirtyTwoBitAddressingBoundaries(runner);
    Asteria::Test::TestZeroAndMaximumAddressBoundaries(runner);
    Asteria::Test::TestViewAndAlignmentFailures(runner);
    Asteria::Test::TestRepeatBuildClearsActiveState(runner);
    return runner.Finish();
}
