// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TEST_USER_FONT_FIXTURE_HPP_INCLUDED
#define ASTERIA_TEST_USER_FONT_FIXTURE_HPP_INCLUDED
// Clean-room synthetic sfnt writer: OpenType 1.9 table formats, Adobe #5176/#5177.
// These programs are invented shapes, never derived from font-library code.
#include <algorithm>
#include <array>
#include <map>
#include <vector>
#include "User/Font/Font.hpp"
using Asteria::Foundation::Int16;
using Asteria::Foundation::UInt16;
using Asteria::Foundation::Int32;
namespace Asteria::TestSupport::FontFixture
{
using Asteria::User::Font::Size;
using Asteria::User::Font::Tag;
using Asteria::User::Font::UInt32;
using Asteria::User::Font::UInt8;
// GCC 15's analyzer loses the initialization of an empty allocator returned
// by value. A concrete initialized state keeps allocator copies observable;
// this is host fixture storage only. Production Font uses no allocator.
template <typename T> struct FixtureAllocator
{
    using value_type = T;
    UInt32 Initialized{};
    constexpr FixtureAllocator() noexcept = default;
    template <typename U> constexpr FixtureAllocator(const FixtureAllocator<U>&) noexcept : Initialized{}
    {
    }
    [[nodiscard]] T* allocate(const Size count)
    {
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* const pointer, const Size count) noexcept
    {
        std::allocator<T>{}.deallocate(pointer, count);
    }
    template <typename U> [[nodiscard]] constexpr bool operator==(const FixtureAllocator<U>&) const noexcept
    {
        return true;
    }
};
template <typename T> using Vector = std::vector<T, FixtureAllocator<T>>;
using Buffer = Vector<UInt8>;
inline void U16(Buffer& b, const UInt32 v)
{
    b.push_back(static_cast<UInt8>(v >> 8U));
    b.push_back(static_cast<UInt8>(v));
}
inline void U32(Buffer& b, const UInt32 v)
{
    U16(b, v >> 16U);
    U16(b, v);
}
inline void Set16(Buffer& b, const Size at, const UInt32 v)
{
    b[at] = static_cast<UInt8>(v >> 8U);
    b[at + 1U] = static_cast<UInt8>(v);
}
inline void Set32(Buffer& b, const Size at, const UInt32 v)
{
    Set16(b, at, v >> 16U);
    Set16(b, at + 2U, v);
}
inline void Append(Buffer& b, const Buffer& data)
{
    b.insert(b.end(), data.begin(), data.end());
}
inline Buffer IndexOf(const Vector<Buffer>& objects)
{
    Size bytes{objects.empty() ? 2U : 3U + 2U * (objects.size() + 1U)};
    for (const auto& object : objects)
    {
        bytes += object.size();
    }
    Buffer b{};
    b.reserve(bytes);
    U16(b, static_cast<UInt32>(objects.size()));
    if (objects.empty())
    {
        return b;
    }
    b.push_back(2U);
    UInt32 at{1U};
    U16(b, at);
    for (const auto& object : objects)
    {
        at += static_cast<UInt32>(object.size());
        U16(b, at);
    }
    for (const auto& object : objects)
    {
        Append(b, object);
    }
    return b;
}
inline void Number(Buffer& b, const Int32 v)
{
    if (v >= -107 && v <= 107)
    {
        b.push_back(static_cast<UInt8>(v + 139));
    }
    else
    {
        b.push_back(28U);
        U16(b, static_cast<UInt32>(v));
    }
}
inline void DictOffset(Buffer& b, const UInt32 value)
{
    b.push_back(29U);
    U32(b, value);
}
inline Buffer RectangleProgram()
{
    Buffer b{};
    Number(b, 0);
    Number(b, 0);
    b.push_back(21U);
    for (const Int32 v : {500, 700, -500, -700})
    {
        Number(b, v);
    }
    b.push_back(6U);
    b.push_back(14U);
    return b;
}
inline Buffer Cff()
{
    Vector<Buffer> glyphs(12U, Buffer{14U});
    glyphs[1U] = RectangleProgram();
    glyphs[2U] = RectangleProgram();
    glyphs[2U].pop_back();
    Number(glyphs[2U], 100);
    Number(glyphs[2U], 100);
    glyphs[2U].push_back(21U);
    for (const Int32 v : {500, 300, -500, -300})
    {
        Number(glyphs[2U], v);
    }
    glyphs[2U].push_back(7U);
    glyphs[2U].push_back(14U);
    glyphs[3U] = {};
    Number(glyphs[3U], 0);
    Number(glyphs[3U], 0);
    glyphs[3U].push_back(21U);
    for (const Int32 v : {150, 400, 200, 0, 150, -400})
    {
        Number(glyphs[3U], v);
    }
    glyphs[3U].push_back(8U);
    glyphs[3U].push_back(14U);
    for (UInt32 i{4U}; i < 11U; ++i)
    {
        glyphs[i] = RectangleProgram();
    }
    Buffer top{};
    DictOffset(top, 0U);
    top.push_back(17U);
    DictOffset(top, 0U);
    DictOffset(top, 0U);
    top.push_back(18U);
    Buffer b{1U, 0U, 4U, 4U};
    Append(b, IndexOf({Buffer{'S', 'y', 'n', 't', 'h'}}));
    const Size topAt{b.size()};
    Append(b, IndexOf({top}));
    Append(b, IndexOf({}));
    Append(b, IndexOf({}));
    const UInt32 strings{static_cast<UInt32>(b.size())};
    Append(b, IndexOf(glyphs));
    const UInt32 privateAt{static_cast<UInt32>(b.size())};
    Buffer priv{};
    Number(priv, 600);
    priv.push_back(20U);
    Append(b, priv);
    Set32(b, topAt + 7U + 1U, strings);
    Set32(b, topAt + 7U + 7U, static_cast<UInt32>(priv.size()));
    Set32(b, topAt + 7U + 12U, privateAt);
    return b;
}
struct TtPoint
{
    Int16 X;
    Int16 Y;
    bool On;
};
inline Buffer Simple(const Vector<Vector<TtPoint>>& contours)
{
    Buffer b{};
    U16(b, static_cast<UInt32>(contours.size()));
    for (UInt32 i{}; i < 4U; ++i)
    {
        U16(b, 0U);
    }
    UInt32 count{};
    for (const auto& c : contours)
    {
        count += static_cast<UInt32>(c.size());
        U16(b, count - 1U);
    }
    U16(b, 0U);
    for (const auto& c : contours)
    {
        for (const auto& p : c)
        {
            b.push_back(p.On ? 1U : 0U);
        }
    }
    for (UInt32 axis{}; axis < 2U; ++axis)
    {
        Int32 previous{};
        for (const auto& c : contours)
        {
            for (const auto& p : c)
            {
                const Int32 v{axis == 0U ? p.X : p.Y};
                U16(b, static_cast<UInt32>(v - previous));
                previous = v;
            }
        }
    }
    return b;
}
inline Buffer Coverage(const UInt16 glyph)
{
    Buffer b{};
    U16(b, 1U);
    U16(b, 1U);
    U16(b, glyph);
    return b;
}
inline Buffer PairOne()
{
    Buffer b{};
    for (const UInt32 v : {1U, 12U, 4U, 1U, 1U, 18U})
    {
        U16(b, v);
    }
    Append(b, Coverage(1U));
    U16(b, 1U);
    U16(b, 2U);
    U16(b, static_cast<UInt16>(-100));
    U16(b, 15U);
    return b;
}
inline Buffer PairClasses()
{
    Buffer b{};
    for (const UInt32 v : {2U, 24U, 4U, 0U, 30U, 38U, 2U, 2U})
    {
        U16(b, v);
    }
    for (const Int32 v : {0, 0, 0, -40})
    {
        U16(b, static_cast<UInt32>(v));
    }
    Append(b, Coverage(2U));
    for (const UInt32 v : {1U, 2U, 1U, 1U, 1U, 1U, 1U, 1U})
    {
        U16(b, v);
    }
    return b;
}
inline Buffer Ligature()
{
    Buffer b{};
    for (const UInt32 v : {1U, 8U, 1U, 14U})
    {
        U16(b, v);
    }
    Append(b, Coverage(6U));
    for (const UInt32 v : {2U, 6U, 12U, 9U, 2U, 7U, 10U, 2U, 8U})
    {
        U16(b, v);
    }
    return b;
}
inline Buffer Layout(const bool positioning, const bool extension = true)
{
    Buffer b{};
    U32(b, 0x10000U);
    U16(b, 10U);
    U16(b, 30U);
    U16(b, 44U);
    // ScriptList DFLT, default LangSys selects feature zero.
    U16(b, 1U);
    U32(b, Tag('D', 'F', 'L', 'T'));
    U16(b, 8U);
    U16(b, 4U);
    U16(b, 0U);
    U16(b, 0U);
    U16(b, 0xFFFFU);
    U16(b, 1U);
    U16(b, 0U);
    U16(b, 1U);
    U32(b, positioning ? Tag('k', 'e', 'r', 'n') : Tag('l', 'i', 'g', 'a'));
    U16(b, 8U);
    U16(b, 0U);
    U16(b, 1U);
    U16(b, 0U);
    U16(b, 1U);
    U16(b, 4U); // LookupList
    const UInt16 type{static_cast<UInt16>(positioning ? 2U : 4U)};
    U16(b, extension ? (positioning ? 9U : 7U) : type);
    U16(b, 0U);
    U16(b, positioning ? 2U : 1U);
    const Size offsets{b.size()};
    U16(b, 0U);
    if (positioning)
    {
        U16(b, 0U);
    }
    const Size base{48U};
    for (UInt32 i{}; i < (positioning ? 2U : 1U); ++i)
    {
        Set16(b, offsets + i * 2U, static_cast<UInt32>(b.size() - base));
        if (extension)
        {
            U16(b, 1U);
            U16(b, type);
            U32(b, 8U);
        }
        Append(b, positioning ? (i == 0U ? PairOne() : PairClasses()) : Ligature());
    }
    return b;
}
inline Buffer Make(const bool cff, const bool extension = true, const bool longLoca = true)
{
    std::map<UInt32, Buffer> tables{};
    Buffer head(54U);
    Set32(head, 0U, 0x10000U);
    Set32(head, 12U, 0x5F0F3CF5U);
    Set16(head, 18U, 1000U);
    Set16(head, 50U, longLoca ? 1U : 0U);
    tables[Tag('h', 'e', 'a', 'd')] = head;
    Buffer maxp(cff ? 6U : 32U);
    Set32(maxp, 0U, cff ? 0x5000U : 0x10000U);
    Set16(maxp, 4U, 12U);
    tables[Tag('m', 'a', 'x', 'p')] = maxp;
    Buffer hhea(36U);
    Set32(hhea, 0U, 0x10000U);
    Set16(hhea, 4U, 800U);
    Set16(hhea, 6U, static_cast<UInt16>(-200));
    Set16(hhea, 8U, 200U);
    Set16(hhea, 34U, 12U);
    tables[Tag('h', 'h', 'e', 'a')] = hhea;
    Buffer hmtx{};
    for (UInt32 i{}; i < 12U; ++i)
    {
        U16(hmtx, i == 9U ? 900U : (i == 10U ? 850U : 600U));
        U16(hmtx, 0U);
    }
    tables[Tag('h', 'm', 't', 'x')] = hmtx;
    Buffer os2(96U);
    Set16(os2, 0U, 4U);
    Set16(os2, 4U, 400U);
    Set16(os2, 6U, 5U);
    Set16(os2, 68U, 750U);
    Set16(os2, 70U, static_cast<UInt16>(-250));
    Set16(os2, 72U, 100U);
    tables[Tag('O', 'S', '/', '2')] = os2;
    Buffer post(32U);
    Set32(post, 0U, 0x30000U);
    Set16(post, 8U, static_cast<UInt16>(-100));
    Set16(post, 10U, 50U);
    tables[Tag('p', 'o', 's', 't')] = post;
    Buffer name{};
    U16(name, 0U);
    U16(name, 2U);
    U16(name, 30U);
    const std::array<const char*, 2U> names{"Synthetic", "Regular"};
    UInt32 offset{};
    for (UInt32 i{}; i < 2U; ++i)
    {
        UInt32 n{};
        while (names[i][n] != '\0')
        {
            ++n;
        }
        for (const UInt32 v : {3U, 1U, 0x409U, i + 1U, n * 2U, offset})
        {
            U16(name, v);
        }
        offset += n * 2U;
    }
    for (const auto* n : names)
    {
        for (Size i{}; n[i] != '\0'; ++i)
        {
            U16(name, static_cast<UInt8>(n[i]));
        }
    }
    tables[Tag('n', 'a', 'm', 'e')] = name;
    const std::map<UInt32, UInt32> mapping{{32U, 0U},  {65U, 1U},  {66U, 4U},  {67U, 5U},     {79U, 3U},     {86U, 2U},
                                           {102U, 6U}, {105U, 7U}, {108U, 8U}, {0xFB01U, 9U}, {0xFB02U, 10U}};
    Buffer four{};
    const UInt32 segments{static_cast<UInt32>(mapping.size() + 1U)};
    for (const UInt32 v : {4U, 16U + 8U * segments, 0U, 2U * segments, 0U, 0U, 0U})
    {
        U16(four, v);
    }
    for (const auto& [p, g] : mapping)
    {
        static_cast<void>(g);
        U16(four, p);
    }
    U16(four, 0xFFFFU);
    U16(four, 0U);
    for (const auto& [p, g] : mapping)
    {
        static_cast<void>(g);
        U16(four, p);
    }
    U16(four, 0xFFFFU);
    for (const auto& [p, g] : mapping)
    {
        U16(four, g - p);
    }
    U16(four, 1U);
    for (UInt32 i{}; i < segments; ++i)
    {
        U16(four, 0U);
    }
    Buffer twelve{};
    U16(twelve, 12U);
    U16(twelve, 0U);
    U32(twelve, 16U + 12U * static_cast<UInt32>(mapping.size() + 1U));
    U32(twelve, 0U);
    U32(twelve, static_cast<UInt32>(mapping.size() + 1U));
    for (const auto& [p, g] : mapping)
    {
        U32(twelve, p);
        U32(twelve, p);
        U32(twelve, g);
    }
    U32(twelve, 0x1F600U);
    U32(twelve, 0x1F600U);
    U32(twelve, 11U);
    Buffer cmap{};
    U16(cmap, 0U);
    U16(cmap, 2U);
    U16(cmap, 3U);
    U16(cmap, 1U);
    U32(cmap, 20U);
    U16(cmap, 3U);
    U16(cmap, 10U);
    U32(cmap, 20U + static_cast<UInt32>(four.size()));
    Append(cmap, four);
    Append(cmap, twelve);
    tables[Tag('c', 'm', 'a', 'p')] = cmap;
    tables[Tag('G', 'P', 'O', 'S')] = Layout(true, extension);
    tables[Tag('G', 'S', 'U', 'B')] = Layout(false, extension);
    if (cff)
    {
        tables[Tag('C', 'F', 'F', ' ')] = Cff();
    }
    else
    {
        const Vector<TtPoint> rectangle{{0, 0, true}, {500, 0, true}, {500, 700, true}, {0, 700, true}};
        Vector<Buffer> glyphs(12U);
        glyphs[1U] = Simple({rectangle});
        glyphs[2U] = Simple({rectangle, {{100, 100, true}, {100, 600, true}, {400, 600, true}, {400, 100, true}}});
        glyphs[3U] = Simple({{{0, 0, true}, {250, 500, false}, {500, 0, true}}});
        glyphs[4U] = Buffer(10U);
        Set16(glyphs[4U], 0U, 0xFFFFU);
        for (const UInt32 v : {11U, 1U, 100U, 200U, 8192U})
        {
            U16(glyphs[4U], v);
        }
        glyphs[5U] = Buffer(10U);
        Set16(glyphs[5U], 0U, 0xFFFFU);
        for (const UInt32 v : {35U, 1U, 0U, 0U, 1U, 1U, 1U, 0U})
        {
            U16(glyphs[5U], v);
        }
        for (UInt32 i{6U}; i < 11U; ++i)
        {
            glyphs[i] = glyphs[1U];
        }
        Buffer glyf{};
        Buffer loca{};
        for (const auto& g : glyphs)
        {
            if (longLoca)
            {
                U32(loca, static_cast<UInt32>(glyf.size()));
            }
            else
            {
                U16(loca, static_cast<UInt32>(glyf.size() / 2U));
            }
            Append(glyf, g);
            if ((glyf.size() & 1U) != 0U)
            {
                glyf.push_back(0U);
            }
        }
        if (longLoca)
        {
            U32(loca, static_cast<UInt32>(glyf.size()));
        }
        else
        {
            U16(loca, static_cast<UInt32>(glyf.size() / 2U));
        }
        tables[Tag('g', 'l', 'y', 'f')] = glyf;
        tables[Tag('l', 'o', 'c', 'a')] = loca;
    }
    Buffer b(12U + tables.size() * 16U);
    Set32(b, 0U, cff ? Tag('O', 'T', 'T', 'O') : 0x10000U);
    Set16(b, 4U, static_cast<UInt32>(tables.size()));
    Size record{12U};
    for (const auto& [tag, data] : tables)
    {
        while ((b.size() & 3U) != 0U)
        {
            b.push_back(0U);
        }
        Set32(b, record, tag);
        Set32(b, record + 8U, static_cast<UInt32>(b.size()));
        Set32(b, record + 12U, static_cast<UInt32>(data.size()));
        Append(b, data);
        record += 16U;
    }
    return b;
}
inline Size TableOffset(const Buffer& b, const UInt32 tag)
{
    const auto read = [&](const Size at)
    {
        return (static_cast<UInt32>(b[at]) << 24U) | (static_cast<UInt32>(b[at + 1U]) << 16U) |
               (static_cast<UInt32>(b[at + 2U]) << 8U) | b[at + 3U];
    };
    const UInt32 count{(static_cast<UInt32>(b[4U]) << 8U) | b[5U]};
    for (UInt32 i{}; i < count; ++i)
    {
        const Size at{12U + static_cast<Size>(i) * 16U};
        if (read(at) == tag)
        {
            return read(at + 8U);
        }
    }
    return 0U;
}
} // namespace Asteria::TestSupport::FontFixture
#endif
