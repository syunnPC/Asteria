// SPDX-License-Identifier: Apache-2.0

#include "User/Font/Internal.hpp"
namespace Asteria::User::Font
{
using Detail::Abs;
using Detail::Coordinate;
using Detail::ReadIndex;
using Detail::S16;
using Detail::S32;
using Detail::U8;
namespace
{
struct Dictionary
{
    UInt32 Charstrings{};
    UInt32 PrivateSize{};
    UInt32 PrivateOffset{};
    UInt32 Subroutines{};
    Int64 DefaultWidth{};
    Int64 NominalWidth{};
    bool HasCharstrings{};
    bool HasPrivate{};
    bool HasSubroutines{};
    bool Cid{};
    bool Matrix{};
    Int64 MatrixScale{};
};
[[nodiscard]] Error Real(const Bytes data, Size& at, Int64& value) noexcept
{
    // #5176 section 4, table 5: decimal nibble encoding, bounded rational conversion.
    Int64 mantissa{};
    Int32 decimals{};
    Int32 exponent{};
    bool negative{};
    bool fraction{};
    bool exp{};
    bool expNegative{};
    bool digits{};
    for (UInt32 count{}; count < 32U; ++count)
    {
        if (!data.Contains(at, 1U))
        {
            return Error::Truncated;
        }
        const UInt8 byte{U8(data, at++)};
        for (UInt32 half{}; half < 2U; ++half)
        {
            const UInt8 nibble{static_cast<UInt8>((byte >> (half == 0U ? 4U : 0U)) & 15U)};
            if (nibble == 15U)
            {
                if (!digits || (half == 0U && (byte & 15U) != 15U))
                {
                    return Error::Malformed;
                }
                if (expNegative)
                {
                    exponent = -exponent;
                }
                exponent -= decimals;
                value = mantissa * One;
                if (exponent > 8 || exponent < -16)
                {
                    return Error::Limit;
                }
                for (Int32 i{}; i < exponent; ++i)
                {
                    if (value > 0x7FFFFFFFFFFFFFFFLL / 10)
                    {
                        return Error::Limit;
                    }
                    value *= 10;
                }
                for (Int32 i{}; i > exponent; --i)
                {
                    value /= 10;
                }
                if (negative)
                {
                    value = -value;
                }
                return Error::None;
            }
            if (nibble <= 9U)
            {
                if (exp)
                {
                    if (exponent > 16)
                    {
                        return Error::Limit;
                    }
                    exponent = exponent * 10 + nibble;
                }
                else
                {
                    if (mantissa > 1000000000000LL)
                    {
                        return Error::Limit;
                    }
                    mantissa = mantissa * 10 + nibble;
                    digits = true;
                    if (fraction)
                    {
                        ++decimals;
                    }
                }
            }
            else if (nibble == 10U && !fraction && !exp)
            {
                fraction = true;
            }
            else if ((nibble == 11U || nibble == 12U) && !exp && digits)
            {
                exp = true;
                expNegative = nibble == 12U;
            }
            else if (nibble == 14U && !digits && !negative && !exp && !fraction)
            {
                negative = true;
            }
            else
            {
                return Error::Malformed;
            }
        }
    }
    return Error::Limit;
}
[[nodiscard]] Error Dict(const Bytes data, const bool top, Dictionary& result) noexcept
{
    if (data.Length > MaximumProgramBytes)
    {
        return Error::Limit;
    }
    Int64 values[6U]{};
    UInt32 count{};
    for (Size at{}; at < data.Length;)
    {
        const UInt8 byte{U8(data, at++)};
        Int64 value{};
        if (byte >= 32U || byte == 28U || byte == 29U || byte == 30U)
        {
            if (count == MaximumOperands)
            {
                return Error::StackOverflow;
            }
            if (byte <= 246U && byte >= 32U)
            {
                value = (static_cast<Int32>(byte) - 139) * One;
            }
            else if (byte >= 247U && byte <= 254U)
            {
                if (!data.Contains(at, 1U))
                {
                    return Error::Truncated;
                }
                value = (byte < 251U ? (static_cast<Int32>(byte) - 247) * 256 + U8(data, at) + 108
                                     : -(static_cast<Int32>(byte) - 251) * 256 - U8(data, at) - 108) *
                        One;
                ++at;
            }
            else if (byte == 28U || byte == 29U)
            {
                const Size size{byte == 28U ? 2U : 4U};
                if (!data.Contains(at, size))
                {
                    return Error::Truncated;
                }
                value = static_cast<Int64>(byte == 28U ? S16(data, at) : S32(data, at)) * One;
                at += size;
            }
            else if (byte == 30U)
            {
                const Error error{Real(data, at, value)};
                if (error != Error::None)
                {
                    return error;
                }
            }
            else
            {
                return Error::Malformed;
            }
            if (count < 6U)
            {
                values[count] = value;
            }
            ++count;
            continue;
        }
        UInt32 op{byte};
        if (byte == 12U)
        {
            if (!data.Contains(at, 1U))
            {
                return Error::Truncated;
            }
            op = 0x100U + U8(data, at++);
        }
        if (count == 0U)
        {
            return Error::Malformed;
        }
        if (top && op == 17U)
        {
            if (count != 1U || values[0U] < 0 || values[0U] % One != 0 ||
                values[0U] / One > static_cast<Int64>(MaximumFontBytes) || result.HasCharstrings)
            {
                return Error::Malformed;
            }
            result.Charstrings = static_cast<UInt32>(values[0U] / One);
            result.HasCharstrings = true;
        }
        else if (top && op == 18U)
        {
            if (count != 2U || values[0U] < 0 || values[1U] < 0 || values[0U] % One != 0 || values[1U] % One != 0 ||
                values[0U] / One > static_cast<Int64>(MaximumFontBytes) ||
                values[1U] / One > static_cast<Int64>(MaximumFontBytes) || result.HasPrivate)
            {
                return Error::Malformed;
            }
            result.PrivateSize = static_cast<UInt32>(values[0U] / One);
            result.PrivateOffset = static_cast<UInt32>(values[1U] / One);
            result.HasPrivate = true;
        }
        else if (top && op == 0x106U)
        {
            if (count != 1U || values[0U] != 2 * One)
            {
                return Error::Unsupported;
            }
        }
        else if (top && op == 0x107U)
        {
            if (count != 6U || values[0U] != values[3U] || values[0U] <= 0 || values[1U] != 0 || values[2U] != 0 ||
                values[4U] != 0 || values[5U] != 0)
            {
                return Error::Unsupported;
            }
            result.Matrix = true;
            result.MatrixScale = values[0U];
        }
        else if (top && (op == 0x11EU || op == 0x124U || op == 0x125U))
        {
            result.Cid = true;
        }
        else if (!top && op == 19U)
        {
            if (count != 1U || values[0U] < 0 || values[0U] % One != 0 ||
                values[0U] / One > static_cast<Int64>(MaximumFontBytes) || result.HasSubroutines)
            {
                return Error::Malformed;
            }
            result.Subroutines = static_cast<UInt32>(values[0U] / One);
            result.HasSubroutines = true;
        }
        else if (!top && (op == 20U || op == 21U))
        {
            if (count != 1U || !Coordinate(values[0U]))
            {
                return Error::Malformed;
            }
            if (op == 20U)
            {
                result.DefaultWidth = values[0U];
            }
            else
            {
                result.NominalWidth = values[0U];
            }
        }
        count = 0U;
    }
    return count == 0U ? Error::None : Error::Malformed;
}
[[nodiscard]] Error Programs(const Index& index) noexcept
{
    for (UInt32 i{}; i < index.Count; ++i)
    {
        if (index.Object(i).Length > MaximumProgramBytes)
        {
            return Error::Limit;
        }
    }
    return Error::None;
}
} // namespace
Error Font::OpenCff() noexcept
{
    // Adobe #5176 sections 6–10, 14–16. Single name-keyed CFF1 font.
    const Bytes data{Table(Tag('C', 'F', 'F', ' '))};
    if (!data.Contains(0U, 4U))
    {
        return Error::Truncated;
    }
    if (U8(data, 0U) != 1U)
    {
        return Error::Unsupported;
    }
    if (U8(data, 2U) < 4U || U8(data, 3U) < 1U || U8(data, 3U) > 4U)
    {
        return Error::Malformed;
    }
    Size at{U8(data, 2U)};
    Index index{};
    Error error{ReadIndex(data, at, index)};
    if (error != Error::None)
    {
        return error;
    }
    if (index.Count != 1U)
    {
        return Error::Unsupported;
    }
    error = ReadIndex(data, at, index);
    if (error != Error::None)
    {
        return error;
    }
    if (index.Count != 1U)
    {
        return Error::Unsupported;
    }
    Dictionary top{};
    error = Dict(index.Object(0U), true, top);
    if (error != Error::None)
    {
        return error;
    }
    if (top.Cid || !top.HasCharstrings)
    {
        return Error::Unsupported;
    }
    if (top.Matrix && Abs(top.MatrixScale - One / m_metrics.UnitsPerEm) > 1)
    {
        return Error::Unsupported;
    }
    if (!top.Matrix && m_metrics.UnitsPerEm != 1000U)
    {
        return Error::Unsupported;
    }
    error = ReadIndex(data, at, index);
    if (error != Error::None)
    {
        return error;
    }
    error = ReadIndex(data, at, m_globals);
    if (error != Error::None)
    {
        return error;
    }
    at = top.Charstrings;
    error = ReadIndex(data, at, m_charstrings);
    if (error != Error::None)
    {
        return error;
    }
    if (m_charstrings.Count != m_metrics.GlyphCount)
    {
        return Error::Malformed;
    }
    error = Programs(m_charstrings);
    if (error != Error::None)
    {
        return error;
    }
    error = Programs(m_globals);
    if (error != Error::None)
    {
        return error;
    }
    if (top.HasPrivate)
    {
        const Bytes privateData{data.Slice(top.PrivateOffset, top.PrivateSize)};
        if (privateData.Data == nullptr)
        {
            return Error::Truncated;
        }
        Dictionary priv{};
        error = Dict(privateData, false, priv);
        if (error != Error::None)
        {
            return error;
        }
        m_defaultWidth = priv.DefaultWidth;
        m_nominalWidth = priv.NominalWidth;
        if (priv.HasSubroutines)
        {
            at = static_cast<Size>(top.PrivateOffset) + priv.Subroutines;
            error = ReadIndex(data, at, m_locals);
            if (error != Error::None)
            {
                return error;
            }
            error = Programs(m_locals);
            if (error != Error::None)
            {
                return error;
            }
        }
    }
    return Error::None;
}
} // namespace Asteria::User::Font
