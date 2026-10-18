// SPDX-License-Identifier: Apache-2.0
#include "Boot/Uefi/AmlBootReport.hpp"

#include <cstdlib>
#include <cstdio>
#include <memory>
#include <vector>

#include "Acpi/Aml/Fixture.hpp"
#include "Boot/Acpi/I8042Evidence.hpp"
#include "Support/TestContext.hpp"

namespace U = Asteria::Boot::Uefi;
namespace A = Asteria::Boot::Acpi;
namespace F = Asteria::Foundation;
namespace
{
F::Size g_allocations{0U};
F::Size g_failAllocations{0U};
U::Status ASTERIA_UEFI_API Allocate(U::AllocateType, U::MemoryType, F::Size pages, F::UInt64* address)
{
    ++g_allocations;
    if (g_allocations <= g_failAllocations)
    {
        return U::NotFound;
    }
    *address = reinterpret_cast<F::UInt64>(std::aligned_alloc(4096U, pages * 4096U));
    return *address != 0U ? U::Success : U::NotFound;
}
U::Status ASTERIA_UEFI_API Free(F::UInt64 address, F::Size)
{
    std::free(reinterpret_cast<void*>(address));
    return U::Success;
}
class Volume final : public U::IAcpiReportVolume
{
public:
    bool NoAml{false};
    bool NoFadt{false};
    bool Writable{false};
    U::FilePresence Exists(const char* name) noexcept override
    {
        return ((name[2U] == 'A' && NoAml) || (name[2U] == 'F' && NoFadt)) ? U::FilePresence::Present
                                                                           : U::FilePresence::Missing;
    }
    bool Start(const char*) noexcept override
    {
        return Writable;
    }
    bool Append(const char*, F::Size) noexcept override
    {
        return Writable;
    }
    bool Finish() noexcept override
    {
        return Writable;
    }
};
} // Namespace
namespace Asteria::Platform::Firmware
{
Foundation::UInt64 ReadCycleCounter() noexcept
{
    return 1000000U;
}
} // Namespace Asteria::Platform::Firmware
int main()
{
    Asteria::TestSupport::TestContext context{"Boot evidence independent of report IO"};
    std::vector<F::UInt8> table{};
    ASTERIA_EXPECT(context, "authored keyboard fixture", Asteria::TestSupport::Aml::Fixture("FloatingKeyboard", table));
    table[0U] = 'D';
    table[1U] = 'S';
    table[2U] = 'D';
    table[3U] = 'T';
    Asteria::TestSupport::Aml::Checksum(table);
    U::BootServices services{};
    services.AllocatePagesFunction = Allocate;
    services.FreePagesFunction = Free;
    static A::AcpiTopology topology{};
    for (F::Size kind{0U}; kind < 5U; ++kind)
    {
        topology = {};
        topology.IapcBootArchitecture = 2U;
        topology.Dsdt.PhysicalAddress = reinterpret_cast<F::UInt64>(table.data());
        topology.Dsdt.Length = static_cast<F::UInt32>(table.size());
        topology.Dsdt.ChecksumValid = 1U;
        Volume volume{};
        volume.Writable = kind == 0U;
        volume.NoAml = kind == 2U;
        volume.NoFadt = true;
        g_allocations = 0U;
        g_failAllocations = kind >= 3U ? kind - 2U : 0U;
        char output[1024U]{};
        Asteria::Inventory::ReportText text{output, sizeof(output)};
        U::ResetAmlBootEvidence();
        U::WriteAmlBootReport(volume, services, topology, text);
        const F::Size allocations = g_allocations;
        U::WriteAmlBootReport(volume, services, topology, text);
        U::ApplyAmlBootEvidence(topology);
        // KeyboardAml (2) with IRQ 1 declared edge, active-high (0x10).
        ASTERIA_EXPECT(context, "once per boot and diagnostic flag mask", allocations == g_allocations,
                       topology.IapcBootArchitecture == 0U, topology.Reserved == (kind < 2U ? 0x12U : 0U));
        if (kind < 2U)
        {
            ASTERIA_EXPECT(
                context, "binding survives sink refusal",
                A::I8042Evidence::Matches(topology, topology.Dsdt.PhysicalAddress, topology.Dsdt.Length, table[9U]));
        }
    }
    // ACPI 6.5 §20.2.3: authored Name(_S5, Package{5,6}); no hardware.
    constexpr F::UInt8 sleep[]{0x08U, 0x5CU, '_', 'S', '5', '_', 0x12U, 0x06U, 0x02U, 0x0AU, 5U, 0x0AU, 6U};
    constexpr F::UInt8 preparation[]{0x14U, 0x06U, '_', 'P', 'T', 'S', 1U};
    for (F::Size kind{0U}; kind < 6U; ++kind)
    {
        table.assign(36U, 0U);
        table[0U] = 'D';
        table[1U] = 'S';
        table[2U] = 'D';
        table[3U] = 'T';
        table[8U] = 2U;
        table.insert(table.end(), sleep, sleep + sizeof(sleep));
        if (kind == 3U)
        {
            table.push_back(0x08U);
        } // Truncated NameOp.
        if (kind == 4U)
        {
            table.insert(table.end(), preparation, preparation + sizeof(preparation));
        }
        for (F::Size byte{0U}; byte < 4U; ++byte)
        {
            table[4U + byte] = static_cast<F::UInt8>(table.size() >> (byte * 8U));
        }
        Asteria::TestSupport::Aml::Checksum(table);
        topology = {};
        topology.Dsdt = {.PhysicalAddress = reinterpret_cast<F::UInt64>(table.data()),
                         .Length = static_cast<F::UInt32>(table.size()),
                         .Signature = {'D', 'S', 'D', 'T'},
                         .OemTableId = {},
                         .Revision = 2U,
                         .ChecksumValid = 1U,
                         .Reserved = {}};
        Volume volume{};
        volume.NoAml = kind == 1U;
        g_allocations = 0U;
        g_failAllocations = kind == 2U ? 1U : 0U;
        char output[1024U]{};
        Asteria::Inventory::ReportText text{output, sizeof(output)};
        U::ResetAmlBootEvidence();
        U::WriteAmlBootReport(volume, services, topology, text);
        if (kind == 5U)
        {
            ++topology.Dsdt.Length;
        }
        A::PowerRegisters power{};
        U::ApplyAmlBootEvidence(topology, &power);
        if (power.SleepKnown != (kind == 0U ? 1U : 0U))
        {
            static_cast<void>(std::printf("S5 variant %zu: known=%u: %s\n", kind, power.SleepKnown, output));
        }
        ASTERIA_EXPECT(context, "S5 publication requires complete bound facts even when report IO fails",
                       power.SleepKnown == (kind == 0U ? 1U : 0U),
                       kind != 0U || (power.TypeA == 5U && power.TypeB == 6U));
    }
    return context.Finish();
}
