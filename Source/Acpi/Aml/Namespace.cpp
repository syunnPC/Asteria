// SPDX-License-Identifier: Apache-2.0

#include "Acpi/Aml/Library.hpp"

#include "Acpi/Aml/Grammar.hpp"

namespace Asteria::Acpi::Aml
{
namespace
{
constexpr UInt32 HashKey(const Id parent, const UInt32 segment) noexcept
{
    // Unsigned multiplication intentionally wraps to mix the key.
    return ((parent * 0x9E3779B9U) ^ (segment * 0x85EBCA6BU)) & (NamespaceHashSlots - 1U);
}
} // namespace

Library::Library(ArenaView& arena) noexcept : m_arena{arena}
{
    Reset();
}

void Library::Reset() noexcept
{
    m_capacityFailed = false;
    m_used = {};
    m_tables = 0U;
    m_diagnostics = 0U;
    m_effects = 0U;
    m_queries = 0U;
    m_parseDepth = 0U;
    m_evaluationDepth = 0U;
    m_callDepth = 0U;
    m_dynamicCount = 0U;
    m_finished = false;
    m_module = false;
    m_journalCount = 0U;
    m_error = Reason::None;
    m_used.Nodes = 2U;
    m_used.SyntaxObjects = 1U;
    m_used.Objects = 1U;
    m_arena.Nodes[0U] = {};
    m_arena.Nodes[1U] = {};
    m_arena.Nodes[1U].Defined = true;
    m_arena.Objects[0U] = {};
    for (Size i{0U}; i < NamespaceHashSlots; ++i)
    {
        m_arena.Hash[i] = NoId;
    }
    // Clamp caller limits: they may never expose storage outside the arena.
    if (m_arena.NodeLimit < 2U || m_arena.NodeLimit > m_arena.NodeCapacity)
    {
        m_arena.NodeLimit = m_arena.NodeCapacity;
    }
    if (m_arena.SyntaxLimit < 1U || m_arena.SyntaxLimit > m_arena.SyntaxCapacity)
    {
        m_arena.SyntaxLimit = m_arena.SyntaxCapacity;
    }
    if (m_arena.ObjectLimit < 1U || m_arena.ObjectLimit > m_arena.ObjectCapacity)
    {
        m_arena.ObjectLimit = m_arena.ObjectCapacity;
    }
    if (m_arena.ElementLimit > m_arena.ElementCapacity)
    {
        m_arena.ElementLimit = m_arena.ElementCapacity;
    }
    if (m_arena.ByteLimit > m_arena.ByteCapacity)
    {
        m_arena.ByteLimit = m_arena.ByteCapacity;
    }
    constexpr UInt8 scopes[][4U]{
        {'_', 'S', 'B', '_'}, {'_', 'P', 'R', '_'}, {'_', 'T', 'Z', '_'}, {'_', 'S', 'I', '_'}, {'_', 'G', 'P', 'E'}};
    for (const auto& scope : scopes)
    {
        const Id node{AddNode(1U, Segment(scope))};
        if (node != NoId)
        {
            m_arena.Nodes[node].Defined = true;
        }
    }
    constexpr UInt8 rev[]{'_', 'R', 'E', 'V'};
    constexpr UInt8 os[]{'_', 'O', 'S', '_'};
    constexpr UInt8 osi[]{'_', 'O', 'S', 'I'};
    m_rev = AddNode(1U, Segment(rev));
    m_os = AddNode(1U, Segment(os));
    m_osi = AddNode(1U, Segment(osi));
    if (m_rev != NoId)
    {
        m_arena.Nodes[m_rev].Defined = true;
        m_arena.Nodes[m_rev].Kind = NodeKind::Name;
        m_arena.Nodes[m_rev].Value = Integer(2U);
    }
    if (m_os != NoId)
    {
        constexpr UInt8 text[]{'M', 'i', 'c', 'r', 'o', 's', 'o', 'f', 't', ' ',
                               'W', 'i', 'n', 'd', 'o', 'w', 's', ' ', 'N', 'T'};
        m_arena.Nodes[m_os].Defined = true;
        m_arena.Nodes[m_os].Kind = NodeKind::Name;
        m_arena.Nodes[m_os].Value = String({text, sizeof(text)});
    }
    if (m_osi != NoId)
    {
        m_arena.Nodes[m_osi].Defined = true;
        m_arena.Nodes[m_osi].Kind = NodeKind::Method;
        m_arena.Nodes[m_osi].Flags = 1U;
        m_arena.Nodes[m_osi].Builtin = true;
    }
}

Id Library::Lookup(const Id parent, const UInt32 segment) const noexcept
{
    Size slot{HashKey(parent, segment)};
    for (Size probe{0U}; probe < NamespaceHashSlots; ++probe)
    {
        const Id id{m_arena.Hash[slot]};
        if (id == NoId)
        {
            return NoId;
        }
        if (m_arena.Nodes[id].Parent == parent && m_arena.Nodes[id].Segment == segment)
        {
            if (m_finished && !m_module && !m_arena.Nodes[id].Active && m_arena.Nodes[id].LoadError == Reason::None)
            {
                return NoId;
            }
            return id;
        }
        slot = (slot + 1U) & (NamespaceHashSlots - 1U);
    }
    return NoId;
}

Id Library::AddNode(const Id parent, const UInt32 segment) noexcept
{
    const Id existing{Lookup(parent, segment)};
    if (existing != NoId)
    {
        return existing;
    }
    if (m_used.Nodes >= m_arena.NodeLimit)
    {
        Diagnose(Reason::Capacity);
        return NoId;
    }
    Size slot{HashKey(parent, segment)};
    for (Size probe{0U}; probe < NamespaceHashSlots; ++probe)
    {
        if (m_arena.Hash[slot] == NoId)
        {
            const Id node{static_cast<Id>(m_used.Nodes++)};
            m_arena.Nodes[node] = {};
            m_arena.Nodes[node].Parent = parent;
            m_arena.Nodes[node].Segment = segment;
            m_arena.Hash[slot] = node;
            return node;
        }
        slot = (slot + 1U) & (NamespaceHashSlots - 1U);
    }
    Diagnose(Reason::Capacity);
    return NoId;
}

Id Library::Resolve(const Bytes path, Id scope, const bool create, const bool search) noexcept
{
    if (path.Data == nullptr || path.Length == 0U || scope == NoId || scope >= m_used.Nodes)
    {
        return NoId;
    }
    Size cursor{0U};
    bool explicitPath{false};
    if (path.Data[cursor] == '\\')
    {
        scope = 1U;
        explicitPath = true;
        ++cursor;
    }
    while (cursor < path.Length && path.Data[cursor] == '^')
    {
        explicitPath = true;
        scope = m_arena.Nodes[scope].Parent;
        if (scope == NoId)
        {
            return NoId;
        }
        ++cursor;
    }
    if (cursor >= path.Length)
    {
        return NoId;
    }
    Size segments{1U};
    if (path.Data[cursor] == 0U)
    {
        return cursor + 1U == path.Length ? scope : NoId;
    }
    if (path.Data[cursor] == 0x2EU)
    {
        segments = 2U;
        ++cursor;
    }
    else if (path.Data[cursor] == 0x2FU)
    {
        ++cursor;
        if (cursor >= path.Length)
        {
            return NoId;
        }
        segments = path.Data[cursor++];
    }
    if (segments == 0U || segments > (path.Length - cursor) / 4U || segments * 4U != path.Length - cursor)
    {
        return NoId;
    }
    if (search && !explicitPath && segments == 1U && !create)
    {
        for (Size depth{0U}; scope != NoId && depth < m_arena.NodeCapacity; ++depth)
        {
            const Id found{Lookup(scope, Segment(path.Data + cursor))};
            if (found != NoId)
            {
                return found;
            }
            scope = m_arena.Nodes[scope].Parent;
        }
        return NoId;
    }
    for (Size i{0U}; i < segments; ++i)
    {
        const UInt32 segment{Segment(path.Data + cursor)};
        scope = create ? AddNode(scope, segment) : Lookup(scope, segment);
        if (scope == NoId)
        {
            return NoId;
        }
        cursor += 4U;
    }
    return scope;
}

Id Library::FollowAlias(Id node) noexcept
{
    for (Size depth{0U}; node != NoId && depth < m_arena.NodeCapacity; ++depth)
    {
        if (m_arena.Nodes[node].Kind != NodeKind::Alias)
        {
            return node;
        }
        if (!TickLoad())
        {
            return NoId;
        }
        const Syntax& alias{m_arena.Code[m_arena.Nodes[node].Definition]};
        const Syntax& source{m_arena.Code[alias.First]};
        node = Resolve(source.Span, source.Scope, false, true);
    }
    if (node != NoId)
    {
        Diagnose(Reason::Cycle, node);
    }
    return NoId;
}

Id Library::Declare(const Id ast, const NodeKind kind) noexcept
{
    Syntax& code{m_arena.Code[ast]};
    Id name{code.First};
    if (kind == NodeKind::Alias)
    {
        name = m_arena.Code[name].Next;
    }
    else if (kind == NodeKind::BufferField)
    {
        while (m_arena.Code[name].Next != NoId)
        {
            name = m_arena.Code[name].Next;
        }
    }
    const Syntax& path{m_arena.Code[name]};
    Id node{Resolve(path.Span, path.Scope, false, kind == NodeKind::Scope)};
    if (node == NoId)
    {
        node = Resolve(path.Span, path.Scope, true, false);
    }
    if (node == NoId)
    {
        return NoId;
    }
    Node& target{m_arena.Nodes[node]};
    if (kind == NodeKind::Scope)
    {
        code.Binding = node;
        if (!target.Defined && target.Definition == NoId)
        {
            target.Definition = ast;
        }
        return node;
    }
    if (kind == NodeKind::External)
    {
        code.Binding = node;
        if (!target.Defined)
        {
            target.Kind = kind;
            const Id type{m_arena.Code[name].Next};
            target.ExternalType = static_cast<UInt8>(m_arena.Code[type].Number);
            const Id arguments{m_arena.Code[type].Next};
            target.Flags = static_cast<UInt8>(m_arena.Code[arguments].Number);
            if (target.Flags > 7U || target.ExternalType > 16U)
            {
                Diagnose(Reason::Malformed, node, code.Opcode);
                target.Invalid = true;
            }
        }
        return node;
    }
    if (target.Defined)
    {
        Diagnose(Reason::Conflict, node, code.Opcode);
        code.Ignored = true;
        return NoId;
    }
    target.Kind = kind;
    target.Defined = true;
    target.Definition = ast;
    target.Table = code.Table;
    code.Binding = node;
    if (kind == NodeKind::Method)
    {
        target.Flags = static_cast<UInt8>(m_arena.Code[m_arena.Code[name].Next].Number);
    }
    return node;
}

Id Library::Find(const char* const path, Id scope) noexcept
{
    if (path == nullptr || scope == NoId || scope >= m_used.Nodes)
    {
        return NoId;
    }
    Size cursor{0U};
    bool search{true};
    if (path[cursor] == '\\')
    {
        scope = 1U;
        ++cursor;
        search = false;
    }
    while (path[cursor] == '^')
    {
        scope = m_arena.Nodes[scope].Parent;
        if (scope == NoId)
        {
            return NoId;
        }
        ++cursor;
        search = false;
    }
    for (Size length{0U}; length < MaximumPathBytes; ++length)
    {
        const char byte{path[cursor + length]};
        if (byte == '.')
        {
            search = false;
            break;
        }
        if (byte == '\0')
        {
            break;
        }
    }
    if (path[cursor] == '\0')
    {
        return scope;
    }
    for (Size depth{0U}; depth < MaximumParseDepth; ++depth)
    {
        UInt32 segment{0U};
        Size count{0U};
        while (count < 4U && path[cursor] != '\0' && path[cursor] != '.')
        {
            const char byte{path[cursor++]};
            if (!((byte >= 'A' && byte <= 'Z') || byte == '_' || (count != 0U && byte >= '0' && byte <= '9')))
            {
                return NoId;
            }
            segment |= static_cast<UInt32>(static_cast<UInt8>(byte)) << (count++ * 8U);
        }
        if (count == 0U)
        {
            return NoId;
        }
        while (count < 4U)
        {
            segment |= static_cast<UInt32>('_') << (count++ * 8U);
        }
        if (search)
        {
            Id found{NoId};
            for (Size parents{0U}; scope != NoId && parents < m_arena.NodeCapacity; ++parents)
            {
                found = Lookup(scope, segment);
                if (found != NoId)
                {
                    break;
                }
                scope = m_arena.Nodes[scope].Parent;
            }
            scope = found;
            search = false;
        }
        else
        {
            scope = Lookup(scope, segment);
        }
        if (scope == NoId)
        {
            return NoId;
        }
        if (path[cursor] == '\0')
        {
            return FollowAlias(scope);
        }
        if (path[cursor] != '.')
        {
            return NoId;
        }
        ++cursor;
    }
    return NoId;
}

Id Library::ResolveText(const Bytes path, const Id scope) noexcept
{
    if (path.Length >= MaximumPathBytes)
    {
        static_cast<void>(Unknown(Reason::Bounds));
        return NoId;
    }
    for (Size i{0U}; i < path.Length; ++i)
    {
        if (path.Data[i] > 0x7FU || path.Data[i] == 0U)
        {
            static_cast<void>(Unknown(Reason::Malformed));
            return NoId;
        }
        m_arena.TextPath[i] = static_cast<char>(path.Data[i]);
    }
    m_arena.TextPath[path.Length] = '\0';
    return Find(m_arena.TextPath, scope);
}

Id Library::Child(const Id parent, const char* const segment) const noexcept
{
    if (parent == NoId || parent >= m_used.Nodes || segment == nullptr)
    {
        return NoId;
    }
    UInt32 encoded{0U};
    Size i{0U};
    while (i < 4U && segment[i] != '\0')
    {
        encoded |= static_cast<UInt32>(static_cast<UInt8>(segment[i])) << (i * 8U);
        ++i;
    }
    while (i < 4U)
    {
        encoded |= static_cast<UInt32>('_') << (i++ * 8U);
    }
    return Lookup(parent, encoded);
}

bool Library::Path(Id node, char* const output, const Size capacity) const noexcept
{
    if (node == NoId || node >= m_used.Nodes || output == nullptr)
    {
        return false;
    }
    Size depth{0U};
    for (Id cursor{node}; cursor != 1U && cursor != NoId && depth < MaximumParseDepth;
         cursor = m_arena.Nodes[cursor].Parent)
    {
        ++depth;
    }
    const Size length{depth == 0U ? 1U : depth * 5U};
    if (depth >= MaximumParseDepth || capacity <= length)
    {
        return false;
    }
    output[0U] = '\\';
    output[length] = '\0';
    while (depth != 0U)
    {
        const UInt32 segment{m_arena.Nodes[node].Segment};
        const Size offset{1U + (depth - 1U) * 5U};
        for (Size i{0U}; i < 4U; ++i)
        {
            output[offset + i] = static_cast<char>((segment >> (i * 8U)) & 0x7FU);
        }
        if (depth > 1U)
        {
            output[offset - 1U] = '.';
        }
        node = m_arena.Nodes[node].Parent;
        --depth;
    }
    return true;
}

const Node* Library::GetNode(const Id node) const noexcept
{
    return node != NoId && node < m_used.Nodes ? &m_arena.Nodes[node] : nullptr;
}
const Syntax* Library::GetSyntax(const Id syntax) const noexcept
{
    return syntax != NoId && syntax < m_used.SyntaxObjects ? &m_arena.Code[syntax] : nullptr;
}
Bytes Library::Table(const Size index) const noexcept
{
    return index < m_tables ? m_arena.Tables[index] : Bytes{};
}
const Budgets& Library::Used() const noexcept
{
    return m_used;
}
Size Library::DiagnosticCount() const noexcept
{
    return m_diagnostics;
}
const Diagnostic* Library::GetDiagnostic(const Size index) const noexcept
{
    return index < m_diagnostics ? &m_arena.Diagnostics[index] : nullptr;
}
Size Library::EffectCount() const noexcept
{
    return m_effects;
}
const Effect* Library::GetEffect(const Size index) const noexcept
{
    return index < m_effects ? &m_arena.Effects[index] : nullptr;
}
Size Library::OsiQueryCount() const noexcept
{
    return m_queries;
}
Bytes Library::OsiQuery(const Size index) const noexcept
{
    return index < m_queries ? Data(m_arena.OsiQueries[index]) : Bytes{};
}
void Library::Diagnose(const Reason reason, const Id node, const UInt16 opcode) noexcept
{
    m_capacityFailed = m_capacityFailed || reason == Reason::Capacity;
    if (m_diagnostics < MaximumDiagnostics)
    {
        m_arena.Diagnostics[m_diagnostics++] = {reason, node, m_table, m_offset, opcode};
    }
    else
    {
        ++m_used.DroppedDiagnostics;
    }
}

bool Library::TickLoad() noexcept
{
    if (m_used.LoadOperations >= MaximumLoadOperations)
    {
        m_error = Reason::Budget;
        return false;
    }
    ++m_used.LoadOperations;
    return true;
}

const char* ReasonText(const Reason reason) noexcept
{
    switch (reason)
    {
    case Reason::None:
        return "known";
    case Reason::Malformed:
        return "malformed";
    case Reason::Truncated:
        return "truncated";
    case Reason::Checksum:
        return "checksum";
    case Reason::Conflict:
        return "conflict";
    case Reason::Unresolved:
        return "unresolved";
    case Reason::Capacity:
        return "capacity";
    case Reason::ParseDepth:
        return "parse-depth";
    case Reason::EvaluationDepth:
        return "evaluation-depth";
    case Reason::CallDepth:
        return "call-depth";
    case Reason::Budget:
        return "budget";
    case Reason::Cycle:
        return "cycle";
    case Reason::RegionAccess:
        return "region-access";
    case Reason::Unsupported:
        return "unsupported";
    case Reason::Type:
        return "type";
    case Reason::Bounds:
        return "bounds";
    case Reason::DivideByZero:
        return "divide-by-zero";
    case Reason::Uninitialized:
        return "uninitialized";
    case Reason::Missing:
        return "missing";
    }
    return "invalid-reason";
}
const char* KindText(const NodeKind kind) noexcept
{
    switch (kind)
    {
    case NodeKind::Scope:
        return "Scope";
    case NodeKind::Device:
        return "Device";
    case NodeKind::Processor:
        return "Processor";
    case NodeKind::PowerResource:
        return "PowerResource";
    case NodeKind::ThermalZone:
        return "ThermalZone";
    case NodeKind::Name:
        return "Name";
    case NodeKind::Method:
        return "Method";
    case NodeKind::OperationRegion:
        return "OperationRegion";
    case NodeKind::Field:
        return "Field";
    case NodeKind::IndexField:
        return "IndexField";
    case NodeKind::BankField:
        return "BankField";
    case NodeKind::Alias:
        return "Alias";
    case NodeKind::Mutex:
        return "Mutex";
    case NodeKind::Event:
        return "Event";
    case NodeKind::External:
        return "External";
    case NodeKind::BufferField:
        return "BufferField";
    }
    return "invalid-kind";
}
} // namespace Asteria::Acpi::Aml
