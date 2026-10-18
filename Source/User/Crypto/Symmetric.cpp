// SPDX-License-Identifier: Apache-2.0

#include "User/Crypto/Internal.hpp"

namespace Asteria::User::Crypto::Detail
{
namespace
{
// FIPS 197-upd1 §§4.2, 5.1.1: inversion in GF(2^8) followed by affine map.
[[nodiscard]] UInt8 Multiply(UInt8 a, UInt8 b) noexcept
{
    UInt32 result = 0U;
    UInt32 x = a;
    UInt32 y = b;
    for (Size bit = 0U; bit < 8U; ++bit)
    {
        result ^= x & Mask(y & 1U);
        x = ((x << 1U) ^ (0x1bU & Mask(x >> 7U))) & 255U;
        y >>= 1U;
    }
    return static_cast<UInt8>(result);
}
[[nodiscard]] UInt8 Sub(UInt8 x) noexcept
{
    UInt8 inverse = 1U;
    // x^254, including x=0. Exponent bits are public.
    for (UInt32 bit = 8U; bit > 0U; --bit)
    {
        inverse = Multiply(inverse, inverse);
        if (((254U >> (bit - 1U)) & 1U) != 0U)
        {
            inverse = Multiply(inverse, x);
        }
    }
    const UInt32 v = inverse;
    return static_cast<UInt8>(v ^ ((v << 1U) | (v >> 7U)) ^ ((v << 2U) | (v >> 6U)) ^ ((v << 3U) | (v >> 5U)) ^
                              ((v << 4U) | (v >> 4U)) ^ 0x63U);
}
void Quarter(UInt32& a, UInt32& b, UInt32& c, UInt32& d) noexcept
{
    a += b;
    d = Rol(d ^ a, 16U);
    c += d;
    b = Rol(b ^ c, 12U);
    a += b;
    d = Rol(d ^ a, 8U);
    c += d;
    b = Rol(b ^ c, 7U);
}
} // namespace
void AesSetup(Workspace& w, Bytes key) noexcept
{
    Copy(w.AesKey, key.Data, key.Length);
    const Size total = key.Length == 16U ? 176U : 240U;
    UInt8 rcon = 1U;
    for (Size offset = key.Length; offset < total; offset += 4U)
    {
        for (Size j = 0U; j < 4U; ++j)
        {
            w.AesTemp[j] = w.AesKey[offset - 4U + j];
        }
        if (offset % key.Length == 0U)
        {
            const UInt8 first = w.AesTemp[0U];
            for (Size j = 0U; j < 3U; ++j)
            {
                w.AesTemp[j] = Sub(w.AesTemp[j + 1U]);
            }
            w.AesTemp[3U] = Sub(first);
            w.AesTemp[0U] ^= rcon;
            rcon = Multiply(rcon, 2U);
        }
        else if (key.Length == 32U && offset % 32U == 16U)
        {
            for (Size j = 0U; j < 4U; ++j)
            {
                w.AesTemp[j] = Sub(w.AesTemp[j]);
            }
        }
        for (Size j = 0U; j < 4U; ++j)
        {
            w.AesKey[offset + j] = w.AesKey[offset - key.Length + j] ^ w.AesTemp[j];
        }
    }
}
void AesBlock(Workspace& w, Size keyBytes, const UInt8* input, UInt8* output) noexcept
{
    const Size rounds = keyBytes == 16U ? 10U : 14U;
    for (Size j = 0U; j < 16U; ++j)
    {
        w.AesState[j] = input[j] ^ w.AesKey[j];
    }
    for (Size round = 1U; round <= rounds; ++round)
    {
        for (Size col = 0U; col < 4U; ++col)
        {
            for (Size row = 0U; row < 4U; ++row)
            {
                w.AesTemp[col * 4U + row] = Sub(w.AesState[((col + row) % 4U) * 4U + row]);
            }
        }
        if (round < rounds)
        {
            for (Size col = 0U; col < 4U; ++col)
            {
                const Size base = col * 4U;
                const UInt8 x = w.AesTemp[base] ^ w.AesTemp[base + 1U] ^ w.AesTemp[base + 2U] ^ w.AesTemp[base + 3U];
                for (Size row = 0U; row < 4U; ++row)
                {
                    w.AesState[base + row] = w.AesTemp[base + row] ^ x ^
                                             Multiply(w.AesTemp[base + row] ^ w.AesTemp[base + (row + 1U) % 4U], 2U);
                }
            }
        }
        else
        {
            Copy(w.AesState, w.AesTemp, 16U);
        }
        for (Size j = 0U; j < 16U; ++j)
        {
            w.AesState[j] ^= w.AesKey[round * 16U + j];
        }
    }
    Copy(output, w.AesState, 16U);
}
// RFC 8439 §2.3. Indices and the 20 rounds are independent of key/data.
void ChaBlock(Workspace& w, Bytes key, Bytes nonce, UInt32 counter) noexcept
{
    w.Cha[0] = 0x61707865U;
    w.Cha[1] = 0x3320646eU;
    w.Cha[2] = 0x79622d32U;
    w.Cha[3] = 0x6b206574U;
    for (Size i = 0U; i < 8U; ++i)
    {
        w.Cha[4U + i] = Load32(key.Data + 4U * i);
    }
    w.Cha[12] = counter;
    for (Size i = 0U; i < 3U; ++i)
    {
        w.Cha[13U + i] = Load32(nonce.Data + 4U * i);
    }
    for (Size i = 0U; i < 16U; ++i)
    {
        w.Cha[16U + i] = w.Cha[i];
    }
    UInt32* x = w.Cha + 16U;
    for (Size round = 0U; round < 10U; ++round)
    {
        Quarter(x[0], x[4], x[8], x[12]);
        Quarter(x[1], x[5], x[9], x[13]);
        Quarter(x[2], x[6], x[10], x[14]);
        Quarter(x[3], x[7], x[11], x[15]);
        Quarter(x[0], x[5], x[10], x[15]);
        Quarter(x[1], x[6], x[11], x[12]);
        Quarter(x[2], x[7], x[8], x[13]);
        Quarter(x[3], x[4], x[9], x[14]);
    }
    for (Size i = 0U; i < 16U; ++i)
    {
        Store32(w.ChaBlock + 4U * i, x[i] + w.Cha[i]);
    }
}
// RFC 8439 §2.5: five radix-2^26 limbs modulo 2^130-5.
void PolyStart(Workspace& w, const UInt8* key) noexcept
{
    Copy(w.PolyKey, key, 32U);
    w.PolyKey[3] &= 15U;
    w.PolyKey[7] &= 15U;
    w.PolyKey[11] &= 15U;
    w.PolyKey[15] &= 15U;
    w.PolyKey[4] &= 252U;
    w.PolyKey[8] &= 252U;
    w.PolyKey[12] &= 252U;
    w.PolyR[0] = Load32(w.PolyKey) & 0x3ffffffU;
    w.PolyR[1] = (Load32(w.PolyKey + 3U) >> 2U) & 0x3ffffffU;
    w.PolyR[2] = (Load32(w.PolyKey + 6U) >> 4U) & 0x3ffffffU;
    w.PolyR[3] = (Load32(w.PolyKey + 9U) >> 6U) & 0x3ffffffU;
    w.PolyR[4] = (Load32(w.PolyKey + 12U) >> 8U) & 0x3ffffffU;
    for (Size i = 0U; i < 5U; ++i)
    {
        w.PolyH[i] = 0U;
    }
    for (Size i = 0U; i < 4U; ++i)
    {
        w.PolyS[i] = Load32(key + 16U + 4U * i);
    }
}
void PolyBlocks(Workspace& w, Bytes input, bool padded) noexcept
{
    for (Size offset = 0U; offset < input.Length; offset += 16U)
    {
        const Size n = input.Length - offset < 16U ? input.Length - offset : 16U;
        UInt8 block[17U]{};
        Copy(block, input.Data + offset, n);
        block[padded ? 16U : n] = 1U;
        w.PolyH[0] += Load32(block) & 0x3ffffffU;
        w.PolyH[1] += (Load32(block + 3U) >> 2U) & 0x3ffffffU;
        w.PolyH[2] += (Load32(block + 6U) >> 4U) & 0x3ffffffU;
        w.PolyH[3] += (Load32(block + 9U) >> 6U) & 0x3ffffffU;
        w.PolyH[4] += (Load32(block + 12U) >> 8U) | (static_cast<UInt32>(block[16]) << 24U);
        for (Size i = 0U; i < 5U; ++i)
        {
            w.PolyProduct[i] = 0U;
            for (Size j = 0U; j < 5U; ++j)
            {
                const Size k = (i + 5U - j) % 5U;
                w.PolyProduct[i] += static_cast<UInt64>(w.PolyH[j]) * w.PolyR[k] * (j > i ? 5U : 1U);
            }
        }
        for (Size i = 0U; i < 4U; ++i)
        {
            w.PolyProduct[i + 1U] += w.PolyProduct[i] >> 26U;
            w.PolyH[i] = static_cast<UInt32>(w.PolyProduct[i]) & 0x3ffffffU;
        }
        w.PolyH[4] = static_cast<UInt32>(w.PolyProduct[4]) & 0x3ffffffU;
        w.PolyH[0] += static_cast<UInt32>(w.PolyProduct[4] >> 26U) * 5U;
        w.PolyH[1] += w.PolyH[0] >> 26U;
        w.PolyH[0] &= 0x3ffffffU;
        Wipe({block, sizeof(block)});
    }
}
void PolyFinish(Workspace& w, UInt8* tag) noexcept
{
    for (Size i = 1U; i < 4U; ++i)
    {
        w.PolyH[i + 1U] += w.PolyH[i] >> 26U;
        w.PolyH[i] &= 0x3ffffffU;
    }
    w.PolyH[0] += (w.PolyH[4] >> 26U) * 5U;
    w.PolyH[4] &= 0x3ffffffU;
    w.PolyH[1] += w.PolyH[0] >> 26U;
    w.PolyH[0] &= 0x3ffffffU;
    UInt32 reduced[5U]{};
    UInt32 carry = 5U;
    for (Size i = 0U; i < 5U; ++i)
    {
        const UInt32 x = w.PolyH[i] + carry;
        reduced[i] = x & 0x3ffffffU;
        carry = x >> 26U;
    }
    const UInt32 select = Mask(carry);
    for (Size i = 0U; i < 5U; ++i)
    {
        w.PolyH[i] = (reduced[i] & select) | (w.PolyH[i] & ~select);
    }
    UInt64 c = 0U;
    for (Size word = 0U; word < 4U; ++word)
    {
        const Size bit = word * 32U;
        const Size limb = bit / 26U;
        const Size shift = bit % 26U;
        const UInt32 x = static_cast<UInt32>(
            (static_cast<UInt64>(w.PolyH[limb]) | (static_cast<UInt64>(w.PolyH[limb + 1U]) << 26U)) >> shift);
        c += static_cast<UInt64>(x) + w.PolyS[word];
        Store32(tag + word * 4U, static_cast<UInt32>(c));
        c >>= 32U;
    }
    Wipe({reinterpret_cast<UInt8*>(reduced), sizeof(reduced)});
}
} // namespace Asteria::User::Crypto::Detail
namespace Asteria::User::Crypto
{
namespace
{
using Detail::AesBlock;
using Detail::AesSetup;
using Detail::Algorithm;
using Detail::ChaBlock;
using Detail::Cleanup;
using Detail::Copy;
using Detail::DisjointWork;
using Detail::Input;
using Detail::Load32;
using Detail::Mask;
using Detail::Overlap;
using Detail::PolyBlocks;
using Detail::PolyFinish;
using Detail::PolyStart;
using Detail::Read;
using Detail::Rol;
using Detail::Store32;
using Detail::Store64;
using Detail::Valid;
[[nodiscard]] bool PayloadBuffers(Workspace& w, Bytes key, Bytes nonce, Bytes input, MutableBytes output) noexcept
{
    return Valid(key) && Valid(nonce) && Valid(input) && Valid(output) && DisjointWork(w, key) &&
           DisjointWork(w, nonce) && DisjointWork(w, input) && DisjointWork(w, Read(output)) &&
           !Overlap(key, Read(output)) && !Overlap(nonce, Read(output)) &&
           (!Overlap(input, Read(output)) || input.Data == output.Data);
}
void ChaXor(Workspace& w, Bytes key, Bytes nonce, UInt32 counter, Bytes input, MutableBytes output) noexcept
{
    for (Size offset = 0U; offset < input.Length; offset += 64U)
    {
        ChaBlock(w, key, nonce, counter++);
        const Size n = input.Length - offset < 64U ? input.Length - offset : 64U;
        for (Size i = 0U; i < n; ++i)
        {
            output.Data[offset + i] = input.Data[offset + i] ^ w.ChaBlock[i];
        }
    }
}
// SP 800-38D §6.3 Algorithm 1, bit-serial GF(2^128) multiplication.
void GhashBlock(Workspace& w, const UInt8* block) noexcept
{
    auto& y = w.Gcm[1];
    auto& v = w.Gcm[2];
    auto& z = w.Gcm[3];
    for (Size j = 0U; j < 16U; ++j)
    {
        y[j] ^= block[j];
        v[j] = w.Gcm[0][j];
        z[j] = 0U;
    }
    for (Size bit = 0U; bit < 128U; ++bit)
    {
        const UInt32 mask = Mask((static_cast<UInt32>(y[bit / 8U]) >> (7U - bit % 8U)) & 1U);
        for (Size j = 0U; j < 16U; ++j)
        {
            z[j] ^= static_cast<UInt8>(v[j] & mask);
        }
        const UInt32 low = v[15] & 1U;
        for (Size j = 16U; j > 1U; --j)
        {
            v[j - 1U] = static_cast<UInt8>((v[j - 1U] >> 1U) | (static_cast<UInt32>(v[j - 2U]) << 7U));
        }
        v[0] = static_cast<UInt8>((v[0] >> 1U) ^ (0xe1U & Mask(low)));
    }
    Copy(y, z, 16U);
}
void Ghash(Workspace& w, Bytes input) noexcept
{
    for (Size offset = 0U; offset < input.Length; offset += 16U)
    {
        const Size n = input.Length - offset < 16U ? input.Length - offset : 16U;
        for (Size i = 0U; i < 16U; ++i)
        {
            w.Gcm[4][i] = 0U;
        }
        Copy(w.Gcm[4], input.Data + offset, n);
        GhashBlock(w, w.Gcm[4]);
    }
}
void GcmStart(Workspace& w, Bytes key, Bytes nonce) noexcept
{
    AesSetup(w, key);
    for (Size i = 0U; i < 16U; ++i)
    {
        w.Gcm[4][i] = 0U;
        w.Gcm[1][i] = 0U;
    }
    AesBlock(w, key.Length, w.Gcm[4], w.Gcm[0]);
    Copy(w.Gcm[5], nonce.Data, 12U);
    w.Gcm[5][12] = 0U;
    w.Gcm[5][13] = 0U;
    w.Gcm[5][14] = 0U;
    w.Gcm[5][15] = 1U;
}
void GcmXor(Workspace& w, Bytes key, Bytes nonce, Bytes input, MutableBytes output) noexcept
{
    Copy(w.Gcm[5], nonce.Data, 12U);
    UInt32 counter = 2U;
    for (Size offset = 0U; offset < input.Length; offset += 16U)
    {
        for (Size i = 0U; i < 4U; ++i)
        {
            w.Gcm[5][12U + i] = static_cast<UInt8>(counter >> ((3U - i) * 8U));
        }
        ++counter;
        AesBlock(w, key.Length, w.Gcm[5], w.Gcm[4]);
        const Size n = input.Length - offset < 16U ? input.Length - offset : 16U;
        for (Size i = 0U; i < n; ++i)
        {
            output.Data[offset + i] = input.Data[offset + i] ^ w.Gcm[4][i];
        }
    }
}
void GcmTag(Workspace& w, Bytes key, Bytes nonce, Bytes aad, Bytes ciphertext, UInt8* tag) noexcept
{
    Ghash(w, aad);
    Ghash(w, ciphertext);
    for (Size i = 0U; i < 8U; ++i)
    {
        w.Gcm[4][i] = static_cast<UInt8>((static_cast<UInt64>(aad.Length) * 8U) >> ((7U - i) * 8U));
        w.Gcm[4][8U + i] = static_cast<UInt8>((static_cast<UInt64>(ciphertext.Length) * 8U) >> ((7U - i) * 8U));
    }
    GhashBlock(w, w.Gcm[4]);
    Copy(w.Gcm[5], nonce.Data, 12U);
    w.Gcm[5][12] = 0U;
    w.Gcm[5][13] = 0U;
    w.Gcm[5][14] = 0U;
    w.Gcm[5][15] = 1U;
    AesBlock(w, key.Length, w.Gcm[5], w.Gcm[4]);
    for (Size i = 0U; i < 16U; ++i)
    {
        tag[i] = w.Gcm[1][i] ^ w.Gcm[4][i];
    }
}
void ChaTag(Workspace& w, Bytes key, Bytes nonce, Bytes aad, Bytes ciphertext, UInt8* tag) noexcept
{
    ChaBlock(w, key, nonce, 0U);
    PolyStart(w, w.ChaBlock);
    PolyBlocks(w, aad, true);
    PolyBlocks(w, ciphertext, true);
    UInt8 length[16U]{};
    Store64(length, aad.Length);
    Store64(length + 8U, ciphertext.Length);
    PolyBlocks(w, {length, 16U}, false);
    PolyFinish(w, tag);
}
[[nodiscard]] bool AeadValid(Workspace& w, AeadAlgorithm a, Bytes key, Bytes nonce, Bytes aad, Bytes input,
                             MutableBytes output, Bytes tag) noexcept
{
    return PayloadBuffers(w, key, nonce, input, output) && Valid(aad) && Valid(tag) && DisjointWork(w, aad) &&
           DisjointWork(w, tag) && !Overlap(aad, Read(output)) && !Overlap(tag, Read(output)) &&
           input.Length <= MaximumAeadBytes && aad.Length <= MaximumAeadBytes && nonce.Length == 12U &&
           tag.Length == 16U && input.Length == output.Length &&
           ((a == AeadAlgorithm::AesGcm && (key.Length == 16U || key.Length == 32U)) ||
            (a == AeadAlgorithm::ChaCha20Poly1305 && key.Length == 32U));
}
} // namespace
Result AesEncrypt(Workspace& w, Bytes key, Bytes block, MutableBytes output) noexcept
{
    if (!DisjointWork(w, key) || !DisjointWork(w, block) || !DisjointWork(w, Read(output)))
    {
        return Result::InvalidArgument;
    }
    Cleanup cleanup{w};
    if (!PayloadBuffers(w, key, {nullptr, 0U}, block, output) || (key.Length != 16U && key.Length != 32U))
    {
        return Result::InvalidArgument;
    }
    if (block.Length != 16U || output.Length != 16U)
    {
        return Result::BufferSize;
    }
    AesSetup(w, key);
    AesBlock(w, key.Length, block.Data, output.Data);
    return Result::Success;
}
Result ChaCha20(Workspace& w, Bytes key, Bytes nonce, UInt32 counter, Bytes input, MutableBytes output) noexcept
{
    if (!DisjointWork(w, key) || !DisjointWork(w, nonce) || !DisjointWork(w, input) || !DisjointWork(w, Read(output)))
    {
        return Result::InvalidArgument;
    }
    Cleanup cleanup{w};
    if (!PayloadBuffers(w, key, nonce, input, output) || key.Length != 32U || nonce.Length != 12U)
    {
        return Result::InvalidArgument;
    }
    if (input.Length != output.Length || input.Length > MaximumAeadBytes ||
        ((input.Length + 63U) / 64U) > (0x100000000ULL - counter))
    {
        return Result::BufferSize;
    }
    ChaXor(w, key, nonce, counter, input, output);
    return Result::Success;
}
Result Poly1305(Workspace& w, Bytes key, Bytes input, MutableBytes tag) noexcept
{
    if (!DisjointWork(w, key) || !DisjointWork(w, input) || !DisjointWork(w, Read(tag)))
    {
        return Result::InvalidArgument;
    }
    Cleanup cleanup{w};
    if (!Valid(key) || !Input(input) || !Valid(tag) || key.Length != 32U)
    {
        return Result::InvalidArgument;
    }
    if (tag.Length != 16U)
    {
        return Result::BufferSize;
    }
    PolyStart(w, key.Data);
    PolyBlocks(w, input, false);
    PolyFinish(w, tag.Data);
    return Result::Success;
}
Result Seal(Workspace& w, AeadAlgorithm a, Bytes key, Bytes nonce, Bytes aad, Bytes plaintext, MutableBytes ciphertext,
            MutableBytes tag) noexcept
{
    if (!DisjointWork(w, key) || !DisjointWork(w, nonce) || !DisjointWork(w, aad) || !DisjointWork(w, plaintext) ||
        !DisjointWork(w, Read(ciphertext)) || !DisjointWork(w, Read(tag)))
    {
        return Result::InvalidArgument;
    }
    Cleanup cleanup{w};
    if (!AeadValid(w, a, key, nonce, aad, plaintext, ciphertext, Read(tag)) || Overlap(Read(tag), key) ||
        Overlap(Read(tag), nonce) || Overlap(Read(tag), aad) || Overlap(Read(tag), plaintext))
    {
        return Result::InvalidArgument;
    }
    if (a == AeadAlgorithm::AesGcm)
    {
        GcmStart(w, key, nonce);
        GcmXor(w, key, nonce, plaintext, ciphertext);
        GcmTag(w, key, nonce, aad, Read(ciphertext), tag.Data);
    }
    else
    {
        ChaXor(w, key, nonce, 1U, plaintext, ciphertext);
        ChaTag(w, key, nonce, aad, Read(ciphertext), tag.Data);
    }
    return Result::Success;
}
Result Open(Workspace& w, AeadAlgorithm a, Bytes key, Bytes nonce, Bytes aad, Bytes ciphertext, Bytes tag,
            MutableBytes plaintext) noexcept
{
    if (!DisjointWork(w, key) || !DisjointWork(w, nonce) || !DisjointWork(w, aad) || !DisjointWork(w, ciphertext) ||
        !DisjointWork(w, tag) || !DisjointWork(w, Read(plaintext)))
    {
        return Result::InvalidArgument;
    }
    Cleanup cleanup{w};
    if (!AeadValid(w, a, key, nonce, aad, ciphertext, plaintext, tag))
    {
        return Result::InvalidArgument;
    }
    if (a == AeadAlgorithm::AesGcm)
    {
        GcmStart(w, key, nonce);
        GcmTag(w, key, nonce, aad, ciphertext, w.Temporary);
    }
    else
    {
        ChaTag(w, key, nonce, aad, ciphertext, w.Temporary);
    }
    if (!Equal({w.Temporary, 16U}, tag))
    {
        return Result::AuthenticationFailed;
    }
    if (a == AeadAlgorithm::AesGcm)
    {
        GcmXor(w, key, nonce, ciphertext, plaintext);
    }
    else
    {
        ChaXor(w, key, nonce, 1U, ciphertext, plaintext);
    }
    return Result::Success;
}
} // namespace Asteria::User::Crypto
