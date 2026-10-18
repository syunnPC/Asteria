// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Boot/Uefi/TableValidation.hpp"
#include "Boot/Uefi/UefiTypes.hpp"
#include "Foundation/Types.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{

struct TestTable
{
    Boot::Uefi::TableHeader Header;
    Foundation::UInt64 Payload[4U];
};

[[nodiscard]] TestTable MakeValidTable() noexcept
{
    TestTable table{
        .Header =
            {
                .Signature = Boot::Uefi::SystemTableSignature,
                .Revision = Boot::Uefi::MinimumSupportedTableRevision,
                .HeaderSize = sizeof(TestTable),
                .Crc32 = 0U,
                .Reserved = 0U,
            },
        .Payload =
            {
                0x0123'4567'89AB'CDEFULL,
                0x1020'3040'5060'7080ULL,
                0xFFFF'0000'AAAA'5555ULL,
                0x0000'1111'2222'3333ULL,
            },
    };
    table.Header.Crc32 = Boot::Uefi::ComputeCrc32(&table, sizeof(table));
    return table;
}

void TestCrc32(TestContext& runner) noexcept
{
    constexpr char StandardVector[]{"123456789"};
    const Foundation::UInt8 zeroBytes[8U]{};
    ASTERIA_EXPECT(runner, "CRC32 matches the standard check vector",
                   Boot::Uefi::ComputeCrc32(StandardVector, 9U) == 0xCBF4'3926U);
    ASTERIA_EXPECT(runner, "CRC32 matches the retained BSS zero vector",
                   Boot::Uefi::ComputeCrc32(zeroBytes, sizeof(zeroBytes)) == 0x6522'DF69U);
    ASTERIA_EXPECT(runner, "CRC32 accepts an empty range", Boot::Uefi::ComputeCrc32(nullptr, 0U) == 0U);

    TestTable table = MakeValidTable();
    ASTERIA_EXPECT(runner, "table CRC treats the stored CRC field as zero",
                   Boot::Uefi::IsTableCrcValid(&table, table.Header));
    table.Payload[2U] ^= 1U;
    ASTERIA_EXPECT(runner, "table CRC detects payload mutation", !Boot::Uefi::IsTableCrcValid(&table, table.Header));
    table = MakeValidTable();
    table.Header.Crc32 ^= 1U;
    ASTERIA_EXPECT(runner, "table CRC detects a corrupted stored value",
                   !Boot::Uefi::IsTableCrcValid(&table, table.Header));
}

void TestTableHeaderContract(TestContext& runner) noexcept
{
    TestTable table = MakeValidTable();
    ASTERIA_EXPECT(runner, "minimum supported table header is accepted",
                   Boot::Uefi::IsTableHeaderValid(table.Header, Boot::Uefi::SystemTableSignature, sizeof(TestTable)));

    table.Header.HeaderSize = 0U;
    ASTERIA_EXPECT(runner, "header extent cannot be smaller than the fixed header",
                   !Boot::Uefi::IsTableHeaderValid(table.Header, Boot::Uefi::SystemTableSignature, 0U));

    table = MakeValidTable();
    table.Header.Revision = Boot::Uefi::MinimumSupportedTableRevision - 1U;
    ASTERIA_EXPECT(runner, "pre-EFI-1.10 revision is rejected",
                   !Boot::Uefi::IsTableHeaderValid(table.Header, Boot::Uefi::SystemTableSignature, sizeof(TestTable)));
    table = MakeValidTable();
    table.Header.Signature ^= 1U;
    ASTERIA_EXPECT(runner, "wrong table signature is rejected",
                   !Boot::Uefi::IsTableHeaderValid(table.Header, Boot::Uefi::SystemTableSignature, sizeof(TestTable)));
    table = MakeValidTable();
    table.Header.HeaderSize = sizeof(TestTable) - 1U;
    ASTERIA_EXPECT(runner, "short table extent is rejected",
                   !Boot::Uefi::IsTableHeaderValid(table.Header, Boot::Uefi::SystemTableSignature, sizeof(TestTable)));
    table = MakeValidTable();
    table.Header.HeaderSize = Boot::Uefi::MaximumTableHeaderBytes + 1U;
    ASTERIA_EXPECT(runner, "oversized table extent is rejected",
                   !Boot::Uefi::IsTableHeaderValid(table.Header, Boot::Uefi::SystemTableSignature, sizeof(TestTable)));
    table = MakeValidTable();
    table.Header.Reserved = 1U;
    ASTERIA_EXPECT(runner, "nonzero reserved header field is rejected",
                   !Boot::Uefi::IsTableHeaderValid(table.Header, Boot::Uefi::SystemTableSignature, sizeof(TestTable)));

    table = MakeValidTable();
    table.Header.HeaderSize = sizeof(Boot::Uefi::TableHeader) - 1U;
    ASTERIA_EXPECT(runner, "CRC validation rejects a truncated header",
                   !Boot::Uefi::IsTableCrcValid(&table, table.Header));
    table.Header.HeaderSize = Boot::Uefi::MaximumTableHeaderBytes + 1U;
    ASTERIA_EXPECT(runner, "CRC validation rejects an oversized header",
                   !Boot::Uefi::IsTableCrcValid(&table, table.Header));
}

void TestExpiredServices(TestContext& runner) noexcept
{
    Boot::Uefi::SystemTable table{};
    Boot::Uefi::BootServices services{};
    Boot::Uefi::SimpleTextOutputProtocol console{};
    Foundation::UInt64 retained{1U};
    table.Header = {Boot::Uefi::SystemTableSignature, Boot::Uefi::MinimumSupportedTableRevision, sizeof(table), 0U, 0U};
    table.ConsoleInHandle = &retained;
    table.ConsoleIn = &retained;
    table.ConsoleOutHandle = &retained;
    table.ConsoleOut = &console;
    table.StandardErrorHandle = &retained;
    table.StandardError = &console;
    table.BootServicesTable = &services;
    table.RuntimeServices = &retained;
    table.FirmwareRevision = 17U;
    table.Header.Crc32 = Boot::Uefi::ComputeCrc32(&table, sizeof(table));
    Boot::Uefi::ClearExpiredBootServices(table);
    ASTERIA_EXPECT(runner, "expired services are absent with a valid system-table CRC",
                   table.BootServicesTable == nullptr, table.ConsoleInHandle == nullptr, table.ConsoleIn == nullptr,
                   table.ConsoleOutHandle == nullptr, table.ConsoleOut == nullptr, table.StandardErrorHandle == nullptr,
                   table.StandardError == nullptr, Boot::Uefi::IsTableCrcValid(&table, table.Header),
                   table.RuntimeServices == &retained, table.FirmwareRevision == 17U);
}

void TestBssProbe(TestContext& runner) noexcept
{
    volatile Foundation::UInt64 zeroProbe{0U};
    volatile Foundation::UInt64 nonzeroProbe{1U};
    ASTERIA_EXPECT(runner, "volatile zero probe is accepted", Boot::Uefi::IsZeroInitializedProbe(zeroProbe));
    ASTERIA_EXPECT(runner, "volatile nonzero probe is rejected", !Boot::Uefi::IsZeroInitializedProbe(nonzeroProbe));
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"TableValidationTests"};
    Asteria::Test::TestCrc32(runner);
    Asteria::Test::TestTableHeaderContract(runner);
    Asteria::Test::TestBssProbe(runner);
    Asteria::Test::TestExpiredServices(runner);
    return runner.Finish();
}
