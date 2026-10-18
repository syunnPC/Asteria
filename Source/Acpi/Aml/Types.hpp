// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ACPI_AML_TYPES_HPP_INCLUDED
#define ASTERIA_ACPI_AML_TYPES_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Acpi::Aml
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using Id = UInt32;
inline constexpr Id NoId{0U};
inline constexpr Size MaximumTables{256U};
inline constexpr Size MaximumTableBytes{1024U * 1024U};
inline constexpr Size MaximumNodes{32768U};
inline constexpr Size NamespaceHashSlots{65536U};
inline constexpr Size MaximumSyntax{262144U};
inline constexpr Size MaximumObjects{262144U};
inline constexpr Size MaximumElements{262144U};
inline constexpr Size MaximumObjectBytes{65536U};
inline constexpr Size MaximumPackageElements{4096U};
inline constexpr Size MaximumByteStorage{8U * 1024U * 1024U};
inline constexpr Size MaximumParseDepth{256U};
inline constexpr Size MaximumEvaluationDepth{256U};
inline constexpr Size MaximumCallDepth{32U};
inline constexpr Size MaximumDiagnostics{1024U};
inline constexpr Size MaximumEffects{4096U};
inline constexpr Size MaximumResources{256U};
inline constexpr Size MaximumIds{64U};
inline constexpr Size MaximumPrtEntries{4096U};
inline constexpr Size MaximumPathBytes{1281U};
inline constexpr UInt64 MaximumLoadOperations{16U * 1024U * 1024U};
inline constexpr UInt64 MaximumEvaluationOperations{100000U};
inline constexpr UInt64 MaximumFactOperations{16U * 1024U * 1024U};

/// Borrowed bytes. Table input must remain immutable and alive through the
/// library's lifetime. Empty spans may have a null pointer.
struct Bytes
{
    const UInt8* Data{nullptr};
    Size Length{0U};
};

enum class Reason : UInt8
{
    None,
    Malformed,
    Truncated,
    Checksum,
    Conflict,
    Unresolved,
    Capacity,
    ParseDepth,
    EvaluationDepth,
    CallDepth,
    Budget,
    Cycle,
    RegionAccess,
    Unsupported,
    Type,
    Bounds,
    DivideByZero,
    Uninitialized,
    Missing,
};
struct Diagnostic
{
    Reason Code{Reason::None};
    Id Node{NoId};
    UInt32 Table{0U};
    UInt32 Offset{0U};
    UInt16 Opcode{0U};
};

enum class NodeKind : UInt8
{
    Scope,
    Device,
    Processor,
    PowerResource,
    ThermalZone,
    Name,
    Method,
    OperationRegion,
    Field,
    IndexField,
    BankField,
    Alias,
    Mutex,
    Event,
    External,
    BufferField,
};
enum class ObjectKind : UInt8
{
    Unknown,
    Integer,
    String,
    Buffer,
    Package,
    Reference,
};
enum class ReferenceKind : UInt8
{
    Node,
    Slot,
    Element,
    Byte,
    BufferField,
    Debug,
};
struct Object
{
    UInt64 Integer{0U};
    Id First{NoId};
    UInt32 Count{0U};
    ObjectKind Kind{ObjectKind::Unknown};
    Reason Error{Reason::Uninitialized};
    ReferenceKind Reference{ReferenceKind::Node};
};
struct Node
{
    Bytes Body{};
    Id Parent{NoId};
    UInt32 Segment{0U};
    Id Definition{NoId};
    Id Value{NoId};
    Id MethodBody{NoId};
    UInt32 Table{0U};
    UInt8 Flags{0U};
    UInt8 ExternalType{0U};
    NodeKind Kind{NodeKind::Scope};
    bool Defined{false};
    bool Evaluating{false};
    bool Invalid{false};
    bool Builtin{false};
    UInt64 BitOffset{0U};
    UInt32 BitLength{0U};
    bool Active{true};
    Reason LoadError{Reason::None};
};
static_assert(sizeof(Node) == 64U);

