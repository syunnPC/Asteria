// SPDX-License-Identifier: Apache-2.0

#include "User/Crypto/Internal.hpp"

namespace Asteria::User::Crypto
{
void Wipe(MutableBytes bytes) noexcept
{
    volatile UInt8* p = bytes.Data;
    for (Size i = 0U; i < bytes.Length; ++i)
    {
        p[i] = 0U;
    }
}
bool Equal(Bytes left, Bytes right) noexcept
{
    if (!Detail::Valid(left) || !Detail::Valid(right) || left.Length != right.Length)
    {
        return false;
    }
    UInt32 difference = 0U;
    for (Size i = 0U; i < left.Length; ++i)
    {
        difference |= static_cast<UInt32>(left.Data[i] ^ right.Data[i]);
    }
    return Detail::Zero(difference) != 0U;
}
namespace Detail
{
bool Overlap(Bytes a, Bytes b) noexcept
{
    if (a.Length == 0U || b.Length == 0U)
    {
        return false;
    }
    const auto x = reinterpret_cast<Foundation::UIntPointer>(a.Data);
    const auto y = reinterpret_cast<Foundation::UIntPointer>(b.Data);
    return x <= y ? y - x < a.Length : x - y < b.Length;
}
bool DisjointWork(Workspace& w, Bytes b) noexcept
{
    return !Overlap({reinterpret_cast<const UInt8*>(&w), sizeof(w)}, b);
}
namespace
{
// FIPS 180-4 (2015), §4.2.3; first 64 bits of fractional cube roots.
constexpr UInt64 K[80U]{
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

void Compress(Sha512State& s) noexcept
{
    UInt64 schedule[16U]{};
    UInt64 a = s.State[0];
    UInt64 b = s.State[1];
    UInt64 c = s.State[2];
    UInt64 d = s.State[3];
    UInt64 e = s.State[4];
    UInt64 f = s.State[5];
    UInt64 g = s.State[6];
    UInt64 h = s.State[7];
    for (Size r = 0U; r < 80U; ++r)
    {
        const Size slot = r & 15U;
        if (r < 16U)
        {
            for (Size j = 0U; j < 8U; ++j)
            {
                schedule[slot] = (schedule[slot] << 8U) | s.Block[r * 8U + j];
            }
        }
        else
        {
            const UInt64 x = schedule[(r + 1U) & 15U];
            const UInt64 y = schedule[(r + 14U) & 15U];
            schedule[slot] += (Ror(x, 1U) ^ Ror(x, 8U) ^ (x >> 7U)) + schedule[(r + 9U) & 15U] +
                              (Ror(y, 19U) ^ Ror(y, 61U) ^ (y >> 6U));
        }
        const UInt64 t = h + (Ror(e, 14U) ^ Ror(e, 18U) ^ Ror(e, 41U)) + ((e & f) ^ (~e & g)) + K[r] + schedule[slot];
        const UInt64 u = (Ror(a, 28U) ^ Ror(a, 34U) ^ Ror(a, 39U)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t;
        d = c;
        c = b;
        b = a;
        a = t + u;
    }
    s.State[0] += a;
    s.State[1] += b;
    s.State[2] += c;
    s.State[3] += d;
    s.State[4] += e;
    s.State[5] += f;
    s.State[6] += g;
    s.State[7] += h;
    Wipe({reinterpret_cast<UInt8*>(schedule), sizeof(schedule)});
}
} // namespace
void HashStart(HashState& s, HashAlgorithm a) noexcept
{
    if (a == HashAlgorithm::Sha256)
    {
        s.Sha256 = Detail::Sha256{};
    }
    else
    {
        for (Size i = 0U; i < 8U; ++i)
        {
            s.Sha512.State[i] = Sha512IV[i];
        }
        s.Sha512.Used = 0U;
        s.Sha512.Total = 0U;
    }
}
void HashUpdate(HashState& s, HashAlgorithm a, Bytes b) noexcept
{
    if (a == HashAlgorithm::Sha256)
    {
        s.Sha256.Update(b.Data, b.Length);
        return;
    }
    auto& t = s.Sha512;
    for (Size i = 0U; i < b.Length; ++i)
    {
        t.Block[t.Used++] = b.Data[i];
        if (t.Used == 128U)
        {
            Compress(t);
            t.Used = 0U;
        }
    }
    t.Total += b.Length;
}
void HashFinish(HashState& s, HashAlgorithm a, UInt8* out) noexcept
{
    if (a == HashAlgorithm::Sha256)
    {
        UInt8 digest[32U]{};
        s.Sha256.Finish(digest);
        Copy(out, digest, 32U);
        Wipe({digest, 32U});
        return;
    }
    auto& t = s.Sha512;
    const UInt64 bits = t.Total << 3U;
    t.Block[t.Used++] = 0x80U;
    if (t.Used > 112U)
    {
        while (t.Used < 128U)
        {
            t.Block[t.Used++] = 0U;
        }
        Compress(t);
        t.Used = 0U;
    }
    while (t.Used < 120U)
    {
        t.Block[t.Used++] = 0U;
    }
    for (Size i = 0U; i < 8U; ++i)
    {
        t.Block[120U + i] = static_cast<UInt8>(bits >> ((7U - i) * 8U));
    }
    Compress(t);
    for (Size i = 0U; i < 64U; ++i)
    {
        out[i] = static_cast<UInt8>(t.State[i / 8U] >> ((7U - i % 8U) * 8U));
    }
}
// RFC 2104 §2 / FIPS 198-1 §4. Branches use public key length only.
void HmacStart(Workspace& w, HashAlgorithm a, Bytes key) noexcept
{
    const Size block = a == HashAlgorithm::Sha256 ? 64U : 128U;
    for (Size i = 0U; i < block; ++i)
    {
        w.Pad[i] = 0U;
    }
    if (key.Length > block)
    {
        HashStart(w.Hash, a);
        HashUpdate(w.Hash, a, key);
        HashFinish(w.Hash, a, w.Pad);
    }
    else
    {
        Copy(w.Pad, key.Data, key.Length);
    }
    for (Size i = 0U; i < block; ++i)
    {
        w.Pad[i] ^= 0x36U;
    }
    HashStart(w.Hash, a);
    HashUpdate(w.Hash, a, {w.Pad, block});
    for (Size i = 0U; i < block; ++i)
    {
        w.Pad[i] ^= 0x36U ^ 0x5cU;
    }
    HashStart(w.Outer, a);
    HashUpdate(w.Outer, a, {w.Pad, block});
}
void HmacFinish(Workspace& w, HashAlgorithm a, UInt8* out) noexcept
{
    HashFinish(w.Hash, a, w.Digest);
    HashUpdate(w.Outer, a, {w.Digest, DigestBytes(a)});
    HashFinish(w.Outer, a, out);
}
} // namespace Detail
Result Hash(Workspace& w, HashAlgorithm a, Bytes input, MutableBytes digest) noexcept
{
    if (!Detail::DisjointWork(w, input) || !Detail::DisjointWork(w, Detail::Read(digest)))
    {
        return Result::InvalidArgument;
    }
    Detail::Cleanup cleanup{w};
    if (!Detail::Algorithm(a) || !Detail::Input(input) || !Detail::Valid(digest))
    {
        return Result::InvalidArgument;
    }
    if (digest.Length != Detail::DigestBytes(a))
    {
        return Result::BufferSize;
    }
    Detail::HashStart(w.Hash, a);
    Detail::HashUpdate(w.Hash, a, input);
    Detail::HashFinish(w.Hash, a, digest.Data);
    return Result::Success;
}
Result Hmac(Workspace& w, HashAlgorithm a, Bytes key, Bytes message, MutableBytes tag) noexcept
{
    if (!Detail::DisjointWork(w, key) || !Detail::DisjointWork(w, message) ||
        !Detail::DisjointWork(w, Detail::Read(tag)))
    {
        return Result::InvalidArgument;
    }
    Detail::Cleanup cleanup{w};
    if (!Detail::Algorithm(a) || !Detail::Input(key) || !Detail::Input(message) || !Detail::Valid(tag) ||
        message.Length > Detail::MaximumHashBytes - 256U)
    {
        return Result::InvalidArgument;
    }
    if (tag.Length != Detail::DigestBytes(a))
    {
        return Result::BufferSize;
    }
    Detail::HmacStart(w, a, key);
    Detail::HashUpdate(w.Hash, a, message);
    Detail::HmacFinish(w, a, tag.Data);
    return Result::Success;
}
Result HkdfExtract(Workspace& w, HashAlgorithm a, Bytes salt, Bytes input, MutableBytes key) noexcept
{
    return Hmac(w, a, salt, input, key);
}
Result HkdfExpand(Workspace& w, HashAlgorithm a, Bytes key, Bytes info, MutableBytes output) noexcept
{
    if (!Detail::DisjointWork(w, key) || !Detail::DisjointWork(w, info) ||
        !Detail::DisjointWork(w, Detail::Read(output)))
    {
        return Result::InvalidArgument;
    }
    Detail::Cleanup cleanup{w};
    if (!Detail::Algorithm(a) || !Detail::Input(key) || !Detail::Input(info) || !Detail::Valid(output) ||
        info.Length > Detail::MaximumHashBytes - 257U || Detail::Overlap(key, Detail::Read(output)) ||
        Detail::Overlap(info, Detail::Read(output)))
    {
        return Result::InvalidArgument;
    }
    const Size n = Detail::DigestBytes(a);
    if (key.Length < n || output.Length > 255U * n)
    {
        return Result::BufferSize;
    }
    Size used = 0U;
    Size previous = 0U;
    for (UInt32 block = 1U; used < output.Length; ++block)
    {
        const UInt8 counter = static_cast<UInt8>(block);
        Detail::HmacStart(w, a, key);
        Detail::HashUpdate(w.Hash, a, {w.Expansion, previous});
        Detail::HashUpdate(w.Hash, a, info);
        Detail::HashUpdate(w.Hash, a, {&counter, 1U});
        Detail::HmacFinish(w, a, w.Expansion);
        const Size amount = output.Length - used < n ? output.Length - used : n;
        Detail::Copy(output.Data + used, w.Expansion, amount);
        used += amount;
        previous = n;
    }
    return Result::Success;
}
} // namespace Asteria::User::Crypto
