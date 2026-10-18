// SPDX-License-Identifier: Apache-2.0

#include "User/Font/Font.hpp"

#include "User/Font/Internal.hpp"
#include "User/Text/Utf8.hpp"
namespace Asteria::User::Font
{
using Detail::Budget;
using Detail::Offset;
using Detail::ReadIndex;
using Detail::S16;
using Detail::S32;
using Detail::U16;
using Detail::U32;
using Detail::U8;
using Detail::ValidateLayout;
Bytes Index::Object(const UInt32 index) const noexcept
{
    if (index >= Count)
    {
        return {};
    }
    const UInt32 first{Offset(Offsets, static_cast<Size>(index) * OffsetSize, OffsetSize)};
    const UInt32 last{Offset(Offsets, static_cast<Size>(index + 1U) * OffsetSize, OffsetSize)};
    return (first != 0U && last >= first) ? Objects.Slice(first - 1U, last - first) : Bytes{};
}
Error Detail::ReadIndex(const Bytes data, Size& at, Index& index) noexcept
{
    index = {};
    if (!data.Contains(at, 2U))
    {
        return Error::Truncated;
    }
    index.Count = U16(data, at);
    at += 2U;
    if (index.Count == 0U)
    {
        return Error::None;
    }
    index.OffsetSize = U8(data, at++);
    if (index.OffsetSize < 1U || index.OffsetSize > 4U)
    {
        return Error::Malformed;
    }
    const Size bytes{static_cast<Size>(index.Count + 1U) * index.OffsetSize};
    if (!data.Contains(at, bytes))
    {
        return Error::Truncated;
    }
    index.Offsets = data.Slice(at, bytes);
    at += bytes;
    UInt32 previous{1U};
    if (Offset(index.Offsets, 0U, index.OffsetSize) != 1U)
    {
        return Error::Malformed;
    }
    for (UInt32 i{1U}; i <= index.Count; ++i)
    {
        const UInt32 next{Offset(index.Offsets, static_cast<Size>(i) * index.OffsetSize, index.OffsetSize)};
        if (next < previous)
        {
            return Error::Malformed;
        }
        previous = next;
    }
    if (!data.Contains(at, previous - 1U))
    {
        return Error::Truncated;
    }
    index.Objects = data.Slice(at, previous - 1U);
    at += previous - 1U;
    return Error::None;
}
Bytes Font::Table(const UInt32 tag) const noexcept
{
    for (UInt32 i{}; i < m_tableCount; ++i)
    {
        const Size at{12U + static_cast<Size>(i) * 16U};
        if (U32(m_data, at) == tag)
        {
            return m_data.Slice(U32(m_data, at + 8U), U32(m_data, at + 12U));
        }
    }
    return {};
}
Error Font::Open(const Bytes data) noexcept
{
    m_open = false;
    m_tableCount = 0U;
    m_metrics = {};
    m_cmap = {};
    m_charstrings = {};
    m_globals = {};
    m_locals = {};
    m_defaultWidth = 0;
    m_nominalWidth = 0;
    if (data.Length > MaximumFontBytes)
    {
        return Error::Limit;
    }
    if (!data.Contains(0U, 12U))
    {
        return Error::Truncated;
    }
    const UInt32 version{U32(data, 0U)};
    if (version != 0x00010000U && version != Tag('O', 'T', 'T', 'O'))
    {
        return Error::Unsupported;
    }
    m_cff = version != 0x00010000U;
    const UInt16 count{U16(data, 4U)};
    if (count == 0U || count > MaximumTables)
    {
        return Error::Limit;
    }
    const Size directoryEnd{12U + static_cast<Size>(count) * 16U};
    if (!data.Contains(0U, directoryEnd))
    {
        return Error::Truncated;
    }
    UInt32 previousTag{};
    for (UInt32 i{}; i < count; ++i)
    {
        const Size at{12U + static_cast<Size>(i) * 16U};
        const UInt32 tag{U32(data, at)};
        const UInt32 start{U32(data, at + 8U)};
        const UInt32 length{U32(data, at + 12U)};
        if (i != 0U && tag <= previousTag)
        {
            return Error::Malformed;
        }
        previousTag = tag;
        if (!data.Contains(start, length))
        {
            return Error::Truncated;
        }
        if (length != 0U && (start < directoryEnd || (start & 3U) != 0U))
        {
            return Error::Malformed;
        }
        for (UInt32 j{}; j < i && length != 0U; ++j)
        {
            const Size other{12U + static_cast<Size>(j) * 16U};
            const UInt32 s{U32(data, other + 8U)};
            const UInt32 n{U32(data, other + 12U)};
            if (n != 0U && start < static_cast<UInt64>(s) + n && s < static_cast<UInt64>(start) + length)
            {
                return Error::Malformed;
            }
        }
    }
    m_data = data;
    m_tableCount = count;
    Error error{OpenMetrics()};
    if (error != Error::None)
    {
        return error;
    }
    error = OpenCmap();
    if (error != Error::None)
    {
        return error;
    }
    error = OpenNames();
    if (error != Error::None)
    {
        return error;
    }
    if (m_cff)
    {
        error = OpenCff();
        if (error != Error::None)
        {
            return error;
        }
    }
    else
    {
        const Bytes loca{Table(Tag('l', 'o', 'c', 'a'))};
        const Bytes glyf{Table(Tag('g', 'l', 'y', 'f'))};
        if (glyf.Data == nullptr ||
            !loca.Contains(0U, static_cast<Size>(m_metrics.GlyphCount + 1U) * (m_locaFormat == 0U ? 2U : 4U)))
        {
            return Error::Truncated;
        }
        UInt32 previous{};
        for (UInt32 i{}; i <= m_metrics.GlyphCount; ++i)
        {
            const UInt32 next{GlyphOffset(i)};
            if (next < previous || next > glyf.Length)
            {
                return Error::Malformed;
            }
            if (next != previous && next - previous < 10U)
            {
                return Error::Truncated;
            }
            previous = next;
        }
    }
    Budget budget{};
    error = ValidateLayout(Table(Tag('G', 'P', 'O', 'S')), true, m_metrics.GlyphCount, budget);
    if (error != Error::None)
    {
        return error;
    }
    error = ValidateLayout(Table(Tag('G', 'S', 'U', 'B')), false, m_metrics.GlyphCount, budget);
    if (error != Error::None)
    {
        return error;
    }
    m_open = true;
    return Error::None;
}
Error Font::OpenMetrics() noexcept
{
    // OpenType 1.9: head, maxp, hhea/hmtx, OS/2 and post table formats.
    const Bytes head{Table(Tag('h', 'e', 'a', 'd'))};
    const Bytes maxp{Table(Tag('m', 'a', 'x', 'p'))};
    const Bytes hhea{Table(Tag('h', 'h', 'e', 'a'))};
    const Bytes hmtx{Table(Tag('h', 'm', 't', 'x'))};
    if (!head.Contains(0U, 54U) || !maxp.Contains(0U, 6U) || !hhea.Contains(0U, 36U))
    {
        return Error::Truncated;
    }
    if (U32(head, 0U) != 0x10000U || U32(head, 12U) != 0x5F0F3CF5U || S16(head, 52U) != 0 ||
        U32(hhea, 0U) != 0x10000U || S16(hhea, 32U) != 0)
    {
        return Error::Malformed;
    }
    const UInt32 mv{U32(maxp, 0U)};
    if (mv != (m_cff ? 0x5000U : 0x10000U))
    {
        return Error::Unsupported;
    }
    if (!m_cff && !maxp.Contains(0U, 32U))
    {
        return Error::Truncated;
    }
    m_metrics.UnitsPerEm = U16(head, 18U);
    m_locaFormat = U16(head, 50U);
    m_metrics.GlyphCount = U16(maxp, 4U);
    m_metrics.HorizontalMetrics = U16(hhea, 34U);
    if (m_metrics.UnitsPerEm < 16U || m_metrics.UnitsPerEm > 16384U || m_locaFormat > 1U ||
        m_metrics.GlyphCount == 0U || m_metrics.HorizontalMetrics == 0U ||
        m_metrics.HorizontalMetrics > m_metrics.GlyphCount)
    {
        return Error::Malformed;
    }
    if (!hmtx.Contains(0U, static_cast<Size>(m_metrics.HorizontalMetrics) * 4U +
                               static_cast<Size>(m_metrics.GlyphCount - m_metrics.HorizontalMetrics) * 2U))
    {
        return Error::Truncated;
    }
    m_metrics.Ascender = S16(hhea, 4U);
    m_metrics.Descender = S16(hhea, 6U);
    m_metrics.LineGap = S16(hhea, 8U);
    const Bytes os2{Table(Tag('O', 'S', '/', '2'))};
    if (os2.Data != nullptr)
    {
        const UInt16 v{U16(os2, 0U)};
        if (v > 5U)
        {
            return Error::Unsupported;
        }
        const Size minimum{v == 0U ? 78U : (v == 1U ? 86U : (v < 5U ? 96U : 100U))};
        if (!os2.Contains(0U, minimum))
        {
            return Error::Truncated;
        }
        m_metrics.Weight = U16(os2, 4U);
        m_metrics.Width = U16(os2, 6U);
        m_metrics.TypoAscender = S16(os2, 68U);
        m_metrics.TypoDescender = S16(os2, 70U);
        m_metrics.TypoLineGap = S16(os2, 72U);
        m_metrics.WinAscender = U16(os2, 74U);
        m_metrics.WinDescender = U16(os2, 76U);
        m_metrics.UseTypoMetrics = (U16(os2, 62U) & 0x80U) != 0U;
        if (v >= 2U)
        {
            m_metrics.XHeight = S16(os2, 86U);
            m_metrics.CapHeight = S16(os2, 88U);
        }
    }
    const Bytes post{Table(Tag('p', 'o', 's', 't'))};
    if (post.Data != nullptr)
    {
        if (!post.Contains(0U, 32U))
        {
            return Error::Truncated;
        }
        const UInt32 v{U32(post, 0U)};
        if (v != 0x10000U && v != 0x20000U && v != 0x25000U && v != 0x30000U)
        {
            return Error::Unsupported;
        }
        m_metrics.ItalicAngle = S32(post, 4U);
        m_metrics.UnderlinePosition = S16(post, 8U);
        m_metrics.UnderlineThickness = S16(post, 10U);
        m_metrics.FixedPitch = U32(post, 12U) != 0U;
        if (v == 0x20000U || v == 0x25000U)
        {
            if (!post.Contains(32U, 2U) || U16(post, 32U) != m_metrics.GlyphCount)
            {
                return Error::Malformed;
            }
            if (!post.Contains(34U, static_cast<Size>(m_metrics.GlyphCount) * (v == 0x20000U ? 2U : 1U)))
            {
                return Error::Truncated;
            }
            UInt32 customCount{};
            for (UInt32 i{}; i < m_metrics.GlyphCount; ++i)
            {
                if (v == 0x25000U)
                {
                    const Int32 id{static_cast<Int32>(i) + static_cast<Int8>(U8(post, 34U + i))};
                    if (id < 0 || id > 257)
                    {
                        return Error::Malformed;
                    }
                }
                else
                {
                    const UInt16 id{U16(post, 34U + static_cast<Size>(i) * 2U)};
                    if (id > 32767U)
                    {
                        return Error::Malformed;
                    }
                    if (id >= 258U && id - 257U > customCount)
                    {
                        customCount = id - 257U;
                    }
                }
            }
            Size at{34U + static_cast<Size>(m_metrics.GlyphCount) * 2U};
            for (UInt32 i{}; v == 0x20000U && i < customCount; ++i)
            {
                if (!post.Contains(at, 1U))
                {
                    return Error::Truncated;
                }
                const UInt8 length{U8(post, at++)};
                if (!post.Contains(at, length))
                {
                    return Error::Truncated;
                }
                at += length;
            }
        }
    }
    return Error::None;
}
UInt32 Font::GlyphOffset(const UInt32 glyph) const noexcept
{
    if (glyph > m_metrics.GlyphCount)
    {
        return 0U;
    }
    const Bytes loca{Table(Tag('l', 'o', 'c', 'a'))};
    return m_locaFormat == 0U ? static_cast<UInt32>(U16(loca, static_cast<Size>(glyph) * 2U)) * 2U
                              : U32(loca, static_cast<Size>(glyph) * 4U);
}
GlyphMetrics Font::Horizontal(const UInt16 glyph) const noexcept
{
    if (!m_open || glyph >= m_metrics.GlyphCount)
    {
        return {};
    }
    const Bytes hmtx{Table(Tag('h', 'm', 't', 'x'))};
    const UInt32 index{glyph < m_metrics.HorizontalMetrics ? glyph
                                                           : static_cast<UInt32>(m_metrics.HorizontalMetrics - 1U)};
    const Size bearing{glyph < m_metrics.HorizontalMetrics
                           ? static_cast<Size>(glyph) * 4U + 2U
                           : static_cast<Size>(m_metrics.HorizontalMetrics) * 4U +
                                 static_cast<Size>(glyph - m_metrics.HorizontalMetrics) * 2U};
    return {U16(hmtx, static_cast<Size>(index) * 4U), S16(hmtx, bearing)};
}
namespace
{
[[nodiscard]] UInt16 Map4(const Bytes sub, const UInt32 scalar) noexcept
{
    if (scalar > 0xFFFFU)
    {
        return 0U;
    }
    const UInt32 n{U16(sub, 6U) / 2U};
    for (UInt32 i{}; i < n; ++i)
    {
        if (scalar > U16(sub, 14U + static_cast<Size>(i) * 2U))
        {
            continue;
        }
        const UInt16 start{U16(sub, 16U + static_cast<Size>(n + i) * 2U)};
        if (scalar < start)
        {
            return 0U;
        }
        const Int16 delta{S16(sub, 16U + static_cast<Size>(2U * n + i) * 2U)};
        const Size roAt{16U + static_cast<Size>(3U * n + i) * 2U};
        const UInt16 ro{U16(sub, roAt)};
        if (ro == 0U)
        {
            return static_cast<UInt16>(scalar + static_cast<UInt32>(delta));
        }
        const UInt16 glyph{U16(sub, roAt + ro + static_cast<Size>(scalar - start) * 2U)};
        return glyph == 0U ? 0U : static_cast<UInt16>(static_cast<Int32>(glyph) + delta);
    }
    return 0U;
}
[[nodiscard]] Error CheckCmap(const Bytes sub, const UInt16 glyphs, Budget& budget) noexcept
{
    if (U16(sub, 0U) == 12U)
    {
        if (!sub.Contains(0U, 16U) || U16(sub, 2U) != 0U)
        {
            return Error::Truncated;
        }
        const UInt32 n{U32(sub, 12U)};
        if (!sub.Contains(16U, static_cast<Size>(n) * 12U))
        {
            return Error::Truncated;
        }
        if (!budget.Spend(n))
        {
            return Error::OperationLimit;
        }
        UInt32 previous{};
        for (UInt32 i{}; i < n; ++i)
        {
            const Size at{16U + static_cast<Size>(i) * 12U};
            const UInt32 start{U32(sub, at)};
            const UInt32 end{U32(sub, at + 4U)};
            const UInt32 first{U32(sub, at + 8U)};
            if (start > end || end > 0x10FFFFU || (i != 0U && start <= previous) ||
                static_cast<UInt64>(first) + end - start >= glyphs)
            {
                return Error::Malformed;
            }
            previous = end;
        }
        return Error::None;
    }
    if (!sub.Contains(0U, 16U))
    {
        return Error::Truncated;
    }
    const UInt32 n{U16(sub, 6U) / 2U};
    if (n == 0U || (U16(sub, 6U) & 1U) != 0U || !sub.Contains(0U, 16U + static_cast<Size>(n) * 8U))
    {
        return Error::Malformed;
    }
    UInt32 previous{};
    for (UInt32 i{}; i < n; ++i)
    {
        const UInt32 end{U16(sub, 14U + static_cast<Size>(i) * 2U)},
            start{U16(sub, 16U + static_cast<Size>(n + i) * 2U)};
        if (start > end || (i != 0U && start <= previous))
        {
            return Error::Malformed;
        }
        const Size roAt{16U + static_cast<Size>(3U * n + i) * 2U};
        const UInt16 ro{U16(sub, roAt)};
        if ((ro & 1U) != 0U || (ro != 0U && (roAt + ro < 16U + static_cast<Size>(n) * 8U ||
                                             !sub.Contains(roAt + ro, static_cast<Size>(end - start + 1U) * 2U))))
        {
            return Error::Truncated;
        }
        if (!budget.Spend(end - start + 1U))
        {
            return Error::OperationLimit;
        }
        // Check this segment directly, avoiding quadratic segment searches.
        const Int16 delta{S16(sub, 16U + static_cast<Size>(2U * n + i) * 2U)};
        for (UInt32 point{start}; point <= end; ++point)
        {
            const UInt16 raw{ro == 0U ? static_cast<UInt16>(point)
                                      : U16(sub, roAt + ro + static_cast<Size>(point - start) * 2U)};
            const UInt16 glyph{ro != 0U && raw == 0U ? UInt16{0U}
                                                     : static_cast<UInt16>(static_cast<Int32>(raw) + delta)};
            if (glyph >= glyphs)
            {
                return Error::InvalidGlyph;
            }
        }
        previous = end;
    }
    return previous == 0xFFFFU && U16(sub, 16U + static_cast<Size>(2U * n - 1U) * 2U) == 0xFFFFU &&
                   U16(sub, 14U + static_cast<Size>(n) * 2U) == 0U
               ? Error::None
               : Error::Malformed;
}
} // namespace
Error Font::OpenCmap() noexcept
{
    const Bytes table{Table(Tag('c', 'm', 'a', 'p'))};
    if (!table.Contains(0U, 4U) || U16(table, 0U) != 0U)
    {
        return Error::Truncated;
    }
    const UInt16 n{U16(table, 2U)};
    if (n > MaximumLayoutItems)
    {
        return Error::Limit;
    }
    if (!table.Contains(4U, static_cast<Size>(n) * 8U))
    {
        return Error::Truncated;
    }
    Budget budget{};
    for (UInt32 i{}; i < n; ++i)
    {
        const Size at{4U + static_cast<Size>(i) * 8U};
        const UInt32 offset{U32(table, at + 4U)};
        if (!table.Contains(offset, 2U))
        {
            return Error::Truncated;
        }
        const UInt16 format{U16(table, offset)};
        if (format != 4U && format != 12U)
        {
            continue;
        }
        if (!table.Contains(offset, format == 4U ? 4U : 8U))
        {
            return Error::Truncated;
        }
        const Size length{format == 4U ? U16(table, offset + 2U) : U32(table, offset + 4U)};
        const Bytes sub{table.Slice(offset, length)};
        if (sub.Data == nullptr)
        {
            return Error::Truncated;
        }
        const Error error{CheckCmap(sub, m_metrics.GlyphCount, budget)};
        if (error != Error::None)
        {
            return error;
        }
        const UInt16 platform{U16(table, at)};
        const UInt16 encoding{U16(table, at + 2U)};
        if ((platform == 0U || (platform == 3U && (encoding == 1U || encoding == 10U))) &&
            (m_cmap.Data == nullptr || format > U16(m_cmap, 0U)))
        {
            m_cmap = sub;
        }
    }
    return m_cmap.Data == nullptr ? Error::Unsupported : Error::None;
}
UInt16 Font::Glyph(const UInt32 scalar) const noexcept
{
    if (!m_open || !Text::IsScalar(scalar))
    {
        return 0U;
    }
    if (U16(m_cmap, 0U) == 4U)
    {
        return Map4(m_cmap, scalar);
    }
    UInt32 low{};
    UInt32 high{U32(m_cmap, 12U)};
    while (low < high)
    {
        const UInt32 mid{low + (high - low) / 2U};
        const Size at{16U + static_cast<Size>(mid) * 12U};
        if (scalar < U32(m_cmap, at))
        {
            high = mid;
        }
        else if (scalar > U32(m_cmap, at + 4U))
        {
            low = mid + 1U;
        }
        else
        {
            return static_cast<UInt16>(U32(m_cmap, at + 8U) + scalar - U32(m_cmap, at));
        }
    }
    return 0U;
}
namespace
{
[[nodiscard]] Error UnicodeName(const Bytes input, UInt8* out, const Size capacity, Size& length) noexcept
{
    length = 0U;
    if ((input.Length & 1U) != 0U)
    {
        return Error::Malformed;
    }
    for (Size at{}; at < input.Length; at += 2U)
    {
        UInt32 point{U16(input, at)};
        if (point >= 0xD800U && point <= 0xDBFFU)
        {
            if (!input.Contains(at + 2U, 2U))
            {
                return Error::Malformed;
            }
            const UInt16 low{U16(input, at + 2U)};
            if (low < 0xDC00U || low > 0xDFFFU)
            {
                return Error::Malformed;
            }
            point = 0x10000U + (point - 0xD800U) * 1024U + low - 0xDC00U;
            at += 2U;
        }
        else if (point >= 0xDC00U && point <= 0xDFFFU)
        {
            return Error::Malformed;
        }
        const Size bytes{point < 0x80U ? 1U : (point < 0x800U ? 2U : (point < 0x10000U ? 3U : 4U))};
        if (out != nullptr)
        {
            if (length > capacity || bytes > capacity - length)
            {
                return Error::BufferTooSmall;
            }
            static_cast<void>(Text::EncodeUtf8(point, out + length));
        }
        length += bytes;
    }
    return Error::None;
}
[[nodiscard]] bool UnicodeRecord(const Bytes table, const Size at) noexcept
{
    return U16(table, at) == 0U || (U16(table, at) == 3U && (U16(table, at + 2U) == 1U || U16(table, at + 2U) == 10U));
}
} // namespace
Error Font::OpenNames() noexcept
{
    const Bytes table{Table(Tag('n', 'a', 'm', 'e'))};
    if (table.Data == nullptr)
    {
        return Error::None;
    }
    if (!table.Contains(0U, 6U))
    {
        return Error::Truncated;
    }
    const UInt16 format{U16(table, 0U)};
    const UInt16 n{U16(table, 2U)};
    const UInt16 base{U16(table, 4U)};
    if (format > 1U)
    {
        return Error::Unsupported;
    }
    if (n > MaximumLayoutItems)
    {
        return Error::Limit;
    }
    Size end{6U + static_cast<Size>(n) * 12U};
    if (!table.Contains(0U, end))
    {
        return Error::Truncated;
    }
    Budget budget{};
    UInt16 languages{};
    if (format == 1U)
    {
        if (!table.Contains(end, 2U))
        {
            return Error::Truncated;
        }
        languages = U16(table, end);
        end += 2U;
        if (languages > MaximumLayoutItems || !table.Contains(end, static_cast<Size>(languages) * 4U))
        {
            return Error::Truncated;
        }
        for (UInt32 i{}; i < languages; ++i)
        {
            const Size at{end + static_cast<Size>(i) * 4U};
            const Bytes text{table.Slice(static_cast<Size>(base) + U16(table, at + 2U), U16(table, at))};
            Size size{};
            if (text.Data == nullptr)
            {
                return Error::Truncated;
            }
            if (!budget.Spend(text.Length))
            {
                return Error::OperationLimit;
            }
            const Error error{UnicodeName(text, nullptr, 0U, size)};
            if (error != Error::None)
            {
                return error;
            }
        }
        end += static_cast<Size>(languages) * 4U;
    }
    if (base < end || base > table.Length)
    {
        return Error::Malformed;
    }
    for (UInt32 i{}; i < n; ++i)
    {
        const Size at{6U + static_cast<Size>(i) * 12U};
        const UInt16 language{U16(table, at + 4U)};
        if (language >= 0x8000U && (format != 1U || language - 0x8000U >= languages))
        {
            return Error::Malformed;
        }
        const Bytes text{table.Slice(static_cast<Size>(base) + U16(table, at + 10U), U16(table, at + 8U))};
        if (text.Data == nullptr)
        {
            return Error::Truncated;
        }
        if (!budget.Spend(text.Length))
        {
            return Error::OperationLimit;
        }
        if (UnicodeRecord(table, at))
        {
            Size size{};
            const Error error{UnicodeName(text, nullptr, 0U, size)};
            if (error != Error::None)
            {
                return error;
            }
        }
    }
    return Error::None;
}
Error Font::Name(const UInt16 id, UInt8* const output, const Size capacity, Size& length) const noexcept
{
    length = 0U;
    if (!m_open || (output == nullptr && capacity != 0U))
    {
        return Error::InvalidArgument;
    }
    const Bytes table{Table(Tag('n', 'a', 'm', 'e'))};
    Size selected{};
    for (UInt32 i{}; i < U16(table, 2U); ++i)
    {
        const Size at{6U + static_cast<Size>(i) * 12U};
        if (U16(table, at + 6U) != id || !UnicodeRecord(table, at))
        {
            continue;
        }
        if (selected == 0U || U16(table, at + 4U) == 0x0409U)
        {
            selected = at;
        }
        if (U16(table, at + 4U) == 0x0409U)
        {
            break;
        }
    }
    if (selected == 0U)
    {
        return Error::None;
    }
    return UnicodeName(
        table.Slice(static_cast<Size>(U16(table, 4U)) + U16(table, selected + 10U), U16(table, selected + 8U)), output,
        capacity, length);
}
Error Font::FamilyName(UInt8* const output, const Size capacity, Size& length) const noexcept
{
    const Error error{Name(16U, output, capacity, length)};
    return error != Error::None || length != 0U ? error : Name(1U, output, capacity, length);
}
Error Font::StyleName(UInt8* const output, const Size capacity, Size& length) const noexcept
{
    const Error error{Name(17U, output, capacity, length)};
    return error != Error::None || length != 0U ? error : Name(2U, output, capacity, length);
}

} // namespace Asteria::User::Font
