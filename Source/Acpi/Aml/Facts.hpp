// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ACPI_AML_FACTS_HPP_INCLUDED
#define ASTERIA_ACPI_AML_FACTS_HPP_INCLUDED

#include "Acpi/Aml/Library.hpp"
#include "Acpi/Aml/Resources.hpp"

namespace Asteria::Acpi::Aml
{
enum class Presence : UInt8
{
    Present,
    Absent,
    Unknown,
};
struct Identifier
{
    Id Value{NoId};
    char Eisa[8U]{};
};
struct Identifiers
{
    Identifier Items[MaximumIds]{};
    Size Count{0U};
    Reason Error{Reason::Missing};
};
struct PrtEntry
{
    UInt64 Address{0U};
    UInt32 Pin{0U};
    UInt32 Index{0U};
    Id Source{NoId};
    Id SourceNode{NoId};
    bool Direct{false};
};
struct DeviceFacts
{
    Id Node{NoId};
    Identifiers Hid{};
    Identifiers Cid{};
    EvaluationResult Uid{};
    EvaluationResult Adr{};
    Presence Sta{Presence::Unknown};
    Reason StaError{Reason::None};
    Resources Crs{};
    PrtEntry Prt[MaximumPrtEntries]{};
    Size PrtCount{0U};
    Reason PrtError{Reason::Missing};
    bool Keyboard{false};
    bool Mouse{false};
    bool PciRoot{false};
};
struct SleepFacts
{
    UInt8 TypeA{0U};
    UInt8 TypeB{0U};
    Reason Error{Reason::Missing};
};

/// Streaming facts API: Prepare evaluates only _PIC(1); NextDevice walks the
/// namespace in load order. ReadDevice and Sleep evaluate requested names,
/// including methods, with a shared total budget as well as per-call bounds.
/// A missing _STA defaults to present; other failures remain Unknown. Caller
/// owns DeviceFacts (about 160 KiB; use arena/pages, not the native stack).
class Facts final
{
public:
    explicit Facts(Library& library, UInt64 budget = MaximumFactOperations) noexcept;
    [[nodiscard]] Reason Prepare() noexcept;
    [[nodiscard]] Id NextDevice(Id previous = NoId) const noexcept;
    void ReadDevice(Id node, DeviceFacts& output) noexcept;
    [[nodiscard]] SleepFacts Sleep() noexcept;
    [[nodiscard]] UInt64 Operations() const noexcept;
    [[nodiscard]] static bool EisaId(UInt64 value, char (&output)[8U]) noexcept;
    [[nodiscard]] bool HasId(const Identifiers& ids, const char* text) const noexcept;

private:
    [[nodiscard]] EvaluationResult Evaluate(Id node, const Id* arguments = nullptr, Size count = 0U) noexcept;
    [[nodiscard]] Id ResolveValue(Id value, Reason& error) noexcept;
    void ReadIds(Id node, Identifiers& output) noexcept;
    [[nodiscard]] Presence ReadSta(Id node, Reason& error) noexcept;
    void ReadPrt(Id node, DeviceFacts& output) noexcept;
    [[nodiscard]] bool PrtRow(Id row, PrtEntry& output, Reason& error) noexcept;
    Library& m_library;
    UInt64 m_remaining;
    UInt64 m_operations{0U};
    Reason m_pic{Reason::Missing};
    bool m_prepared{false};
};
} // namespace Asteria::Acpi::Aml

#endif
