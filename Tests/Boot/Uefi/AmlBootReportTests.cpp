// SPDX-License-Identifier: Apache-2.0
#include "Boot/Uefi/AmlBootReport.hpp"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "Acpi/Aml/Report.hpp"
#include "Support/TestContext.hpp"

namespace
{
namespace Uefi = Asteria::Boot::Uefi;
namespace Aml = Asteria::Acpi::Aml;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt16;
using Asteria::Foundation::UInt64;
constinit Uefi::File g_file{};
constinit Uefi::File g_directory{};
constinit Uefi::BootServices g_services{};
constinit Asteria::Boot::Acpi::AcpiTopology g_topology{};
constinit UInt8 g_table[36U]{'D', 'S', 'D', 'T', 36U};
std::string g_output{};
std::string g_expected{};
void* g_pages{nullptr};
Size g_allocations{0U};
Size g_frees{0U};
Size g_failAllocations{0U};
Size g_flushes{0U};
Size g_closes{0U};
bool g_marker{false};
bool g_failWrite{false};
bool g_exists{false};
bool g_failFlush{false};

Uefi::Status ASTERIA_UEFI_API Allocate(Uefi::AllocateType, Uefi::MemoryType type, Size pages, UInt64* address)
{
    ++g_allocations;
    if (g_allocations <= g_failAllocations || type != Uefi::MemoryType::LoaderData)
    {
        return Uefi::NotFound;
    }
    g_pages = std::aligned_alloc(4096U, pages * 4096U);
    *address = reinterpret_cast<UInt64>(g_pages);
    return g_pages == nullptr ? Uefi::NotFound : Uefi::Success;
}
Uefi::Status ASTERIA_UEFI_API Free(UInt64 address, Size)
{
    ++g_frees;
    if (reinterpret_cast<void*>(address) != g_pages)
    {
        return Uefi::NotFound;
    }
    std::free(g_pages);
    g_pages = nullptr;
    return Uefi::Success;
}
Uefi::Status ASTERIA_UEFI_API Close(Uefi::File*)
{
    ++g_closes;
    return Uefi::Success;
}
Uefi::Status ASTERIA_UEFI_API Delete(Uefi::File*)
{
    g_exists = false;
    g_output.clear();
    return Uefi::Success;
}
Uefi::Status ASTERIA_UEFI_API Flush(Uefi::File*)
{
    ++g_flushes;
    return g_failFlush ? Uefi::NotFound : Uefi::Success;
}
Uefi::Status ASTERIA_UEFI_API Write(Uefi::File*, Size* length, const void* bytes)
{
    if (g_failWrite)
    {
        *length = 0U;
        return Uefi::Success;
    }
    // Force short writes, including across streaming buffer boundaries.
    if (*length > 7U)
    {
        *length = 7U;
    }
    g_output.append(static_cast<const char*>(bytes), *length);
    return Uefi::Success;
}
Uefi::Status ASTERIA_UEFI_API Open(Uefi::File*, Uefi::File** file, UInt16* name, UInt64 mode, UInt64)
{
    *file = nullptr;
    if (name[0U] == 'N')
    {
        *file = g_marker ? &g_file : nullptr;
        return g_marker ? Uefi::Success : Uefi::NotFound;
    }
    if (mode == 3U && !g_exists)
    {
        return Uefi::NotFound;
    }
    g_exists = true;
    *file = &g_file;
    return Uefi::Success;
}
Uefi::Status ASTERIA_UEFI_API Stall(Size)
{
    return Uefi::Success;
}
class ExpectedSink final : public Aml::IReportSink
{
public:
    bool Write(const char* bytes, const Size length) noexcept override
    {
        g_expected.append(bytes, length);
        return true;
    }
};

template <Size Nodes> void Expected() noexcept
{
    const auto arena{std::make_unique<Aml::BoundedArena<Nodes>>()};
    const auto storage{std::make_unique<Aml::ReportStorage>()};
    Aml::Library library{arena->Get()};
    static_cast<void>(library.Load({g_table, sizeof(g_table)}));
    static_cast<void>(library.FinishLoading());
    Aml::Facts facts{library};
    ExpectedSink sink{};
    static_cast<void>(Aml::Report::Print(library, facts, *storage, sink));
}
void Reset() noexcept
{
    g_allocations = 0U;
    g_frees = 0U;
    g_flushes = 0U;
    g_closes = 0U;
    g_failAllocations = 0U;
    g_marker = false;
    g_failWrite = false;
    g_failFlush = false;
    g_exists = false;
    g_output.clear();
    g_expected.clear();
}
void Run(Asteria::TestSupport::TestContext& context, Size failedAllocations) noexcept
{
    Reset();
    g_failAllocations = failedAllocations;
    char outcome[320U]{};
    Asteria::Inventory::ReportText text{outcome, sizeof(outcome)};
    Uefi::FirmwareReportVolume volume{g_directory};
    Uefi::ResetAmlBootEvidence();
    Uefi::WriteAmlBootReport(volume, g_services, g_topology, text);
    if (failedAllocations < 2U)
    {
        if (failedAllocations == 0U)
        {
            Expected<Aml::MaximumNodes>();
        }
        else
        {
            Expected<Aml::ReducedNodes>();
        }
        ASTERIA_EXPECT(context, "boot formatter matches host and releases pages after flushing", g_output == g_expected,
                       g_allocations == failedAllocations + 1U, g_frees == 1U, g_flushes == 1U, g_closes == 1U,
                       g_pages == nullptr, std::strstr(outcome, "devices=0") != nullptr,
                       std::strstr(outcome, "elapsed-us=") != nullptr);
    }
    else
    {
        ASTERIA_EXPECT(context, "allocation refusal skips output and remains recoverable", g_output.empty(),
                       g_allocations == 2U, g_frees == 0U, std::strstr(outcome, "AllocatePages FAILED") != nullptr);
    }
}
void Failures(Asteria::TestSupport::TestContext& context) noexcept
{
    for (Size kind{0U}; kind < 3U; ++kind)
    {
        Reset();
        g_marker = kind == 0U;
        g_failWrite = kind == 1U;
        g_failFlush = kind == 2U;
        char outcome[320U]{};
        Asteria::Inventory::ReportText text{outcome, sizeof(outcome)};
        Uefi::FirmwareReportVolume volume{g_directory};
        Uefi::ResetAmlBootEvidence();
        Uefi::WriteAmlBootReport(volume, g_services, g_topology, text);
        if (g_marker)
        {
            ASTERIA_EXPECT(context, "NOAML precedes arena allocation", g_allocations == 0U, g_output.empty(),
                           std::strstr(outcome, "AML skipped: NOAML") != nullptr);
        }
        else
        {
            ASTERIA_EXPECT(context, "write/flush failure closes file and releases arena", g_frees == 1U, g_closes == 1U,
                           std::strstr(outcome, "AML FAILED writing ACPI.TXT") != nullptr);
        }
    }
}
} // namespace
namespace Asteria::Platform::Firmware
{
Foundation::UInt64 ReadCycleCounter() noexcept
{
    return 1000000U;
}
} // namespace Asteria::Platform::Firmware
int main()
{
    Asteria::TestSupport::TestContext context{"Boot AML export and recovery"};
    g_file.Close = Close;
    g_file.Delete = Delete;
    g_file.Write = Write;
    g_file.Flush = Flush;
    g_directory.Open = Open;
    g_services.AllocatePagesFunction = Allocate;
    g_services.FreePagesFunction = Free;
    g_services.StallFunction = Stall;
    g_table[8U] = 2U;
    UInt8 sum{0U};
    for (const UInt8 byte : g_table)
    {
        sum = static_cast<UInt8>(sum + byte);
    }
    g_table[9U] = static_cast<UInt8>(0U - sum);
    g_topology.Dsdt.PhysicalAddress = reinterpret_cast<UInt64>(g_table);
    g_topology.Dsdt.Length = sizeof(g_table);
    Run(context, 0U);
    Run(context, 1U);
    Run(context, 2U);
    Failures(context);
    return context.Finish();
}
