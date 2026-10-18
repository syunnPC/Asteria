// SPDX-License-Identifier: Apache-2.0

#include "Acpi/Aml/Library.hpp"

namespace Asteria::Acpi::Aml
{
Id Library::AddObject(const ObjectKind kind) noexcept
{
    if (m_used.Objects >= m_arena.ObjectLimit)
    {
        m_error = Reason::Capacity;
        return NoId;
    }
    const Id object{static_cast<Id>(m_used.Objects++)};
    m_arena.Objects[object] = {};
    m_arena.Objects[object].Kind = kind;
    m_arena.Objects[object].Error = Reason::None;
    return object;
}

Id Library::Unknown(const Reason reason, const Id ast) noexcept
{
    if (m_error == Reason::None)
    {
        m_error = reason;
        if (ast != NoId && ast < m_used.SyntaxObjects)
        {
            m_table = m_arena.Code[ast].Table;
            m_offset = m_arena.Code[ast].Offset;
            Diagnose(reason, m_arena.Code[ast].Scope, m_arena.Code[ast].Opcode);
        }
        else
        {
            Diagnose(reason);
        }
    }
    const Id value{AddObject(ObjectKind::Unknown)};
    if (value != NoId)
    {
        m_arena.Objects[value].Error = reason;
    }
    return value;
}

Id Library::Integer(const UInt64 value) noexcept
{
    const Id object{AddObject(ObjectKind::Integer)};
    if (object != NoId)
    {
        m_arena.Objects[object].Integer = value;
    }
    return object;
}

Id Library::MakeBytes(const ObjectKind kind, const Size length) noexcept
{
    if (length > MaximumObjectBytes || length > m_arena.ByteLimit - m_used.BytesUsed)
    {
        return Unknown(Reason::Capacity);
    }
    const Id object{AddObject(kind)};
    if (object == NoId)
    {
        return NoId;
    }
    Object& out{m_arena.Objects[object]};
    out.First = static_cast<Id>(m_used.BytesUsed);
    out.Count = static_cast<UInt32>(length);
    for (Size i{0U}; i < length; ++i)
    {
        m_arena.ByteStorage[m_used.BytesUsed + i] = 0U;
    }
    m_used.BytesUsed += length;
    return object;
}

Id Library::String(const Bytes bytes) noexcept
{
    if (bytes.Data == nullptr && bytes.Length != 0U)
    {
        return Unknown(Reason::Malformed);
    }
    const Id object{MakeBytes(ObjectKind::String, bytes.Length)};
    if (GetObject(object).Kind != ObjectKind::String)
    {
        return object;
    }
    const Id offset{m_arena.Objects[object].First};
    for (Size i{0U}; i < bytes.Length; ++i)
    {
        m_arena.ByteStorage[offset + i] = bytes.Data[i];
    }
    return object;
}

Id Library::Package(const Size length) noexcept
{
    if (length > MaximumPackageElements || length > m_arena.ElementLimit - m_used.Elements)
    {
        return Unknown(Reason::Capacity);
    }
    const Id object{AddObject(ObjectKind::Package)};
    if (object == NoId)
    {
        return NoId;
    }
    Object& out{m_arena.Objects[object]};
    out.First = static_cast<Id>(m_used.Elements);
    out.Count = static_cast<UInt32>(length);
    for (Size i{0U}; i < length; ++i)
    {
        m_arena.Elements[m_used.Elements + i] = NoId;
    }
    m_used.Elements += length;
    return object;
}

const Object& Library::GetObject(const Id value) const noexcept
{
    return m_arena.Objects[value < m_used.Objects ? value : NoId];
}
Bytes Library::Data(const Id value) const noexcept
{
    const Object& object{GetObject(value)};
    if ((object.Kind != ObjectKind::String && object.Kind != ObjectKind::Buffer) || object.First > m_used.BytesUsed ||
        object.Count > m_used.BytesUsed - object.First)
    {
        return {};
    }
    return {m_arena.ByteStorage + object.First, object.Count};
}
Id Library::Element(const Id value, const Size index) const noexcept
{
    const Object& object{GetObject(value)};
    if (object.Kind != ObjectKind::Package || index >= object.Count || object.First >= m_used.Elements ||
        index >= m_used.Elements - object.First)
    {
        return NoId;
    }
    return m_arena.Elements[object.First + index];
}

Id Library::Reference(const ReferenceKind kind, const Id first, const UInt32 count, const UInt64 bits) noexcept
{
    const Id result{AddObject(ObjectKind::Reference)};
    if (result != NoId)
    {
        Object& reference{m_arena.Objects[result]};
        reference.Reference = kind;
        reference.First = first;
        reference.Count = count;
        reference.Integer = bits;
    }
    return result;
}

Id Library::ReadReference(const Id id) noexcept
{
    const Object& reference{GetObject(id)};
    if (reference.Kind != ObjectKind::Reference)
    {
        return id;
    }
    switch (reference.Reference)
    {
    case ReferenceKind::Debug:
        return Unknown(Reason::Unsupported);
    case ReferenceKind::Node:
        if (reference.First == NoId || reference.First >= m_used.Nodes)
        {
            return Unknown(Reason::Unresolved);
        }
        switch (m_arena.Nodes[reference.First].Kind)
        {
        case NodeKind::OperationRegion:
        case NodeKind::Field:
        case NodeKind::IndexField:
        case NodeKind::BankField:
            return Unknown(Reason::RegionAccess);
        case NodeKind::Name:
        case NodeKind::BufferField:
            return m_arena.Nodes[reference.First].Value;
        case NodeKind::Scope:
        case NodeKind::Device:
        case NodeKind::Processor:
        case NodeKind::PowerResource:
        case NodeKind::ThermalZone:
        case NodeKind::Method:
        case NodeKind::Alias:
        case NodeKind::Mutex:
        case NodeKind::Event:
        case NodeKind::External:
            return Unknown(Reason::Type);
        }
        break;
    case ReferenceKind::Slot:
        if (reference.First / 15U >= m_callDepth)
        {
            return Unknown(Reason::Bounds);
        }
        return m_arena.Calls[reference.First / 15U].Slots[reference.First % 15U];
    case ReferenceKind::Element:
        if (reference.First >= m_used.Elements)
        {
            return Unknown(Reason::Bounds);
        }
        return m_arena.Elements[reference.First];
    case ReferenceKind::Byte:
        if (reference.First >= m_used.BytesUsed)
        {
            return Unknown(Reason::Bounds);
        }
        return Integer(m_arena.ByteStorage[reference.First]);
    case ReferenceKind::BufferField:
    {
        const Object& buffer{GetObject(reference.First)};
        if (buffer.Kind != ObjectKind::Buffer || reference.Integer > static_cast<UInt64>(buffer.Count) * 8U ||
            reference.Count > static_cast<UInt64>(buffer.Count) * 8U - reference.Integer)
        {
            return Unknown(Reason::Bounds);
        }
        UInt64 value{0U};
        const Id wide{reference.Count > 64U ? MakeBytes(ObjectKind::Buffer, (Size{reference.Count} + 7U) / 8U) : NoId};
        if (reference.Count > 64U && GetObject(wide).Kind != ObjectKind::Buffer)
        {
            return wide;
        }
        for (Size i{0U}; i < reference.Count; ++i)
        {
            if (!Tick())
            {
                return NoId;
            }
            const UInt64 bit{reference.Integer + i};
            const UInt8 byte{m_arena.ByteStorage[buffer.First + static_cast<Size>(bit / 8U)]};
            const UInt8 set{static_cast<UInt8>((byte >> (bit % 8U)) & 1U)};
            if (reference.Count <= 64U)
            {
                value |= static_cast<UInt64>(set) << i;
            }
            else
            {
                m_arena.ByteStorage[GetObject(wide).First + i / 8U] |=
                    static_cast<UInt8>(static_cast<UInt32>(set) << (i % 8U));
            }
        }
        return reference.Count <= 64U ? Integer(value) : wide;
    }
    }
    return Unknown(Reason::Type);
}

Id Library::Value(Id value) noexcept
{
    for (Size depth{0U}; depth < MaximumEvaluationDepth; ++depth)
    {
        if (!Tick())
        {
            return Unknown(Reason::Budget);
        }
        const Object& object{GetObject(value)};
        if (object.Kind != ObjectKind::Reference)
        {
            return value;
        }
        value = ReadReference(value);
        if (m_error != Reason::None)
        {
            return value;
        }
    }
    return Unknown(Reason::Cycle);
}

Id Library::AsInteger(const Id input) noexcept
{
    const Id value{Value(input)};
    const Object& object{GetObject(value)};
    if (object.Kind == ObjectKind::Integer)
    {
        return value;
    }
    if (object.Kind == ObjectKind::Buffer)
    {
        const Bytes bytes{Data(value)};
        if (bytes.Length == 0U)
        {
            return Unknown(Reason::Type);
        }
        UInt64 number{0U};
        for (Size i{0U}; i < bytes.Length && i < 8U; ++i)
        {
            number |= static_cast<UInt64>(bytes.Data[i]) << (i * 8U);
        }
        return Integer(number);
    }
    if (object.Kind == ObjectKind::String)
    {
        const Bytes bytes{Data(value)};
        Size cursor{0U};
        while (cursor < bytes.Length && (bytes.Data[cursor] == ' ' || bytes.Data[cursor] == '\t'))
        {
            ++cursor;
        }
        UInt64 base{10U};
        if (bytes.Length - cursor >= 2U && bytes.Data[cursor] == '0' &&
            (bytes.Data[cursor + 1U] == 'x' || bytes.Data[cursor + 1U] == 'X'))
        {
            base = 16U;
            cursor += 2U;
        }
        const Size firstDigit{cursor};
        UInt64 number{0U};
        for (; cursor < bytes.Length; ++cursor)
        {
            const UInt8 byte{bytes.Data[cursor]};
            UInt64 digit{16U};
            if (byte >= '0' && byte <= '9')
            {
                digit = byte - UInt8{'0'};
            }
            else if (byte >= 'A' && byte <= 'F')
            {
                digit = byte - UInt8{'A'} + 10U;
            }
            else if (byte >= 'a' && byte <= 'f')
            {
                digit = byte - UInt8{'a'} + 10U;
            }
            if (digit >= base)
            {
                return Unknown(Reason::Type);
            }
            if (number > (~UInt64{0U} - digit) / base)
            {
                return Unknown(Reason::Bounds);
            }
            number = number * base + digit;
        }
        if (cursor == firstDigit)
        {
            return Unknown(Reason::Type);
        }
        return Integer(number);
    }
    return Unknown(object.Kind == ObjectKind::Unknown ? object.Error : Reason::Type);
}

Id Library::TargetNode(const Id value) noexcept
{
    const Object& reference{GetObject(value)};
    return reference.Kind == ObjectKind::Reference && reference.Reference == ReferenceKind::Node ? reference.First
                                                                                                 : NoId;
}

bool Library::WriteReference(const Id target, const Id input, const bool copy) noexcept
{
    const Object* referencePointer{&GetObject(target)};
    if (referencePointer->Kind == ObjectKind::Reference && referencePointer->Reference == ReferenceKind::Node &&
        referencePointer->First != NoId && referencePointer->First < m_used.Nodes &&
        m_arena.Nodes[referencePointer->First].Kind == NodeKind::BufferField)
    {
        referencePointer = &GetObject(m_arena.Nodes[referencePointer->First].Value);
    }
    const Object& reference{*referencePointer};
    if (reference.Kind == ObjectKind::Integer && reference.Integer == 0U)
    {
        return true; // NullName target.
    }
    if (reference.Kind != ObjectKind::Reference)
    {
        static_cast<void>(Unknown(Reason::Type));
        return false;
    }
    Id value{input};
    if (copy)
    {
        value = Copy(input);
    }
    switch (reference.Reference)
    {
    case ReferenceKind::Debug:
        // ACPI 6.5 19.6.26: writes are benign without a debugger.
        Record(0x5B31U, target, input);
        return m_error == Reason::None;
    case ReferenceKind::Node:
    {
        if (reference.First == NoId || reference.First >= m_used.Nodes)
        {
            static_cast<void>(Unknown(Reason::Unresolved));
            return false;
        }
        Node& node{m_arena.Nodes[reference.First]};
        if (node.Kind == NodeKind::OperationRegion || node.Kind == NodeKind::Field ||
            node.Kind == NodeKind::IndexField || node.Kind == NodeKind::BankField)
        {
            static_cast<void>(Unknown(Reason::RegionAccess));
            return false;
        }
        if (node.Kind != NodeKind::Name || reference.First == m_os || reference.First == m_rev)
        {
            static_cast<void>(Unknown(Reason::Type));
            return false;
        }
        if (!copy && node.Value != NoId)
        {
            value = ConvertForStore(input, GetObject(node.Value).Kind);
        }
        if (!Save(JournalKind::Node, reference.First, node.Value))
        {
            return false;
        }
        node.Value = value;
        return m_error == Reason::None;
    }
    case ReferenceKind::Slot:
        if (reference.First / 15U >= m_callDepth)
        {
            static_cast<void>(Unknown(Reason::Bounds));
            return false;
        }
        m_arena.Calls[reference.First / 15U].Slots[reference.First % 15U] = value;
        return m_error == Reason::None;
    case ReferenceKind::Element:
        if (reference.First >= m_used.Elements)
        {
            static_cast<void>(Unknown(Reason::Bounds));
            return false;
        }
        if (!Save(JournalKind::Element, reference.First, m_arena.Elements[reference.First]))
        {
            return false;
        }
        m_arena.Elements[reference.First] = value;
        return m_error == Reason::None;
    case ReferenceKind::Byte:
    {
        const Id number{AsInteger(value)};
        if (reference.First >= m_used.BytesUsed || m_error != Reason::None)
        {
            static_cast<void>(Unknown(Reason::Bounds));
            return false;
        }
        if (!Save(JournalKind::Byte, reference.First, m_arena.ByteStorage[reference.First]))
        {
            return false;
        }
        m_arena.ByteStorage[reference.First] = static_cast<UInt8>(GetObject(number).Integer & 0xFFU);
        return true;
    }
    case ReferenceKind::BufferField:
    {
        const Id number{reference.Count <= 64U ? AsInteger(value) : ConvertForStore(value, ObjectKind::Buffer)};
        const Object& buffer{GetObject(reference.First)};
        if (m_error != Reason::None || buffer.Kind != ObjectKind::Buffer ||
            reference.Integer > static_cast<UInt64>(buffer.Count) * 8U ||
            reference.Count > static_cast<UInt64>(buffer.Count) * 8U - reference.Integer)
        {
            static_cast<void>(Unknown(Reason::Bounds));
            return false;
        }
        for (Size i{0U}; i < reference.Count; ++i)
        {
            const UInt64 bit{reference.Integer + i};
            UInt8& byte{m_arena.ByteStorage[buffer.First + static_cast<Size>(bit / 8U)]};
            const UInt32 mask{UInt32{1U} << (bit % 8U)};
            if (!Tick())
            {
                return false;
            }
            if ((i == 0U || bit % 8U == 0U) && !Save(JournalKind::Byte, buffer.First + static_cast<Id>(bit / 8U), byte))
            {
                return false;
            }
            UInt32 sourceBit{0U};
            if (reference.Count <= 64U)
            {
                sourceBit = static_cast<UInt32>((GetObject(number).Integer >> i) & 1U);
            }
            else
            {
                const Bytes source{Data(number)};
                if (i / 8U < source.Length)
                {
                    sourceBit = (UInt32{source.Data[i / 8U]} >> (i % 8U)) & 1U;
                }
            }
            byte = static_cast<UInt8>((byte & ~mask) | (sourceBit << (bit % 8U)));
        }
        return true;
    }
    }
    return false;
}

Id Library::Copy(const Id input) noexcept
{
    const Object& object{GetObject(input)};
    if (object.Kind == ObjectKind::String || object.Kind == ObjectKind::Buffer)
    {
        const Id result{MakeBytes(object.Kind, object.Count)};
        if (GetObject(result).Kind != object.Kind)
        {
            return result;
        }
        for (Size i{0U}; i < object.Count; ++i)
        {
            m_arena.ByteStorage[GetObject(result).First + i] = m_arena.ByteStorage[object.First + i];
        }
        return result;
    }
    if (object.Kind != ObjectKind::Package)
    {
        const Id result{AddObject(object.Kind)};
        if (result != NoId)
        {
            m_arena.Objects[result] = object;
        }
        return result;
    }
    const Id result{Package(object.Count)};
    Size depth{1U};
    m_arena.Parse[0U] = {};
    m_arena.Parse[0U].Ast = input;
    m_arena.Parse[0U].Last = result;
    while (depth != 0U && m_error == Reason::None)
    {
        if (!Tick())
        {
            return Unknown(Reason::Budget);
        }
        ParseFrame& frame{m_arena.Parse[depth - 1U]};
        const Object& source{GetObject(frame.Ast)};
        if (frame.Position == source.Count)
        {
            --depth;
            continue;
        }
        const Id child{Element(frame.Ast, frame.Position)};
        const Object& item{GetObject(child)};
        Id copied{NoId};
        if (item.Kind == ObjectKind::Package)
        {
            copied = Package(item.Count);
        }
        else if (item.Kind == ObjectKind::String || item.Kind == ObjectKind::Buffer)
        {
            copied = MakeBytes(item.Kind, item.Count);
            if (GetObject(copied).Kind == item.Kind)
            {
                for (Size i{0U}; i < item.Count; ++i)
                {
                    m_arena.ByteStorage[GetObject(copied).First + i] = m_arena.ByteStorage[item.First + i];
                }
            }
        }
        else
        {
            copied = AddObject(item.Kind);
            if (copied != NoId)
            {
                m_arena.Objects[copied] = item;
            }
        }
        if (GetObject(frame.Last).Kind != ObjectKind::Package)
        {
            return Unknown(Reason::Capacity);
        }
        m_arena.Elements[GetObject(frame.Last).First + frame.Position++] = copied;
        if (item.Kind == ObjectKind::Package)
        {
            if (depth == MaximumParseDepth)
            {
                return Unknown(Reason::EvaluationDepth);
            }
            m_arena.Parse[depth] = {};
            m_arena.Parse[depth].Ast = child;
            m_arena.Parse[depth++].Last = copied;
        }
    }
    return result;
}
bool Library::Save(const JournalKind kind, const Id index, const Id previous) noexcept
{
    if (m_journalCount == MaximumJournalEntries)
    {
        static_cast<void>(Unknown(Reason::Capacity));
        return false;
    }
    m_arena.Journal[m_journalCount++] = {index, previous, kind};
    return true;
}
void Library::Rollback(const Size checkpoint) noexcept
{
    while (m_journalCount > checkpoint)
    {
        const JournalEntry& entry{m_arena.Journal[--m_journalCount]};
        switch (entry.Kind)
        {
        case JournalKind::Active:
            m_arena.Nodes[entry.Index].Active = entry.Previous != 0U;
            break;
        case JournalKind::Node:
            m_arena.Nodes[entry.Index].Value = entry.Previous;
            break;
        case JournalKind::Element:
            m_arena.Elements[entry.Index] = entry.Previous;
            break;
        case JournalKind::Byte:
            m_arena.ByteStorage[entry.Index] = static_cast<UInt8>(entry.Previous);
            break;
        }
    }
}

} // namespace Asteria::Acpi::Aml
