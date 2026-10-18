// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/AmlBootReport.hpp"

#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

#include "Acpi/Aml/Report.hpp"
#include "Boot/Acpi/I8042Evidence.hpp"
#include "Platform/Firmware/BootEntropySources.hpp"

namespace Asteria::Boot::Uefi
{
namespace
{
namespace Aml = Asteria::Acpi::Aml;
constexpr Foundation::Size PageBytes{4096U};
// This memory owner confines page/address conversion and object lifetime to
// firmware's identity-mapped, page-aligned LoaderData allocation. No destructor
// at firmware shutdown is needed: Release runs on every completed attempt.
class Pages final
{
public:
    explicit Pages(BootServices& services) noexcept : m_services{services}
    {
    }
    Pages(const Pages&) = delete;
    Pages& operator=(const Pages&) = delete;
    ~Pages() noexcept
    {
        static_cast<void>(Release());
    }
    [[nodiscard]] bool Allocate(const Foundation::Size bytes) noexcept
    {
        if (bytes == 0U || bytes > ~Foundation::Size{0U} - (PageBytes - 1U))
        {
            return false;
        }
        m_count = (bytes + PageBytes - 1U) / PageBytes;
        if (m_services.AllocatePagesFunction == nullptr || m_services.FreePagesFunction == nullptr)
        {
            return false;
        }
        m_owned = m_services.AllocatePagesFunction(AllocateType::AnyPages, MemoryType::LoaderData, m_count,
                                                   &m_address) == Success;
        if (m_owned && (m_address == 0U || m_address % PageBytes != 0U))
        {
            static_cast<void>(Release());
            return false;
        }
        return m_owned;
    }
    [[nodiscard]] void* Data() const noexcept
    {
        return reinterpret_cast<void*>(m_address);
    }
    [[nodiscard]] bool Release() noexcept
    {
        if (!m_owned)
        {
            return true;
        }
        m_owned = false;
        return m_services.FreePagesFunction(m_address, m_count) == Success;
    }

private:
    BootServices& m_services;
    Foundation::UInt64 m_address{0U};
    Foundation::Size m_count{0U};
    bool m_owned{false};
};

class FileSink final : public Aml::IReportSink
{
public:
    [[nodiscard]] bool Open(IAcpiReportVolume& volume) noexcept
    {
        m_volume = &volume;
        return volume.Start("ACPI.TXT");
    }
    [[nodiscard]] bool Write(const char* bytes, const Aml::Size length) noexcept override
    {
        m_failed = !Append(bytes, length);
        return !m_failed;
    }
    [[nodiscard]] bool Finish() noexcept
    {
        bool good{!m_failed && Drain()};
        good = m_volume->Finish() && good;
        m_volume = nullptr;
        return good;
    }

private:
    [[nodiscard]] bool Drain() noexcept
    {
        if (!m_volume->Append(m_bytes, m_used))
        {
            return false;
        }
        m_used = 0U;
        return true;
    }
    [[nodiscard]] bool Append(const char* bytes, const Foundation::Size length) noexcept
    {
        for (Foundation::Size i{0U}; i < length; ++i)
        {
            if (m_used == sizeof(m_bytes) && !Drain())
            {
                return false;
            }
            m_bytes[m_used++] = bytes[i];
        }
        return true;
    }
    bool m_failed{false};
    IAcpiReportVolume* m_volume{nullptr};
    Foundation::Size m_used{0U};
    char m_bytes[16384U]{};
};

template <Foundation::Size Nodes> struct Workspace
{
    Aml::BoundedArena<Nodes> Arena{};
    Aml::Library Library{Arena.Get()};
    Aml::Facts Facts{Library};
    Aml::ReportStorage Report{};
    FileSink Sink{};
};

[[nodiscard]] bool Load(Aml::Library& library, const Acpi::AcpiTableRecord& table) noexcept
{
    if (table.PhysicalAddress == 0U || table.Length < 36U || table.Length > Aml::MaximumTableBytes)
    {
        library.Diagnose(Aml::Reason::Missing);
        return false;
    }
    // Firmware ABI: parsed SDTs remain immutable, identity-addressable and firmware-owned until
    // ExitBootServices. Library::Load rechecks checksum.
    return library.Load({reinterpret_cast<const Foundation::UInt8*>(table.PhysicalAddress), table.Length});
}

template <Foundation::Size Nodes>
void Evaluate(void* memory, IAcpiReportVolume& volume, const Acpi::AcpiTopology& topology,
              Inventory::ReportText& outcome) noexcept
{
    auto* const workspace = ::new (memory) Workspace<Nodes>{};
    bool loaded{Load(workspace->Library, topology.Dsdt)};
    for (Foundation::Size i{0U}; i < topology.SsdtCount && i < Acpi::MaximumAcpiSsdts; ++i)
    {
        loaded = Load(workspace->Library, topology.Ssdts[i]) && loaded;
    }
    if (topology.SsdtOverflowCount != 0U)
    {
        workspace->Library.Diagnose(Aml::Reason::Capacity);
        loaded = false;
    }
    loaded = workspace->Library.FinishLoading() && loaded;
    bool saved{workspace->Sink.Open(volume)};
    const bool printed = Aml::Report::Print(workspace->Library, workspace->Facts, workspace->Report, workspace->Sink);
    saved = workspace->Sink.Finish() && printed && saved;
    // Partial AML loads or preparation methods prevent publishing S5 power facts.
    const Aml::SleepFacts sleep = workspace->Report.Summary.Sleep;
    if (loaded && !workspace->Library.CapacityFailed() && Nodes == Aml::MaximumNodes &&
        sleep.Error == Aml::Reason::None && workspace->Library.Find("\\_PTS") == Aml::NoId &&
        workspace->Library.Find("\\_TTS") == Aml::NoId && topology.Dsdt.ChecksumValid != 0U)
    {
        GetAmlBootEvidence().SleepBinding = topology.Dsdt;
        GetAmlBootEvidence().TypeA = sleep.TypeA;
        GetAmlBootEvidence().TypeB = sleep.TypeB;
        GetAmlBootEvidence().SleepKnown = true;
    }
    const bool arenaFailed = topology.SsdtOverflowCount != 0U || workspace->Library.CapacityFailed();
    // A reduced arena is a diagnostic fallback, never publication authority.
    if (!arenaFailed && Nodes == Aml::MaximumNodes &&
        (workspace->Report.Summary.I8042Flags & Abi::I8042KeyboardAml) != 0U && topology.Dsdt.ChecksumValid != 0U)
    {
        GetAmlBootEvidence().Address = topology.Dsdt.PhysicalAddress;
        GetAmlBootEvidence().Length = topology.Dsdt.Length;
        GetAmlBootEvidence().Flags = workspace->Report.Summary.I8042Flags;
        GetAmlBootEvidence().Identifier = workspace->Report.Summary.I8042KeyboardId;
        // Firmware ABI: Load has checked this bounded identity-addressable SDT.
        const Foundation::UInt8* const bytes =
            reinterpret_cast<const Foundation::UInt8*>(topology.Dsdt.PhysicalAddress);
        GetAmlBootEvidence().Checksum = bytes[9U];
    }
    outcome.Text(saved ? "AML saved ACPI.TXT" : "AML FAILED writing ACPI.TXT");
    if (!loaded)
    {
        outcome.Text(" partial-load");
    }
    outcome.Text(" i8042-rule=");
    const bool fadt = !GetAmlBootEvidence().MaskFadt && (topology.IapcBootArchitecture & 2U) != 0U;
    if (fadt)
    {
        outcome.Text("FADT-8042");
    }
    else
    {
        outcome.Text(GetAmlBootEvidence().Flags != 0U ? "AML-PNP" : "no-evidence");
    }
    if (!fadt && GetAmlBootEvidence().Flags != 0U)
    {
        outcome.Hex(GetAmlBootEvidence().Identifier, 4U);
        if ((GetAmlBootEvidence().Flags & Abi::I8042StaUnknown) != 0U)
        {
            outcome.Text("-STA-UNKNOWN");
        }
    }
    outcome.Text(" (kernel status gate pending)");
    outcome.Text(" devices=");
    outcome.Decimal(workspace->Report.Summary.Devices);
    outcome.Text(" ps2-candidates=");
    outcome.Decimal(workspace->Report.Summary.Ps2Candidates);
    outcome.Text(" prt-entries=");
    outcome.Decimal(workspace->Report.Summary.PrtEntries);
    outcome.Text(" diagnostics=");
    outcome.Decimal(workspace->Library.DiagnosticCount());
    outcome.Text(" nodes=");
    outcome.Decimal(Nodes);
    outcome.Text(" arena-bytes=");
    outcome.Decimal(sizeof(workspace->Arena));
    workspace->~Workspace<Nodes>();
}
} // Namespace

void WriteAmlBootReport(IAcpiReportVolume& volume, BootServices& services, const Acpi::AcpiTopology& topology,
                        Inventory::ReportText& outcome) noexcept
{
    if (GetAmlBootEvidence().Attempted)
    {
        return;
    }
    GetAmlBootEvidence().Attempted = true;
    GetAmlBootEvidence().MaskFadt = volume.Exists("NOFADT8042") == FilePresence::Present;
    const FilePresence marker{volume.Exists("NOAML")};
    if (marker == FilePresence::Present)
    {
        outcome.Text("AML skipped: NOAML\r\n");
        return;
    }
    if (marker == FilePresence::Failed)
    {
        outcome.Text("AML skipped: NOAML lookup FAILED\r\n");
        return;
    }
    if (topology.Dsdt.PhysicalAddress == 0U || topology.Dsdt.Length < 36U)
    {
        outcome.Text("AML skipped: DSDT unavailable\r\n");
        return;
    }
    // Calibrate against the firmware's 1 ms Stall so even CPUs without a known
    // counter frequency have readable elapsed microseconds. Counter differences
    // intentionally use unsigned wraparound; no step can span a full 64-bit wrap.
    const Foundation::UInt64 calibrationStart{Platform::Firmware::ReadCycleCounter()};
    const bool calibrated{services.StallFunction != nullptr && services.StallFunction(1000U) == Success};
    const Foundation::UInt64 calibrationTicks{Platform::Firmware::ReadCycleCounter() - calibrationStart};
    const Foundation::UInt64 ticksPerMicrosecond{calibrated ? calibrationTicks / 1000U : 0U};
    const Foundation::UInt64 start{Platform::Firmware::ReadCycleCounter()};
    Pages pages{services};
    if (pages.Allocate(sizeof(Workspace<Aml::MaximumNodes>)))
    {
        Evaluate<Aml::MaximumNodes>(pages.Data(), volume, topology, outcome);
    }
    else if (pages.Allocate(sizeof(Workspace<Aml::ReducedNodes>)))
    {
        Evaluate<Aml::ReducedNodes>(pages.Data(), volume, topology, outcome);
        outcome.Text(" reduced-arena");
    }
    else
    {
        outcome.Text("AML skipped: AllocatePages FAILED (full and reduced arena)");
    }
    if (!pages.Release())
    {
        outcome.Text(" FreePages FAILED");
    }
    const Foundation::UInt64 elapsed{Platform::Firmware::ReadCycleCounter() - start};
#if defined(ASTERIA_TARGET_IA64)
    outcome.Text(" counter=ITC");
#else
    outcome.Text(" counter=TSC");
#endif
    outcome.Text(" elapsed-ticks=");
    outcome.Decimal(elapsed);
    outcome.Text(" elapsed-us=");
    if (ticksPerMicrosecond != 0U)
    {
        outcome.Decimal(elapsed / ticksPerMicrosecond);
    }
    else
    {
        outcome.Text("unavailable");
    }
    outcome.Text("\r\n");
}
} // Namespace Asteria::Boot::Uefi
