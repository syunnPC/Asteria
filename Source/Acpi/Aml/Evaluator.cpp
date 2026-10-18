// SPDX-License-Identifier: Apache-2.0

#include "Acpi/Aml/Library.hpp"

#include "Acpi/Aml/Grammar.hpp"

namespace Asteria::Acpi::Aml
{
namespace
{
constexpr UInt8 Operands{1U};
constexpr UInt8 Initializer{2U};
constexpr UInt8 MethodResult{3U};
constexpr UInt8 FinishedLoop{4U};
constexpr UInt8 LazyReference{5U};
constexpr UInt8 ApplyReady{6U};
}

bool Library::Tick() noexcept
{
    if (m_operations >= m_budget)
    {
        static_cast<void>(Unknown(Reason::Budget));
        return false;
    }
    ++m_operations;
    ++m_used.EvaluationOperations;
    return true;
}

UInt64 Library::Mask(const UInt64 value, const Id ast) const noexcept
{
    UInt32 table{0U};
    if (ast != NoId && ast < m_used.SyntaxObjects)
    {
        table = m_arena.Code[ast].Table;
    }
    if (table < m_tables && m_arena.Tables[table].Data[8U] == 1U)
    {
        return value & 0xFFFFFFFFU;
    }
    return value;
}

bool Library::PushEvaluation(const Id ast, const Size call, const bool reference) noexcept
{
    if (ast == NoId || ast >= m_used.SyntaxObjects)
    {
        static_cast<void>(Unknown(Reason::Malformed));
        return false;
    }
    if (m_evaluationDepth >= MaximumEvaluationDepth)
    {
        static_cast<void>(Unknown(Reason::EvaluationDepth, ast));
        return false;
    }
    EvaluationFrame& frame{m_arena.Evaluation[m_evaluationDepth++]};
    frame = {};
    frame.Ast = ast;
    frame.Call = call;
    frame.Reference = reference;
    if (m_evaluationDepth > m_used.EvaluationDepth)
    {
        m_used.EvaluationDepth = m_evaluationDepth;
    }
    return true;
}

void Library::CompleteEvaluation(const Id value) noexcept
{
    --m_evaluationDepth;
    if (m_evaluationDepth == 0U)
    {
        m_result = value;
        return;
    }
    EvaluationFrame& parent{m_arena.Evaluation[m_evaluationDepth - 1U]};
    if (parent.Count < 7U)
    {
        parent.Values[parent.Count] = value;
    }
    parent.Result = value;
}

bool Library::StartNode(EvaluationFrame& frame, Id node, const bool reference) noexcept
{
    node = FollowAlias(node);
    if (node == NoId || node >= m_used.Nodes)
    {
        if (reference)
        {
            CompleteEvaluation(Reference(ReferenceKind::Node, NoId));
            return true;
        }
        static_cast<void>(Unknown(Reason::Unresolved, frame.Ast));
        return false;
    }
    Node& target{m_arena.Nodes[node]};
    if (!m_module && !target.Active)
    {
        if (reference && target.LoadError == Reason::None)
        {
            CompleteEvaluation(Reference(ReferenceKind::Node, NoId));
            return true;
        }
        static_cast<void>(Unknown(target.LoadError == Reason::None ? Reason::Missing : target.LoadError, frame.Ast));
        return false;
    }
    if (reference)
    {
        CompleteEvaluation(Reference(ReferenceKind::Node, node));
        return true;
    }
    if (target.Invalid || !target.Defined)
    {
        static_cast<void>(Unknown(Reason::Unresolved, frame.Ast));
        return false;
    }
    switch (target.Kind)
    {
    case NodeKind::Name:
        if (target.Value != NoId)
        {
            CompleteEvaluation(Value(target.Value));
            return m_error == Reason::None;
        }
        if (target.Evaluating)
        {
            static_cast<void>(Unknown(Reason::Cycle, frame.Ast));
            return false;
        }
        if (target.Definition == NoId)
        {
            static_cast<void>(Unknown(Reason::Uninitialized, frame.Ast));
            return false;
        }
        target.Evaluating = true;
        frame.Stage = Initializer;
        frame.Node = node;
        return PushEvaluation(m_arena.Code[m_arena.Code[target.Definition].First].Next, frame.Call);
    case NodeKind::BufferField:
        if (target.Value != NoId)
        {
            CompleteEvaluation(Value(target.Value));
            return m_error == Reason::None;
        }
        if (target.Evaluating)
        {
            static_cast<void>(Unknown(Reason::Cycle, frame.Ast));
            return false;
        }
        target.Evaluating = true;
        frame.Node = node;
        frame.Stage = Initializer;
        return PushEvaluation(target.Definition, frame.Call);
    case NodeKind::Method:
        frame.Node = node;
        return Invoke(frame, node);
    case NodeKind::OperationRegion:
    case NodeKind::Field:
    case NodeKind::IndexField:
    case NodeKind::BankField:
        static_cast<void>(Unknown(Reason::RegionAccess, frame.Ast));
        return false;
    case NodeKind::Scope:
    case NodeKind::Device:
    case NodeKind::Processor:
    case NodeKind::PowerResource:
    case NodeKind::ThermalZone:
    case NodeKind::Alias:
    case NodeKind::Mutex:
    case NodeKind::Event:
    case NodeKind::External:
        static_cast<void>(Unknown(Reason::Type, frame.Ast));
        return false;
    }
    return false;
}

bool Library::Invoke(EvaluationFrame& frame, const Id node) noexcept
{
    const Node& method{m_arena.Nodes[node]};
    if (frame.Count != (method.Flags & 7U))
    {
        static_cast<void>(Unknown(Reason::Type, frame.Ast));
        return false;
    }
    if (method.Builtin)
    {
        CompleteEvaluation(Builtin(node, frame));
        return m_error == Reason::None;
    }
    if (method.Invalid || method.MethodBody == NoId)
    {
        static_cast<void>(Unknown(Reason::Malformed, frame.Ast));
        return false;
    }
    if (m_callDepth >= MaximumCallDepth)
    {
        static_cast<void>(Unknown(Reason::CallDepth, frame.Ast));
        return false;
    }
    const Size call{m_callDepth++};
    CallFrame& activation{m_arena.Calls[call]};
    activation = {};
    activation.Method = node;
    activation.EvaluationBase = m_evaluationDepth;
    activation.DynamicBase = m_dynamicCount;
    for (Size i{0U}; i < frame.Count; ++i)
    {
        activation.Slots[i] = frame.Values[i];
    }
    if (m_callDepth > m_used.CallDepth)
    {
        m_used.CallDepth = m_callDepth;
    }
    frame.Stage = MethodResult;
    return PushEvaluation(method.MethodBody, call);
}

Id Library::Builtin(const Id node, EvaluationFrame& frame) noexcept
{
    if (node != m_osi || frame.Count != 1U)
    {
        return Unknown(Reason::Unsupported, frame.Ast);
    }
    const Id value{Value(frame.Values[0U])};
    if (GetObject(value).Kind != ObjectKind::String)
    {
        return Unknown(Reason::Type, frame.Ast);
    }
    if (m_queries >= MaximumEffects)
    {
        return Unknown(Reason::Capacity, frame.Ast);
    }
    const Id recorded{Copy(value)};
    m_arena.OsiQueries[m_queries++] = recorded;
    const Bytes query{Data(value)};
    // Compatibility strings are facts from Microsoft's public documentation:
    // https://learn.microsoft.com/windows-hardware/drivers/acpi/winacpi-osi
    constexpr const char* versions[]{
        "Windows 2000",       "Windows 2001", "Windows 2001 SP1", "Windows 2001.1", "Windows 2001 SP2",
        "Windows 2001.1 SP1", "Windows 2006", "Windows 2006 SP1", "Windows 2006.1", "Windows 2009",
        "Windows 2012",       "Windows 2013", "Windows 2015",     "Windows 2016",   "Windows 2017",
        "Windows 2017.2",     "Windows 2018", "Windows 2018.2",   "Windows 2019",   "Windows 2020",
        "Windows 2021",       "Windows 2022",
    };
    for (const char* const version : versions)
    {
        Size i{0U};
        while (i < query.Length && version[i] != '\0' && query.Data[i] == static_cast<UInt8>(version[i]))
        {
            ++i;
        }
        if (i == query.Length && version[i] == '\0')
        {
            return Integer(0xFFFFFFFFU);
        }
    }
    return Integer(0U);
}

Id Library::DynamicValue(const Bytes path, const Id scope) noexcept
{
    if (path.Length != 4U)
    {
        return NoId;
    }
    const UInt32 segment{Segment(path.Data)};
    for (Size i{m_dynamicCount}; i != 0U; --i)
    {
        const DynamicName& dynamic{m_arena.Dynamic[i - 1U]};
        if (dynamic.Scope == scope && dynamic.Segment == segment && dynamic.Call + 1U == m_callDepth)
        {
            return dynamic.Value;
        }
    }
    return NoId;
}

bool Library::SequenceStep(EvaluationFrame& frame) noexcept
{
    const CallFrame& call{m_arena.Calls[frame.Call]};
    if (call.Returning)
    {
        CompleteEvaluation(call.ReturnValue);
        return true;
    }
    if (frame.Stage == 0U)
    {
        frame.Child = m_arena.Code[frame.Ast].First;
        frame.Stage = Operands;
    }
    else if (frame.Child != NoId)
    {
        frame.Child = m_arena.Code[frame.Child].Next;
    }
    if (frame.Child == NoId)
    {
        CompleteEvaluation(NoId);
        return true;
    }
    frame.JournalBase = m_journalCount;
    return PushEvaluation(frame.Child, frame.Call);
}

bool Library::ControlStep(EvaluationFrame& frame) noexcept
{
    const Syntax& ast{m_arena.Code[frame.Ast]};
    const bool loop{ast.Opcode == 0xA2U};
    if (frame.Stage == FinishedLoop || m_arena.Calls[frame.Call].Returning)
    {
        CompleteEvaluation(NoId);
        return true;
    }
    if (frame.Stage == 0U)
    {
        frame.Stage = Operands;
        frame.Count = 0U;
        return PushEvaluation(ast.First, frame.Call);
    }
    if (frame.Stage == Operands)
    {
        const Id predicate{AsInteger(frame.Result)};
        if (m_error != Reason::None)
        {
            return false;
        }
        if (GetObject(predicate).Integer != 0U)
        {
            frame.Stage = Initializer;
            return PushEvaluation(m_arena.Code[ast.First].Next, frame.Call);
        }
        if (!loop && ast.Next != NoId && m_arena.Code[ast.Next].Opcode == 0xA1U)
        {
            frame.Stage = Initializer;
            return PushEvaluation(m_arena.Code[ast.Next].First, frame.Call);
        }
        CompleteEvaluation(NoId);
        return true;
    }
    if (loop)
    {
        frame.Stage = 0U;
    }
    else
    {
        CompleteEvaluation(NoId);
    }
    return true;
}

bool Library::IsReferenceOperand(const UInt16 opcode, const Size index) const noexcept
{
    if (opcode == 0x12U || opcode == 0x13U)
    {
        return index != 0U;
    }
    if (opcode == 0x71U || opcode == 0x75U || opcode == 0x76U || opcode == 0x87U || opcode == 0x8EU)
    {
        return true;
    }
    const char* const shape{ArgumentShape(opcode)};
    return shape != nullptr && (shape[index] == 't' || shape[index] == 'n');
}

bool Library::OperandStep(EvaluationFrame& frame) noexcept
{
    const Syntax& ast{m_arena.Code[frame.Ast]};
    const bool package{ast.Opcode == 0x12U || ast.Opcode == 0x13U};
    if (frame.Stage == ApplyReady)
    {
        CompleteEvaluation(Apply(frame));
        return m_error == Reason::None;
    }
    if (frame.Stage == 0U)
    {
        frame.Child = ast.First;
        frame.Stage = Operands;
        frame.Count = 0U;
    }
    else if (frame.Stage == MethodResult)
    {
        const Id value{m_arena.Calls[m_callDepth - 1U].ReturnValue};
        m_dynamicCount = m_arena.Calls[m_callDepth - 1U].DynamicBase;
        --m_callDepth;
        CompleteEvaluation(value);
        return true;
    }
    else
    {
        if (package)
        {
            if (frame.Count == 0U)
            {
                const Id size{AsInteger(frame.Result)};
                if (m_error != Reason::None)
                {
                    return false;
                }
                const UInt64 length{GetObject(size).Integer};
                if (length > MaximumPackageElements)
                {
                    static_cast<void>(Unknown(Reason::Capacity, frame.Ast));
                    return false;
                }
                frame.Node = Package(static_cast<Size>(length));
            }
            else
            {
                const Object& out{GetObject(frame.Node)};
                if (frame.Count - 1U >= out.Count)
                {
                    static_cast<void>(Unknown(Reason::Malformed, frame.Ast));
                    return false;
                }
                m_arena.Elements[out.First + frame.Count - 1U] = frame.Result;
            }
        }
        ++frame.Count;
        frame.Child = m_arena.Code[frame.Child].Next;
    }
    if (frame.Child != NoId)
    {
        return PushEvaluation(frame.Child, frame.Call, IsReferenceOperand(ast.Opcode, frame.Count));
    }
    if (ast.Opcode == InvokeOp)
    {
        const Id node{FollowAlias(Resolve(ast.Span, ast.Scope, false, true))};
        if (node == NoId)
        {
            static_cast<void>(Unknown(Reason::Unresolved, frame.Ast));
            return false;
        }
        return Invoke(frame, node);
    }
    // Operations on SuperNames may need the Name's lazy initializer. This
    // uses another explicit frame and preserves the operand reference.
    if (ast.Opcode == 0x83U || ast.Opcode == 0x87U || ast.Opcode == 0x8EU || ast.Opcode == 0x75U ||
        ast.Opcode == 0x76U || ast.Opcode == 0x70U)
    {
        const Size operand{ast.Opcode == 0x70U ? 1U : 0U};
        Id value{frame.Values[operand]};
        if (ast.Opcode == 0x83U && GetObject(value).Kind == ObjectKind::String)
        {
            value = Reference(ReferenceKind::Node, ResolveText(Data(value), ast.Scope));
        }
        for (Size depth{0U}; depth < MaximumEvaluationDepth; ++depth)
        {
            const Object& reference{GetObject(value)};
            if (reference.Kind != ObjectKind::Reference)
            {
                break;
            }
            if (reference.Reference == ReferenceKind::Node)
            {
                const Id node{reference.First};
                if (node != NoId && node < m_used.Nodes &&
                    (m_arena.Nodes[node].Kind == NodeKind::Name || m_arena.Nodes[node].Kind == NodeKind::BufferField) &&
                    m_arena.Nodes[node].Value == NoId)
                {
                    frame.Stage = ApplyReady;
                    if (m_evaluationDepth == MaximumEvaluationDepth)
                    {
                        static_cast<void>(Unknown(Reason::EvaluationDepth, frame.Ast));
                        return false;
                    }
                    EvaluationFrame& lazy{m_arena.Evaluation[m_evaluationDepth++]};
                    lazy = {};
                    lazy.Node = node;
                    lazy.Call = frame.Call;
                    return true;
                }
                break;
            }
            if (reference.Reference == ReferenceKind::Element)
            {
                value = reference.First < m_used.Elements ? m_arena.Elements[reference.First] : NoId;
            }
            else if (reference.Reference == ReferenceKind::Slot)
            {
                value = reference.First / 15U < m_callDepth
                            ? m_arena.Calls[reference.First / 15U].Slots[reference.First % 15U]
                            : NoId;
            }
            else
            {
                break;
            }
        }
    }
    CompleteEvaluation(package ? frame.Node : Apply(frame));
    return m_error == Reason::None;
}

bool Library::EvaluationStep() noexcept
{
    EvaluationFrame& frame{m_arena.Evaluation[m_evaluationDepth - 1U]};
    if (frame.Stage == Initializer && frame.Node != NoId)
    {
        Node& node{m_arena.Nodes[frame.Node]};
        if (!Save(JournalKind::Node, frame.Node, node.Value))
        {
            return false;
        }
        node.Value = frame.Result;
        node.Evaluating = false;
        CompleteEvaluation(node.Kind == NodeKind::BufferField ? Value(frame.Result) : frame.Result);
        return m_error == Reason::None;
    }
    if (frame.Stage == LazyReference)
    {
        CompleteEvaluation(Value(frame.Result));
        return m_error == Reason::None;
    }
    if (frame.Ast == NoId)
    {
        return frame.Stage == MethodResult ? OperandStep(frame) : StartNode(frame, frame.Node, false);
    }
    const Syntax& ast{m_arena.Code[frame.Ast]};
    if (m_module && frame.Call == 0U && NamespaceDeclaration(ast.Opcode))
    {
        return ModuleDeclaration(frame);
    }
    if (m_module && frame.Call == 0U && ast.Opcode == 0xA4U)
    {
        static_cast<void>(Unknown(Reason::Malformed, frame.Ast));
        return false;
    }
    switch (ast.Opcode)
    {
    case ListOp:
        return SequenceStep(frame);
    case 0xA0U:
    case 0xA2U:
        return ControlStep(frame);
    case 0xA1U:
        CompleteEvaluation(NoId);
        return true;
    case LiteralOp:
        CompleteEvaluation(Integer(Mask(ast.Number, frame.Ast)));
        return true;
    case 0x0DU:
        CompleteEvaluation(String(ast.Span));
        return true;
    case NamePathOp:
    {
        const Id dynamic{DynamicValue(ast.Span, ast.Scope)};
        if (dynamic != NoId)
        {
            CompleteEvaluation(frame.Reference ? dynamic : Value(dynamic));
            return m_error == Reason::None;
        }
        return StartNode(frame, Resolve(ast.Span, ast.Scope, false, true), frame.Reference);
    }
    case 0x60U:
    case 0x61U:
    case 0x62U:
    case 0x63U:
    case 0x64U:
    case 0x65U:
    case 0x66U:
    case 0x67U:
    case 0x68U:
    case 0x69U:
    case 0x6AU:
    case 0x6BU:
    case 0x6CU:
    case 0x6DU:
    case 0x6EU:
    {
        const Size slot{ast.Opcode < 0x68U ? ast.Opcode - 0x60U + 7U : ast.Opcode - 0x68U};
        if (!frame.Reference && m_arena.Calls[frame.Call].Slots[slot] == NoId)
        {
            static_cast<void>(Unknown(Reason::Uninitialized, frame.Ast));
            return false;
        }
        CompleteEvaluation(frame.Reference ? Reference(ReferenceKind::Slot, static_cast<Id>(frame.Call * 15U + slot))
                                           : m_arena.Calls[frame.Call].Slots[slot]);
        return true;
    }
    case 0x9FU:
    case 0xA5U:
        for (Size i{m_evaluationDepth - 1U}; i != 0U; --i)
        {
            EvaluationFrame& enclosing{m_arena.Evaluation[i - 1U]};
            if (enclosing.Call != frame.Call)
            {
                break;
            }
            if (enclosing.Ast != NoId && m_arena.Code[enclosing.Ast].Opcode == 0xA2U)
            {
                enclosing.Stage = ast.Opcode == 0xA5U ? FinishedLoop : UInt8{0U};
                m_evaluationDepth = i;
                return true;
            }
        }
        static_cast<void>(Unknown(Reason::Malformed, frame.Ast));
        return false;
    case 0x06U:
    case 0x10U:
    case 0x14U:
    case 0x15U:
    case 0x5B01U:
    case 0x5B02U:
    case 0x5B80U:
    case 0x5B81U:
    case 0x5B82U:
    case 0x5B83U:
    case 0x5B84U:
    case 0x5B85U:
    case 0x5B86U:
    case 0x5B87U:
    case 0x5B88U:
        // Runtime namespace creation, other than buffer fields, is outside
        // this evaluator's subset. Load-time declarations are already retained.
        static_cast<void>(Unknown(Reason::Unsupported, frame.Ast));
        return false;
    case 0x5B1FU:
    case 0x5B20U:
    case 0x5B2AU:
    case 0x5B32U:
    case 0xCCU:
        static_cast<void>(Unknown(Reason::Unsupported, frame.Ast));
        return false;
    case 0x5B33U:
    {
        // The pure evaluator has no clock. Retain an unknown data value;
        // an ignored Debug write can consume it without needing its value.
        const Id value{AddObject(ObjectKind::Unknown)};
        if (value == NoId)
        {
            return false;
        }
        m_arena.Objects[value].Error = Reason::Unsupported;
        CompleteEvaluation(value);
        return true;
    }
    case 0x5B31U:
        if (!frame.Reference)
        {
            static_cast<void>(Unknown(Reason::Unsupported, frame.Ast));
            return false;
        }
        CompleteEvaluation(Reference(ReferenceKind::Debug, NoId));
        return true;
    default:
        return OperandStep(frame);
    }
}

void Library::ClearEvaluating() noexcept
{
    for (Id i{1U}; i < m_used.Nodes; ++i)
    {
        m_arena.Nodes[i].Evaluating = false;
    }
}

EvaluationResult Library::Evaluate(const Id node, const Id* const arguments, const Size count,
                                   const UInt64 budget) noexcept
{
    return Run(node, NoId, arguments, count, budget);
}

EvaluationResult Library::Run(const Id node, const Id ast, const Id* const arguments, const Size count,
                              const UInt64 budget) noexcept
{
    m_error = Reason::None;
    m_operations = 0U;
    const UInt64 maximum{m_module ? MaximumLoadOperations : MaximumEvaluationOperations};
    m_budget = budget > maximum ? maximum : budget;
    m_result = NoId;
    m_dynamicCount = 0U;
    m_journalCount = 0U;
    m_evaluationDepth = 1U;
    m_callDepth = 1U;
    if (!m_finished || node == NoId || node >= m_used.Nodes || count > 7U || (count != 0U && arguments == nullptr))
    {
        return {NoId, Reason::Missing, 0U};
    }
    EvaluationFrame& root{m_arena.Evaluation[0U]};
    root = {};
    root.Node = node;
    root.Ast = ast;
    root.Count = count;
    m_arena.Calls[0U] = {};
    for (Size i{0U}; i < count; ++i)
    {
        if (arguments[i] >= m_used.Objects)
        {
            return {NoId, Reason::Bounds, 0U};
        }
        root.Values[i] = arguments[i];
    }
    Reason moduleError{Reason::None};
    while (m_evaluationDepth != 0U && m_error == Reason::None)
    {
        if (!Tick() || !EvaluationStep())
        {
            // A module TermList contains independent statements. An unknown
            // predicate invalidates its declarations, not the rest of a table.
            // Never recover exhausted resources or malformed execution.
            bool recovered{false};
            if (m_module && (m_error == Reason::RegionAccess || m_error == Reason::Unsupported ||
                             m_error == Reason::Unresolved || m_error == Reason::Missing || m_error == Reason::Type))
            {
                const Reason error{m_error};
                for (Size depth{m_evaluationDepth}; depth != 0U; --depth)
                {
                    EvaluationFrame& sequence{m_arena.Evaluation[depth - 1U]};
                    if (sequence.Call == 0U && sequence.Ast != NoId && m_arena.Code[sequence.Ast].Opcode == ListOp &&
                        sequence.Child != NoId)
                    {
                        Rollback(sequence.JournalBase);
                        ClearEvaluating();
                        m_error = Reason::None;
                        if (!MarkModuleError(sequence.Child, error))
                        {
                            break;
                        }
                        const Syntax& statement{m_arena.Code[sequence.Child]};
                        if (statement.Opcode == 0xA0U && statement.Next != NoId &&
                            m_arena.Code[statement.Next].Opcode == 0xA1U && !MarkModuleError(statement.Next, error))
                        {
                            break;
                        }
                        if (moduleError == Reason::None)
                        {
                            moduleError = error;
                        }
                        m_evaluationDepth = depth;
                        m_callDepth = 1U;
                        m_dynamicCount = 0U;
                        recovered = true;
                        break;
                    }
                }
            }
            if (!recovered)
            {
                break;
            }
        }
    }
    if (m_error == Reason::None && m_result != NoId && GetObject(m_result).Kind == ObjectKind::Unknown)
    {
        static_cast<void>(Unknown(GetObject(m_result).Error));
    }
    if (m_error != Reason::None)
    {
        Rollback();
        ClearEvaluating();
    }
    m_callDepth = 0U;
    m_evaluationDepth = 0U;
    return {m_result, m_error == Reason::None ? moduleError : m_error, m_operations};
}

bool Library::MarkModuleError(const Id ast, const Reason reason) noexcept
{
    // Parser scratch is idle during evaluation. Walk only this statement's
    // subtree, with the same fixed depth bound as parsing.
    Size depth{1U};
    m_arena.Parse[0U] = {};
    m_arena.Parse[0U].Ast = ast;
    while (depth != 0U)
    {
        ParseFrame& frame{m_arena.Parse[depth - 1U]};
        if (frame.Position == 0U)
        {
            if (!Tick())
            {
                return false;
            }
            const Syntax& code{m_arena.Code[frame.Ast]};
            if (!code.Ignored && code.Binding != NoId && !m_arena.Nodes[code.Binding].Active)
            {
                m_arena.Nodes[code.Binding].LoadError = reason;
            }
            frame.Last = code.First;
            frame.Position = 1U;
        }
        if (frame.Last == NoId)
        {
            --depth;
            continue;
        }
        const Id child{frame.Last};
        frame.Last = m_arena.Code[child].Next;
        if (depth == MaximumParseDepth)
        {
            static_cast<void>(Unknown(Reason::EvaluationDepth, ast));
            return false;
        }
        m_arena.Parse[depth] = {};
        m_arena.Parse[depth++].Ast = child;
    }
    return true;
}

bool Library::Activate(const Id node) noexcept
{
    if (node == NoId || node >= m_used.Nodes || m_arena.Nodes[node].Active)
    {
        return true;
    }
    if (!Save(JournalKind::Active, node, 0U))
    {
        return false;
    }
    m_arena.Nodes[node].Active = true;
    return true;
}

bool Library::ModuleDeclaration(EvaluationFrame& frame) noexcept
{
    const Syntax& ast{m_arena.Code[frame.Ast]};
    const UInt16 opcode{ast.Opcode};
    if (ast.Ignored)
    {
        CompleteEvaluation(NoId);
        return true;
    }
    if (frame.Stage == 8U)
    {
        CompleteEvaluation(NoId);
        return true;
    }
    if (opcode != 0x10U && opcode != 0x15U && !Activate(ast.Binding))
    {
        return false;
    }
    if (opcode == 0x10U || (opcode >= 0x5B82U && opcode <= 0x5B85U))
    {
        Id body{ast.First};
        while (body != NoId && m_arena.Code[body].Opcode != ListOp)
        {
            body = m_arena.Code[body].Next;
        }
        if (body != NoId)
        {
            frame.Stage = 8U;
            return PushEvaluation(body, frame.Call);
        }
    }
    if (opcode == 0x5B81U || opcode == 0x5B86U || opcode == 0x5B87U)
    {
        for (Id child{ast.First}; child != NoId; child = m_arena.Code[child].Next)
        {
            if (!Tick() || !Activate(m_arena.Code[child].Binding))
            {
                return false;
            }
        }
    }
    CompleteEvaluation(NoId);
    return true;
}

bool Library::LoadModules() noexcept
{
    // Parsing retains potential declarations. Execution publishes only the
    // selected module-level branches, without initializing unused Names or
    // entering control-method bodies. Predefined nodes have no Definition.
    for (Id node{1U}; node < m_used.Nodes; ++node)
    {
        Node& item{m_arena.Nodes[node]};
        item.Active = item.Definition == NoId && item.Defined;
        item.LoadError = item.Defined ? Reason::None : Reason::Unresolved;
    }
    m_module = true;
    bool good{true};
    for (Size table{0U}; table < m_tables; ++table)
    {
        const Id root{m_arena.Roots[table]};
        if (root == NoId)
        {
            good = false;
            for (Id node{1U}; node < m_used.Nodes; ++node)
            {
                if (!m_arena.Nodes[node].Active && m_arena.Nodes[node].Table == table)
                {
                    m_arena.Nodes[node].LoadError = m_arena.TableErrors[table];
                }
            }
            continue;
        }
        const UInt64 remaining{MaximumLoadOperations - m_used.LoadOperations};
        const EvaluationResult result{Run(1U, root, nullptr, 0U, remaining)};
        m_used.LoadOperations += result.Operations;
        if (!result.Known())
        {
            good = false;
            if (m_error != Reason::None)
            {
                for (Id node{1U}; node < m_used.Nodes; ++node)
                {
                    Node& item{m_arena.Nodes[node]};
                    if (!item.Active && item.Table == table && item.Definition != NoId)
                    {
                        item.LoadError = result.Error;
                    }
                }
            }
        }
    }
    m_module = false;
    return good;
}

void Library::Record(const UInt16 opcode, const Id subject, const Id value) noexcept
{
    if (m_effects >= MaximumEffects)
    {
        static_cast<void>(Unknown(Reason::Capacity));
        return;
    }
    m_arena.Effects[m_effects++] = {opcode, subject, value};
}
} // namespace Asteria::Acpi::Aml
