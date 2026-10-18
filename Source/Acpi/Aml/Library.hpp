// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ACPI_AML_LIBRARY_HPP_INCLUDED
#define ASTERIA_ACPI_AML_LIBRARY_HPP_INCLUDED

#include "Acpi/Aml/Types.hpp"

namespace Asteria::Acpi::Aml
{
/// Single-threaded, deterministic, allocation-free AML context. No operation
/// performs hardware, waits, notifications or table loading. Stores affect
/// only this arena's model. Calls use fixed explicit stacks, including lazy
/// Name initialization. Reset invalidates all result IDs. Input tables must
/// outlive the context. Call FinishLoading once after loading tables in order;
/// Evaluate is forbidden before it. Not interrupt-safe (bounded but costly).
class Library final
{
public:
    explicit Library(ArenaView& arena) noexcept;
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    void Reset() noexcept;
    [[nodiscard]] bool Load(Bytes table) noexcept;
    [[nodiscard]] bool FinishLoading() noexcept;
    [[nodiscard]] EvaluationResult Evaluate(Id node, const Id* arguments = nullptr, Size count = 0U,
                                            UInt64 budget = MaximumEvaluationOperations) noexcept;
    [[nodiscard]] Id Find(const char* path, Id scope = 1U) noexcept;
    [[nodiscard]] Id Child(Id parent, const char* segment) const noexcept;
    [[nodiscard]] bool Path(Id node, char* output, Size capacity) const noexcept;
    [[nodiscard]] const Node* GetNode(Id node) const noexcept;
    [[nodiscard]] const Syntax* GetSyntax(Id syntax) const noexcept;
    [[nodiscard]] Bytes Table(Size index) const noexcept;
    [[nodiscard]] const Object& GetObject(Id value) const noexcept;
    [[nodiscard]] Bytes Data(Id value) const noexcept;
    [[nodiscard]] Id Element(Id value, Size index) const noexcept;
    [[nodiscard]] Id Integer(UInt64 value) noexcept;
    [[nodiscard]] Id String(Bytes value) noexcept;
    [[nodiscard]] const ArenaView& Limits() const noexcept
    {
        return m_arena;
    }
    [[nodiscard]] const Budgets& Used() const noexcept;
    [[nodiscard]] Size DiagnosticCount() const noexcept;
    [[nodiscard]] const Diagnostic* GetDiagnostic(Size index) const noexcept;
    [[nodiscard]] Size EffectCount() const noexcept;
    [[nodiscard]] const Effect* GetEffect(Size index) const noexcept;
    [[nodiscard]] Size OsiQueryCount() const noexcept;
    [[nodiscard]] Bytes OsiQuery(Size index) const noexcept;
    /// Sticky arena-exhaustion fact, retained even when diagnostics overflow.
    [[nodiscard]] bool CapacityFailed() const noexcept
    {
        return m_capacityFailed;
    }
    void Diagnose(Reason reason, Id node = NoId, UInt16 opcode = 0U) noexcept;

private:
    [[nodiscard]] Id AddNode(Id parent, UInt32 segment) noexcept;
    [[nodiscard]] Id Lookup(Id parent, UInt32 segment) const noexcept;
    [[nodiscard]] Id Resolve(Bytes path, Id scope, bool create, bool search) noexcept;
    [[nodiscard]] Id FollowAlias(Id node) noexcept;
    [[nodiscard]] Id ResolveText(Bytes path, Id scope) noexcept;
    [[nodiscard]] Id Declare(Id ast, NodeKind kind) noexcept;
    [[nodiscard]] Id AddSyntax(UInt16 opcode, Size offset) noexcept;
    [[nodiscard]] Id AddObject(ObjectKind kind) noexcept;
    [[nodiscard]] Id Unknown(Reason reason, Id ast = NoId) noexcept;
    [[nodiscard]] Id MakeBytes(ObjectKind kind, Size length) noexcept;
    [[nodiscard]] Id Package(Size length) noexcept;
    [[nodiscard]] bool ReadInteger(Size width, UInt64& output) noexcept;
    [[nodiscard]] bool ReadName(Bytes& output) noexcept;
    [[nodiscard]] bool ReadLength(UInt32& output) noexcept;
    [[nodiscard]] bool BeginTerm(Id& output, bool reference, bool dataOnly) noexcept;
    [[nodiscard]] bool PushParse(Id ast, const char* shape, Size end, Id scope) noexcept;
    [[nodiscard]] bool ParseStep() noexcept;
    [[nodiscard]] bool CompleteParse() noexcept;
    [[nodiscard]] bool FieldList(ParseFrame& frame) noexcept;
    [[nodiscard]] Id Parse(Bytes bytes, Id scope, bool method, UInt32 table, UInt32 offset) noexcept;
    void Attach(ParseFrame& frame, Id child) noexcept;
    [[nodiscard]] bool TickLoad() noexcept;
    [[nodiscard]] bool Tick() noexcept;
    [[nodiscard]] bool PushEvaluation(Id ast, Size call, bool reference = false) noexcept;
    void CompleteEvaluation(Id value) noexcept;
    [[nodiscard]] bool StartNode(EvaluationFrame& frame, Id node, bool reference) noexcept;
    [[nodiscard]] bool EvaluationStep() noexcept;
    [[nodiscard]] bool SequenceStep(EvaluationFrame& frame) noexcept;
    [[nodiscard]] bool ControlStep(EvaluationFrame& frame) noexcept;
    [[nodiscard]] bool OperandStep(EvaluationFrame& frame) noexcept;
    [[nodiscard]] Id Apply(EvaluationFrame& frame) noexcept;
    [[nodiscard]] Id Arithmetic(EvaluationFrame& frame) noexcept;
    [[nodiscard]] Id Convert(EvaluationFrame& frame) noexcept;
    [[nodiscard]] Id Concatenate(EvaluationFrame& frame) noexcept;
    [[nodiscard]] Id Match(EvaluationFrame& frame) noexcept;
    [[nodiscard]] Id CreateBufferField(EvaluationFrame& frame) noexcept;
    [[nodiscard]] bool Invoke(EvaluationFrame& frame, Id node) noexcept;
    [[nodiscard]] Id Builtin(Id node, EvaluationFrame& frame) noexcept;
    [[nodiscard]] Id Reference(ReferenceKind kind, Id first, UInt32 count = 0U, UInt64 bits = 0U) noexcept;
    [[nodiscard]] Id ReadReference(Id reference) noexcept;
    [[nodiscard]] bool WriteReference(Id reference, Id value, bool copy = false) noexcept;
    [[nodiscard]] Id ConvertForStore(Id value, ObjectKind kind) noexcept;
    [[nodiscard]] Id Copy(Id value) noexcept;
    [[nodiscard]] Id AsInteger(Id value) noexcept;
    [[nodiscard]] Id Value(Id value) noexcept;
    [[nodiscard]] Id DynamicValue(Bytes path, Id scope) noexcept;
    [[nodiscard]] Id TargetNode(Id value) noexcept;
    [[nodiscard]] bool IsReferenceOperand(UInt16 opcode, Size index) const noexcept;
    [[nodiscard]] UInt64 Mask(UInt64 value, Id ast) const noexcept;
    void Record(UInt16 opcode, Id subject, Id value) noexcept;
    void ClearEvaluating() noexcept;
    [[nodiscard]] EvaluationResult Run(Id node, Id ast, const Id* arguments, Size count, UInt64 budget) noexcept;
    [[nodiscard]] bool ModuleDeclaration(EvaluationFrame& frame) noexcept;
    [[nodiscard]] bool Activate(Id node) noexcept;
    [[nodiscard]] bool LoadModules() noexcept;
    [[nodiscard]] bool MarkModuleError(Id ast, Reason reason) noexcept;
    [[nodiscard]] bool Save(JournalKind kind, Id index, Id previous) noexcept;
    void Rollback(Size checkpoint = 0U) noexcept;
    [[nodiscard]] Id CreateName(EvaluationFrame& frame) noexcept;

    ArenaView& m_arena;
    Budgets m_used{};
    Size m_tables{0U};
    Size m_diagnostics{0U};
    Size m_effects{0U};
    Size m_queries{0U};
    Size m_parseDepth{0U};
    Size m_evaluationDepth{0U};
    Size m_callDepth{0U};
    Size m_dynamicCount{0U};
    Size m_journalCount{0U};
    Bytes m_input{};
    Size m_cursor{0U};
    UInt32 m_table{0U};
    UInt32 m_offset{0U};
    bool m_capacityFailed{false};
    bool m_method{false};
    bool m_finished{false};
    bool m_module{false};
    Reason m_error{Reason::None};
    UInt64 m_budget{0U};
    UInt64 m_operations{0U};
    Id m_result{NoId};
    Id m_rev{NoId};
    Id m_os{NoId};
    Id m_osi{NoId};
};
} // namespace Asteria::Acpi::Aml

#endif
