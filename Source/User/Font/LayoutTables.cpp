// SPDX-License-Identifier: Apache-2.0

#include "User/Font/LayoutInternal.hpp"
namespace Asteria::User::Font::Detail
{
namespace
{
[[nodiscard]] Error CheckCoverage(const Bytes b, const UInt16 glyphs, UInt32& total, Budget& budget) noexcept
{
    if (!b.Contains(0U, 4U))
    {
        return Error::Truncated;
    }
    const UInt16 format{U16(b, 0U)};
    const UInt16 count{U16(b, 2U)};
    total = 0U;
    if (format != 1U && format != 2U)
    {
        return Error::Unsupported;
    }
    if (!b.Contains(4U, static_cast<Size>(count) * (format == 1U ? 2U : 6U)))
    {
        return Error::Truncated;
    }
    if (!budget.Spend(count))
    {
        return Error::OperationLimit;
    }
    UInt32 previous{};
    for (UInt32 i{}; i < count; ++i)
    {
        const Size at{4U + static_cast<Size>(i) * (format == 1U ? 2U : 6U)};
        const UInt16 first{U16(b, at)};
        const UInt16 last{format == 1U ? first : U16(b, at + 2U)};
        if (first > last || last >= glyphs || (i != 0U && first <= previous))
        {
            return Error::Malformed;
        }
        if (format == 2U && U16(b, at + 4U) != total)
        {
            return Error::Malformed;
        }
        total += last - first + 1U;
        previous = last;
    }
    return Error::None;
}
[[nodiscard]] Error CheckClass(const Bytes b, const UInt16 glyphs, const UInt16 classes, Budget& budget) noexcept
{
    if (!b.Contains(0U, 4U))
    {
        return Error::Truncated;
    }
    const UInt16 format{U16(b, 0U)};
    if (format == 1U)
    {
        if (!b.Contains(0U, 6U))
        {
            return Error::Truncated;
        }
        const UInt16 start{U16(b, 2U)};
        const UInt16 count{U16(b, 4U)};
        if (static_cast<UInt32>(start) + count > glyphs || !b.Contains(6U, static_cast<Size>(count) * 2U))
        {
            return Error::Malformed;
        }
        if (!budget.Spend(count))
        {
            return Error::OperationLimit;
        }
        for (UInt32 i{}; i < count; ++i)
        {
            if (U16(b, 6U + static_cast<Size>(i) * 2U) >= classes)
            {
                return Error::Malformed;
            }
        }
    }
    else if (format == 2U)
    {
        const UInt16 count{U16(b, 2U)};
        UInt32 previous{};
        if (!b.Contains(4U, static_cast<Size>(count) * 6U))
        {
            return Error::Truncated;
        }
        if (!budget.Spend(count))
        {
            return Error::OperationLimit;
        }
        for (UInt32 i{}; i < count; ++i)
        {
            const Size at{4U + static_cast<Size>(i) * 6U};
            const UInt16 start{U16(b, at)};
            const UInt16 end{U16(b, at + 2U)};
            if (start > end || end >= glyphs || (i != 0U && start <= previous) || U16(b, at + 4U) >= classes)
            {
                return Error::Malformed;
            }
            previous = end;
        }
    }
    else
    {
        return Error::Unsupported;
    }
    return Error::None;
}
[[nodiscard]] UInt32 ValueSize(const UInt16 format) noexcept
{
    UInt32 count{};
    for (UInt32 bit{}; bit < 8U; ++bit)
    {
        if ((format & (1U << bit)) != 0U)
        {
            count += 2U;
        }
    }
    return count;
}
[[nodiscard]] Error Device(const Bytes b) noexcept
{
    if (!b.Contains(0U, 6U))
    {
        return Error::Truncated;
    }
    const UInt16 start{U16(b, 0U)};
    const UInt16 end{U16(b, 2U)};
    const UInt16 format{U16(b, 4U)};
    if (format == 0x8000U)
    {
        return Error::Unsupported;
    }
    if (start > end || format < 1U || format > 3U)
    {
        return Error::Malformed;
    }
    const UInt32 bits{1U << format};
    const UInt32 words{((static_cast<UInt32>(end) - start + 1U) * bits + 15U) / 16U};
    return b.Contains(6U, static_cast<Size>(words) * 2U) ? Error::None : Error::Truncated;
}
[[nodiscard]] Error Value(const Bytes parent, Size& at, const UInt16 format) noexcept
{
    if ((format & 0xFF00U) != 0U)
    {
        return Error::Unsupported;
    }
    if (!parent.Contains(at, ValueSize(format)))
    {
        return Error::Truncated;
    }
    for (UInt32 bit{}; bit < 8U; ++bit)
    {
        if ((format & (1U << bit)) == 0U)
        {
            continue;
        }
        if (bit >= 4U)
        {
            const UInt16 offset{U16(parent, at)};
            if (offset != 0U)
            {
                const Error e{Device(Tail(parent, offset))};
                if (e != Error::None)
                {
                    return e;
                }
            }
        }
        at += 2U;
    }
    return Error::None;
}
[[nodiscard]] Error PairTable(const Bytes b, const UInt16 glyphs, Budget& budget) noexcept
{
    if (!b.Contains(0U, 10U) || U16(b, 2U) == 0U)
    {
        return Error::Truncated;
    }
    const UInt16 format{U16(b, 0U)};
    const UInt16 f1{U16(b, 4U)};
    const UInt16 f2{U16(b, 6U)};
    if (((f1 | f2) & 0xFF00U) != 0U)
    {
        return Error::Unsupported;
    }
    UInt32 coverage{};
    Error error{CheckCoverage(Tail(b, U16(b, 2U)), glyphs, coverage, budget)};
    if (error != Error::None)
    {
        return error;
    }
    if (format == 1U)
    {
        const UInt16 count{U16(b, 8U)};
        if (count != coverage || !b.Contains(10U, static_cast<Size>(count) * 2U))
        {
            return Error::Malformed;
        }
        for (UInt32 i{}; i < count; ++i)
        {
            const UInt16 offset{U16(b, 10U + static_cast<Size>(i) * 2U)};
            if (offset == 0U)
            {
                return Error::Malformed;
            }
            const Bytes set{Tail(b, offset)};
            if (!set.Contains(0U, 2U))
            {
                return Error::Truncated;
            }
            const UInt16 pairs{U16(set, 0U)};
            const UInt32 size{2U + ValueSize(f1) + ValueSize(f2)};
            if (!set.Contains(2U, static_cast<Size>(pairs) * size))
            {
                return Error::Truncated;
            }
            if (!budget.Spend(pairs))
            {
                return Error::OperationLimit;
            }
            UInt32 previous{};
            Size at{2U};
            for (UInt32 j{}; j < pairs; ++j)
            {
                const UInt16 glyph{U16(set, at)};
                at += 2U;
                if (glyph >= glyphs || (j != 0U && glyph <= previous))
                {
                    return Error::Malformed;
                }
                previous = glyph;
                error = Value(set, at, f1);
                if (error != Error::None)
                {
                    return error;
                }
                error = Value(set, at, f2);
                if (error != Error::None)
                {
                    return error;
                }
            }
        }
    }
    else if (format == 2U)
    {
        if (!b.Contains(0U, 16U) || U16(b, 8U) == 0U || U16(b, 10U) == 0U)
        {
            return Error::Truncated;
        }
        const UInt16 n1{U16(b, 12U)};
        const UInt16 n2{U16(b, 14U)};
        if (n1 == 0U || n2 == 0U)
        {
            return Error::Malformed;
        }
        const Size cells{static_cast<Size>(n1) * n2};
        if (!budget.Spend(cells))
        {
            return Error::OperationLimit;
        }
        if (!b.Contains(16U, cells * (ValueSize(f1) + ValueSize(f2))))
        {
            return Error::Truncated;
        }
        error = CheckClass(Tail(b, U16(b, 8U)), glyphs, n1, budget);
        if (error != Error::None)
        {
            return error;
        }
        error = CheckClass(Tail(b, U16(b, 10U)), glyphs, n2, budget);
        if (error != Error::None)
        {
            return error;
        }
        Size at{16U};
        for (Size i{}; i < cells; ++i)
        {
            error = Value(b, at, f1);
            if (error != Error::None)
            {
                return error;
            }
            error = Value(b, at, f2);
            if (error != Error::None)
            {
                return error;
            }
        }
    }
    else
    {
        return Error::Unsupported;
    }
    return Error::None;
}
[[nodiscard]] Error LigatureTable(const Bytes b, const UInt16 glyphs, Budget& budget) noexcept
{
    if (!b.Contains(0U, 6U) || U16(b, 2U) == 0U)
    {
        return Error::Truncated;
    }
    if (U16(b, 0U) != 1U)
    {
        return Error::Unsupported;
    }
    UInt32 total{};
    Error error{CheckCoverage(Tail(b, U16(b, 2U)), glyphs, total, budget)};
    if (error != Error::None)
    {
        return error;
    }
    const UInt16 n{U16(b, 4U)};
    if (n != total || !b.Contains(6U, static_cast<Size>(n) * 2U))
    {
        return Error::Malformed;
    }
    for (UInt32 i{}; i < n; ++i)
    {
        const UInt16 offset{U16(b, 6U + static_cast<Size>(i) * 2U)};
        if (offset == 0U)
        {
            return Error::Malformed;
        }
        const Bytes set{Tail(b, offset)};
        if (!set.Contains(0U, 2U))
        {
            return Error::Truncated;
        }
        const UInt16 ligatures{U16(set, 0U)};
        if (!set.Contains(2U, static_cast<Size>(ligatures) * 2U))
        {
            return Error::Truncated;
        }
        if (!budget.Spend(ligatures))
        {
            return Error::OperationLimit;
        }
        for (UInt32 j{}; j < ligatures; ++j)
        {
            const UInt16 off{U16(set, 2U + static_cast<Size>(j) * 2U)};
            if (off == 0U)
            {
                return Error::Malformed;
            }
            const Bytes lig{Tail(set, off)};
            if (!lig.Contains(0U, 4U) || U16(lig, 0U) >= glyphs)
            {
                return Error::InvalidGlyph;
            }
            const UInt16 components{U16(lig, 2U)};
            if (components < 2U || components > MaximumLigatureComponents)
            {
                return Error::Limit;
            }
            if (!lig.Contains(4U, static_cast<Size>(components - 1U) * 2U))
            {
                return Error::Truncated;
            }
            if (!budget.Spend(components))
            {
                return Error::OperationLimit;
            }
            for (UInt32 k{1U}; k < components; ++k)
            {
                if (U16(lig, 2U + static_cast<Size>(k) * 2U) >= glyphs)
                {
                    return Error::InvalidGlyph;
                }
            }
        }
    }
    return Error::None;
}
[[nodiscard]] Error LangSys(const Bytes b, const UInt16 features, Budget& budget) noexcept
{
    if (!b.Contains(0U, 6U) || U16(b, 0U) != 0U)
    {
        return Error::Truncated;
    }
    const UInt16 required{U16(b, 2U)};
    const UInt16 n{U16(b, 4U)};
    if (required != 0xFFFFU && required >= features)
    {
        return Error::Malformed;
    }
    if (n > MaximumLayoutItems || !b.Contains(6U, static_cast<Size>(n) * 2U))
    {
        return Error::Truncated;
    }
    if (!budget.Spend(n))
    {
        return Error::OperationLimit;
    }
    for (UInt32 i{}; i < n; ++i)
    {
        if (U16(b, 6U + static_cast<Size>(i) * 2U) >= features)
        {
            return Error::Malformed;
        }
    }
    return Error::None;
}
[[nodiscard]] Error Scripts(const Bytes b, const UInt16 features, Budget& budget) noexcept
{
    if (!b.Contains(0U, 2U))
    {
        return Error::Truncated;
    }
    const UInt16 n{U16(b, 0U)};
    if (n > MaximumLayoutItems || !b.Contains(2U, static_cast<Size>(n) * 6U))
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
        const Size at{2U + static_cast<Size>(i) * 6U};
        const UInt32 tag{U32(b, at)};
        const UInt16 offset{U16(b, at + 4U)};
        if ((i != 0U && tag <= previous) || offset == 0U)
        {
            return Error::Malformed;
        }
        previous = tag;
        const Bytes script{Tail(b, offset)};
        if (!script.Contains(0U, 4U))
        {
            return Error::Truncated;
        }
        const UInt16 def{U16(script, 0U)};
        const UInt16 languages{U16(script, 2U)};
        if (def != 0U)
        {
            const Error e{LangSys(Tail(script, def), features, budget)};
            if (e != Error::None)
            {
                return e;
            }
        }
        if (languages > MaximumLayoutItems || !script.Contains(4U, static_cast<Size>(languages) * 6U))
        {
            return Error::Truncated;
        }
        if (!budget.Spend(languages))
        {
            return Error::OperationLimit;
        }
        UInt32 previousLanguage{};
        for (UInt32 j{}; j < languages; ++j)
        {
            const Size langAt{4U + static_cast<Size>(j) * 6U};
            const UInt32 lang{U32(script, langAt)};
            const UInt16 off{U16(script, langAt + 4U)};
            if ((j != 0U && lang <= previousLanguage) || off == 0U)
            {
                return Error::Malformed;
            }
            previousLanguage = lang;
            const Error e{LangSys(Tail(script, off), features, budget)};
            if (e != Error::None)
            {
                return e;
            }
        }
    }
    return Error::None;
}
} // namespace
Error ValidateLayout(const Bytes table, const bool positioning, const UInt16 glyphs, Budget& budget) noexcept
{
    if (table.Data == nullptr)
    {
        return Error::None;
    }
    if (!table.Contains(0U, 10U))
    {
        return Error::Truncated;
    }
    if (U16(table, 0U) != 1U || U16(table, 2U) > 1U)
    {
        return Error::Unsupported;
    }
    if (U16(table, 2U) == 1U)
    {
        if (!table.Contains(0U, 14U))
        {
            return Error::Truncated;
        }
        if (U32(table, 10U) != 0U)
        {
            return Error::Unsupported;
        }
    }
    if (U16(table, 4U) == 0U || U16(table, 6U) == 0U || U16(table, 8U) == 0U)
    {
        return Error::Malformed;
    }
    const Bytes features{Tail(table, U16(table, 6U))};
    const Bytes lookups{Tail(table, U16(table, 8U))};
    if (!features.Contains(0U, 2U) || !lookups.Contains(0U, 2U))
    {
        return Error::Truncated;
    }
    const UInt16 nf{U16(features, 0U)};
    const UInt16 nl{U16(lookups, 0U)};
    if (nf > MaximumLayoutItems || nl > MaximumLayoutItems)
    {
        return Error::Limit;
    }
    if (!features.Contains(2U, static_cast<Size>(nf) * 6U) || !lookups.Contains(2U, static_cast<Size>(nl) * 2U))
    {
        return Error::Truncated;
    }
    Error error{Scripts(Tail(table, U16(table, 4U)), nf, budget)};
    if (error != Error::None)
    {
        return error;
    }
    UInt32 previous{};
    for (UInt32 i{}; i < nf; ++i)
    {
        if (!budget.Spend())
        {
            return Error::OperationLimit;
        }
        const Size at{2U + static_cast<Size>(i) * 6U};
        const UInt32 tag{U32(features, at)};
        const UInt16 off{U16(features, at + 4U)};
        if ((i != 0U && tag < previous) || off == 0U)
        {
            return Error::Malformed;
        }
        previous = tag;
        const Bytes feature{Tail(features, off)};
        if (!feature.Contains(0U, 4U))
        {
            return Error::Truncated;
        }
        if (U16(feature, 0U) != 0U && !feature.Contains(U16(feature, 0U), 2U))
        {
            return Error::Truncated;
        }
        const UInt16 n{U16(feature, 2U)};
        if (n > MaximumLayoutItems || !feature.Contains(4U, static_cast<Size>(n) * 2U))
        {
            return Error::Truncated;
        }
        if (!budget.Spend(n))
        {
            return Error::OperationLimit;
        }
        for (UInt32 j{}; j < n; ++j)
        {
            if (U16(feature, 4U + static_cast<Size>(j) * 2U) >= nl)
            {
                return Error::Malformed;
            }
        }
    }
    for (UInt32 i{}; i < nl; ++i)
    {
        if (!budget.Spend())
        {
            return Error::OperationLimit;
        }
        const UInt16 offset{U16(lookups, 2U + static_cast<Size>(i) * 2U)};
        if (offset == 0U)
        {
            return Error::Malformed;
        }
        const Bytes lookup{Tail(lookups, offset)};
        if (!lookup.Contains(0U, 6U))
        {
            return Error::Truncated;
        }
        const UInt16 type{U16(lookup, 0U)};
        const UInt16 flags{U16(lookup, 2U)};
        const UInt16 count{U16(lookup, 4U)};
        if (type == 0U || type > (positioning ? 9U : 8U))
        {
            return Error::Unsupported;
        }
        if (count > MaximumLayoutItems ||
            !lookup.Contains(6U, static_cast<Size>(count) * 2U + ((flags & 16U) != 0U ? 2U : 0U)))
        {
            return Error::Truncated;
        }
        UInt16 extensionType{};
        for (UInt32 j{}; j < count; ++j)
        {
            if (!budget.Spend())
            {
                return Error::OperationLimit;
            }
            const UInt16 off{U16(lookup, 6U + static_cast<Size>(j) * 2U)};
            if (off == 0U)
            {
                return Error::Malformed;
            }
            Bytes sub{Tail(lookup, off)};
            if (!sub.Contains(0U, 2U))
            {
                return Error::Truncated;
            }
            UInt16 actual{type};
            if (type == (positioning ? 9U : 7U))
            {
                if (!sub.Contains(0U, 8U) || U16(sub, 0U) != 1U)
                {
                    return Error::Truncated;
                }
                actual = U16(sub, 2U);
                const UInt32 ext{U32(sub, 4U)};
                if (actual == 0U || actual > (positioning ? 8U : 8U) || actual == type || ext == 0U)
                {
                    return Error::Malformed;
                }
                if (extensionType != 0U && actual != extensionType)
                {
                    return Error::Malformed;
                }
                extensionType = actual;
                sub = Tail(sub, ext);
                if (!sub.Contains(0U, 2U))
                {
                    return Error::Truncated;
                }
            }
            if (flags != 0U)
            {
                continue;
            } // No GDEF filtering in v1; never apply filtered lookups.
            if (actual == (positioning ? 2U : 4U))
            {
                error = positioning ? PairTable(sub, glyphs, budget) : LigatureTable(sub, glyphs, budget);
                if (error != Error::None)
                {
                    return error;
                }
            }
        }
    }
    return Error::None;
}
Int32 Coverage(const Bytes b, const UInt16 glyph) noexcept
{
    const UInt16 format{U16(b, 0U)};
    const UInt16 n{U16(b, 2U)};
    UInt32 low{};
    UInt32 high{n};
    while (low < high)
    {
        const UInt32 mid{low + (high - low) / 2U};
        const Size at{4U + static_cast<Size>(mid) * (format == 1U ? 2U : 6U)};
        const UInt16 start{U16(b, at)};
        const UInt16 end{format == 1U ? start : U16(b, at + 2U)};
        if (glyph < start)
        {
            high = mid;
        }
        else if (glyph > end)
        {
            low = mid + 1U;
        }
        else
        {
            return format == 1U ? static_cast<Int32>(mid) : U16(b, at + 4U) + glyph - start;
        }
    }
    return -1;
}
UInt16 Class(const Bytes b, const UInt16 glyph) noexcept
{
    if (U16(b, 0U) == 1U)
    {
        const UInt16 start{U16(b, 2U)};
        const UInt16 count{U16(b, 4U)};
        return glyph >= start && glyph - start < count ? U16(b, 6U + static_cast<Size>(glyph - start) * 2U)
                                                       : UInt16{0U};
    }
    UInt32 low{};
    UInt32 high{U16(b, 2U)};
    while (low < high)
    {
        const UInt32 mid{low + (high - low) / 2U};
        const Size at{4U + static_cast<Size>(mid) * 6U};
        if (glyph < U16(b, at))
        {
            high = mid;
        }
        else if (glyph > U16(b, at + 2U))
        {
            low = mid + 1U;
        }
        else
        {
            return U16(b, at + 4U);
        }
    }
    return 0U;
}
Bytes LookupSubtable(const Bytes lookup, const UInt32 index, const bool positioning, UInt16& type) noexcept
{
    type = U16(lookup, 0U);
    const Bytes sub{Tail(lookup, U16(lookup, 6U + static_cast<Size>(index) * 2U))};
    if (type != (positioning ? 9U : 7U))
    {
        return sub;
    }
    type = U16(sub, 2U);
    return Tail(sub, U32(sub, 4U));
}
bool Selected(const Bytes table, const UInt32 lookup, const UInt32 featureTag, Budget& budget) noexcept
{
    const Bytes scripts{Tail(table, U16(table, 4U))};
    Bytes chosen{};
    for (UInt32 i{}; i < U16(scripts, 0U); ++i)
    {
        if (!budget.Spend())
        {
            return false;
        }
        const Size at{2U + static_cast<Size>(i) * 6U};
        const UInt32 tag{U32(scripts, at)};
        if (tag == Tag('D', 'F', 'L', 'T'))
        {
            chosen = Tail(scripts, U16(scripts, at + 4U));
            break;
        }
        if (tag == Tag('l', 'a', 't', 'n'))
        {
            chosen = Tail(scripts, U16(scripts, at + 4U));
        }
    }
    const UInt16 def{U16(chosen, 0U)};
    if (def == 0U)
    {
        return false;
    }
    const Bytes lang{Tail(chosen, def)};
    const Bytes features{Tail(table, U16(table, 6U))};
    const UInt32 count{U16(lang, 4U)};
    for (UInt32 i{}; i <= count; ++i)
    {
        if (!budget.Spend())
        {
            return false;
        }
        const UInt16 id{i == count ? U16(lang, 2U) : U16(lang, 6U + static_cast<Size>(i) * 2U)};
        if (id == 0xFFFFU)
        {
            continue;
        }
        const Size at{2U + static_cast<Size>(id) * 6U};
        if (U32(features, at) != featureTag)
        {
            continue;
        }
        const Bytes feature{Tail(features, U16(features, at + 4U))};
        for (UInt32 j{}; j < U16(feature, 2U); ++j)
        {
            if (!budget.Spend())
            {
                return false;
            }
            if (U16(feature, 4U + static_cast<Size>(j) * 2U) == lookup)
            {
                return true;
            }
        }
    }
    return false;
}
namespace
{
[[nodiscard]] Adjustment ReadValue(const Bytes b, Size& at, const UInt16 format) noexcept
{
    Adjustment value{};
    for (UInt32 bit{}; bit < 8U; ++bit)
    {
        if ((format & (1U << bit)) == 0U)
        {
            continue;
        }
        const Int16 v{S16(b, at)};
        at += 2U;
        switch (bit)
        {
        case 0U:
            value.XPlacement = v;
            break;
        case 1U:
            value.YPlacement = v;
            break;
        case 2U:
            value.XAdvance = v;
            break;
        case 3U:
            value.YAdvance = v;
            break;
        default:
            break;
        }
    }
    return value;
}
void Accumulate(Adjustment& to, const Adjustment from) noexcept
{
    to.XPlacement += from.XPlacement;
    to.YPlacement += from.YPlacement;
    to.XAdvance += from.XAdvance;
    to.YAdvance += from.YAdvance;
}
[[nodiscard]] bool FindPair(const Bytes sub, const UInt16 left, const UInt16 right, PairAdjustment& result) noexcept
{
    const Int32 coverage{Coverage(Tail(sub, U16(sub, 2U)), left)};
    if (coverage < 0)
    {
        return false;
    }
    const UInt16 f1{U16(sub, 4U)};
    const UInt16 f2{U16(sub, 6U)};
    Size at{};
    Bytes values{sub};
    if (U16(sub, 0U) == 1U)
    {
        values = Tail(sub, U16(sub, 10U + static_cast<Size>(coverage) * 2U));
        UInt32 low{};
        UInt32 high{U16(values, 0U)};
        const UInt32 size{2U + ValueSize(f1) + ValueSize(f2)};
        bool found{};
        while (low < high)
        {
            const UInt32 mid{low + (high - low) / 2U};
            at = 2U + static_cast<Size>(mid) * size;
            const UInt16 second{U16(values, at)};
            if (right < second)
            {
                high = mid;
            }
            else if (right > second)
            {
                low = mid + 1U;
            }
            else
            {
                found = true;
                at += 2U;
                break;
            }
        }
        if (!found)
        {
            return false;
        }
    }
    else
    {
        const UInt16 c1{Class(Tail(sub, U16(sub, 8U)), left)};
        const UInt16 c2{Class(Tail(sub, U16(sub, 10U)), right)};
        at = 16U + (static_cast<Size>(c1) * U16(sub, 14U) + c2) * (ValueSize(f1) + ValueSize(f2));
    }
    result.First = ReadValue(values, at, f1);
    result.Second = ReadValue(values, at, f2);
    return true;
}
} // namespace
Error Pair(const Font& font, const UInt16 left, const UInt16 right, PairAdjustment& result, Budget& budget) noexcept
{
    result = {};
    const Bytes table{font.Table(Tag('G', 'P', 'O', 'S'))};
    if (table.Data == nullptr)
    {
        return Error::None;
    }
    const Bytes lookups{Tail(table, U16(table, 8U))};
    for (UInt32 i{}; i < U16(lookups, 0U); ++i)
    {
        if (!budget.Spend())
        {
            return Error::OperationLimit;
        }
        if (!Selected(table, i, Tag('k', 'e', 'r', 'n'), budget))
        {
            if (budget.Left == 0U)
            {
                return Error::OperationLimit;
            }
            continue;
        }
        const Bytes lookup{Tail(lookups, U16(lookups, 2U + static_cast<Size>(i) * 2U))};
        if (U16(lookup, 2U) != 0U)
        {
            continue;
        }
        for (UInt32 j{}; j < U16(lookup, 4U); ++j)
        {
            if (!budget.Spend())
            {
                return Error::OperationLimit;
            }
            UInt16 type{};
            const Bytes sub{LookupSubtable(lookup, j, true, type)};
            if (type != 2U)
            {
                continue;
            }
            PairAdjustment value{};
            if (FindPair(sub, left, right, value))
            {
                Accumulate(result.First, value.First);
                Accumulate(result.Second, value.Second);
                break;
            }
        }
    }
    return Error::None;
}
Error Ligatures(const Font& font, Run& run, Budget& budget) noexcept
{
    const Bytes table{font.Table(Tag('G', 'S', 'U', 'B'))};
    if (table.Data == nullptr)
    {
        return Error::None;
    }
    const Bytes lookups{Tail(table, U16(table, 8U))};
    for (UInt32 lookupId{}; lookupId < U16(lookups, 0U); ++lookupId)
    {
        if (!budget.Spend())
        {
            return Error::OperationLimit;
        }
        if (!Selected(table, lookupId, Tag('l', 'i', 'g', 'a'), budget))
        {
            if (budget.Left == 0U)
            {
                return Error::OperationLimit;
            }
            continue;
        }
        const Bytes lookup{Tail(lookups, U16(lookups, 2U + static_cast<Size>(lookupId) * 2U))};
        if (U16(lookup, 2U) != 0U)
        {
            continue;
        }
        for (UInt32 i{}; i < run.Count; ++i)
        {
            if (run.Glyphs[i].Scalar == 10U)
            {
                continue;
            }
            bool matched{};
            for (UInt32 j{}; j < U16(lookup, 4U) && !matched; ++j)
            {
                if (!budget.Spend())
                {
                    return Error::OperationLimit;
                }
                UInt16 type{};
                const Bytes sub{LookupSubtable(lookup, j, false, type)};
                if (type != 4U)
                {
                    continue;
                }
                const Int32 coverage{Coverage(Tail(sub, U16(sub, 2U)), run.Glyphs[i].Glyph)};
                if (coverage < 0)
                {
                    continue;
                }
                const Bytes set{Tail(sub, U16(sub, 6U + static_cast<Size>(coverage) * 2U))};
                for (UInt32 k{}; k < U16(set, 0U) && !matched; ++k)
                {
                    if (!budget.Spend())
                    {
                        return Error::OperationLimit;
                    }
                    const Bytes lig{Tail(set, U16(set, 2U + static_cast<Size>(k) * 2U))};
                    const UInt16 count{U16(lig, 2U)};
                    if (count > run.Count - i)
                    {
                        continue;
                    }
                    bool same{true};
                    for (UInt32 n{1U}; n < count; ++n)
                    {
                        if (!budget.Spend())
                        {
                            return Error::OperationLimit;
                        }
                        if (run.Glyphs[i + n].Scalar == 10U ||
                            run.Glyphs[i + n].Glyph != U16(lig, 2U + static_cast<Size>(n) * 2U))
                        {
                            same = false;
                            break;
                        }
                    }
                    if (!same)
                    {
                        continue;
                    }
                    run.Glyphs[i].Glyph = U16(lig, 0U);
                    run.Glyphs[i].ClusterEnd = run.Glyphs[i + count - 1U].ClusterEnd;
                    if (!budget.Spend(run.Count - i - count))
                    {
                        return Error::OperationLimit;
                    }
                    for (UInt32 n{i + count}; n < run.Count; ++n)
                    {
                        run.Glyphs[n - count + 1U] = run.Glyphs[n];
                    }
                    run.Count -= count - 1U;
                    matched = true;
                }
            }
        }
    }
    return Error::None;
}
} // namespace Asteria::User::Font::Detail
