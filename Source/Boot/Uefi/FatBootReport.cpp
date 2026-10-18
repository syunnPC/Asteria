// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/FatBootReport.hpp"

#include "Boot/Uefi/AmlBootReport.hpp"

namespace Asteria::Boot::Uefi
{
namespace
{
class Dump final : public Acpi::IAcpiTableVisitor
{
public:
    explicit Dump(IAcpiReportVolume& volume) noexcept : m_volume{volume}
    {
    }
    void Visit(const Acpi::AcpiTableRecord& record, const Foundation::UInt8* bytes) noexcept override
    {
        if (record.ChecksumValid == 0U)
        {
            return;
        }
        Foundation::Size slot{0U};
        for (; slot < m_signatures; ++slot)
        {
            bool same{true};
            for (Foundation::Size i{0U}; i < 4U; ++i)
            {
                same = same && m_counts[slot].Signature[i] == record.Signature[i];
            }
            if (same)
            {
                break;
            }
        }
        if (Counting)
        {
            if (slot == m_signatures)
            {
                if (m_signatures == MaximumSignatures)
                {
                    ++Failed;
                    return;
                }
                for (Foundation::Size i{0U}; i < 4U; ++i)
                {
                    m_counts[slot].Signature[i] = record.Signature[i];
                }
                ++m_signatures;
            }
            ++m_counts[slot].Count;
            return;
        }
        if (slot == m_signatures)
        {
            ++Failed;
            return;
        }
        Foundation::UInt16 words[AcpiDumpNameCharacters]{};
        auto& count{m_counts[slot]};
        ++count.Written;
        if (!AcpiDumpName(record.Signature, count.Count > 1U ? count.Written : 0U, words))
        {
            ++Failed;
            return;
        }
        char name[18U]{'A', 'C', 'P', 'I', '/'};
        for (Foundation::Size i{0U}; i < AcpiDumpNameCharacters; ++i)
        {
            name[i + 5U] = static_cast<char>(words[i]);
            if (words[i] == 0U)
            {
                break;
            }
        }
        bool saved{m_volume.Start(name)};
        if (saved)
        {
            saved = m_volume.Append(reinterpret_cast<const char*>(bytes), record.Length);
            saved = m_volume.Finish() && saved;
        }
        if (saved)
        {
            ++Saved;
        }
        else
        {
            ++Failed;
        }
    }
    bool Counting{true};
    Foundation::Size Saved{0U};
    Foundation::Size Failed{0U};

private:
    static constexpr Foundation::Size MaximumSignatures{Acpi::MaximumAcpiRootTableEntries + 2U};
    struct Count
    {
        char Signature[4U]{};
        Foundation::UInt16 Count{0U};
        Foundation::UInt16 Written{0U};
    };
    IAcpiReportVolume& m_volume;
    Foundation::Size m_signatures{0U};
    Count m_counts[MaximumSignatures]{};
};
constexpr Foundation::Size DumpPages{3U};
static_assert(sizeof(Dump) <= DumpPages * 4096U);
}
}

// The second page-backed owner holds signature counts, never table bytes.
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Boot::Uefi
{
bool WriteFatBootReport(Handle device, BootServices& services, const BootInfo& snapshot, char* report,
                        const Foundation::Size capacity, Foundation::Size& bytes) noexcept
{
    FatReportVolume owner{services};
    IAcpiReportVolume* const volume{owner.Open(device)};
    if (volume == nullptr)
    {
        return false;
    }
    Inventory::ReportText prefix{report + bytes, capacity - bytes};
    prefix.Text("Report volume: FAT32 Block I/O fallback\r\n");
    bytes += prefix.Bytes();
    bool saved{volume->Start("REPORT.TXT")};
    if (saved)
    {
        saved = volume->Append(report, bytes);
        saved = volume->Finish() && saved;
    }
    Foundation::UInt64 address{0U};
    Inventory::ReportText outcome{report + bytes, capacity - bytes};
    if (services.AllocatePagesFunction(AllocateType::AnyPages, MemoryType::LoaderData, DumpPages, &address) == Success)
    {
        auto* const dump = ::new (reinterpret_cast<void*>(address)) Dump{*volume};
        Acpi::AcpiTableParser::VisitTables(snapshot, snapshot.Acpi, *dump);
        dump->Counting = false;
        Acpi::AcpiTableParser::VisitTables(snapshot, snapshot.Acpi, *dump);
        outcome.Text("ACPI dump saved=");
        outcome.Decimal(dump->Saved);
        outcome.Text(" failed=");
        outcome.Decimal(dump->Failed);
        outcome.Text("\r\n");
        dump->~Dump();
        if (services.FreePagesFunction(address, DumpPages) != Success)
        {
            outcome.Text("ACPI dump FreePages FAILED\r\n");
        }
    }
    else
    {
        outcome.Text("ACPI dump FAILED: AllocatePages\r\n");
    }
    WriteAmlBootReport(*volume, services, snapshot.Acpi, outcome);
    bytes += outcome.Bytes();
    saved = volume->Start("REPORT.TXT");
    if (saved)
    {
        saved = volume->Append(report, bytes);
        saved = volume->Finish() && saved;
    }
    return owner.Release() && saved;
}
}
