// SPDX-License-Identifier: Apache-2.0

#include "User/Settings/Database.hpp"
#include "Foundation/Crc32c.hpp"
#include "User/Text/Utf8.hpp"

namespace Asteria::User::Settings
{
namespace
{
constexpr UInt8 Magic[8U]{'A', 'S', 'T', 'S', 'E', 'T', '0', '2'};
constexpr UInt8 SealMagic[8U]{'A', 'S', 'T', 'D', 'O', 'N', 'E', '2'};
[[nodiscard]] UInt64 Little(const UInt8* p, const Size size) noexcept
{
    UInt64 value{0U};
    for (Size i{0U}; i < size; ++i)
    {
        value |= static_cast<UInt64>(p[i]) << (i * 8U);
    }
    return value;
}
void Put(UInt8* p, const Size size, const UInt64 value) noexcept
{
    for (Size i{0U}; i < size; ++i)
    {
        p[i] = static_cast<UInt8>(value >> (i * 8U));
    }
}
[[nodiscard]] bool Equal(const UInt8* a, const UInt8* b, const Size size) noexcept
{
    for (Size i{0U}; i < size; ++i)
    {
        if (a[i] != b[i])
        {
            return false;
        }
    }
    return true;
}
[[nodiscard]] bool Zero(const UInt8* p, const Size size) noexcept
{
    for (Size i{0U}; i < size; ++i)
    {
        if (p[i] != 0U)
        {
            return false;
        }
    }
    return true;
}
[[nodiscard]] UInt32 BodyCrc(const UInt8* bytes) noexcept
{
    UInt32 crc{Foundation::Crc32c::Start()};
    for (Size i{0U}; i < BodyBytes; ++i)
    {
        crc = Foundation::Crc32c::Update(crc, ((i >= 28U) && (i < HeaderBytes)) ? UInt8{0U} : bytes[i]);
    }
    return Foundation::Crc32c::Finish(crc);
}
[[nodiscard]] const UInt8* Record(const UInt8* bytes, const Size index) noexcept
{
    return bytes + HeaderBytes + index * KeyRecordBytes;
}
[[nodiscard]] bool ValidName(const UInt8* p, const Size size) noexcept
{
    if ((size == 0U) || (size > MaximumNameBytes) || !Text::ValidUtf8(p, size))
    {
        return false;
    }
    if ((p[0U] == '.') && ((size == 1U) || ((size == 2U) && (p[1U] == '.'))))
    {
        return false;
    }
    for (Size i{0U}; i < size; ++i)
    {
        if ((p[i] == 0U) || (p[i] == '/'))
        {
            return false;
        }
    }
    return true;
}
[[nodiscard]] bool ValidRecords(const UInt8* bytes) noexcept
{
    for (Size i{0U}; i < KeyCapacity; ++i)
    {
        const UInt8* const p = Record(bytes, i);
        if (p[0U] == 0U)
        {
            if (!Zero(p, KeyRecordBytes))
            {
                return false;
            }
            continue;
        }
        if ((p[0U] != 1U) || (p[1U] >= KeyCapacity) || (p[2U] > MaximumNameBytes) || (p[3U] > AclCapacity) ||
            !Zero(p + 4U, 4U))
        {
            return false;
        }
        if (i == 0U)
        {
            if ((p[1U] != 0U) || (p[2U] != 0U))
            {
                return false;
            }
        }
        else if (!ValidName(p + 8U, p[2U]))
        {
            return false;
        }
        if (!Zero(p + 8U + p[2U], MaximumNameBytes - p[2U]))
        {
            return false;
        }
        for (Size a{0U}; a < AclCapacity; ++a)
        {
            const UInt8* const entry = p + 8U + MaximumNameBytes + a * AclRecordBytes;
            if (a >= p[3U])
            {
                if (!Zero(entry, AclRecordBytes))
                {
                    return false;
                }
            }
            else if ((entry[8U] == 0U) || ((entry[8U] & ~AllRights) != 0U) || (entry[9U] > 1U) || (entry[10U] > 1U) ||
                     (entry[11U] != 0U))
            {
                return false;
            }
        }
        Size current{i};
        bool rooted{false};
        for (Size depth{0U}; depth <= MaximumDepth; ++depth)
        {
            const UInt8* const ancestor = Record(bytes, current);
            if ((ancestor[0U] != 1U) || (ancestor[1U] >= KeyCapacity))
            {
                return false;
            }
            if (current == 0U)
            {
                rooted = true;
                break;
            }
            current = ancestor[1U];
        }
        if (!rooted)
        {
            return false;
        }
        for (Size previous{1U}; previous < i; ++previous)
        {
            const UInt8* const sibling = Record(bytes, previous);
            if ((sibling[0U] == 1U) && (sibling[1U] == p[1U]) && (sibling[2U] == p[2U]) &&
                Equal(sibling + 8U, p + 8U, p[2U]))
            {
                return false;
            }
        }
    }
    return Record(bytes, 0U)[0U] == 1U;
}
[[nodiscard]] const UInt8* ValueRecord(const UInt8* bytes, const Size index) noexcept
{
    return bytes + ValueTableOffset + index * ValueRecordBytes;
}
[[nodiscard]] bool ValidValues(const UInt8* bytes) noexcept
{
    UInt8 counts[KeyCapacity]{};
    for (Size i{0U}; i < ValueCapacity; ++i)
    {
        const UInt8* const p = ValueRecord(bytes, i);
        if (p[0U] == 0U)
        {
            if (!Zero(p, ValueRecordBytes))
            {
                return false;
            }
            continue;
        }
        if ((p[0U] != 1U) || (p[1U] >= KeyCapacity) || (Record(bytes, p[1U])[0U] != 1U) ||
            (++counts[p[1U]] > ValuesPerKey) || !Zero(p + 6U, 2U) || !FormatCodec::ValidValueName({p + 8U, p[2U]}))
        {
            return false;
        }
        if (!Zero(p + 8U + p[2U], MaximumNameBytes - p[2U]))
        {
            return false;
        }
        const Size length = static_cast<Size>(Little(p + 4U, 2U));
        if (!FormatCodec::ValidValue(static_cast<ValueType>(p[3U]), p + 8U + MaximumNameBytes, length) ||
            !Zero(p + 8U + MaximumNameBytes + length, MaximumValueBytes - length))
        {
            return false;
        }
        for (Size previous{0U}; previous < i; ++previous)
        {
            const UInt8* const other = ValueRecord(bytes, previous);
            if ((other[0U] == 1U) && (other[1U] == p[1U]) && (other[2U] == p[2U]) && Equal(other + 8U, p + 8U, p[2U]))
            {
                return false;
            }
        }
    }
    return true;
}
} // namespace
bool FormatCodec::ValidValueName(const TextView name) noexcept
{
    if ((name.Length > MaximumNameBytes) || !Text::ValidUtf8(name.Bytes, name.Length))
    {
        return false;
    }
    for (Size i{0U}; i < name.Length; ++i)
    {
        if (name.Bytes[i] == 0U)
        {
            return false;
        }
    }
    return true;
}
bool FormatCodec::ValidValue(const ValueType type, const UInt8* const bytes, const Size length) noexcept
{
    if ((length > MaximumValueBytes) || ((bytes == nullptr) && (length != 0U)))
    {
        return false;
    }
    switch (type)
    {
    case ValueType::None:
        return length == 0U;
    case ValueType::Integer:
        return length == sizeof(UInt64);
    case ValueType::Boolean:
        return (length == 1U) && (bytes[0U] <= 1U);
    case ValueType::String:
        return Text::ValidUtf8(bytes, length);
    case ValueType::Binary:
        return true;
    case ValueType::StringList:
    {
        if (length < 2U)
        {
            return false;
        }
        const Size count = static_cast<Size>(Little(bytes, 2U));
        if (count > (MaximumValueBytes - 2U) / 2U)
        {
            return false;
        }
        Size at{2U};
        for (Size item{0U}; item < count; ++item)
        {
            if (length - at < 2U)
            {
                return false;
            }
            const Size size = static_cast<Size>(Little(bytes + at, 2U));
            at += 2U;
            if ((size > length - at) || !Text::ValidUtf8(bytes + at, size))
            {
                return false;
            }
            at += size;
        }
        return at == length;
    }
    }
    return false;
}
bool FormatCodec::ValidAcl(const AclEntry* const entries, const Size count) noexcept
{
    if ((count > AclCapacity) || ((entries == nullptr) && (count != 0U)))
    {
        return false;
    }
    for (Size i{0U}; i < count; ++i)
    {
        if ((entries[i].Rights == 0U) || ((entries[i].Rights & ~AllRights) != 0U))
        {
            return false;
        }
    }
    return true;
}
bool FormatCodec::Encode(const Snapshot& state, const UInt64 generation, const UInt8 slot, UInt8* const bytes,
                         const Size length) noexcept
{
    if ((bytes == nullptr) || (length != SlotBytes) || (generation == 0U) || (slot > 1U))
    {
        return false;
    }
    for (Size i{0U}; i < SlotBytes; ++i)
    {
        bytes[i] = 0U;
    }
    for (Size i{0U}; i < 8U; ++i)
    {
        bytes[i] = Magic[i];
        bytes[SealOffset + i] = SealMagic[i];
    }
    Put(bytes + 8U, 4U, 2U);
    Put(bytes + 12U, 4U, BodyBytes);
    Put(bytes + 16U, 8U, generation);
    Put(bytes + 24U, 4U, slot);
    for (Size i{0U}; i < KeyCapacity; ++i)
    {
        const Key& key = state.Keys[i];
        if (!key.Used)
        {
            continue;
        }
        if ((key.NameLength > MaximumNameBytes) || !ValidAcl(key.Acl, key.AclCount))
        {
            return false;
        }
        UInt8* const p = bytes + HeaderBytes + i * KeyRecordBytes;
        p[0U] = 1U;
        p[1U] = key.Parent;
        p[2U] = key.NameLength;
        p[3U] = key.AclCount;
        for (Size n{0U}; n < key.NameLength; ++n)
        {
            p[8U + n] = key.Name[n];
        }
        for (Size a{0U}; a < key.AclCount; ++a)
        {
            UInt8* const entry = p + 8U + MaximumNameBytes + a * AclRecordBytes;
            Put(entry, 8U, key.Acl[a].Principal);
            entry[8U] = key.Acl[a].Rights;
            entry[9U] = key.Acl[a].Deny ? 1U : 0U;
            entry[10U] = key.Acl[a].Inherit ? 1U : 0U;
        }
    }
    for (Size i{0U}; i < ValueCapacity; ++i)
    {
        const NamedValue& value = state.Values[i];
        if (!value.Used)
        {
            continue;
        }
        if (!ValidValueName({value.Name, value.NameLength}) ||
            !ValidValue(value.Data.Type, value.Data.Bytes, value.Data.Length))
        {
            return false;
        }
        UInt8* const p = bytes + ValueTableOffset + i * ValueRecordBytes;
        p[0U] = 1U;
        p[1U] = value.KeyIndex;
        p[2U] = value.NameLength;
        p[3U] = static_cast<UInt8>(value.Data.Type);
        Put(p + 4U, 2U, value.Data.Length);
        for (Size n{0U}; n < value.NameLength; ++n)
        {
            p[8U + n] = value.Name[n];
        }
        for (Size v{0U}; v < value.Data.Length; ++v)
        {
            p[8U + MaximumNameBytes + v] = value.Data.Bytes[v];
        }
    }
    if (!ValidRecords(bytes) || !ValidValues(bytes))
    {
        return false;
    }
    const UInt32 crc = BodyCrc(bytes);
    Put(bytes + 28U, 4U, crc);
    UInt8* const seal = bytes + SealOffset;
    Put(seal + 8U, 8U, generation);
    Put(seal + 16U, 4U, crc);
    Put(seal + 20U, 4U, slot);
    Put(seal + 24U, 4U, Foundation::Crc32c::Compute(seal, 24U));
    return true;
}
bool FormatCodec::Validate(const UInt8* const bytes, const Size length, const UInt8 slot, UInt64& generation) noexcept
{
    if ((bytes == nullptr) || (length != SlotBytes) || (slot > 1U))
    {
        return false;
    }
    const UInt8* const seal = bytes + SealOffset;
    if (!Equal(bytes, Magic, 8U) || !Equal(seal, SealMagic, 8U) || (Little(bytes + 8U, 4U) != 2U) ||
        (Little(bytes + 12U, 4U) != BodyBytes) || (Little(bytes + 24U, 4U) != slot) ||
        (Little(seal + 20U, 4U) != slot) || !Zero(seal + 28U, 4U))
    {
        return false;
    }
    const UInt64 found = Little(bytes + 16U, 8U);
    if ((found == 0U) || (Little(seal + 8U, 8U) != found) ||
        (Little(seal + 24U, 4U) != Foundation::Crc32c::Compute(seal, 24U)))
    {
        return false;
    }
    const UInt32 crc = BodyCrc(bytes);
    if ((Little(bytes + 28U, 4U) != crc) || (Little(seal + 16U, 4U) != crc) || !ValidRecords(bytes) ||
        !ValidValues(bytes))
    {
        return false;
    }
    generation = found;
    return true;
}
bool FormatCodec::Decode(const UInt8* const bytes, const Size length, const UInt8 slot, Snapshot& state,
                         UInt64& generation) noexcept
{
    UInt64 found{0U};
    if (!Validate(bytes, length, slot, found))
    {
        return false;
    }
    for (Size i{0U}; i < KeyCapacity; ++i)
    {
        Key& key = state.Keys[i];
        const UInt8* const p = Record(bytes, i);
        key.Used = p[0U] != 0U;
        key.Parent = p[1U];
        key.NameLength = p[2U];
        key.AclCount = p[3U];
        for (Size n{0U}; n < MaximumNameBytes; ++n)
        {
            key.Name[n] = p[8U + n];
        }
        for (Size a{0U}; a < AclCapacity; ++a)
        {
            const UInt8* const entry = p + 8U + MaximumNameBytes + a * AclRecordBytes;
            key.Acl[a].Principal = Little(entry, 8U);
            key.Acl[a].Rights = entry[8U];
            key.Acl[a].Deny = entry[9U] != 0U;
            key.Acl[a].Inherit = entry[10U] != 0U;
        }
    }
    for (Size i{0U}; i < ValueCapacity; ++i)
    {
        NamedValue& value = state.Values[i];
        const UInt8* const p = ValueRecord(bytes, i);
        value.Used = p[0U] != 0U;
        value.KeyIndex = p[1U];
        value.NameLength = p[2U];
        value.Data.Type = static_cast<ValueType>(p[3U]);
        value.Data.Length = static_cast<UInt16>(Little(p + 4U, 2U));
        for (Size n{0U}; n < MaximumNameBytes; ++n)
        {
            value.Name[n] = p[8U + n];
        }
        for (Size v{0U}; v < MaximumValueBytes; ++v)
        {
            value.Data.Bytes[v] = p[8U + MaximumNameBytes + v];
        }
    }
    generation = found;
    return true;
}
} // namespace Asteria::User::Settings
