// SPDX-License-Identifier: Apache-2.0
#include "Boot/Acpi/DebugPortTables.hpp"

#include <cstring>
#include <initializer_list>
#include <sys/mman.h>

#include "Support/TestContext.hpp"

namespace
{
using Asteria::Boot::BootDebugPort;
using Asteria::Boot::BootDebugPortSource;
using Asteria::Boot::BootDebugPortSpace;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt16;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::UInt8;
namespace Acpi = Asteria::Boot::Acpi;

constexpr Size TableCapacity{512U};
constexpr UInt8 Memory{0U};
constexpr UInt8 Io{1U};

void Store(UInt8* const bytes, const UInt64 value, const Size count) noexcept
{
    for (Size index{0U}; index < count; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

// A table under construction: header, then fields at their offsets.
class Table final
{
public:
    Table(const char* const signature, const Size length, const UInt8 revision) noexcept : m_length{length}
    {
        std::memcpy(m_bytes, signature, 4U);
        Store(m_bytes + 4U, length, 4U);
        m_bytes[8U] = revision;
    }

    void Put(const Size offset, const UInt64 value, const Size count) noexcept
    {
        Store(m_bytes + offset, value, count);
    }

    // ACPI 6.5 5.2.3.2: space, bit width, bit offset, access size, address.
    void Address(const Size offset, const UInt8 space, const UInt8 width, const UInt8 access,
                 const UInt64 address) noexcept
    {
        m_bytes[offset] = space;
        m_bytes[offset + 1U] = width;
        m_bytes[offset + 2U] = 0U;
        m_bytes[offset + 3U] = access;
        Store(m_bytes + offset + 4U, address, 8U);
    }

    [[nodiscard]] const UInt8* Seal() noexcept
    {
        m_bytes[9U] = 0U;
        UInt8 sum{0U};
        for (Size index{0U}; index < m_length; ++index)
        {
            sum = static_cast<UInt8>(sum + m_bytes[index]);
        }
        m_bytes[9U] = static_cast<UInt8>(0U - sum);
        return m_bytes;
    }

    [[nodiscard]] UInt8* Bytes() noexcept
    {
        return m_bytes;
    }
    [[nodiscard]] Size Length() const noexcept
    {
        return m_length;
    }
    [[nodiscard]] Acpi::DebugPortTableView View() noexcept
    {
        return {Seal(), m_length};
    }

private:
    UInt8 m_bytes[TableCapacity]{};
    Size m_length;
};

// DBGP 1.0c: interface type at 36, base address at 40.
[[nodiscard]] Table Dbgp(const UInt8 interfaceType, const UInt8 space, const UInt64 address) noexcept
{
    Table table{"DBGP", 52U, 1U};
    table.Put(36U, interfaceType, 1U);
    table.Address(40U, space, 8U, 0U, address);
    return table;
}

// SPCR: interface type, base address, configured baud rate at 58.
[[nodiscard]] Table Spcr(const UInt8 revision, const UInt8 interfaceType, const UInt8 space, const UInt64 address,
                         const UInt8 width = 8U, const UInt8 access = 1U) noexcept
{
    Table table{"SPCR", revision >= 4U ? 88U : 80U, revision};
    table.Put(36U, interfaceType, 1U);
    table.Address(40U, space, width, access, address);
    table.Put(58U, 7U, 1U);
    return table;
}

// HCDP 1.0a / PCDP 2.0: count at 36, 48-byte UART descriptors from 40 (type,
// baud at 8, generic address at 16), then a VGA descriptor PCDP-style.
[[nodiscard]] Table Hcdp(const UInt8 firstType, const UInt8 secondType, const UInt64 secondAddress) noexcept
{
    Table table{"HCDP", 40U + (2U * 48U) + 16U, 3U};
    table.Put(36U, 2U, 4U);
    table.Put(40U, firstType, 1U);
    table.Put(48U, 115'200U, 8U);
    table.Address(56U, Io, 8U, 0U, 0x3F8U);
    table.Put(88U, secondType, 1U);
    table.Put(96U, 9'600U, 8U);
    table.Address(104U, Memory, 8U, 0U, secondAddress);
    table.Put(136U, 0x0AU, 1U);
    table.Put(138U, 16U, 2U);
    return table;
}

// DBG2: a USB debug device, then a serial one of the given subtype.
[[nodiscard]] Table Dbg2(const UInt16 subtype, const UInt8 width, const UInt8 access, const UInt64 address) noexcept
{
    constexpr Size first{44U};
    constexpr Size device{36U};
    Table table{"DBG2", first + (2U * device), 0U};
    table.Put(36U, first, 4U);
    table.Put(40U, 2U, 4U);
    for (Size index{0U}; index < 2U; ++index)
    {
        const Size base = first + (index * device);
        table.Put(base + 1U, device, 2U);
        table.Put(base + 3U, 1U, 1U);
        table.Put(base + 12U, index == 0U ? 0x8002U : 0x8000U, 2U);
        table.Put(base + 14U, index == 0U ? 0U : subtype, 2U);
        table.Put(base + 18U, 22U, 2U);
        table.Address(base + 22U, Memory, width, access, index == 0U ? 0xFED0'0000U : address);
    }
    return table;
}

[[nodiscard]] bool Is(const BootDebugPort& port, const BootDebugPortSource source, const UInt8 space,
                      const UInt64 address, const UInt8 stride, const UInt32 baud) noexcept
{
    return (port.Source == static_cast<UInt8>(source)) && (port.AddressSpace == space) && (port.Address == address) &&
           (port.RegisterStrideBytes == stride) && (port.AccessBytes == 1U) && (port.BaudRate == baud);
}

void TestDecoders(Asteria::TestSupport::TestContext& context) noexcept
{
    BootDebugPort port{};
    Table vpc = Dbgp(0U, Memory, 0x47'F000'1000U);
    ASTERIA_EXPECT(context, "itanium2-vpc's DBGP names its MMIO debug UART",
                   Acpi::DecodeDbgpDebugPort(vpc.Seal(), vpc.Length(), port),
                   Is(port, BootDebugPortSource::Dbgp, Memory, 0x47'F000'1000U, 1U, 0U));
    Table i2000 = Spcr(1U, 0U, Io, 0x3F8U);
    ASTERIA_EXPECT(context, "hp-i2000's SPCR names COM1 at its configured 115200 baud",
                   Acpi::DecodeSpcrConsolePort(i2000.Seal(), i2000.Length(), port),
                   Is(port, BootDebugPortSource::Spcr, Io, 0x3F8U, 1U, 115'200U));
    Table precise = Spcr(4U, 0x12U, Memory, 0x9000'0000U, 32U, 1U);
    precise.Put(80U, 1'500'000U, 4U);
    ASTERIA_EXPECT(context, "SPCR 4 takes a 0x12 subtype's stride and the precise baud rate",
                   Acpi::DecodeSpcrConsolePort(precise.Seal(), precise.Length(), port),
                   Is(port, BootDebugPortSource::Spcr, Memory, 0x9000'0000U, 4U, 1'500'000U));
    Table hcdp = Hcdp(0U, 1U, 0xF000'0000U);
    ASTERIA_EXPECT(context, "HCDP gives the first descriptor of each UART type",
                   Acpi::DecodeHcdpUart(hcdp.Seal(), hcdp.Length(), Acpi::HcdpUartType::Console, port),
                   Is(port, BootDebugPortSource::HcdpConsole, Io, 0x3F8U, 1U, 115'200U),
                   Acpi::DecodeHcdpUart(hcdp.Seal(), hcdp.Length(), Acpi::HcdpUartType::DebugPort, port),
                   Is(port, BootDebugPortSource::HcdpDebugPort, Memory, 0xF000'0000U, 1U, 9'600U));
    Table dbg2 = Dbg2(0x12U, 32U, 1U, 0xFE00'1000U);
    ASTERIA_EXPECT(context, "DBG2 skips a USB debug device and takes a GAS-described 16550",
                   Acpi::DecodeDbg2DebugPort(dbg2.Seal(), dbg2.Length(), port),
                   Is(port, BootDebugPortSource::Dbg2, Memory, 0xFE00'1000U, 4U, 0U));
}

void TestRefusals(Asteria::TestSupport::TestContext& context) noexcept
{
    const BootDebugPort sentinel{0x1234U, 7U, 2U, 3U, 5U, 9U};
    struct Case
    {
        const char* Description;
        Table Bytes;
        int Corruption;
    };
    Case cases[]{
        {"a bad checksum", Dbgp(0U, Memory, 0x1000U), 1},
        {"a declared length beyond the bytes", Dbgp(0U, Memory, 0x1000U), 2},
        {"a DBGP interface type other than 16550", Dbgp(2U, Memory, 0x1000U), 0},
        {"an SPCR whose base address is zero (redirection disabled)", Spcr(2U, 0U, Io, 0U), 0},
        {"an SPCR naming a PL011", Spcr(2U, 3U, Memory, 0x1000U), 0},
        {"an SPCR 1 interface type other than 16550 or 16450", Spcr(1U, 2U, Io, 0x3F8U), 0},
        {"an I/O register file past port 0xFFFF", Spcr(1U, 0U, Io, 0xFFF9U), 0},
        {"a memory register file past the physical limit", Dbgp(0U, Memory, (UInt64{1U} << 50U) - 4U), 0},
        {"word register access", Spcr(2U, 0x12U, Memory, 0x1000U, 16U, 2U), 0},
        {"a register stride that is not a power of two bytes", Spcr(2U, 0x12U, Memory, 0x1000U, 24U, 1U), 0},
        {"PCI configuration space", Dbgp(0U, 2U, 0x1000U), 0},
        {"a DBG2 serial device of dword access", Dbg2(0x12U, 32U, 3U, 0xFE00'1000U), 0},
        {"a DBG2 PL011", Dbg2(0x03U, 32U, 1U, 0xFE00'1000U), 0},
    };
    for (Case& item : cases)
    {
        const UInt8* const bytes = item.Bytes.Seal();
        Size length = item.Bytes.Length();
        if (item.Corruption == 1)
        {
            ++item.Bytes.Bytes()[20U];
        }
        else if (item.Corruption == 2)
        {
            --length;
        }
        BootDebugPort port{sentinel};
        const bool accepted = Acpi::DecodeDbgpDebugPort(bytes, length, port) ||
                              Acpi::DecodeSpcrConsolePort(bytes, length, port) ||
                              Acpi::DecodeDbg2DebugPort(bytes, length, port);
        ASTERIA_EXPECT(context, item.Description, !accepted, std::memcmp(&port, &sentinel, sizeof(port)) == 0);
    }
    Table overcounted = Hcdp(0U, 0U, 0x1000U);
    overcounted.Put(36U, 3U, 4U);
    Table consoles = Hcdp(0U, 0U, 0x1000U);
    BootDebugPort port{sentinel};
    ASTERIA_EXPECT(context, "an HCDP count beyond its descriptors and a missing debug type are refused",
                   !Acpi::DecodeHcdpUart(overcounted.Seal(), overcounted.Length(), Acpi::HcdpUartType::Console, port),
                   !Acpi::DecodeHcdpUart(consoles.Seal(), consoles.Length(), Acpi::HcdpUartType::DebugPort, port),
                   std::memcmp(&port, &sentinel, sizeof(port)) == 0);
}

void TestPriority(Asteria::TestSupport::TestContext& context) noexcept
{
    Table dbg2 = Dbg2(0x0U, 8U, 0U, 0x2000U);
    Table dbgp = Dbgp(1U, Memory, 0x3000U);
    Table hcdpDebug = Hcdp(0U, 1U, 0x4000U);
    Table spcr = Spcr(1U, 0U, Io, 0x2F8U);
    Table hcdpConsole = Hcdp(0U, 0U, 0x5000U);
    const BootDebugPort fallback{
        0x47'F000'1000U, 0U, Memory, 1U, 1U, static_cast<UInt8>(BootDebugPortSource::PlatformDefault)};
    Acpi::DebugPortTableSet set{dbg2.View(), dbgp.View(), hcdpDebug.View(), spcr.View()};
    const UInt8 order[]{1U, 2U, 3U, 4U, 5U, 6U};
    for (const UInt8 expected : order)
    {
        const BootDebugPort port = Acpi::SelectDebugPort(set, fallback);
        ASTERIA_EXPECT(context, "dedicated debug ports precede the console, the platform default comes last",
                       port.Source == expected);
        switch (expected)
        {
        case 1U:
            set.Dbg2 = {};
            break;
        case 2U:
            set.Dbgp = {};
            break;
        case 3U:
            set.Hcdp = hcdpConsole.View();
            break;
        case 4U:
            set.Spcr = {};
            break;
        default:
            set.Hcdp = {};
            break;
        }
    }
}

// The tables in one page below 4 GiB, reached through an RSDP: ACPI 2.0
// with an XSDT, or 1.0 with an RSDT. The XSDT also lists an unrelated table.
void TestLocate(Asteria::TestSupport::TestContext& context) noexcept
{
    constexpr Size PageBytes{4096U};
    void* const memory =
        mmap(nullptr, PageBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (!ASTERIA_EXPECT(context, "a page below 4 GiB for the firmware tables", memory != MAP_FAILED))
    {
        return;
    }
    auto* const page = static_cast<UInt8*>(memory);
    const auto address = [page](const Size offset) noexcept
    {
        return static_cast<UInt64>(reinterpret_cast<Asteria::Foundation::UIntPointer>(page + offset));
    };
    Table spcr = Spcr(1U, 0U, Io, 0x3F8U);
    std::memcpy(page + 512U, spcr.Seal(), spcr.Length());
    Table dbgp = Dbgp(0U, Memory, 0x47'F000'1000U);
    std::memcpy(page + 1024U, dbgp.Seal(), dbgp.Length());
    Table other{"APIC", 44U, 1U};
    std::memcpy(page + 1536U, other.Seal(), other.Length());
    Table hcdp = Hcdp(0U, 1U, 0xF000'0000U);
    std::memcpy(page + 2048U, hcdp.Seal(), hcdp.Length());
    for (const bool extended : {true, false})
    {
        const Size entryBytes = extended ? 8U : 4U;
        Table root{extended ? "XSDT" : "RSDT", 36U + (3U * entryBytes), 1U};
        root.Put(36U, address(1536U), entryBytes);
        root.Put(36U + entryBytes, address(512U), entryBytes);
        root.Put(36U + (2U * entryBytes), address(1024U), entryBytes);
        std::memcpy(page + 64U, root.Seal(), root.Length());
        UInt8* const rsdp = page;
        std::memset(rsdp, 0, 36U);
        std::memcpy(rsdp, "RSD PTR ", 8U);
        rsdp[15U] = extended ? 2U : 0U;
        Store(rsdp + 16U, extended ? 0U : address(64U), 4U);
        Store(rsdp + 20U, 36U, 4U);
        Store(rsdp + 24U, extended ? address(64U) : 0U, 8U);
        UInt8 sum{0U};
        for (Size index{0U}; index < 20U; ++index)
        {
            sum = static_cast<UInt8>(sum + rsdp[index]);
        }
        rsdp[8U] = static_cast<UInt8>(0U - sum);
        sum = 0U;
        for (Size index{0U}; index < 36U; ++index)
        {
            sum = static_cast<UInt8>(sum + rsdp[index]);
        }
        rsdp[32U] = static_cast<UInt8>(0U - sum);
        const Acpi::DebugPortTableSet tables = Acpi::LocateDebugPortTables(rsdp, page + 2048U);
        ASTERIA_EXPECT(context, "the root table reaches SPCR and DBGP, the configuration table HCDP",
                       tables.Spcr.Bytes == page + 512U, tables.Spcr.Length == 80U, tables.Dbgp.Bytes == page + 1024U,
                       tables.Hcdp.Bytes == page + 2048U, tables.Dbg2.Bytes == nullptr,
                       Acpi::SelectDebugPort(tables, {}).Source == static_cast<UInt8>(BootDebugPortSource::Dbgp));
    }
    page[1024U + 20U] ^= 1U;
    const Acpi::DebugPortTableSet corrupt = Acpi::LocateDebugPortTables(page, nullptr);
    ASTERIA_EXPECT(context, "a table with a bad checksum is not located; no RSDP locates nothing",
                   corrupt.Dbgp.Bytes == nullptr, corrupt.Spcr.Bytes == page + 512U, corrupt.Hcdp.Bytes == nullptr,
                   Acpi::LocateDebugPortTables(nullptr, nullptr).Spcr.Bytes == nullptr);
    static_cast<void>(munmap(memory, PageBytes));
}
} // namespace

int main()
{
    Asteria::TestSupport::TestContext context{"DebugPortTables"};
    TestDecoders(context);
    TestRefusals(context);
    TestPriority(context);
    TestLocate(context);
    return context.Finish();
}