/// Internal syntax uses opcode values from ACPI 6.5 section 20. Synthetic
/// opcodes occupy the range above all one-byte and 0x5B extended encodings.
inline constexpr UInt16 LiteralOp{0x100U};
inline constexpr UInt16 NamePathOp{0x101U};
inline constexpr UInt16 InvokeOp{0x102U};
inline constexpr UInt16 ListOp{0x103U};
struct Syntax
{
    Bytes Span{};
    UInt64 Number{0U};
    Id First{NoId};
    Id Next{NoId};
    Id Scope{NoId};
    Id Binding{NoId};
    UInt32 Table{0U};
    UInt32 Offset{0U};
    UInt16 Opcode{0U};
    bool Reference{false};
    bool Ignored{false};
};
struct Effect
{
    UInt16 Opcode{0U};
    Id Subject{NoId};
    Id Value{NoId};
};
struct Budgets
{
    UInt64 LoadOperations{0U};
    UInt64 EvaluationOperations{0U};
    Size ParseDepth{0U};
    Size EvaluationDepth{0U};
    Size CallDepth{0U};
    Size Nodes{0U};
    Size SyntaxObjects{0U};
    Size Objects{0U};
    Size Elements{0U};
    Size BytesUsed{0U};
    Size DroppedDiagnostics{0U};
};
struct ParseFrame
{
    const char* Shape{nullptr};
    Size End{0U};
    Id Ast{NoId};
    Id Last{NoId};
    Id Scope{NoId};
    Size Position{0U};
    bool Method{false};
};
struct EvaluationFrame
{
    Id Ast{NoId};
    Id Child{NoId};
    Id Values[7U]{};
    Id Result{NoId};
    Id Node{NoId};
    Size Call{0U};
    Size Count{0U};
    UInt8 Stage{0U};
    bool Reference{false};
    Size JournalBase{0U};
};
struct CallFrame
{
    Id Slots[15U]{};
    Id Method{NoId};
    Size EvaluationBase{0U};
    Id ReturnValue{NoId};
    bool Returning{false};
    Size DynamicBase{0U};
};
struct DynamicName
{
    Size Call{0U};
    Id Scope{NoId};
    UInt32 Segment{0U};
    Id Value{NoId};
};
inline constexpr Size MaximumDynamicNames{1024U};
inline constexpr Size MaximumJournalEntries{MaximumNodes * 4U};
enum class JournalKind : UInt8
{
    Node,
    Element,
    Byte,
    Active,
};
struct JournalEntry
{
    Id Index{NoId};
    Id Previous{NoId};
    JournalKind Kind{JournalKind::Node};
};

/// Borrowed arena storage. Only BoundedArena supplies this view; its capacities
/// describe actual backing arrays. Limits may be lowered before Library::Reset.
struct ArenaView
{
    Node* Nodes;
    Syntax* Code;
    Object* Objects;
    Id* Elements;
    UInt8* ByteStorage;
    const Size NodeCapacity;
    const Size SyntaxCapacity;
    const Size ObjectCapacity;
    const Size ElementCapacity;
    const Size ByteCapacity;
    Size NodeLimit;
    Size SyntaxLimit;
    Size ObjectLimit;
    Size ElementLimit;
    Size ByteLimit;
    Id Hash[NamespaceHashSlots]{};
    ParseFrame Parse[MaximumParseDepth]{};
    EvaluationFrame Evaluation[MaximumEvaluationDepth]{};
    CallFrame Calls[MaximumCallDepth]{};
    DynamicName Dynamic[MaximumDynamicNames]{};
    JournalEntry Journal[MaximumJournalEntries]{};
    Diagnostic Diagnostics[MaximumDiagnostics]{};
    Effect Effects[MaximumEffects]{};
    Id OsiQueries[MaximumEffects]{};
    Bytes Tables[MaximumTables]{};
    Id Roots[MaximumTables]{};
    Reason TableErrors[MaximumTables]{};
    char TextPath[MaximumPathBytes]{};
};

/// Caller-owned typed storage. Construct in pages/host heap, never on the stack.
/// A reduced node budget also reduces syntax, objects, elements and byte storage
/// proportionally; explicit parser/evaluator stacks and diagnostic bounds stay
/// fixed. No allocation. The view and its pointers cannot outlive this owner.
template <Size NodeCount> class BoundedArena final
{
public:
    static_assert(NodeCount >= 64U && NodeCount <= MaximumNodes);
    constexpr BoundedArena() noexcept
        : m_view{m_nodes,      m_code,    m_objects, m_elements,  m_bytes,     NodeCount,    SyntaxCount, ObjectCount,
                 ElementCount, ByteCount, NodeCount, SyntaxCount, ObjectCount, ElementCount, ByteCount}
    {
    }
    BoundedArena(const BoundedArena&) = delete;
    BoundedArena& operator=(const BoundedArena&) = delete;
    [[nodiscard]] constexpr ArenaView& Get() noexcept
    {
        return m_view;
    }

private:
    static constexpr Size SyntaxCount{MaximumSyntax * NodeCount / MaximumNodes};
    static constexpr Size ObjectCount{MaximumObjects * NodeCount / MaximumNodes};
    static constexpr Size ElementCount{MaximumElements * NodeCount / MaximumNodes};
    static constexpr Size ByteCount{MaximumByteStorage * NodeCount / MaximumNodes};
    Node m_nodes[NodeCount]{};
    Syntax m_code[SyntaxCount]{};
    Object m_objects[ObjectCount]{};
    Id m_elements[ElementCount]{};
    UInt8 m_bytes[ByteCount]{};
    ArenaView m_view;
};
using Arena = BoundedArena<MaximumNodes>;
inline constexpr Size ReducedNodes{8192U};
using ReducedArena = BoundedArena<ReducedNodes>;
struct EvaluationResult
{
    Id Value{NoId};
    Reason Error{Reason::None};
    UInt64 Operations{0U};
    [[nodiscard]] bool Known() const noexcept
    {
        return Error == Reason::None;
    }
};
[[nodiscard]] const char* ReasonText(Reason reason) noexcept;
[[nodiscard]] const char* KindText(NodeKind kind) noexcept;
} // namespace Asteria::Acpi::Aml

#endif
