// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Security/Sha256.hpp"

namespace Asteria::Kernel::Security
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;

constexpr UInt32 RoundConstants[64U]{
    0x428A'2F98U, 0x7137'4491U, 0xB5C0'FBCFU, 0xE9B5'DBA5U, 0x3956'C25BU, 0x59F1'11F1U, 0x923F'82A4U, 0xAB1C'5ED5U,
    0xD807'AA98U, 0x1283'5B01U, 0x2431'85BEU, 0x550C'7DC3U, 0x72BE'5D74U, 0x80DE'B1FEU, 0x9BDC'06A7U, 0xC19B'F174U,
    0xE49B'69C1U, 0xEFBE'4786U, 0x0FC1'9DC6U, 0x240C'A1CCU, 0x2DE9'2C6FU, 0x4A74'84AAU, 0x5CB0'A9DCU, 0x76F9'88DAU,
    0x983E'5152U, 0xA831'C66DU, 0xB003'27C8U, 0xBF59'7FC7U, 0xC6E0'0BF3U, 0xD5A7'9147U, 0x06CA'6351U, 0x1429'2967U,
    0x27B7'0A85U, 0x2E1B'2138U, 0x4D2C'6DFCU, 0x5338'0D13U, 0x650A'7354U, 0x766A'0ABBU, 0x81C2'C92EU, 0x9272'2C85U,
    0xA2BF'E8A1U, 0xA81A'664BU, 0xC24B'8B70U, 0xC76C'51A3U, 0xD192'E819U, 0xD699'0624U, 0xF40E'3585U, 0x106A'A070U,
    0x19A4'C116U, 0x1E37'6C08U, 0x2748'774CU, 0x34B0'BCB5U, 0x391C'0CB3U, 0x4ED8'AA4AU, 0x5B9C'CA4FU, 0x682E'6FF3U,
    0x748F'82EEU, 0x78A5'636FU, 0x84C8'7814U, 0x8CC7'0208U, 0x90BE'FFFAU, 0xA450'6CEBU, 0xBEF9'A3F7U, 0xC671'78F2U,
};

constexpr Size LengthFieldBytes{8U};
constexpr UInt8 InnerPadByte{0x36U};
constexpr UInt8 OuterPadByte{0x5CU};

// All arithmetic below is modulo 2^32 by definition of SHA-256.
[[nodiscard]] constexpr UInt32 RotateRight(const UInt32 value, const UInt32 distance) noexcept
{
    return (value >> distance) | (value << (32U - distance));
}

[[nodiscard]] UInt32 LoadBigEndian32(const UInt8* const bytes) noexcept
{
    return (static_cast<UInt32>(bytes[0U]) << 24U) | (static_cast<UInt32>(bytes[1U]) << 16U) |
           (static_cast<UInt32>(bytes[2U]) << 8U) | static_cast<UInt32>(bytes[3U]);
}

void StoreBigEndian32(UInt8* const bytes, const UInt32 value) noexcept
{
    bytes[0U] = static_cast<UInt8>(value >> 24U);
    bytes[1U] = static_cast<UInt8>(value >> 16U);
    bytes[2U] = static_cast<UInt8>(value >> 8U);
    bytes[3U] = static_cast<UInt8>(value);
}
} // namespace

