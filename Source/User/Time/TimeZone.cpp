// SPDX-License-Identifier: Apache-2.0

#include "User/Time/TimeZone.hpp"
#include "User/Time/Calendar.hpp"
#include "User/Text/Utf8.hpp"

namespace Asteria::User::Time
{
namespace
{
using Foundation::UInt8;
using Foundation::UInt64;
inline constexpr Size HeaderSize{44U};
inline constexpr Size TypeSize{6U};
static_assert(HeaderSize == 4U + 1U + 15U + 6U * 4U);
[[nodiscard]] UInt32 Unsigned(const UInt8* const data) noexcept
{
    UInt32 value{0U};
    for (Size index{0U}; index < 4U; ++index)
    {
        value = (value << 8U) | data[index];
    }
    return value;
}
[[nodiscard]] Int64 Signed(const UInt8* const data, const Size width) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < width; ++index)
    {
        value = (value << 8U) | data[index];
    }
    if ((data[0U] & 0x80U) == 0U)
    {
        return static_cast<Int64>(value);
    }
    const UInt64 mask = width == 4U ? 0xFFFFFFFFULL : ~UInt64{0U};
    return -1 - static_cast<Int64>((~value) & mask);
}
[[nodiscard]] bool SameText(const TextView left, const TextView right) noexcept
{
    if (left.Length != right.Length)
    {
        return false;
    }
    for (Size index{0U}; index < left.Length; ++index)
    {
        if (left.Data[index] != right.Data[index])
        {
            return false;
        }
    }
    return true;
}
[[nodiscard]] TextView Designation(const Detail::Block& block, const UInt32 index) noexcept
{
    const Size start = block.Types[index * TypeSize + 5U];
    Size length{0U};
    for (; start + length < block.NameCount && block.Names[start + length] != 0U; ++length)
    {
    }
    return {reinterpret_cast<const char*>(block.Names + start), length};
}
struct Counts final
{
    UInt32 Ut;
    UInt32 Standard;
    UInt32 Leaps;
    UInt32 Times;
    UInt32 Types;
    UInt32 Names;
};
// RFC 8536 (February 2019), §§3.1, 3.2 and 4. Bounds precede all slicing;
// both blocks are validated even though only the last block drives conversion.
[[nodiscard]] bool ReadBlock(const UInt8* const bytes, const Size size, Size& position, const UInt8 version,
                             const Size width, Detail::Block& block) noexcept
{
    if (size - position < HeaderSize)
    {
        return false;
    }
    const UInt8* const header = bytes + position;
    if (header[0U] != 'T' || header[1U] != 'Z' || header[2U] != 'i' || header[3U] != 'f' || header[4U] != version)
    {
        return false;
    }
    const Counts counts{Unsigned(header + 20U), Unsigned(header + 24U), Unsigned(header + 28U),
                        Unsigned(header + 32U), Unsigned(header + 36U), Unsigned(header + 40U)};
    if (counts.Times > TimeZone::MaximumTransitions || counts.Types == 0U || counts.Types > TimeZone::MaximumTypes ||
        counts.Names == 0U || counts.Names > TimeZone::MaximumDesignations || counts.Leaps > TimeZone::MaximumLeaps ||
        (counts.Ut != 0U && counts.Ut != counts.Types) || (counts.Standard != 0U && counts.Standard != counts.Types))
    {
        return false;
    }
    position += HeaderSize;
    const Size length = static_cast<Size>(counts.Times) * (width + 1U) + static_cast<Size>(counts.Types) * TypeSize +
                        counts.Names + static_cast<Size>(counts.Leaps) * (width + 4U) + counts.Standard + counts.Ut;
    if (length > size - position)
    {
        return false;
    }
    block = {bytes + position,
             bytes + position + counts.Times * width,
             bytes + position + counts.Times * (width + 1U),
             nullptr,
             counts.Times,
             counts.Types,
             counts.Names,
             width};
    block.Names = block.Types + counts.Types * TypeSize;
    const UInt8* const leaps = block.Names + counts.Names;
    const UInt8* const standard = leaps + counts.Leaps * (width + 4U);
    const UInt8* const ut = standard + counts.Standard;
    position += length;
    for (UInt32 index{0U}; index < counts.Times; ++index)
    {
        if (block.Indices[index] >= counts.Types ||
            (index != 0U &&
             Signed(block.Times + index * width, width) <= Signed(block.Times + (index - 1U) * width, width)))
        {
            return false;
        }
    }
    if (block.Names[counts.Names - 1U] != 0U || !Text::ValidUtf8(block.Names, counts.Names))
    {
        return false;
    }
    for (UInt32 index{0U}; index < counts.Types; ++index)
    {
        const UInt8* const type = block.Types + index * TypeSize;
        if (Signed(type, 4U) == -2147483648LL || type[4U] > 1U || type[5U] >= counts.Names)
        {
            return false;
        }
        const TextView name = Designation(block, index);
        if (!Text::ValidUtf8(reinterpret_cast<const UInt8*>(name.Data), name.Length))
        {
            return false;
        }
        const UInt8 standardFlag = counts.Standard == 0U ? 0U : standard[index];
        const UInt8 utFlag = counts.Ut == 0U ? 0U : ut[index];
        if (standardFlag > 1U || utFlag > 1U || (utFlag == 1U && standardFlag != 1U))
        {
            return false;
        }
    }
    Int64 previousTime{0};
    Int64 previousCorrection{0};
    for (UInt32 index{0U}; index < counts.Leaps; ++index)
    {
        const Int64 occurrence = Signed(leaps + index * (width + 4U), width);
        const Int64 correction = Signed(leaps + index * (width + 4U) + width, 4U);
        const Int64 difference = correction - previousCorrection;
        if (occurrence < 0 || (index != 0U && (occurrence <= previousTime || occurrence - previousTime < 2419199)) ||
            (difference != 1 && difference != -1))
        {
            return false;
        }
        previousTime = occurrence;
        previousCorrection = correction;
    }
    return true;
}
}
Status TimeZone::Load(const Foundation::UInt8* const bytes, const Size size) noexcept
{
    m_loaded = false;
    m_block = {};
    m_footer = {};
    if (bytes == nullptr || size < HeaderSize || size > MaximumFileSize)
    {
        return Status::Invalid;
    }
    const UInt8 version = bytes[4U];
    if (version != 0U && version != '2' && version != '3')
    {
        return Status::Invalid;
    }
    Size position{0U};
    if (!ReadBlock(bytes, size, position, version, 4U, m_block))
    {
        return Status::Invalid;
    }
    if (version == 0U)
    {
        m_loaded = position == size;
    }
    else
    {
        if (!ReadBlock(bytes, size, position, version, 8U, m_block) || size - position < 2U ||
            size - position - 2U > MaximumFooterSize || bytes[position] != '\n' || bytes[size - 1U] != '\n' ||
            !Detail::ParseFooter(reinterpret_cast<const char*>(bytes + position + 1U), size - position - 2U,
                                 version == '3', m_footer))
        {
            return Status::Invalid;
        }
        if (m_footer.Present && m_block.TimeCount != 0U)
        {
            const UInt32 last = m_block.TimeCount - 1U;
            const LocalType table = TableType(m_block.Indices[last]);
            const LocalType footer = Detail::FooterType(m_footer, Signed(m_block.Times + last * 8U, 8U));
            if (table.OffsetSeconds != footer.OffsetSeconds || table.Daylight != footer.Daylight ||
                !SameText(table.Designation, footer.Designation))
            {
                return Status::Invalid;
            }
        }
        m_loaded = true;
    }
    return m_loaded ? Status::Ok : Status::Invalid;
}
LocalType TimeZone::TableType(const UInt32 index) const noexcept
{
    const UInt8* const data = m_block.Types + index * TypeSize;
    return {static_cast<Int32>(Signed(data, 4U)), data[4U] != 0U, Designation(m_block, index)};
}
Status TimeZone::TypeAt(const Int64 utc, LocalType& type) const noexcept
{
    if (!m_loaded)
    {
        return Status::Invalid;
    }
    UInt32 lower{0U};
    UInt32 upper = m_block.TimeCount;
    // At most ceil(log2(MaximumTransitions+1)) iterations.
    for (; lower < upper;)
    {
        const UInt32 middle = lower + (upper - lower) / 2U;
        if (Signed(m_block.Times + middle * m_block.TimeWidth, m_block.TimeWidth) <= utc)
        {
            lower = middle + 1U;
        }
        else
        {
            upper = middle;
        }
    }
    if (lower == m_block.TimeCount && m_footer.Present)
    {
        type = Detail::FooterType(m_footer, utc);
        return Status::Ok;
    }
    if (lower == m_block.TimeCount && lower != 0U)
    {
        return Status::Unspecified;
    }
    type = TableType(lower == 0U ? 0U : m_block.Indices[lower - 1U]);
    return Status::Ok;
}
Status TimeZone::ToLocal(const Int64 utc, LocalTime& local) const noexcept
{
    LocalType type{};
    const Status status = TypeAt(utc, type);
    if (status != Status::Ok)
    {
        return status;
    }
    // This also protects utc+offset from overflow at either Int64 endpoint.
    if (utc < Detail::MinimumSeconds - type.OffsetSeconds || utc > Detail::MaximumSeconds - type.OffsetSeconds)
    {
        return Status::OutOfRange;
    }
    Civil civil{};
    const Status converted = FromSeconds(utc + type.OffsetSeconds, civil);
    if (converted == Status::Ok)
    {
        local = {civil, type};
    }
    return converted;
}
Int32 TimeZone::CandidateOffset(const UInt32 index) const noexcept
{
    if (index < m_block.TypeCount)
    {
        return static_cast<Int32>(Signed(m_block.Types + index * TypeSize, 4U));
    }
    return index == m_block.TypeCount ? m_footer.Standard.OffsetSeconds : m_footer.Daylight.OffsetSeconds;
}
Status TimeZone::FromLocal(const Civil& local, LocalMatches& matches) const noexcept
{
    matches = {};
    if (!m_loaded)
    {
        return Status::Invalid;
    }
    Int64 civil{0};
    const Status status = ToSeconds(local, civil);
    if (status != Status::Ok)
    {
        return status;
    }
    const UInt32 count = m_block.TypeCount + (m_footer.Present ? (m_footer.HasDaylight ? 2U : 1U) : 0U);
    bool unknown{false};
    for (UInt32 index{0U}; index < count; ++index)
    {
        const Int32 offset = CandidateOffset(index);
        bool duplicate{false};
        for (UInt32 previous{0U}; previous < index; ++previous)
        {
            if (CandidateOffset(previous) == offset)
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
        {
            continue;
        }
        const Int64 utc = civil - offset;
        LocalType type{};
        const Status resolved = TypeAt(utc, type);
        unknown = unknown || resolved == Status::Unspecified;
        if (resolved == Status::Ok && type.OffsetSeconds == offset)
        {
            if (matches.Count == 0U || utc < matches.Earliest)
            {
                matches.Earliest = utc;
            }
            if (matches.Count == 0U || utc > matches.Latest)
            {
                matches.Latest = utc;
            }
            ++matches.Count;
        }
    }
    if (unknown)
    {
        matches = {};
        return Status::Unspecified;
    }
    return matches.Count == 0U ? Status::Gap : (matches.Count == 1U ? Status::Ok : Status::Ambiguous);
}
} // namespace Asteria::User::Time
