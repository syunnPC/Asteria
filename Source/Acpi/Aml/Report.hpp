// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ACPI_AML_REPORT_HPP_INCLUDED
#define ASTERIA_ACPI_AML_REPORT_HPP_INCLUDED

#include "Acpi/Aml/Facts.hpp"

namespace Asteria::Acpi::Aml
{
struct ReportSummary
{
    Size Devices{0U};
    Size Ps2Candidates{0U};
    Size PrtEntries{0U};
    /// Abi/I8042.hpp declared-device and IRQ evidence bits (I8042AmlEvidenceMask).
    UInt32 I8042Flags{0U};
    UInt32 I8042KeyboardId{0U};
    SleepFacts Sleep{};
};
struct ReportStorage
{
    ReportSummary Summary{};
    DeviceFacts Device{};
    char Path[MaximumPathBytes]{};
};
/// Synchronous, allocation-free streaming destination. Write borrows bytes
/// only for the call and returns false on failure. A sink may block (firmware IO).
class IReportSink
{
public:
    [[nodiscard]] virtual bool Write(const char* bytes, Size length) noexcept = 0;

protected:
    ~IReportSink() noexcept = default;
};
/// Sink failures stop output; the facts walk and evidence derivation still complete. Output is deterministic UTF-8-compatible
/// ASCII, with byte strings escaped; no temporary allocation or libc format
/// routines. This versioned format is shared by AmlFacts and the boot ACPI.TXT writer. Reporting evaluates facts and mutates the model.
class Report final
{
public:
    [[nodiscard]] static bool Print(Library& library, Facts& facts, ReportStorage& storage, IReportSink& sink) noexcept;

private:
    Report(Library& library, Facts& facts, ReportStorage& storage, IReportSink& sink) noexcept;
    void Text(const char* text) noexcept;
    void Number(UInt64 number, bool hex = false) noexcept;
    void Quoted(Bytes bytes) noexcept;
    void Path(Id node) noexcept;
    void Value(EvaluationResult value) noexcept;
    void Ids(const Identifiers& ids) noexcept;
    void Device(const DeviceFacts& device) noexcept;
    void ResourceLine(const Resource& resource) noexcept;
    void Unknown(Reason reason) noexcept;
    Library& m_library;
    Facts& m_facts;
    ReportStorage& m_storage;
    IReportSink& m_sink;
    bool m_good{true};
};
} // namespace Asteria::Acpi::Aml

#endif
