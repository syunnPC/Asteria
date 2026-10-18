// SPDX-License-Identifier: Apache-2.0

#include "User/Imaging/Internal.hpp"
namespace Asteria::User::Imaging
{
namespace
{
// RFC 1951 (May 1996), 3.2.5: full length and distance alphabets.
constexpr UInt16 LengthBase[]{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                              31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr UInt8 LengthExtra[]{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr UInt16 DistanceBase[]{1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr UInt8 DistanceExtra[]{0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr UInt8 CodeOrder[]{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
static_assert(sizeof(LengthBase) / sizeof(LengthBase[0]) == sizeof(LengthExtra));
static_assert(sizeof(DistanceBase) / sizeof(DistanceBase[0]) == sizeof(DistanceExtra));
} // namespace
bool Image::Valid() const noexcept
{
    return Pixels != nullptr && Width != 0U && Height != 0U && Width <= MaximumDimension &&
           Height <= MaximumDimension && Width <= MaximumPixels / Height &&
           Bytes == static_cast<Size>(Width) * Height * 4U;
}
UInt32 Crc32(Bytes bytes) noexcept
{
    // PNG Third Edition 5.5: reflected polynomial, all-one preconditioning
    // and complemented result. Derived from its polynomial, no sample annex.
    UInt32 remainder{0xFFFFFFFFU};
    for (Size i{0U}; i < bytes.Count; ++i)
    {
        remainder ^= bytes.Data[i];
        for (UInt32 bit{0U}; bit < 8U; ++bit)
        {
            const bool low{(remainder & 1U) != 0U};
            remainder >>= 1U;
            if (low)
            {
                remainder ^= 0xEDB88320U;
            }
        }
    }
    return ~remainder;
}
Status InflateReader::Begin(const Bytes* segments, UInt32 count, bool zlib) noexcept
{
    m_status = Status::InvalidBuffer;
    m_count = count;
    m_segments = segments;
    m_segment = 0U;
    m_position = 0U;
    m_bits = 0U;
    m_bitCount = 0U;
    m_blocks = 0U;
    m_produced = 0U;
    m_s1 = 1U;
    m_s2 = 0U;
    m_remaining = 0U;
    m_distance = 0U;
    m_zlib = zlib;
    m_final = false;
    m_block = false;
    m_stored = false;
    m_ended = false;
    m_windowLimit = WindowBytes;
    if (segments == nullptr || count == 0U)
    {
        return m_status;
    }
    if (count > MaximumChunks)
    {
        return m_status = Status::Limit;
    }
    Size total{0U};
    for (UInt32 i{0U}; i < count; ++i)
    {
        if (segments[i].Data == nullptr && segments[i].Count != 0U)
        {
            return m_status;
        }
        if (segments[i].Count > MaximumInput - total)
        {
            return m_status = Status::Limit;
        }
        total += segments[i].Count;
    }
    m_status = Status::Ok;
    if (zlib)
    {
        UInt32 cmf{};
        UInt32 flg{};
        if (!Byte(cmf) || !Byte(flg))
        {
            return m_status = Status::Truncated;
        }
        // RFC 1950 2.2: CM=8, CINFO<=7, FCHECK and no dictionary.
        if ((cmf & 15U) != 8U || (cmf >> 4U) > 7U || (cmf * 256U + flg) % 31U != 0U)
        {
            return m_status = Status::Header;
        }
        if ((flg & 32U) != 0U)
        {
            return m_status = Status::Unsupported;
        }
        m_windowLimit = 1U << ((cmf >> 4U) + 8U);
    }
    return m_status;
}
bool InflateReader::Byte(UInt32& value) noexcept
{
    while (m_segment < m_count && m_position == m_segments[m_segment].Count)
    {
        ++m_segment;
        m_position = 0U;
    }
    if (m_segment == m_count)
    {
        return false;
    }
    value = m_segments[m_segment].Data[m_position++];
    return true;
}
bool InflateReader::Bits(UInt32 count, UInt32& value) noexcept
{
    while (m_bitCount < count)
    {
        UInt32 byte{};
        if (!Byte(byte))
        {
            return false;
        }
        m_bits |= byte << m_bitCount;
        m_bitCount += 8U;
    }
    value = m_bits & ((1U << count) - 1U);
    m_bits >>= count;
    m_bitCount -= count;
    return true;
}
Status InflateReader::Tree(Huffman& tree, const UInt8* lengths, UInt32 count, bool single, bool empty) noexcept
{
    // Canonical ordering from RFC 1951 3.2.2. Store symbols grouped by depth;
    // decoder tracks each depth's first integer code instead of a bit trie.
    for (UInt32 i{0U}; i < 16U; ++i)
    {
        tree.Counts[i] = 0U;
    }
    UInt32 used{0U};
    for (UInt32 i{0U}; i < count; ++i)
    {
        if (lengths[i] > 15U)
        {
            return Status::Huffman;
        }
        if (lengths[i] != 0U)
        {
            ++tree.Counts[lengths[i]];
            ++used;
        }
    }
    Int32 available{1};
    for (UInt32 depth{1U}; depth <= 15U; ++depth)
    {
        available = available * 2 - tree.Counts[depth];
        if (available < 0)
        {
            return Status::Huffman;
        }
    }
    if (available != 0 && !(single && used == 1U && tree.Counts[1] == 1U) && !(empty && used == 0U))
    {
        return Status::Huffman;
    }
    UInt32 slot{0U};
    for (UInt32 depth{1U}; depth <= 15U; ++depth)
    {
        for (UInt32 symbol{0U}; symbol < count; ++symbol)
        {
            if (lengths[symbol] == depth)
            {
                tree.Symbols[slot++] = static_cast<UInt16>(symbol);
            }
        }
    }
    return Status::Ok;
}
Status InflateReader::Symbol(const Huffman& tree, UInt32& value) noexcept
{
    UInt32 code{0U};
    UInt32 first{0U};
    UInt32 offset{0U};
    for (UInt32 depth{1U}; depth <= 15U; ++depth)
    {
        UInt32 bit{};
        if (!Bits(1U, bit))
        {
            return Status::Truncated;
        }
        code = code * 2U + bit;
        if (code >= first && code - first < tree.Counts[depth])
        {
            value = tree.Symbols[offset + code - first];
            return Status::Ok;
        }
        offset += tree.Counts[depth];
        first = (first + tree.Counts[depth]) * 2U;
    }
    return Status::Huffman;
}
Status InflateReader::Dynamic() noexcept
{
    UInt32 nl{};
    UInt32 nd{};
    UInt32 nc{};
    if (!Bits(5U, nl) || !Bits(5U, nd) || !Bits(4U, nc))
    {
        return Status::Truncated;
    }
    nl += 257U;
    nd += 1U;
    nc += 4U;
    if (nl > 286U)
    {
        return Status::Huffman;
    }
    for (UInt32 i{0U}; i < 19U; ++i)
    {
        m_codeLengths[i] = 0U;
    }
    for (UInt32 i{0U}; i < nc; ++i)
    {
        UInt32 v{};
        if (!Bits(3U, v))
        {
            return Status::Truncated;
        }
        m_codeLengths[CodeOrder[i]] = static_cast<UInt8>(v);
    }
    Status status{Tree(m_codes, m_codeLengths, 19U, false, false)};
    if (status != Status::Ok)
    {
        return status;
    }
    UInt32 pos{0U};
    while (pos < nl + nd)
    {
        UInt32 value{};
        status = Symbol(m_codes, value);
        if (status != Status::Ok)
        {
            return status;
        }
        if (value < 16U)
        {
            m_lengths[pos++] = static_cast<UInt8>(value);
        }
        else
        {
            if (value == 16U && pos == 0U)
            {
                return Status::Huffman;
            }
            UInt32 extra{};
            const UInt32 bits{value == 16U ? 2U : (value == 17U ? 3U : 7U)};
            if (!Bits(bits, extra))
            {
                return Status::Truncated;
            }
            const UInt32 repeat{extra + (value == 18U ? 11U : 3U)};
            if (repeat > nl + nd - pos)
            {
                return Status::Huffman;
            }
            const UInt8 length{value == 16U ? m_lengths[pos - 1U] : UInt8{0U}};
            for (UInt32 i{0U}; i < repeat; ++i)
            {
                m_lengths[pos++] = length;
            }
        }
    }
    if (m_lengths[256] == 0U)
    {
        return Status::Huffman;
    }
    status = Tree(m_literal, m_lengths, nl, true, false);
    if (status != Status::Ok)
    {
        return status;
    }
    return Tree(m_distances, m_lengths + nl, nd, true, true);
}
Status InflateReader::Block() noexcept
{
    if (++m_blocks > MaximumBlocks)
    {
        return Status::Limit;
    }
    UInt32 final{};
    UInt32 type{};
    if (!Bits(1U, final) || !Bits(2U, type))
    {
        return Status::Truncated;
    }
    m_final = final != 0U;
    m_block = true;
    m_stored = type == 0U;
    if (type == 3U)
    {
        return Status::Header;
    }
    if (type == 0U)
    {
        m_bits = 0U;
        m_bitCount = 0U;
        UInt32 lo{};
        UInt32 hi{};
        UInt32 nlo{};
        UInt32 nhi{};
        if (!Byte(lo) || !Byte(hi) || !Byte(nlo) || !Byte(nhi))
        {
            return Status::Truncated;
        }
        m_remaining = lo + hi * 256U;
        if ((m_remaining ^ (nlo + nhi * 256U)) != 65535U)
        {
            return Status::Header;
        }
        return Status::Ok;
    }
    if (type == 2U)
    {
        return Dynamic();
    }
    for (UInt32 i{0U}; i < 288U; ++i)
    {
        m_lengths[i] = static_cast<UInt8>(i <= 143U ? 8U : (i <= 255U ? 9U : (i <= 279U ? 7U : 8U)));
    }
    Status status{Tree(m_literal, m_lengths, 288U, false, false)};
    if (status != Status::Ok)
    {
        return status;
    }
    for (UInt32 i{0U}; i < 32U; ++i)
    {
        m_lengths[i] = 5U;
    }
    return Tree(m_distances, m_lengths, 32U, false, false);
}
Status InflateReader::Finish() noexcept
{
    m_bits = 0U;
    m_bitCount = 0U;
    if (m_zlib)
    {
        UInt32 expected{0U};
        for (UInt32 i{0U}; i < 4U; ++i)
        {
            UInt32 byte{};
            if (!Byte(byte))
            {
                return Status::Truncated;
            }
            expected = expected * 256U + byte;
        }
        if (expected != m_s2 * 65536U + m_s1)
        {
            return Status::Adler;
        }
    }
    UInt32 extra{};
    if (Byte(extra))
    {
        return Status::SizeMismatch;
    }
    m_ended = true;
    return Status::End;
}
Status InflateReader::Emit(UInt8 value, UInt8& output) noexcept
{
    if (m_produced == MaximumInflated)
    {
        return m_status = Status::Limit;
    }
    m_window[m_produced % WindowBytes] = value;
    ++m_produced;
    output = value;
    // RFC 1950 2.2: reduce both sums each byte, avoiding overflow.
    m_s1 = (m_s1 + value) % 65521U;
    m_s2 = (m_s2 + m_s1) % 65521U;
    return Status::Ok;
}
Status InflateReader::Next(UInt8& value) noexcept
{
    if (m_status != Status::Ok)
    {
        return m_status;
    }
    if (m_ended)
    {
        return Status::End;
    }
    for (;;)
    {
        if (m_remaining != 0U)
        {
            UInt32 byte{};
            if (m_stored)
            {
                if (!Byte(byte))
                {
                    return m_status = Status::Truncated;
                }
            }
            else
            {
                byte = m_window[(m_produced - m_distance) % WindowBytes];
            }
            --m_remaining;
            return Emit(static_cast<UInt8>(byte), value);
        }
        if (!m_block)
        {
            m_status = Block();
            if (m_status != Status::Ok)
            {
                return m_status;
            }
            continue;
        }
        if (!m_stored)
        {
            UInt32 symbol{};
            m_status = Symbol(m_literal, symbol);
            if (m_status != Status::Ok)
            {
                return m_status;
            }
            if (symbol < 256U)
            {
                return Emit(static_cast<UInt8>(symbol), value);
            }
            if (symbol != 256U)
            {
                if (symbol > 285U)
                {
                    return m_status = Status::Huffman;
                }
                UInt32 extra{};
                if (!Bits(LengthExtra[symbol - 257U], extra))
                {
                    return m_status = Status::Truncated;
                }
                // RFC 1951 3.2.5 gives code 284 only 227..257.
                if (symbol == 284U && extra == 31U)
                {
                    return m_status = Status::Huffman;
                }
                m_remaining = LengthBase[symbol - 257U] + extra;
                m_status = Symbol(m_distances, symbol);
                if (m_status != Status::Ok)
                {
                    return m_status;
                }
                if (symbol >= 30U)
                {
                    return m_status = Status::Distance;
                }
                if (!Bits(DistanceExtra[symbol], extra))
                {
                    return m_status = Status::Truncated;
                }
                m_distance = DistanceBase[symbol] + extra;
                if (m_distance > m_produced || m_distance > m_windowLimit)
                {
                    return m_status = Status::Distance;
                }
                continue;
            }
        }
        m_block = false;
        if (m_final)
        {
            return m_status = Finish();
        }
    }
}
Status Inflate(const Bytes* segments, UInt32 count, Buffer output, InflateReader& reader, Size& written,
               bool zlib) noexcept
{
    written = 0U;
    if (output.Data == nullptr && output.Count != 0U)
    {
        return Status::InvalidBuffer;
    }
    Status status{reader.Begin(segments, count, zlib)};
    if (status != Status::Ok)
    {
        return status;
    }
    UInt8 value{};
    while ((status = reader.Next(value)) == Status::Ok)
    {
        if (written == output.Count)
        {
            return Status::Limit;
        }
        output.Data[written++] = value;
    }
    return status == Status::End ? Status::Ok : status;
}
} // namespace Asteria::User::Imaging
