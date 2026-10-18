// SPDX-License-Identifier: Apache-2.0

#include "Acpi/Aml/Library.hpp"

#include "Acpi/Aml/Grammar.hpp"

namespace Asteria::Acpi::Aml
{
namespace
{
constexpr UInt8 HexDigits[]{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
Size Digits(UInt64 number, const UInt64 base) noexcept
{
    Size count{1U};
    while (number >= base)
    {
        number /= base;
        ++count;
    }
    return count;
}
void WriteDigits(UInt8* const output, UInt64 number, const Size length, const UInt64 base) noexcept
{
    for (Size i{length}; i != 0U; --i)
    {
        output[i - 1U] = HexDigits[number % base];
        number /= base;
    }
}
UInt64 ObjectType(const ObjectKind kind) noexcept
{
    switch (kind)
    {
    case ObjectKind::Unknown:
        return 0U;
    case ObjectKind::Integer:
        return 1U;
    case ObjectKind::String:
        return 2U;
    case ObjectKind::Buffer:
        return 3U;
    case ObjectKind::Package:
        return 4U;
    case ObjectKind::Reference:
        return 0U;
    }
    return 0U;
}
UInt64 NodeType(const NodeKind kind) noexcept
{
    switch (kind)
    {
    case NodeKind::Name:
    case NodeKind::Alias:
    case NodeKind::External:
    case NodeKind::Scope:
        return 0U;
    case NodeKind::Field:
    case NodeKind::IndexField:
    case NodeKind::BankField:
        return 5U;
    case NodeKind::Device:
        return 6U;
    case NodeKind::Event:
        return 7U;
    case NodeKind::Method:
        return 8U;
    case NodeKind::Mutex:
        return 9U;
    case NodeKind::OperationRegion:
        return 10U;
    case NodeKind::PowerResource:
        return 11U;
    case NodeKind::Processor:
        return 12U;
    case NodeKind::ThermalZone:
        return 13U;
    case NodeKind::BufferField:
        return 14U;
    }
    return 0U;
}
bool MatchInteger(const UInt64 value, const UInt64 operand, const UInt64 comparison) noexcept
{
    switch (comparison)
    {
    case 0U:
        return true;
    case 1U:
        return value == operand;
    case 2U:
        return value <= operand;
    case 3U:
        return value < operand;
    case 4U:
        return value >= operand;
    case 5U:
        return value > operand;
    default:
        return false;
    }
}
} // namespace

Id Library::ConvertForStore(const Id input, const ObjectKind kind) noexcept
{
    const Id value{Value(input)};
    const Object& object{GetObject(value)};
    if (kind == object.Kind || kind == ObjectKind::Unknown || kind == ObjectKind::Reference)
    {
        return value;
    }
    if (kind == ObjectKind::Integer)
    {
        return AsInteger(value);
    }
    if (kind == ObjectKind::Buffer)
    {
        if (object.Kind == ObjectKind::Integer)
        {
            const Id ast{m_evaluationDepth == 0U ? NoId : m_arena.Evaluation[m_evaluationDepth - 1U].Ast};
            const Size width{Mask(~UInt64{0U}, ast) == ~UInt64{0U} ? 8U : 4U};
            const Id result{MakeBytes(ObjectKind::Buffer, width)};
            if (GetObject(result).Kind == ObjectKind::Buffer)
            {
                for (Size i{0U}; i < width; ++i)
                {
                    m_arena.ByteStorage[GetObject(result).First + i] =
                        static_cast<UInt8>((object.Integer >> (i * 8U)) & 0xFFU);
                }
            }
            return result;
        }
        if (object.Kind == ObjectKind::String)
        {
            const Id result{MakeBytes(ObjectKind::Buffer, object.Count == 0U ? 0U : object.Count + 1U)};
            if (GetObject(result).Kind == ObjectKind::Buffer)
            {
                for (Size i{0U}; i < object.Count; ++i)
                {
                    m_arena.ByteStorage[GetObject(result).First + i] = m_arena.ByteStorage[object.First + i];
                }
            }
            return result;
        }
    }
    if (kind == ObjectKind::String && object.Kind == ObjectKind::Integer)
    {
        const Size digits{Digits(object.Integer, 16U)};
        const Id result{MakeBytes(ObjectKind::String, digits)};
        if (GetObject(result).Kind == ObjectKind::String)
        {
            WriteDigits(m_arena.ByteStorage + GetObject(result).First, object.Integer, digits, 16U);
        }
        return result;
    }
    return Unknown(Reason::Type);
}

Id Library::Convert(EvaluationFrame& frame) noexcept
{
    const UInt16 opcode{m_arena.Code[frame.Ast].Opcode};
    const Id value{Value(frame.Values[0U])};
    const Object& object{GetObject(value)};
    if (m_error != Reason::None)
    {
        return value;
    }
    if (opcode == 0x99U)
    {
        const Id number{AsInteger(value)};
        return Integer(Mask(GetObject(number).Integer, frame.Ast));
    }
    if (opcode == 0x96U)
    {
        return ConvertForStore(value, ObjectKind::Buffer);
    }
    if (opcode == 0x9CU)
    {
        if (object.Kind != ObjectKind::Buffer)
        {
            return Unknown(Reason::Type, frame.Ast);
        }
        const Id limit{AsInteger(frame.Values[1U])};
        Size length{0U};
        const Bytes bytes{Data(value)};
        const UInt64 maximum{GetObject(limit).Integer};
        while (length < bytes.Length && length < maximum && bytes.Data[length] != 0U)
        {
            ++length;
        }
        return String({bytes.Data, length});
    }
    if (object.Kind == ObjectKind::String)
    {
        return Copy(value);
    }
    const UInt64 base{opcode == 0x97U ? 10U : 16U};
    if (object.Kind == ObjectKind::Integer)
    {
        const Size digits{Digits(object.Integer, base)};
        const Id result{MakeBytes(ObjectKind::String, digits)};
        if (GetObject(result).Kind == ObjectKind::String)
        {
            WriteDigits(m_arena.ByteStorage + GetObject(result).First, object.Integer, digits, base);
        }
        return result;
    }
    if (object.Kind == ObjectKind::Buffer)
    {
        Size length{0U};
        const Bytes bytes{Data(value)};
        for (Size i{0U}; i < bytes.Length; ++i)
        {
            length += opcode == 0x98U ? 4U : Digits(bytes.Data[i], base);
            if (i != 0U)
            {
                ++length;
            }
        }
        const Id result{MakeBytes(ObjectKind::String, length)};
        if (GetObject(result).Kind != ObjectKind::String)
        {
            return result;
        }
        UInt8* const output{m_arena.ByteStorage + GetObject(result).First};
        Size cursor{0U};
        for (Size i{0U}; i < bytes.Length; ++i)
        {
            if (i != 0U)
            {
                output[cursor++] = ',';
            }
            Size digits{Digits(bytes.Data[i], base)};
            if (opcode == 0x98U)
            {
                output[cursor++] = '0';
                output[cursor++] = 'x';
                digits = 2U;
            }
            WriteDigits(output + cursor, bytes.Data[i], digits, base);
            cursor += digits;
        }
        return result;
    }
    return Unknown(Reason::Type, frame.Ast);
}

Id Library::Concatenate(EvaluationFrame& frame) noexcept
{
    const Id left{Value(frame.Values[0U])};
    const Object& first{GetObject(left)};
    const Id right{ConvertForStore(frame.Values[1U], first.Kind)};
    const Object& second{GetObject(right)};
    if (m_error != Reason::None)
    {
        return right;
    }
    if (first.Kind == ObjectKind::Integer && second.Kind == ObjectKind::Integer)
    {
        const Size width{Mask(~UInt64{0U}, frame.Ast) == ~UInt64{0U} ? 8U : 4U};
        const Id result{MakeBytes(ObjectKind::Buffer, width * 2U)};
        if (GetObject(result).Kind != ObjectKind::Buffer)
        {
            return result;
        }
        for (Size i{0U}; i < width; ++i)
        {
            m_arena.ByteStorage[GetObject(result).First + i] = static_cast<UInt8>((first.Integer >> (i * 8U)) & 0xFFU);
            m_arena.ByteStorage[GetObject(result).First + width + i] =
                static_cast<UInt8>((second.Integer >> (i * 8U)) & 0xFFU);
        }
        return result;
    }
    if ((first.Kind != ObjectKind::Buffer && first.Kind != ObjectKind::String) || second.Kind != first.Kind)
    {
        return Unknown(Reason::Type, frame.Ast);
    }
    const Id result{MakeBytes(first.Kind, Size{first.Count} + second.Count)};
    if (GetObject(result).Kind != first.Kind)
    {
        return result;
    }
    for (Size i{0U}; i < first.Count; ++i)
    {
        m_arena.ByteStorage[GetObject(result).First + i] = m_arena.ByteStorage[first.First + i];
    }
    for (Size i{0U}; i < second.Count; ++i)
    {
        m_arena.ByteStorage[GetObject(result).First + first.Count + i] = m_arena.ByteStorage[second.First + i];
    }
    return result;
}

Id Library::Arithmetic(EvaluationFrame& frame) noexcept
{
    const UInt16 opcode{m_arena.Code[frame.Ast].Opcode};
    const Id firstId{Value(frame.Values[0U])};
    const Object& first{GetObject(firstId)};
    if (opcode >= 0x93U && opcode <= 0x95U)
    {
        const Id secondId{ConvertForStore(frame.Values[1U], first.Kind)};
        const Object& second{GetObject(secondId)};
        bool equal{false};
        bool less{false};
        if (first.Kind == ObjectKind::Integer && second.Kind == ObjectKind::Integer)
        {
            equal = first.Integer == second.Integer;
            less = first.Integer < second.Integer;
        }
        else if ((first.Kind == ObjectKind::String || first.Kind == ObjectKind::Buffer) && second.Kind == first.Kind)
        {
            const Bytes a{Data(firstId)};
            const Bytes b{Data(secondId)};
            Size i{0U};
            while (i < a.Length && i < b.Length && a.Data[i] == b.Data[i])
            {
                ++i;
            }
            equal = i == a.Length && i == b.Length;
            less = i == a.Length ? i != b.Length : i != b.Length && a.Data[i] < b.Data[i];
        }
        else
        {
            return Unknown(Reason::Type, frame.Ast);
        }
        const bool result{opcode == 0x93U ? equal : opcode == 0x94U ? !equal && !less : less};
        return Integer(result ? Mask(~UInt64{0U}, frame.Ast) : 0U);
    }
    const Id left{AsInteger(frame.Values[0U])};
    const UInt64 a{GetObject(left).Integer};
    UInt64 b{0U};
    if (frame.Count > 1U && opcode != 0x80U && opcode != 0x81U && opcode != 0x82U && opcode != 0x92U)
    {
        b = GetObject(AsInteger(frame.Values[1U])).Integer;
    }
    UInt64 result{0U};
    // AML integer arithmetic intentionally wraps modulo the table's width.
    switch (opcode)
    {
    case 0x72U:
        result = a + b;
        break;
    case 0x74U:
        result = a - b;
        break;
    case 0x77U:
        result = a * b;
        break;
    case 0x78U:
        if (b == 0U)
        {
            return Unknown(Reason::DivideByZero, frame.Ast);
        }
        if (!WriteReference(frame.Values[2U], Integer(a % b)))
        {
            return NoId;
        }
        result = a / b;
        break;
    case 0x79U:
        result = b >= 64U ? 0U : a << b;
        break;
    case 0x7AU:
        result = b >= 64U ? 0U : a >> b;
        break;
    case 0x7BU:
        result = a & b;
        break;
    case 0x7CU:
        result = ~(a & b);
        break;
    case 0x7DU:
        result = a | b;
        break;
    case 0x7EU:
        result = ~(a | b);
        break;
    case 0x7FU:
        result = a ^ b;
        break;
    case 0x80U:
        result = ~a;
        break;
    case 0x81U:
        for (UInt64 copy{a}; copy != 0U; copy >>= 1U)
        {
            ++result;
        }
        break;
    case 0x82U:
        if (a != 0U)
        {
            result = 1U;
            for (UInt64 copy{a}; (copy & 1U) == 0U; copy >>= 1U)
            {
                ++result;
            }
        }
        break;
    case 0x85U:
        if (b == 0U)
        {
            return Unknown(Reason::DivideByZero, frame.Ast);
        }
        result = a % b;
        break;
    case 0x90U:
        result = a != 0U && b != 0U ? ~UInt64{0U} : 0U;
        break;
    case 0x91U:
        result = a != 0U || b != 0U ? ~UInt64{0U} : 0U;
        break;
    case 0x92U:
        result = a == 0U ? ~UInt64{0U} : 0U;
        break;
    default:
        return Unknown(Reason::Unsupported, frame.Ast);
    }
    return Integer(Mask(result, frame.Ast));
}

Id Library::CreateBufferField(EvaluationFrame& frame) noexcept
{
    const Syntax& ast{m_arena.Code[frame.Ast]};
    const Id buffer{Value(frame.Values[0U])};
    UInt64 bit{GetObject(AsInteger(frame.Values[1U])).Integer};
    UInt64 width{0U};
    switch (ast.Opcode)
    {
    case 0x8AU:
        width = 32U;
        break;
    case 0x8BU:
        width = 16U;
        break;
    case 0x8CU:
        width = 8U;
        break;
    case 0x8DU:
        width = 1U;
        break;
    case 0x8FU:
        width = 64U;
        break;
    case 0x5B13U:
        width = GetObject(AsInteger(frame.Values[2U])).Integer;
        break;
    default:
        return Unknown(Reason::Unsupported, frame.Ast);
    }
    if (ast.Opcode != 0x8DU && ast.Opcode != 0x5B13U)
    {
        if (bit > ~UInt64{0U} / 8U)
        {
            return Unknown(Reason::Bounds, frame.Ast);
        }
        bit *= 8U;
    }
    const Object& object{GetObject(buffer)};
    if (object.Kind != ObjectKind::Buffer || width == 0U || width > MaximumObjectBytes * 8U ||
        bit > static_cast<UInt64>(object.Count) * 8U || width > static_cast<UInt64>(object.Count) * 8U - bit)
    {
        return Unknown(Reason::Bounds, frame.Ast);
    }
    if (ast.Binding != NoId)
    {
        return Reference(ReferenceKind::BufferField, buffer, static_cast<UInt32>(width), bit);
    }
    Id name{ast.First};
    while (m_arena.Code[name].Next != NoId)
    {
        name = m_arena.Code[name].Next;
    }
    const Syntax& path{m_arena.Code[name]};
    if (path.Span.Length != 4U || m_dynamicCount >= MaximumDynamicNames)
    {
        return Unknown(Reason::Capacity, frame.Ast);
    }
    const UInt32 segment{Segment(path.Span.Data)};
    for (Size i{m_arena.Calls[frame.Call].DynamicBase}; i < m_dynamicCount; ++i)
    {
        if (m_arena.Dynamic[i].Scope == path.Scope && m_arena.Dynamic[i].Segment == segment)
        {
            return Unknown(Reason::Conflict, frame.Ast);
        }
    }
    const Id reference{Reference(ReferenceKind::BufferField, buffer, static_cast<UInt32>(width), bit)};
    m_arena.Dynamic[m_dynamicCount++] = {frame.Call, path.Scope, segment, reference};
    return NoId;
}

Id Library::CreateName(EvaluationFrame& frame) noexcept
{
    const Syntax& ast{m_arena.Code[frame.Ast]};
    const Syntax& path{m_arena.Code[ast.First]};
    if (path.Span.Length != 4U || m_dynamicCount == MaximumDynamicNames || m_used.Elements == m_arena.ElementLimit)
    {
        return Unknown(Reason::Capacity, frame.Ast);
    }
    const UInt32 segment{Segment(path.Span.Data)};
    if (Lookup(path.Scope, segment) != NoId || DynamicValue(path.Span, path.Scope) != NoId)
    {
        return Unknown(Reason::Conflict, frame.Ast);
    }
    const Id slot{static_cast<Id>(m_used.Elements++)};
    m_arena.Elements[slot] = frame.Values[1U];
    const Id reference{Reference(ReferenceKind::Element, slot)};
    m_arena.Dynamic[m_dynamicCount++] = {frame.Call, path.Scope, segment, reference};
    return NoId;
}

Id Library::Match(EvaluationFrame& frame) noexcept
{
    const Id package{Value(frame.Values[0U])};
    const Object& object{GetObject(package)};
    const UInt64 op1{GetObject(frame.Values[1U]).Integer};
    const UInt64 op2{GetObject(frame.Values[3U]).Integer};
    const UInt64 start{GetObject(AsInteger(frame.Values[5U])).Integer};
    if (object.Kind != ObjectKind::Package || op1 > 5U || op2 > 5U)
    {
        return Unknown(Reason::Type, frame.Ast);
    }
    const Id a{Value(frame.Values[2U])};
    const Id b{Value(frame.Values[4U])};
    if ((op1 != 0U && GetObject(a).Kind != ObjectKind::Integer) ||
        (op2 != 0U && GetObject(b).Kind != ObjectKind::Integer))
    {
        return Unknown(Reason::Unsupported, frame.Ast);
    }
    for (UInt64 i{start}; i < object.Count; ++i)
    {
        if (!Tick())
        {
            return NoId;
        }
        const Object& item{GetObject(Element(package, static_cast<Size>(i)))};
        if (item.Kind == ObjectKind::Integer && MatchInteger(item.Integer, GetObject(a).Integer, op1) &&
            MatchInteger(item.Integer, GetObject(b).Integer, op2))
        {
            return Integer(i);
        }
    }
    return Integer(Mask(~UInt64{0U}, frame.Ast));
}

Id Library::Apply(EvaluationFrame& frame) noexcept
{
    const UInt16 opcode{m_arena.Code[frame.Ast].Opcode};
    Id result{NoId};
    switch (opcode)
    {
    case 0x08U:
        return CreateName(frame);
    case 0x11U:
    {
        const UInt64 length{GetObject(AsInteger(frame.Values[0U])).Integer};
        if (length > MaximumObjectBytes)
        {
            return Unknown(Reason::Capacity, frame.Ast);
        }
        const Bytes initializer{m_arena.Code[frame.Ast].Span};
        const Size size{initializer.Length > length ? initializer.Length : static_cast<Size>(length)};
        result = MakeBytes(ObjectKind::Buffer, size);
        if (GetObject(result).Kind != ObjectKind::Buffer)
        {
            return result;
        }
        for (Size i{0U}; i < initializer.Length; ++i)
        {
            m_arena.ByteStorage[GetObject(result).First + i] = initializer.Data[i];
        }
        return result;
    }
    case 0x70U:
    case 0x9DU:
        result = frame.Values[0U];
        static_cast<void>(WriteReference(frame.Values[1U], result, opcode == 0x9DU));
        return result;
    case 0x71U:
        return frame.Values[0U];
    case 0x75U:
    case 0x76U:
    {
        const UInt64 number{GetObject(AsInteger(frame.Values[0U])).Integer};
        result = Integer(Mask(opcode == 0x75U ? number + 1U : number - 1U, frame.Ast));
        static_cast<void>(WriteReference(frame.Values[0U], result));
        return result;
    }
    case 0x73U:
        result = Concatenate(frame);
        break;
    case 0x72U:
    case 0x74U:
    case 0x77U:
    case 0x78U:
    case 0x79U:
    case 0x7AU:
    case 0x7BU:
    case 0x7CU:
    case 0x7DU:
    case 0x7EU:
    case 0x7FU:
    case 0x80U:
    case 0x81U:
    case 0x82U:
    case 0x85U:
    case 0x90U:
    case 0x91U:
    case 0x92U:
    case 0x93U:
    case 0x94U:
    case 0x95U:
        result = Arithmetic(frame);
        if (opcode >= 0x90U && opcode <= 0x95U)
        {
            return result;
        }
        break;
    case 0x83U:
    {
        const Object& source{GetObject(frame.Values[0U])};
        if (source.Kind == ObjectKind::String)
        {
            const Bytes path{Data(frame.Values[0U])};
            const Id node{ResolveText(path, m_arena.Code[frame.Ast].Scope)};
            if (node == NoId)
            {
                return Unknown(Reason::Unresolved, frame.Ast);
            }
            return Value(Reference(ReferenceKind::Node, node));
        }
        if (source.Kind != ObjectKind::Reference)
        {
            return Unknown(Reason::Type, frame.Ast);
        }
        return Value(frame.Values[0U]);
    }
    case 0x87U:
    {
        const Object& value{GetObject(Value(frame.Values[0U]))};
        if (value.Kind == ObjectKind::String || value.Kind == ObjectKind::Buffer || value.Kind == ObjectKind::Package)
        {
            return Integer(value.Count);
        }
        return Unknown(Reason::Type, frame.Ast);
    }
    case 0x88U:
    {
        const Id value{Value(frame.Values[0U])};
        const Object& object{GetObject(value)};
        const UInt64 index{GetObject(AsInteger(frame.Values[1U])).Integer};
        if (index >= object.Count)
        {
            return Unknown(Reason::Bounds, frame.Ast);
        }
        if (object.Kind == ObjectKind::Package)
        {
            result = Reference(ReferenceKind::Element, object.First + static_cast<Id>(index));
        }
        else if (object.Kind == ObjectKind::Buffer || object.Kind == ObjectKind::String)
        {
            result = Reference(ReferenceKind::Byte, object.First + static_cast<Id>(index));
        }
        else
        {
            return Unknown(Reason::Type, frame.Ast);
        }
        break;
    }
    case 0x89U:
        return Match(frame);
    case 0x8AU:
    case 0x8BU:
    case 0x8CU:
    case 0x8DU:
    case 0x8FU:
    case 0x5B13U:
        return CreateBufferField(frame);
    case 0x8EU:
    {
        if (GetObject(frame.Values[0U]).Kind == ObjectKind::Reference &&
            GetObject(frame.Values[0U]).Reference == ReferenceKind::Debug)
        {
            return Integer(16U);
        }
        const Id node{TargetNode(frame.Values[0U])};
        if (node != NoId)
        {
            const Node& target{m_arena.Nodes[node]};
            if (target.Kind != NodeKind::Name)
            {
                return Integer(NodeType(target.Kind));
            }
        }
        return Integer(ObjectType(GetObject(Value(frame.Values[0U])).Kind));
    }
    case 0x96U:
    case 0x97U:
    case 0x98U:
    case 0x99U:
    case 0x9CU:
        result = Convert(frame);
        break;
    case 0x9EU:
    {
        const Id value{Value(frame.Values[0U])};
        const Object& object{GetObject(value)};
        const UInt64 start{GetObject(AsInteger(frame.Values[1U])).Integer};
        UInt64 length{GetObject(AsInteger(frame.Values[2U])).Integer};
        if (object.Kind != ObjectKind::String && object.Kind != ObjectKind::Buffer)
        {
            return Unknown(Reason::Type, frame.Ast);
        }
        const Size offset{start > object.Count ? object.Count : static_cast<Size>(start)};
        if (length > object.Count - offset)
        {
            length = object.Count - offset;
        }
        result = MakeBytes(object.Kind, static_cast<Size>(length));
        if (GetObject(result).Kind != object.Kind)
        {
            return result;
        }
        for (Size i{0U}; i < length; ++i)
        {
            m_arena.ByteStorage[GetObject(result).First + i] = m_arena.ByteStorage[object.First + offset + i];
        }
        break;
    }
    case 0xA3U:
        return NoId;
    case 0xA4U:
        result = frame.Values[0U];
        m_arena.Calls[frame.Call].ReturnValue = result;
        m_arena.Calls[frame.Call].Returning = true;
        return result;
    case 0x86U:
    case 0x5B21U:
    case 0x5B22U:
    case 0x5B23U:
    case 0x5B24U:
    case 0x5B25U:
    case 0x5B26U:
    case 0x5B27U:
        Record(opcode, frame.Values[0U], frame.Count > 1U ? frame.Values[1U] : NoId);
        return opcode == 0x5B23U || opcode == 0x5B25U ? Integer(0U) : NoId;
    case 0x5B12U:
    {
        const Object& reference{GetObject(frame.Values[0U])};
        const bool found{reference.Kind == ObjectKind::Reference &&
                         (reference.Reference != ReferenceKind::Node || reference.First != NoId)};
        if (found)
        {
            static_cast<void>(WriteReference(frame.Values[1U], frame.Values[0U]));
        }
        return Integer(found ? Mask(~UInt64{0U}, frame.Ast) : 0U);
    }
    case 0x5B30U:
        return Integer(2U);
    default:
        return Unknown(Reason::Unsupported, frame.Ast);
    }
    if (m_error != Reason::None)
    {
        return result;
    }
    const Size target{opcode == 0x78U || opcode == 0x9EU ? 3U
                      : opcode == 0x80U || opcode == 0x81U || opcode == 0x82U || (opcode >= 0x96U && opcode <= 0x99U)
                          ? 1U
                          : 2U};
    if (target < frame.Count)
    {
        static_cast<void>(
            WriteReference(frame.Values[target], result, (opcode >= 0x96U && opcode <= 0x99U) || opcode == 0x9CU));
    }
    return result;
}
} // namespace Asteria::Acpi::Aml
