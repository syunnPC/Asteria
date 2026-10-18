// SPDX-License-Identifier: Apache-2.0

#include "Acpi/Aml/Library.hpp"

#include "Acpi/Aml/Grammar.hpp"

namespace Asteria::Acpi::Aml
{
namespace
{
NodeKind DeclarationKind(const UInt16 opcode) noexcept
{
    switch (opcode)
    {
    case 0x06U:
        return NodeKind::Alias;
    case 0x08U:
        return NodeKind::Name;
    case 0x10U:
        return NodeKind::Scope;
    case 0x14U:
        return NodeKind::Method;
    case 0x15U:
        return NodeKind::External;
    case 0x5B01U:
        return NodeKind::Mutex;
    case 0x5B02U:
        return NodeKind::Event;
    case 0x5B82U:
        return NodeKind::Device;
    case 0x5B83U:
        return NodeKind::Processor;
    case 0x5B84U:
        return NodeKind::PowerResource;
    case 0x5B85U:
        return NodeKind::ThermalZone;
    case 0x8AU:
    case 0x8BU:
    case 0x8CU:
    case 0x8DU:
    case 0x8FU:
    case 0x5B13U:
        return NodeKind::BufferField;
    default:
        return NodeKind::OperationRegion;
    }
}
bool IsDeclaration(const UInt16 opcode) noexcept
{
    return opcode == 0x06U || opcode == 0x08U || opcode == 0x10U || opcode == 0x14U || opcode == 0x15U ||
           opcode == 0x5B01U || opcode == 0x5B02U || opcode == 0x5B80U || opcode == 0x5B88U ||
           (opcode >= 0x5B82U && opcode <= 0x5B85U) || opcode == 0x8AU || opcode == 0x8BU || opcode == 0x8CU ||
           opcode == 0x8DU || opcode == 0x8FU || opcode == 0x5B13U;
}
} // namespace

Id Library::AddSyntax(const UInt16 opcode, const Size offset) noexcept
{
    if (m_used.SyntaxObjects >= m_arena.SyntaxLimit)
    {
        m_error = Reason::Capacity;
        return NoId;
    }
    const Id ast{static_cast<Id>(m_used.SyntaxObjects++)};
    m_arena.Code[ast] = {};
    m_arena.Code[ast].Opcode = opcode;
    m_arena.Code[ast].Table = m_table;
    m_arena.Code[ast].Offset = m_offset + static_cast<UInt32>(offset);
    return ast;
}

bool Library::ReadInteger(const Size width, UInt64& output) noexcept
{
    Size end{m_input.Length};
    if (m_parseDepth != 0U)
    {
        end = m_arena.Parse[m_parseDepth - 1U].End;
    }
    if (m_cursor > end || width > end - m_cursor || width > 8U)
    {
        m_error = Reason::Truncated;
        return false;
    }
    output = 0U;
    for (Size i{0U}; i < width; ++i)
    {
        output |= static_cast<UInt64>(m_input.Data[m_cursor++]) << (i * 8U);
    }
    return true;
}

bool Library::ReadLength(UInt32& output) noexcept
{
    UInt64 lead{0U};
    if (!ReadInteger(1U, lead))
    {
        return false;
    }
    const Size following{static_cast<Size>(lead >> 6U)};
    output = static_cast<UInt32>(lead & (following == 0U ? 0x3FU : 0x0FU));
    if (following != 0U && (lead & 0x30U) != 0U)
    {
        m_error = Reason::Malformed;
        return false;
    }
    UInt64 tail{0U};
    if (!ReadInteger(following, tail))
    {
        return false;
    }
    output |= static_cast<UInt32>(tail << 4U);
    return true;
}

bool Library::ReadName(Bytes& output) noexcept
{
    const Size start{m_cursor};
    UInt64 lead{0U};
    if (!ReadInteger(1U, lead))
    {
        return false;
    }
    if (lead == '\\')
    {
        if (!ReadInteger(1U, lead))
        {
            return false;
        }
    }
    else
    {
        Size parents{0U};
        while (lead == '^')
        {
            if (++parents > MaximumParseDepth || !ReadInteger(1U, lead))
            {
                m_error = Reason::Malformed;
                return false;
            }
        }
    }
    Size count{1U};
    if (lead == 0U)
    {
        count = 0U;
    }
    else if (lead == 0x2EU)
    {
        count = 2U;
    }
    else if (lead == 0x2FU)
    {
        if (!ReadInteger(1U, lead) || lead == 0U)
        {
            m_error = Reason::Malformed;
            return false;
        }
        count = static_cast<Size>(lead);
    }
    else
    {
        --m_cursor;
    }
    for (Size i{0U}; i < count; ++i)
    {
        for (Size byte{0U}; byte < 4U; ++byte)
        {
            if (!ReadInteger(1U, lead))
            {
                return false;
            }
            if (!((lead >= 'A' && lead <= 'Z') || lead == '_' || (byte != 0U && lead >= '0' && lead <= '9')))
            {
                m_error = Reason::Malformed;
                return false;
            }
        }
    }
    output = {m_input.Data + start, m_cursor - start};
    return true;
}

bool Library::PushParse(const Id ast, const char* const shape, const Size end, const Id scope) noexcept
{
    if (m_parseDepth >= MaximumParseDepth)
    {
        m_error = Reason::ParseDepth;
        return false;
    }
    m_arena.Parse[m_parseDepth++] = {shape, end, ast, NoId, scope, 0U, m_method};
    if (m_parseDepth > m_used.ParseDepth)
    {
        m_used.ParseDepth = m_parseDepth;
    }
    return true;
}

void Library::Attach(ParseFrame& frame, const Id child) noexcept
{
    if (frame.Last == NoId)
    {
        m_arena.Code[frame.Ast].First = child;
    }
    else
    {
        m_arena.Code[frame.Last].Next = child;
    }
    frame.Last = child;
}

bool Library::BeginTerm(Id& output, const bool reference, const bool dataOnly) noexcept
{
    const Size start{m_cursor};
    const Size end{m_arena.Parse[m_parseDepth - 1U].End};
    const Id scope{m_arena.Parse[m_parseDepth - 1U].Scope};
    UInt64 lead{0U};
    if (!ReadInteger(1U, lead))
    {
        return false;
    }
    if (NameLead(static_cast<UInt8>(lead)))
    {
        --m_cursor;
        output = AddSyntax(NamePathOp, start);
        if (output == NoId || !ReadName(m_arena.Code[output].Span))
        {
            return false;
        }
        Syntax& ast{m_arena.Code[output]};
        ast.Scope = scope;
        ast.Reference = reference || dataOnly;
        const Id node{FollowAlias(Resolve(ast.Span, scope, false, true))};
        // Method-created Names shadow ancestor methods even before execution.
        // Reuse the bounded dynamic-name scratch during parsing; Run resets it.
        bool local{false};
        if (m_method && ast.Span.Length == 4U)
        {
            for (Size i{0U}; i < m_dynamicCount; ++i)
            {
                local = local ||
                        (m_arena.Dynamic[i].Scope == scope && m_arena.Dynamic[i].Segment == Segment(ast.Span.Data));
            }
        }
        if (!ast.Reference && !local && node != NoId &&
            (m_arena.Nodes[node].Kind == NodeKind::Method ||
             (m_arena.Nodes[node].Kind == NodeKind::External && m_arena.Nodes[node].ExternalType == 8U)))
        {
            ast.Opcode = InvokeOp;
            const Size count{m_arena.Nodes[node].Flags & 7U};
            constexpr const char* arguments[]{"", "e", "ee", "eee", "eeee", "eeeee", "eeeeee", "eeeeeee"};
            return PushParse(output, arguments[count], end, scope);
        }
        return true;
    }
    UInt16 opcode{static_cast<UInt16>(lead)};
    if (opcode == 0x5BU)
    {
        if (!ReadInteger(1U, lead))
        {
            return false;
        }
        opcode = static_cast<UInt16>(0x5B00U | lead);
    }
    output = AddSyntax(opcode, start);
    if (output == NoId)
    {
        return false;
    }
    Syntax& ast{m_arena.Code[output]};
    ast.Scope = scope;
    ast.Reference = reference;
    if (opcode == 0x00U || opcode == 0x01U || opcode == 0xFFU)
    {
        ast.Opcode = LiteralOp;
        ast.Number = opcode == 0xFFU ? ~UInt64{0U} : opcode;
        return true;
    }
    if (opcode == 0x0AU || opcode == 0x0BU || opcode == 0x0CU || opcode == 0x0EU)
    {
        ast.Opcode = LiteralOp;
        const Size width{opcode == 0x0EU ? 8U : Size{1U} << (opcode - 0x0AU)};
        return ReadInteger(width, ast.Number);
    }
    if (opcode == 0x0DU)
    {
        const Size begin{m_cursor};
        do
        {
            if (!ReadInteger(1U, lead))
            {
                return false;
            }
            if (lead > 0x7FU)
            {
                m_error = Reason::Malformed;
                return false;
            }
        } while (lead != 0U);
        ast.Span = {m_input.Data + begin, m_cursor - begin - 1U};
        return true;
    }
    if (opcode >= 0x60U && opcode <= 0x6EU)
    {
        return true;
    }
    const char* const shape{ArgumentShape(opcode)};
    if (shape == nullptr)
    {
        m_error = Reason::Malformed;
        return false;
    }
    Size termEnd{end};
    if (HasPackageLength(opcode))
    {
        const Size lengthStart{m_cursor};
        UInt32 length{0U};
        if (!ReadLength(length))
        {
            return false;
        }
        if (length < m_cursor - lengthStart || length > end - lengthStart)
        {
            m_error = length > end - lengthStart ? Reason::Truncated : Reason::Malformed;
            return false;
        }
        termEnd = lengthStart + length;
    }
    return PushParse(output, shape, termEnd, scope);
}

bool Library::FieldList(ParseFrame& frame) noexcept
{
    const UInt16 opcode{m_arena.Code[frame.Ast].Opcode};
    UInt64 bitOffset{m_arena.Code[frame.Ast].Number};
    while (m_cursor < frame.End)
    {
        if (!TickLoad())
        {
            return false;
        }
        const Size start{m_cursor};
        UInt64 lead{0U};
        if (!ReadInteger(1U, lead))
        {
            return false;
        }
        if (lead == 1U || lead == 3U)
        {
            UInt64 flags{0U};
            if (!ReadInteger(lead == 1U ? 2U : 3U, flags))
            {
                return false;
            }
        }
        else if (lead == 2U)
        {
            Id connection{NoId};
            if (!BeginTerm(connection, true, true))
            {
                return false;
            }
            Attach(frame, connection);
            // Buffer connection parsing resumes this FieldList frame. Its
            // bit offset is retained in the parent syntax record.
            m_arena.Code[frame.Ast].Number = bitOffset;
            if (m_parseDepth != 0U && m_arena.Parse[m_parseDepth - 1U].Ast != frame.Ast)
            {
                return true;
            }
        }
        else
        {
            Id unit{NoId};
            if (lead != 0U)
            {
                --m_cursor;
                unit = AddSyntax(NamePathOp, start);
                if (unit == NoId)
                {
                    return false;
                }
                // NamedField contains exactly a NameSeg, not a NameString.
                UInt64 name{0U};
                if (!ReadInteger(4U, name))
                {
                    return false;
                }
                for (Size i{0U}; i < 4U; ++i)
                {
                    const UInt8 byte{static_cast<UInt8>((name >> (i * 8U)) & 0xFFU)};
                    if (!((byte >= 'A' && byte <= 'Z') || byte == '_' || (i != 0U && byte >= '0' && byte <= '9')))
                    {
                        m_error = Reason::Malformed;
                        return false;
                    }
                }
                Syntax& code{m_arena.Code[unit]};
                code.Span = {m_input.Data + start, 4U};
                code.Scope = frame.Scope;
                code.Number = bitOffset;
                Attach(frame, unit);
                if (!m_method)
                {
                    const Id node{Resolve(code.Span, frame.Scope, true, false)};
                    if (node != NoId)
                    {
                        Node& target{m_arena.Nodes[node]};
                        if (target.Defined)
                        {
                            Diagnose(Reason::Conflict, node, opcode);
                        }
                        else
                        {
                            target.Defined = true;
                            target.Definition = frame.Ast;
                            target.Kind = opcode == 0x5B81U   ? NodeKind::Field
                                          : opcode == 0x5B86U ? NodeKind::IndexField
                                                              : NodeKind::BankField;
                            target.Table = m_table;
                            code.Binding = node;
                        }
                    }
                }
            }
            UInt32 bits{0U};
            if (!ReadLength(bits) || bitOffset > ~UInt64{0U} - bits)
            {
                m_error = Reason::Malformed;
                return false;
            }
            if (unit != NoId && m_arena.Code[unit].Binding != NoId)
            {
                Node& field{m_arena.Nodes[m_arena.Code[unit].Binding]};
                field.BitOffset = bitOffset;
                field.BitLength = bits;
                field.Body = {m_input.Data + start, m_cursor - start};
            }
            bitOffset += bits;
        }
    }
    return true;
}

bool Library::ParseStep() noexcept
{
    ParseFrame& frame{m_arena.Parse[m_parseDepth - 1U]};
    Syntax& ast{m_arena.Code[frame.Ast]};
    if (m_cursor > frame.End)
    {
        m_error = Reason::Truncated;
        return false;
    }
    const char argument{frame.Shape[frame.Position]};
    if (argument == '\0')
    {
        return CompleteParse();
    }
    if (argument == 'l' && ast.Opcode != ListOp)
    {
        if (!m_method && IsDeclaration(ast.Opcode))
        {
            const Id node{Declare(frame.Ast, DeclarationKind(ast.Opcode))};
            if (node != NoId)
            {
                frame.Scope = node;
            }
            if (ast.Opcode == 0x14U)
            {
                if (node != NoId)
                {
                    m_arena.Nodes[node].Body = {m_input.Data + m_cursor, frame.End - m_cursor};
                }
                m_cursor = frame.End;
                ++frame.Position;
                return true;
            }
        }
        const Id body{AddSyntax(ListOp, m_cursor)};
        if (body == NoId)
        {
            return false;
        }
        m_arena.Code[body].Scope = frame.Scope;
        Attach(frame, body);
        ++frame.Position;
        return PushParse(body, "l", frame.End, frame.Scope);
    }
    if (argument == 'l' || argument == 'k')
    {
        if (m_cursor == frame.End)
        {
            ++frame.Position;
            return true;
        }
        Id child{NoId};
        const bool good{BeginTerm(child, false, argument == 'k')};
        if (good)
        {
            Attach(frame, child);
        }
        return good;
    }
    if (argument == 'r')
    {
        ast.Span = {m_input.Data + m_cursor, frame.End - m_cursor};
        m_cursor = frame.End;
        ++frame.Position;
        return true;
    }
    if (argument == 'f')
    {
        if (!FieldList(frame))
        {
            return false;
        }
        if (m_arena.Parse[m_parseDepth - 1U].Ast == frame.Ast && m_cursor == frame.End)
        {
            ++frame.Position;
        }
        return true;
    }
    ++frame.Position;
    Id child{NoId};
    if (argument == 'n')
    {
        child = AddSyntax(NamePathOp, m_cursor);
        if (child == NoId || !ReadName(m_arena.Code[child].Span))
        {
            return false;
        }
        m_arena.Code[child].Scope = frame.Scope;
        m_arena.Code[child].Reference = true;
    }
    else if (argument == 'b' || argument == 'w' || argument == 'd')
    {
        child = AddSyntax(LiteralOp, m_cursor);
        const Size width{argument == 'b' ? 1U : argument == 'w' ? 2U : 4U};
        if (child == NoId || !ReadInteger(width, m_arena.Code[child].Number))
        {
            return false;
        }
    }
    else if (!BeginTerm(child, argument == 't', argument == 'q'))
    {
        return false;
    }
    Attach(frame, child);
    return true;
}

bool Library::CompleteParse() noexcept
{
    const ParseFrame& frame{m_arena.Parse[m_parseDepth - 1U]};
    Syntax& ast{m_arena.Code[frame.Ast]};
    if (HasPackageLength(ast.Opcode) && m_cursor != frame.End)
    {
        m_error = Reason::Malformed;
        return false;
    }
    if (m_method && (ast.Opcode == 0x08U || DeclarationKind(ast.Opcode) == NodeKind::BufferField))
    {
        Id name{ast.First};
        if (ast.Opcode != 0x08U)
        {
            while (m_arena.Code[name].Next != NoId)
            {
                name = m_arena.Code[name].Next;
            }
        }
        const Syntax& path{m_arena.Code[name]};
        if (path.Span.Length == 4U)
        {
            if (m_dynamicCount == MaximumDynamicNames)
            {
                m_error = Reason::Capacity;
                return false;
            }
            m_arena.Dynamic[m_dynamicCount++] = {0U, path.Scope, Segment(path.Span.Data), NoId};
        }
    }
    if (!m_method && IsDeclaration(ast.Opcode) && ast.Binding == NoId && ast.Opcode != 0x10U && ast.Opcode != 0x14U &&
        !(ast.Opcode >= 0x5B82U && ast.Opcode <= 0x5B85U))
    {
        static_cast<void>(Declare(frame.Ast, DeclarationKind(ast.Opcode)));
    }
    if (IsDeclaration(ast.Opcode) || ast.Opcode == 0x5B81U || ast.Opcode == 0x5B86U || ast.Opcode == 0x5B87U)
    {
        const Size start{ast.Offset - m_offset};
        ast.Span = {m_input.Data + start, m_cursor - start};
        // A later External or reopened Scope borrows a binding; it must not
        // replace the defining object's initializer/body or source table.
        if (ast.Binding != NoId && ast.Opcode != 0x14U && m_arena.Nodes[ast.Binding].Definition == frame.Ast)
        {
            m_arena.Nodes[ast.Binding].Body = ast.Span;
        }
    }
    --m_parseDepth;
    return true;
}

Id Library::Parse(const Bytes bytes, const Id scope, const bool method, const UInt32 table,
                  const UInt32 offset) noexcept
{
    m_input = bytes;
    m_cursor = 0U;
    m_table = table;
    m_offset = offset;
    m_method = method;
    m_error = Reason::None;
    m_parseDepth = 0U;
    m_dynamicCount = 0U;
    const Id root{AddSyntax(ListOp, 0U)};
    if (root == NoId || !PushParse(root, "l", bytes.Length, scope))
    {
        Diagnose(m_error, scope);
        return NoId;
    }
    m_arena.Code[root].Scope = scope;
    while (m_parseDepth != 0U)
    {
        if (!TickLoad() || !ParseStep())
        {
            m_offset = offset + static_cast<UInt32>(m_cursor);
            Diagnose(m_error, scope);
            m_parseDepth = 0U;
            return NoId;
        }
    }
    return root;
}

bool Library::Load(const Bytes table) noexcept
{
    if (m_finished || table.Data == nullptr || table.Length < 36U || table.Length > MaximumTableBytes)
    {
        Diagnose(Reason::Malformed);
        return false;
    }
    if (m_tables >= MaximumTables)
    {
        Diagnose(Reason::Capacity);
        return false;
    }
    const bool dsdt{table.Data[0U] == 'D' && table.Data[1U] == 'S' && table.Data[2U] == 'D' && table.Data[3U] == 'T'};
    const bool ssdt{table.Data[0U] == 'S' && table.Data[1U] == 'S' && table.Data[2U] == 'D' && table.Data[3U] == 'T'};
    if ((!dsdt && !ssdt) || Segment(table.Data + 4U) != table.Length || table.Data[8U] == 0U)
    {
        Diagnose(Reason::Malformed);
        return false;
    }
    UInt8 sum{0U};
    for (Size i{0U}; i < table.Length; ++i)
    {
        // ACPI checksum addition intentionally wraps modulo 256.
        sum = static_cast<UInt8>((static_cast<UInt32>(sum) + table.Data[i]) & 0xFFU);
    }
    if (sum != 0U)
    {
        Diagnose(Reason::Checksum);
        return false;
    }
    const UInt32 index{static_cast<UInt32>(m_tables++)};
    m_arena.Tables[index] = table;
    m_arena.Roots[index] = Parse({table.Data + 36U, table.Length - 36U}, 1U, false, index, 36U);
    m_arena.TableErrors[index] = m_arena.Roots[index] == NoId ? m_error : Reason::None;
    return m_arena.Roots[index] != NoId;
}

bool Library::FinishLoading() noexcept
{
    if (m_finished)
    {
        return m_error == Reason::None;
    }
    bool good{true};
    // Scope placeholders retain their descendants until later tables define
    // the target. A lone relative NameSeg may instead bind in an ancestor.
    for (Id i{2U}; i < m_used.Nodes; ++i)
    {
        Node& node{m_arena.Nodes[i]};
        if (!node.Defined && node.Kind == NodeKind::Scope && node.Definition != NoId)
        {
            const Syntax& path{m_arena.Code[m_arena.Code[node.Definition].First]};
            Id ancestor{m_arena.Nodes[node.Parent].Parent};
            Id resolved{Resolve(path.Span, ancestor, false, true)};
            if (resolved != NoId && m_arena.Nodes[resolved].Defined)
            {
                for (Id child{2U}; child < m_used.Nodes; ++child)
                {
                    if (m_arena.Nodes[child].Parent == i)
                    {
                        m_arena.Nodes[child].Parent = resolved;
                    }
                }
                for (Id ast{1U}; ast < m_used.SyntaxObjects; ++ast)
                {
                    if (m_arena.Code[ast].Scope == i)
                    {
                        m_arena.Code[ast].Scope = resolved;
                    }
                }
                node.Defined = true;
                node.Invalid = true;
            }
            else
            {
                Diagnose(Reason::Unresolved, i);
                good = false;
            }
        }
    }
    // Rebuild after any deferred Scope reparenting. Table order determines
    // the first definition when two children now have the same full path.
    for (Size i{0U}; i < NamespaceHashSlots; ++i)
    {
        m_arena.Hash[i] = NoId;
    }
    for (Id i{2U}; i < m_used.Nodes; ++i)
    {
        if (!TickLoad())
        {
            Diagnose(Reason::Budget);
            good = false;
            break;
        }
        const Node& node{m_arena.Nodes[i]};
        const Id prior{Lookup(node.Parent, node.Segment)};
        if (prior != NoId)
        {
            m_arena.Nodes[i].Invalid = true;
            Diagnose(Reason::Conflict, i);
            continue;
        }
        // The hash table has at least twice the maximum node count.
        Size slot{((node.Parent * 0x9E3779B9U) ^ (node.Segment * 0x85EBCA6BU)) & (NamespaceHashSlots - 1U)};
        Size probe{0U};
        for (; probe < NamespaceHashSlots; ++probe)
        {
            if (m_arena.Hash[slot] == NoId)
            {
                break;
            }
            slot = (slot + 1U) & (NamespaceHashSlots - 1U);
        }
        if (probe == NamespaceHashSlots)
        {
            Diagnose(Reason::Capacity, i);
            good = false;
            break;
        }
        m_arena.Hash[slot] = i;
    }
    for (Id i{2U}; i < m_used.Nodes; ++i)
    {
        Node& node{m_arena.Nodes[i]};
        if (node.Kind == NodeKind::Method && !node.Builtin && !node.Invalid)
        {
            const Bytes table{m_arena.Tables[node.Table]};
            // Body always borrows a checked subspan of this table.
            const UInt32 offset{static_cast<UInt32>(node.Body.Data - table.Data)};
            node.MethodBody = Parse(node.Body, i, true, node.Table, offset);
            if (node.MethodBody == NoId)
            {
                node.Invalid = true;
                good = false;
            }
        }
        if (node.Kind == NodeKind::Alias && FollowAlias(i) == NoId)
        {
            Diagnose(Reason::Unresolved, i);
            node.Invalid = true;
            good = false;
        }
        if (node.Kind == NodeKind::External && !node.Defined)
        {
            Diagnose(Reason::Unresolved, i);
        }
    }
    m_finished = true;
    return LoadModules() && good;
}
} // namespace Asteria::Acpi::Aml