void Sha256::Compress(const UInt8* const block) noexcept
{
    // The message schedule is kept as a rolling 16-word window: word t
    // replaces word t - 16 in slot t mod 16.
    UInt32 schedule[16U]{};
    UInt32 a{m_state[0U]};
    UInt32 b{m_state[1U]};
    UInt32 c{m_state[2U]};
    UInt32 d{m_state[3U]};
    UInt32 e{m_state[4U]};
    UInt32 f{m_state[5U]};
    UInt32 g{m_state[6U]};
    UInt32 h{m_state[7U]};
    for (Size round{0U}; round < 64U; ++round)
    {
        const Size slot = round & 15U;
        if (round < 16U)
        {
            schedule[slot] = LoadBigEndian32(block + (round * 4U));
        }
        else
        {
            const UInt32 older = schedule[(round + 1U) & 15U];
            const UInt32 recent = schedule[(round + 14U) & 15U];
            const UInt32 sigma0 = RotateRight(older, 7U) ^ RotateRight(older, 18U) ^ (older >> 3U);
            const UInt32 sigma1 = RotateRight(recent, 17U) ^ RotateRight(recent, 19U) ^ (recent >> 10U);
            schedule[slot] = schedule[slot] + sigma0 + schedule[(round + 9U) & 15U] + sigma1;
        }
        const UInt32 upperSigma1 = RotateRight(e, 6U) ^ RotateRight(e, 11U) ^ RotateRight(e, 25U);
        const UInt32 choice = (e & f) ^ (~e & g);
        const UInt32 first = h + upperSigma1 + choice + RoundConstants[round] + schedule[slot];
        const UInt32 upperSigma0 = RotateRight(a, 2U) ^ RotateRight(a, 13U) ^ RotateRight(a, 22U);
        const UInt32 majority = (a & b) ^ (a & c) ^ (b & c);
        const UInt32 second = upperSigma0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    m_state[0U] += a;
    m_state[1U] += b;
    m_state[2U] += c;
    m_state[3U] += d;
    m_state[4U] += e;
    m_state[5U] += f;
    m_state[6U] += g;
    m_state[7U] += h;
}

void Sha256::Update(const UInt8* const bytes, const Size sizeBytes) noexcept
{
    for (Size index{0U}; index < sizeBytes; ++index)
    {
        m_block[m_blockBytes] = bytes[index];
        ++m_blockBytes;
        if (m_blockBytes == Sha256BlockBytes)
        {
            Compress(m_block);
            m_blockBytes = 0U;
        }
    }
    m_messageBytes += static_cast<UInt64>(sizeBytes);
}

void Sha256::Finish(UInt8 (&digest)[Sha256DigestBytes]) noexcept
{
    // The bit length is taken modulo 2^64, as FIPS 180-4 defines it.
    const UInt64 messageBits = m_messageBytes << 3U;
    m_block[m_blockBytes] = 0x80U;
    ++m_blockBytes;
    if (m_blockBytes > (Sha256BlockBytes - LengthFieldBytes))
    {
        while (m_blockBytes < Sha256BlockBytes)
        {
            m_block[m_blockBytes] = 0U;
            ++m_blockBytes;
        }
        Compress(m_block);
        m_blockBytes = 0U;
    }
    while (m_blockBytes < (Sha256BlockBytes - LengthFieldBytes))
    {
        m_block[m_blockBytes] = 0U;
        ++m_blockBytes;
    }
    for (Size index{0U}; index < LengthFieldBytes; ++index)
    {
        m_block[m_blockBytes + index] = static_cast<UInt8>(messageBits >> ((7U - index) * 8U));
    }
    Compress(m_block);
    for (Size word{0U}; word < 8U; ++word)
    {
        StoreBigEndian32(digest + (word * 4U), m_state[word]);
    }
    Restart();
}

void Sha256::Restart() noexcept
{
    *this = Sha256{};
}

HmacSha256::HmacSha256(const UInt8* const key, const Size keyBytes) noexcept
{
    UInt8 blockKey[Sha256BlockBytes]{};
    if (keyBytes > Sha256BlockBytes)
    {
        UInt8 keyDigest[Sha256DigestBytes]{};
        Sha256 keyHash{};
        keyHash.Update(key, keyBytes);
        keyHash.Finish(keyDigest);
        for (Size index{0U}; index < Sha256DigestBytes; ++index)
        {
            blockKey[index] = keyDigest[index];
        }
    }
    else
    {
        for (Size index{0U}; index < keyBytes; ++index)
        {
            blockKey[index] = key[index];
        }
    }
    UInt8 innerPad[Sha256BlockBytes]{};
    for (Size index{0U}; index < Sha256BlockBytes; ++index)
    {
        innerPad[index] = static_cast<UInt8>(blockKey[index] ^ InnerPadByte);
        m_outerPad[index] = static_cast<UInt8>(blockKey[index] ^ OuterPadByte);
    }
    m_inner.Update(innerPad, Sha256BlockBytes);
}

void HmacSha256::Update(const UInt8* const bytes, const Size sizeBytes) noexcept
{
    m_inner.Update(bytes, sizeBytes);
}

void HmacSha256::Finish(UInt8 (&tag)[Sha256DigestBytes]) noexcept
{
    UInt8 innerDigest[Sha256DigestBytes]{};
    m_inner.Finish(innerDigest);
    Sha256 outer{};
    outer.Update(m_outerPad, Sha256BlockBytes);
    outer.Update(innerDigest, Sha256DigestBytes);
    outer.Finish(tag);
}
} // namespace Asteria::Kernel::Security
