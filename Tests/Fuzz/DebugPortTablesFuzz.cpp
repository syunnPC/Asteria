// SPDX-License-Identifier: Apache-2.0
#include "Boot/Acpi/DebugPortTables.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace
{
namespace Acpi = Asteria::Boot::Acpi;
using Asteria::Boot::BootDebugPort;
using Asteria::Boot::BootDebugPortSource;

void Check(const bool valid) noexcept
{
    if (!valid)
    {
        std::abort();
    }
}

// Properties of one decoder over one input: deterministic, atomic on
// failure, and only ever a valid port of the decoder's own source.
template <typename TDecode>
void Decode(const TDecode decode, const std::uint8_t* const data, const std::size_t size,
            const std::uint8_t source) noexcept
{
    const BootDebugPort sentinel{0x5A5A'5A5A'5A5A'5A5AU, 0xA5A5'A5A5U, 0xA5U, 0x5AU, 0xA5U, 0x5AU};
    BootDebugPort first{sentinel};
    BootDebugPort second{sentinel};
    const bool accepted = decode(data, size, first);
    Check(accepted == decode(data, size, second));
    Check(std::memcmp(&first, &second, sizeof(first)) == 0);
    if (!accepted)
    {
        Check(std::memcmp(&first, &sentinel, sizeof(first)) == 0);
        return;
    }
    Check(Asteria::Boot::IsValidBootDebugPort(first) && (first.Source == source));
}

void DecodeAll(const std::uint8_t* const data, const std::size_t size) noexcept
{
    Decode(Acpi::DecodeDbg2DebugPort, data, size, static_cast<std::uint8_t>(BootDebugPortSource::Dbg2));
    Decode(Acpi::DecodeDbgpDebugPort, data, size, static_cast<std::uint8_t>(BootDebugPortSource::Dbgp));
    Decode(Acpi::DecodeSpcrConsolePort, data, size, static_cast<std::uint8_t>(BootDebugPortSource::Spcr));
    Decode(
        [](const std::uint8_t* const bytes, const std::size_t length, BootDebugPort& port) noexcept
        {
            return Acpi::DecodeHcdpUart(bytes, length, Acpi::HcdpUartType::Console, port);
        },
        data, size, static_cast<std::uint8_t>(BootDebugPortSource::HcdpConsole));
    Decode(
        [](const std::uint8_t* const bytes, const std::size_t length, BootDebugPort& port) noexcept
        {
            return Acpi::DecodeHcdpUart(bytes, length, Acpi::HcdpUartType::DebugPort, port);
        },
        data, size, static_cast<std::uint8_t>(BootDebugPortSource::HcdpDebugPort));
    // Selection over the input as every table never yields an invalid port.
    const Acpi::DebugPortTableView view{data, size};
    const BootDebugPort selected = Acpi::SelectDebugPort({view, view, view, view}, {});
    Check(Asteria::Boot::IsValidBootDebugPort(selected));
}

// Gives the input each signature with a matching length and checksum, so
// mutations reach the fields past the header checks.
void DecodeRepaired(const std::uint8_t* const data, const std::size_t size) noexcept
{
    if ((size < 36U) || (size > 4096U))
    {
        return;
    }
    for (const char* const signature : {"DBG2", "DBGP", "SPCR", "HCDP"})
    {
        std::uint8_t repaired[4096U]{};
        std::memcpy(repaired, data, size);
        std::memcpy(repaired, signature, 4U);
        for (std::size_t index{0U}; index < 4U; ++index)
        {
            repaired[4U + index] = static_cast<std::uint8_t>(size >> (index * 8U));
        }
        repaired[9U] = 0U;
        std::uint8_t sum{0U};
        for (std::size_t index{0U}; index < size; ++index)
        {
            sum = static_cast<std::uint8_t>(sum + repaired[index]);
        }
        repaired[9U] = static_cast<std::uint8_t>(0U - sum);
        DecodeAll(repaired, size);
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) noexcept
{
    DecodeAll(data, size);
    DecodeRepaired(data, size);
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    // A DBGP/SPCR-shaped body: 16550 interface, COM1 in I/O space, 115200.
    std::uint8_t legacy[88U]{};
    legacy[8U] = 4U;
    legacy[40U] = 1U;
    legacy[41U] = 8U;
    legacy[43U] = 1U;
    legacy[44U] = 0xF8U;
    legacy[45U] = 0x03U;
    legacy[58U] = 7U;
    emit(legacy, sizeof(legacy));
    // A DBG2-shaped body: one serial 0x12 device with a 32-bit stride.
    std::uint8_t dbg2[80U]{};
    dbg2[36U] = 44U;
    dbg2[40U] = 1U;
    dbg2[45U] = 36U;
    dbg2[47U] = 1U;
    dbg2[57U] = 0x80U;
    dbg2[58U] = 0x12U;
    dbg2[62U] = 22U;
    dbg2[67U] = 32U;
    dbg2[69U] = 1U;
    dbg2[72U] = 0x10U;
    dbg2[74U] = 0xFEU;
    emit(dbg2, sizeof(dbg2));
    // An HCDP-shaped body: one debug-port descriptor in memory space.
    std::uint8_t hcdp[88U]{};
    hcdp[36U] = 1U;
    hcdp[40U] = 1U;
    hcdp[57U] = 8U;
    hcdp[62U] = 0x10U;
    hcdp[63U] = 0xF0U;
    emit(hcdp, sizeof(hcdp));
    emit(legacy, 0U);
}
