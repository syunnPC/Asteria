// SPDX-License-Identifier: Apache-2.0

#include "Acpi/Aml/Facts.hpp"

namespace Asteria::Acpi::Aml
{
Facts::Facts(Library& library, const UInt64 budget) noexcept : m_library{library}, m_remaining{budget}
{
}

EvaluationResult Facts::Evaluate(const Id node, const Id* const arguments, const Size count) noexcept
{
    if (node == NoId)
    {
        return {NoId, Reason::Missing, 0U};
    }
    const UInt64 budget{m_remaining < MaximumEvaluationOperations ? m_remaining : MaximumEvaluationOperations};
    const EvaluationResult result{m_library.Evaluate(node, arguments, count, budget)};
    m_remaining -= result.Operations;
    m_operations += result.Operations;
    return result;
}

Reason Facts::Prepare() noexcept
{
    if (!m_prepared)
    {
        m_prepared = true;
        const Id argument{m_library.Integer(1U)};
        const Id pic{m_library.Find("\\_PIC")};
        m_pic = pic == NoId ? Reason::None : Evaluate(pic, &argument, 1U).Error;
    }
    return m_pic;
}
Id Facts::NextDevice(const Id previous) const noexcept
{
    if (previous >= m_library.Used().Nodes)
    {
        return NoId;
    }
    for (Id node{previous + 1U}; node < m_library.Used().Nodes; ++node)
    {
        const Node* const item{m_library.GetNode(node)};
        if (item != nullptr && item->Defined && !item->Invalid && item->Kind == NodeKind::Device &&
            (item->Active || item->LoadError != Reason::None))
        {
            return node;
        }
    }
    return NoId;
}

Id Facts::ResolveValue(Id value, Reason& error) noexcept
{
    for (Size depth{0U}; depth < MaximumEvaluationDepth; ++depth)
    {
        const Object& object{m_library.GetObject(value)};
        if (object.Kind != ObjectKind::Reference)
        {
            if (object.Kind == ObjectKind::Unknown)
            {
                error = object.Error;
            }
            return value;
        }
        if (object.Reference != ReferenceKind::Node)
        {
            error = Reason::Type;
            return NoId;
        }
        const EvaluationResult result{Evaluate(object.First)};
        if (!result.Known())
        {
            error = result.Error;
            return NoId;
        }
        value = result.Value;
    }
    error = Reason::Cycle;
    return NoId;
}

bool Facts::EisaId(const UInt64 value, char (&output)[8U]) noexcept
{
    if (value > 0xFFFFFFFFU)
    {
        return false;
    }
    const UInt32 big{static_cast<UInt32>(((value & 0xFFU) << 24U) | ((value & 0xFF00U) << 8U) |
                                         ((value & 0xFF0000U) >> 8U) | ((value & 0xFF000000U) >> 24U))};
    if ((big & 0x80000000U) != 0U)
    {
        return false;
    }
    for (Size i{0U}; i < 3U; ++i)
    {
        const UInt32 letter{(big >> (26U - i * 5U)) & 31U};
        if (letter == 0U || letter > 26U)
        {
            return false;
        }
        output[i] = static_cast<char>('A' + letter - 1U);
    }
    constexpr char digits[]{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    for (Size i{0U}; i < 4U; ++i)
    {
        output[3U + i] = digits[(big >> (12U - i * 4U)) & 15U];
    }
    output[7U] = '\0';
    return true;
}

void Facts::ReadIds(const Id node, Identifiers& output) noexcept
{
    output.Count = 0U;
    const EvaluationResult result{Evaluate(node)};
    output.Error = result.Error;
    if (!result.Known())
    {
        return;
    }
    const Id value{ResolveValue(result.Value, output.Error)};
    const Object& object{m_library.GetObject(value)};
    const bool package{object.Kind == ObjectKind::Package};
    const Size count{package ? object.Count : 1U};
    if (count > MaximumIds)
    {
        output.Error = Reason::Capacity;
        return;
    }
    for (Size i{0U}; i < count; ++i)
    {
        const Id item{ResolveValue(package ? m_library.Element(value, i) : value, output.Error)};
        const Object& id{m_library.GetObject(item)};
        Identifier& identifier{output.Items[output.Count]};
        identifier = {};
        identifier.Value = item;
        if (id.Kind == ObjectKind::Integer)
        {
            if (!EisaId(id.Integer, identifier.Eisa))
            {
                output.Error = Reason::Type;
                return;
            }
        }
        else if (id.Kind != ObjectKind::String)
        {
            output.Error = Reason::Type;
            return;
        }
        ++output.Count;
    }
}

bool Facts::HasId(const Identifiers& ids, const char* const text) const noexcept
{
    if (text == nullptr || ids.Error != Reason::None)
    {
        return false;
    }
    for (Size i{0U}; i < ids.Count; ++i)
    {
        const Identifier& id{ids.Items[i]};
        const Bytes bytes{m_library.Data(id.Value)};
        Size cursor{0U};
        if (id.Eisa[0U] != '\0')
        {
            while (cursor < 7U && text[cursor] != '\0' && id.Eisa[cursor] == text[cursor])
            {
                ++cursor;
            }
            if (cursor == 7U && text[cursor] == '\0')
            {
                return true;
            }
        }
        else
        {
            while (cursor < bytes.Length && text[cursor] != '\0' &&
                   bytes.Data[cursor] == static_cast<UInt8>(text[cursor]))
            {
                ++cursor;
            }
            if (cursor == bytes.Length && text[cursor] == '\0')
            {
                return true;
            }
        }
    }
    return false;
}

Presence Facts::ReadSta(Id node, Reason& error) noexcept
{
    Presence verdict{Presence::Present};
    error = Reason::None;
    for (Size depth{0U}; node != NoId && depth < MaximumNodes; ++depth)
    {
        const Node* const current{m_library.GetNode(node)};
        if (current != nullptr && !current->Active)
        {
            if (current->LoadError == Reason::None)
            {
                verdict = Presence::Absent;
                error = Reason::None;
            }
            else if (verdict != Presence::Absent)
            {
                verdict = Presence::Unknown;
                error = current->LoadError;
            }
        }
        const Id sta{m_library.Child(node, "_STA")};
        if (sta != NoId)
        {
            const EvaluationResult result{Evaluate(sta)};
            if (!result.Known() || m_library.GetObject(result.Value).Kind != ObjectKind::Integer)
            {
                if (verdict != Presence::Absent)
                {
                    verdict = Presence::Unknown;
                    error = result.Known() ? Reason::Type : result.Error;
                }
            }
            else if ((m_library.GetObject(result.Value).Integer & 3U) != 3U)
            {
                verdict = Presence::Absent;
                error = Reason::None;
            }
        }
        const Node* const ancestor{m_library.GetNode(node)};
        node = ancestor == nullptr ? NoId : ancestor->Parent;
    }
    return verdict;
}

bool Facts::PrtRow(const Id row, PrtEntry& output, Reason& error) noexcept
{
    const Object& object{m_library.GetObject(row)};
    if (object.Kind != ObjectKind::Package || object.Count != 4U)
    {
        error = Reason::Type;
        return false;
    }
    const Id address{ResolveValue(m_library.Element(row, 0U), error)};
    const Id pin{ResolveValue(m_library.Element(row, 1U), error)};
    const Id index{ResolveValue(m_library.Element(row, 3U), error)};
    if (error != Reason::None || m_library.GetObject(address).Kind != ObjectKind::Integer ||
        m_library.GetObject(pin).Kind != ObjectKind::Integer ||
        m_library.GetObject(index).Kind != ObjectKind::Integer || m_library.GetObject(pin).Integer > 3U ||
        m_library.GetObject(index).Integer > 0xFFFFFFFFU)
    {
        error = Reason::Type;
        return false;
    }
    output = {};
    output.Address = m_library.GetObject(address).Integer;
    output.Pin = static_cast<UInt32>(m_library.GetObject(pin).Integer);
    output.Index = static_cast<UInt32>(m_library.GetObject(index).Integer);
    output.Source = m_library.Element(row, 2U);
    const Object& source{m_library.GetObject(output.Source)};
    if (source.Kind == ObjectKind::Integer && source.Integer == 0U)
    {
        output.Direct = true;
    }
    else if (source.Kind == ObjectKind::Reference && source.Reference == ReferenceKind::Node &&
             m_library.GetNode(source.First) != nullptr)
    {
        output.SourceNode = source.First;
    }
    else if (source.Kind == ObjectKind::String)
    {
        if (m_library.Data(output.Source).Length == 0U)
        {
            error = Reason::Type;
            return false;
        }
    }
    else
    {
        error = Reason::Type;
        return false;
    }
    return true;
}

void Facts::ReadPrt(const Id node, DeviceFacts& output) noexcept
{
    output.PrtCount = 0U;
    output.PrtError = Prepare();
    if (output.PrtError != Reason::None)
    {
        return;
    }
    const EvaluationResult result{Evaluate(m_library.Child(node, "_PRT"))};
    output.PrtError = result.Error;
    const Object& object{m_library.GetObject(result.Value)};
    if (!result.Known())
    {
        return;
    }
    if (object.Kind != ObjectKind::Package || object.Count > MaximumPrtEntries)
    {
        output.PrtError = object.Count > MaximumPrtEntries ? Reason::Capacity : Reason::Type;
        return;
    }
    for (Size i{0U}; i < object.Count; ++i)
    {
        const Id row{ResolveValue(m_library.Element(result.Value, i), output.PrtError)};
        if (!PrtRow(row, output.Prt[i], output.PrtError))
        {
            return;
        }
        ++output.PrtCount;
    }
}

void Facts::ReadDevice(const Id node, DeviceFacts& output) noexcept
{
    output.Node = node;
    ReadIds(m_library.Child(node, "_HID"), output.Hid);
    ReadIds(m_library.Child(node, "_CID"), output.Cid);
    output.Uid = Evaluate(m_library.Child(node, "_UID"));
    output.Adr = Evaluate(m_library.Child(node, "_ADR"));
    if (output.Uid.Known() && m_library.GetObject(output.Uid.Value).Kind != ObjectKind::Integer &&
        m_library.GetObject(output.Uid.Value).Kind != ObjectKind::String)
    {
        output.Uid.Error = Reason::Type;
    }
    if (output.Adr.Known() && m_library.GetObject(output.Adr.Value).Kind != ObjectKind::Integer)
    {
        output.Adr.Error = Reason::Type;
    }
    output.Sta = ReadSta(node, output.StaError);
    const EvaluationResult crs{Evaluate(m_library.Child(node, "_CRS"))};
    output.Crs.Count = 0U;
    output.Crs.Error = crs.Error;
    output.Crs.ErrorOffset = 0U;
    if (crs.Known())
    {
        if (m_library.GetObject(crs.Value).Kind == ObjectKind::Buffer)
        {
            static_cast<void>(ResourceDecoder::Decode(m_library.Data(crs.Value), output.Crs));
        }
        else
        {
            output.Crs.Error = Reason::Type;
        }
    }
    output.Keyboard = HasId(output.Hid, "PNP0320") || HasId(output.Cid, "PNP0320");
    constexpr char digits[]{"0123456789AB"};
    for (const char digit : digits)
    {
        if (digit == '\0')
        {
            break;
        }
        char id[]{"PNP0300"};
        id[6U] = digit;
        output.Keyboard = output.Keyboard || HasId(output.Hid, id) || HasId(output.Cid, id);
    }
    output.Mouse = HasId(output.Hid, "PNP0F03") || HasId(output.Hid, "PNP0F13") || HasId(output.Cid, "PNP0F03") ||
                   HasId(output.Cid, "PNP0F13");
    output.PciRoot = HasId(output.Hid, "PNP0A03") || HasId(output.Hid, "PNP0A08") || HasId(output.Cid, "PNP0A03") ||
                     HasId(output.Cid, "PNP0A08");
    output.PrtCount = 0U;
    output.PrtError = Reason::Missing;
    if (output.PciRoot)
    {
        ReadPrt(node, output);
    }
}

SleepFacts Facts::Sleep() noexcept
{
    SleepFacts output{};
    const EvaluationResult result{Evaluate(m_library.Find("\\_S5"))};
    output.Error = result.Error;
    const Object& package{m_library.GetObject(result.Value)};
    if (!result.Known())
    {
        return output;
    }
    if (package.Kind != ObjectKind::Package || package.Count < 2U)
    {
        output.Error = Reason::Type;
        return output;
    }
    const Id a{ResolveValue(m_library.Element(result.Value, 0U), output.Error)};
    const Id b{ResolveValue(m_library.Element(result.Value, 1U), output.Error)};
    if (m_library.GetObject(a).Kind != ObjectKind::Integer || m_library.GetObject(b).Kind != ObjectKind::Integer ||
        m_library.GetObject(a).Integer > 7U || m_library.GetObject(b).Integer > 7U)
    {
        output.Error = Reason::Type;
        return output;
    }
    output.TypeA = static_cast<UInt8>(m_library.GetObject(a).Integer);
    output.TypeB = static_cast<UInt8>(m_library.GetObject(b).Integer);
    return output;
}
UInt64 Facts::Operations() const noexcept
{
    return m_operations;
}
} // namespace Asteria::Acpi::Aml
