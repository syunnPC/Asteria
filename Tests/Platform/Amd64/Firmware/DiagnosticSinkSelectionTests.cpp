// SPDX-License-Identifier: Apache-2.0
#include <cstring>
#include <sys/mman.h>

#include "Platform/Amd64/Firmware/DiagnosticSinkSelection.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using Platform::Amd64::Firmware::FindSpcrSerialPort;
using Platform::Amd64::Firmware::IsQemuDebugPortSignature;
using Platform::Amd64::Firmware::SpcrSerialPort;
using Platform::Amd64::Firmware::SpcrSerialStatus;

constexpr Size PageBytes{4096U};
constexpr Size RsdpOffset{0U};
constexpr Size RootOffset{64U};
constexpr Size OtherOffset{256U};
constexpr Size SpcrOffset{512U};

// The vendor signature registers of CPUID leaf 0x40000000 for text.
void Registers(const char* const text, UInt32& ebx, UInt32& ecx, UInt32& edx) noexcept
{
    UInt32 words[3U]{};
    std::memcpy(words, text, 12U);
    ebx = words[0U];
    ecx = words[1U];
    edx = words[2U];
}

[[nodiscard]] bool IsQemu(const char* const text) noexcept
{
    UInt32 ebx{0U};
    UInt32 ecx{0U};
    UInt32 edx{0U};
    Registers(text, ebx, ecx, edx);
    return IsQemuDebugPortSignature(ebx, ecx, edx);
}

void TestSignatures(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "only QEMU's TCG and KVM signatures have the debug port", IsQemu("TCGTCGTCGTCG"),
                   IsQemu("KVMKVMKVM\0\0\0"), !IsQemu("VMwareVMware"), !IsQemu("Microsoft Hv"), !IsQemu("TCGTCGTCGTCH"),
                   !IsQemu("KVMKVMKVMKVM"), !IsQemu("\0\0\0\0\0\0\0\0\0\0\0\0"));
}

void Store(UInt8* const bytes, const UInt64 value, const Size count) noexcept
{
    for (Size index{0U}; index < count; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

void Checksum(UInt8* const bytes, const Size count, const Size offset) noexcept
{
    bytes[offset] = 0U;
    UInt8 sum{0U};
    for (Size index{0U}; index < count; ++index)
    {
        sum = static_cast<UInt8>(sum + bytes[index]);
    }
    bytes[offset] = static_cast<UInt8>(0U - sum);
}

void Header(UInt8* const table, const char* const signature, const Size length) noexcept
{
    std::memcpy(table, signature, 4U);
    Store(table + 4U, length, 4U);
    table[8U] = 2U;
}

// One page of firmware tables below 4 GiB: an RSDP (ACPI 2.0 with an XSDT,
// or 1.0 with an RSDT), a root table naming another table and then an SPCR
// of the given interface type and generic address, all checksummed.
class Tables final
{
public:
    Tables() noexcept
    {
        void* const memory =
            mmap(nullptr, PageBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        m_bytes = (memory == MAP_FAILED) ? nullptr : static_cast<UInt8*>(memory);
    }

    ~Tables() noexcept
    {
        if (m_bytes != nullptr)
        {
            static_cast<void>(munmap(m_bytes, PageBytes));
        }
    }

    Tables(const Tables&) = delete;
    Tables& operator=(const Tables&) = delete;

    [[nodiscard]] bool IsReady() const noexcept
    {
        return m_bytes != nullptr;
    }

    [[nodiscard]] const void* Build(const bool extended, const bool withSpcr, const UInt8 interfaceType,
                                    const UInt8 space, const UInt64 address) noexcept
    {
        std::memset(m_bytes, 0, PageBytes);
        UInt8* const other = m_bytes + OtherOffset;
        Header(other, "APIC", 44U);
        Checksum(other, 44U, 9U);
        UInt8* const spcr = m_bytes + SpcrOffset;
        Header(spcr, "SPCR", 80U);
        spcr[36U] = interfaceType;
        spcr[40U] = space;
        spcr[41U] = 8U;
        spcr[43U] = 1U;
        Store(spcr + 44U, address, 8U);
        spcr[58U] = 7U;
        Checksum(spcr, 80U, 9U);
        const Size entryBytes = extended ? 8U : 4U;
        const Size entries = withSpcr ? 2U : 1U;
        UInt8* const root = m_bytes + RootOffset;
        Header(root, extended ? "XSDT" : "RSDT", 36U + (entries * entryBytes));
        Store(root + 36U, Address(OtherOffset), entryBytes);
        if (withSpcr)
        {
            Store(root + 36U + entryBytes, Address(SpcrOffset), entryBytes);
        }
        Checksum(root, 36U + (entries * entryBytes), 9U);
        UInt8* const rsdp = m_bytes + RsdpOffset;
        std::memcpy(rsdp, "RSD PTR ", 8U);
        rsdp[15U] = extended ? 2U : 0U;
        Store(rsdp + 16U, extended ? 0U : Address(RootOffset), 4U);
        if (extended)
        {
            Store(rsdp + 20U, 36U, 4U);
            Store(rsdp + 24U, Address(RootOffset), 8U);
        }
        Checksum(rsdp, 20U, 8U);
        if (extended)
        {
            Checksum(rsdp, 36U, 32U);
        }
        return rsdp;
    }

    [[nodiscard]] UInt8* Bytes() noexcept
    {
        return m_bytes;
    }

private:
    [[nodiscard]] UInt64 Address(const Size offset) const noexcept
    {
        return static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(m_bytes + offset));
    }

    UInt8* m_bytes{nullptr};
};

[[nodiscard]] bool Is(const SpcrSerialPort result, const SpcrSerialStatus status, const UInt16 port) noexcept
{
    return (result.Status == status) && (result.Port == port);
}

void TestSpcr(TestContext& runner) noexcept
{
    Tables tables;
    ASTERIA_EXPECT(runner, "the table page maps below 4 GiB", tables.IsReady());
    if (!tables.IsReady())
    {
        return;
    }
    ASTERIA_EXPECT(
        runner, "the SPCR's 16550 in I/O space is found through the XSDT or the RSDT",
        Is(FindSpcrSerialPort(tables.Build(true, true, 0x00U, 1U, 0x3F8U)), SpcrSerialStatus::Found, 0x3F8U),
        Is(FindSpcrSerialPort(tables.Build(false, true, 0x01U, 1U, 0x2F8U)), SpcrSerialStatus::Found, 0x2F8U),
        Is(FindSpcrSerialPort(tables.Build(true, true, 0x12U, 1U, 0x3E8U)), SpcrSerialStatus::Found, 0x3E8U));
    ASTERIA_EXPECT(
        runner, "a memory-mapped or foreign UART, or an unusable port, is not driven",
        Is(FindSpcrSerialPort(tables.Build(true, true, 0x00U, 0U, 0xFED0'0000U)), SpcrSerialStatus::MemoryMapped, 0U),
        Is(FindSpcrSerialPort(tables.Build(true, true, 0x03U, 0U, 0x0900'0000U)), SpcrSerialStatus::Unsupported, 0U),
        Is(FindSpcrSerialPort(tables.Build(true, true, 0x00U, 1U, 0xFFF9U)), SpcrSerialStatus::Unsupported, 0U),
        Is(FindSpcrSerialPort(tables.Build(true, true, 0x00U, 1U, 0U)), SpcrSerialStatus::Unsupported, 0U));

    const void* const corruptSpcr = tables.Build(true, true, 0x00U, 1U, 0x3F8U);
    ++tables.Bytes()[SpcrOffset + 44U];
    const SpcrSerialPort spcrChecksum = FindSpcrSerialPort(corruptSpcr);
    const void* const corruptRoot = tables.Build(true, true, 0x00U, 1U, 0x3F8U);
    ++tables.Bytes()[RootOffset + 10U];
    const SpcrSerialPort rootChecksum = FindSpcrSerialPort(corruptRoot);
    const void* const corruptRsdp = tables.Build(true, true, 0x00U, 1U, 0x3F8U);
    ++tables.Bytes()[RsdpOffset + 9U];
    const SpcrSerialPort rsdpChecksum = FindSpcrSerialPort(corruptRsdp);
    ASTERIA_EXPECT(runner, "no SPCR, a missing RSDP or a corrupt table finds no UART",
                   Is(FindSpcrSerialPort(tables.Build(true, false, 0x00U, 1U, 0x3F8U)), SpcrSerialStatus::Absent, 0U),
                   Is(FindSpcrSerialPort(nullptr), SpcrSerialStatus::Absent, 0U),
                   Is(spcrChecksum, SpcrSerialStatus::Absent, 0U), Is(rootChecksum, SpcrSerialStatus::Absent, 0U),
                   Is(rsdpChecksum, SpcrSerialStatus::Absent, 0U));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"DiagnosticSinkSelectionTests"};
    Asteria::Test::TestSignatures(runner);
    Asteria::Test::TestSpcr(runner);
    return runner.Finish();
}
