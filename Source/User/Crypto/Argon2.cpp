// SPDX-License-Identifier: Apache-2.0

#include "User/Crypto/Internal.hpp"

namespace Asteria::User::Crypto
{
namespace
{
using Detail::BlakeState;
using Detail::Cleanup;
using Detail::Copy;
using Detail::DisjointWork;
using Detail::Load64;
using Detail::Mask;
using Detail::Overlap;
using Detail::Read;
using Detail::Ror;
using Detail::Same;
using Detail::Sha512IV;
using Detail::Store32;
using Detail::Valid;
// RFC 7693 §§2.6/2.7, Blake2b IV and public message permutations.

constexpr UInt8 Sigma[10U][16U]{
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0}};
void BlakeG(UInt64& a, UInt64& b, UInt64& c, UInt64& d, UInt64 x, UInt64 y) noexcept
{
    a += b + x;
    d = Ror(d ^ a, 32U);
    c += d;
    b = Ror(b ^ c, 24U);
    a += b + y;
    d = Ror(d ^ a, 16U);
    c += d;
    b = Ror(b ^ c, 63U);
}
void BlakeCompress(BlakeState& s, bool final) noexcept
{
    for (Size i = 0U; i < 8U; ++i)
    {
        s.V[i] = s.H[i];
        s.V[i + 8U] = Sha512IV[i];
    }
    s.V[12] ^= s.Total;
    s.V[14] ^= final ? ~0ULL : 0ULL;
    for (Size i = 0U; i < 16U; ++i)
    {
        s.M[i] = Load64(s.Block + 8U * i);
    }
    for (Size r = 0U; r < 12U; ++r)
    {
        const auto* p = Sigma[r % 10U];
        auto* v = s.V;
        BlakeG(v[0], v[4], v[8], v[12], s.M[p[0]], s.M[p[1]]);
        BlakeG(v[1], v[5], v[9], v[13], s.M[p[2]], s.M[p[3]]);
        BlakeG(v[2], v[6], v[10], v[14], s.M[p[4]], s.M[p[5]]);
        BlakeG(v[3], v[7], v[11], v[15], s.M[p[6]], s.M[p[7]]);
        BlakeG(v[0], v[5], v[10], v[15], s.M[p[8]], s.M[p[9]]);
        BlakeG(v[1], v[6], v[11], v[12], s.M[p[10]], s.M[p[11]]);
        BlakeG(v[2], v[7], v[8], v[13], s.M[p[12]], s.M[p[13]]);
        BlakeG(v[3], v[4], v[9], v[14], s.M[p[14]], s.M[p[15]]);
    }
    for (Size i = 0U; i < 8U; ++i)
    {
        s.H[i] ^= s.V[i] ^ s.V[i + 8U];
    }
}
void BlakeStart(BlakeState& s, Size n) noexcept
{
    for (Size i = 0U; i < 8U; ++i)
    {
        s.H[i] = Sha512IV[i];
    }
    s.H[0] ^= 0x01010000ULL | n;
    s.Used = 0U;
    s.Total = 0U;
    s.Output = n;
}
void BlakeUpdate(BlakeState& s, Bytes b) noexcept
{
    for (Size i = 0U; i < b.Length; ++i)
    {
        if (s.Used == 128U)
        {
            s.Total += 128U;
            BlakeCompress(s, false);
            s.Used = 0U;
        }
        s.Block[s.Used++] = b.Data[i];
    }
}
void BlakeFinish(BlakeState& s, UInt8* output) noexcept
{
    s.Total += s.Used;
    for (Size i = s.Used; i < 128U; ++i)
    {
        s.Block[i] = 0U;
    }
    BlakeCompress(s, true);
    for (Size i = 0U; i < s.Output; ++i)
    {
        output[i] = static_cast<UInt8>(s.H[i / 8U] >> ((i % 8U) * 8U));
    }
}
void BlakeWord(BlakeState& s, UInt32 x) noexcept
{
    UInt8 b[4U]{};
    Store32(b, x);
    BlakeUpdate(s, {b, 4U});
}
void BlakeInput(BlakeState& s, Bytes b) noexcept
{
    BlakeWord(s, static_cast<UInt32>(b.Length));
    BlakeUpdate(s, b);
}
// RFC 9106 §3.3, H'. Intermediate digest lives outside the output block.
void LongHash(Workspace& w, Bytes input, MutableBytes out) noexcept
{
    BlakeStart(w.Blake, out.Length <= 64U ? out.Length : 64U);
    BlakeWord(w.Blake, static_cast<UInt32>(out.Length));
    BlakeUpdate(w.Blake, input);
    if (out.Length <= 64U)
    {
        BlakeFinish(w.Blake, out.Data);
        return;
    }
    BlakeFinish(w.Blake, w.ArgonHash);
    Size offset = 0U;
    while (out.Length - offset > 64U)
    {
        Copy(out.Data + offset, w.ArgonHash, 32U);
        offset += 32U;
        if (out.Length - offset > 64U)
        {
            BlakeStart(w.Blake, 64U);
            BlakeUpdate(w.Blake, {w.ArgonHash, 64U});
            BlakeFinish(w.Blake, w.ArgonHash);
        }
    }
    BlakeStart(w.Blake, out.Length - offset);
    BlakeUpdate(w.Blake, {w.ArgonHash, 64U});
    BlakeFinish(w.Blake, out.Data + offset);
}
// RFC 9106 §§3.5/3.6: BlaMka additions use low 32-bit products.
[[nodiscard]] UInt64 BlaAdd(UInt64 a, UInt64 b) noexcept
{
    return a + b + 2U * static_cast<UInt64>(static_cast<UInt32>(a)) * static_cast<UInt32>(b);
}
void ArgonG(UInt64& a, UInt64& b, UInt64& c, UInt64& d) noexcept
{
    a = BlaAdd(a, b);
    d = Ror(d ^ a, 32U);
    c = BlaAdd(c, d);
    b = Ror(b ^ c, 24U);
    a = BlaAdd(a, b);
    d = Ror(d ^ a, 16U);
    c = BlaAdd(c, d);
    b = Ror(b ^ c, 63U);
}
void ArgonRound(UInt64* x, Size base, Size stride) noexcept
{
    // Register i is one pair of words; stride 2 selects rows, 16 columns.
    const auto index = [base, stride](Size i) noexcept -> Size
    {
        return base + (i / 2U) * stride + i % 2U;
    };
    ArgonG(x[index(0)], x[index(4)], x[index(8)], x[index(12)]);
    ArgonG(x[index(1)], x[index(5)], x[index(9)], x[index(13)]);
    ArgonG(x[index(2)], x[index(6)], x[index(10)], x[index(14)]);
    ArgonG(x[index(3)], x[index(7)], x[index(11)], x[index(15)]);
    ArgonG(x[index(0)], x[index(5)], x[index(10)], x[index(15)]);
    ArgonG(x[index(1)], x[index(6)], x[index(11)], x[index(12)]);
    ArgonG(x[index(2)], x[index(7)], x[index(8)], x[index(13)]);
    ArgonG(x[index(3)], x[index(4)], x[index(9)], x[index(14)]);
}
void Compress(Workspace& w, const Argon2Block& a, const Argon2Block& b, Argon2Block& out, bool xorOld) noexcept
{
    auto& r = w.Argon[0];
    auto& z = w.Argon[1];
    for (Size i = 0U; i < 128U; ++i)
    {
        r.Word[i] = a.Word[i] ^ b.Word[i];
        z.Word[i] = r.Word[i];
    }
    for (Size i = 0U; i < 8U; ++i)
    {
        ArgonRound(z.Word, i * 16U, 2U);
    }
    for (Size i = 0U; i < 8U; ++i)
    {
        ArgonRound(z.Word, i * 2U, 16U);
    }
    for (Size i = 0U; i < 128U; ++i)
    {
        out.Word[i] = (xorOld ? out.Word[i] : 0U) ^ r.Word[i] ^ z.Word[i];
    }
}
void ZeroBlock(Argon2Block& b) noexcept
{
    for (Size i = 0U; i < 128U; ++i)
    {
        b.Word[i] = 0U;
    }
}
// Fixed 32-round binary remainder; no division on password-derived values.
[[nodiscard]] UInt32 Remainder(UInt32 x, UInt32 divisor) noexcept
{
    UInt32 r = 0U;
    for (UInt32 bit = 32U; bit > 0U; --bit)
    {
        r = (r << 1U) | ((x >> (bit - 1U)) & 1U);
        const UInt32 d = r - divisor;
        r -= divisor & Mask((d >> 31U) ^ 1U);
    }
    return r;
}
[[nodiscard]] UInt32 Reference(UInt64 random, UInt32 lane, UInt32 pass, UInt32 slice, UInt32 index, UInt32 lanes,
                               UInt32 segment) noexcept
{
    UInt32 refLane = Remainder(static_cast<UInt32>(random >> 32U), lanes);
    if (pass == 0U && slice == 0U)
    {
        refLane = lane;
    }
    const UInt32 same = Same(refLane, lane);
    const UInt32 mask = Mask(same);
    const UInt32 length = segment * 4U;
    UInt32 own = 0U;
    UInt32 other = 0U;
    if (pass == 0U)
    {
        own = slice * segment + index - 1U;
        other = slice * segment - (index == 0U ? 1U : 0U);
    }
    else
    {
        own = length - segment + index - 1U;
        other = length - segment - (index == 0U ? 1U : 0U);
    }
    const UInt32 area = (own & mask) | (other & ~mask);
    const UInt64 j = static_cast<UInt32>(random);
    const UInt64 square = (j * j) >> 32U;
    const UInt32 position = area - 1U - static_cast<UInt32>((static_cast<UInt64>(area) * square) >> 32U);
    const UInt32 start = pass == 0U ? 0U : ((slice + 1U) * segment) % length;
    return refLane * length + Remainder(start + position, length);
}
[[nodiscard]] bool ArgonInput(Bytes b) noexcept
{
    return Valid(b) && b.Length <= 1024U;
}
[[nodiscard]] bool ArenaDisjoint(Workspace& w, const Argon2Block* memory, Size memoryBlocks) noexcept
{
    // Cap this preflight size before multiplication, even for a refused count.
    // Arena overlap must be refused before Cleanup can erase caller memory.
    const Size bounded = memoryBlocks < MaximumArgonBlocks ? memoryBlocks : MaximumArgonBlocks;
    return DisjointWork(w, {reinterpret_cast<const UInt8*>(memory), bounded * sizeof(Argon2Block)});
}
} // namespace
Result Argon2id(Workspace& w, Argon2Parameters p, Bytes password, Bytes salt, Bytes secret, Bytes associated,
                Argon2Block* memory, Size memoryBlocks, MutableBytes tag, Argon2Progress progress) noexcept
{
    const Bytes buffers[]{password, salt, secret, associated, Read(tag)};
    for (const auto b : buffers)
    {
        if (!DisjointWork(w, b))
        {
            return Result::InvalidArgument;
        }
    }
    if (!ArenaDisjoint(w, memory, memoryBlocks))
    {
        return Result::InvalidArgument;
    }
    Cleanup cleanup{w};
    if (!ArgonInput(password) || !ArgonInput(salt) || !ArgonInput(secret) || !ArgonInput(associated) || !Valid(tag) ||
        salt.Length < 8U || p.Lanes == 0U || p.Lanes > 4U || p.Passes == 0U || p.Passes > 32U ||
        p.MemoryKiB < 8U * p.Lanes || p.MemoryKiB > MaximumArgonBlocks || memory == nullptr)
    {
        return Result::InvalidArgument;
    }
    if (tag.Length < 4U || tag.Length > 1024U)
    {
        return Result::BufferSize;
    }
    const UInt32 blocks = (p.MemoryKiB / (4U * p.Lanes)) * (4U * p.Lanes);
    const UInt32 segment = blocks / (4U * p.Lanes);
    const UInt32 length = 4U * segment;
    if (memoryBlocks < blocks)
    {
        return Result::BufferSize;
    }
    const Bytes arena{reinterpret_cast<const UInt8*>(memory), static_cast<Size>(blocks) * sizeof(Argon2Block)};
    if (!DisjointWork(w, arena))
    {
        return Result::InvalidArgument;
    }
    for (const auto b : buffers)
    {
        if (Overlap(arena, b))
        {
            return Result::InvalidArgument;
        }
    }
    for (Size i = 0U; i < 4U; ++i)
    {
        if (Overlap(buffers[i], Read(tag)))
        {
            return Result::InvalidArgument;
        }
    }
    // RFC 9106 §3.2 H0: input memory cost is original m, not rounded m'.
    BlakeStart(w.Blake, 64U);
    BlakeWord(w.Blake, p.Lanes);
    BlakeWord(w.Blake, static_cast<UInt32>(tag.Length));
    BlakeWord(w.Blake, p.MemoryKiB);
    BlakeWord(w.Blake, p.Passes);
    BlakeWord(w.Blake, 0x13U);
    BlakeWord(w.Blake, 2U);
    BlakeInput(w.Blake, password);
    BlakeInput(w.Blake, salt);
    BlakeInput(w.Blake, secret);
    BlakeInput(w.Blake, associated);
    BlakeFinish(w.Blake, w.ArgonSeed);
    for (UInt32 lane = 0U; lane < p.Lanes; ++lane)
    {
        for (UInt32 column = 0U; column < 2U; ++column)
        {
            Store32(w.ArgonSeed + 64U, column);
            Store32(w.ArgonSeed + 68U, lane);
            LongHash(w, {w.ArgonSeed, 72U},
                     {reinterpret_cast<UInt8*>(&memory[lane * length + column]), sizeof(Argon2Block)});
        }
    }
    auto& zero = w.Argon[2];
    auto& address = w.Argon[3];
    auto& input = w.Argon[4];
    auto& selected = w.Argon[5];
    ZeroBlock(zero);
    for (UInt32 pass = 0U; pass < p.Passes; ++pass)
    {
        for (UInt32 slice = 0U; slice < 4U; ++slice)
        {
            for (UInt32 lane = 0U; lane < p.Lanes; ++lane)
            {
                const bool independent = pass == 0U && slice < 2U;
                const UInt32 begin = pass == 0U && slice == 0U ? 2U : 0U;
                ZeroBlock(input);
                input.Word[0] = pass;
                input.Word[1] = lane;
                input.Word[2] = slice;
                input.Word[3] = blocks;
                input.Word[4] = p.Passes;
                input.Word[5] = 2U;
                if (independent && begin != 0U)
                {
                    ++input.Word[6];
                    Compress(w, zero, input, address, false);
                    Compress(w, zero, address, address, false);
                }
                for (UInt32 index = begin; index < segment; ++index)
                {
                    const UInt32 column = slice * segment + index;
                    const UInt32 current = lane * length + column;
                    const UInt32 previous = lane * length + (column == 0U ? length - 1U : column - 1U);
                    if (independent && index % 128U == 0U)
                    {
                        ++input.Word[6];
                        Compress(w, zero, input, address, false);
                        Compress(w, zero, address, address, false);
                    }
                    const UInt64 random = independent ? address.Word[index % 128U] : memory[previous].Word[0];
                    const UInt32 reference = Reference(random, lane, pass, slice, index, p.Lanes, segment);
                    // RFC 9106 §3.4.1.3 / §3.4.2: this reference read is the sole
                    // secret-address exception,
                    // after the first half of pass 0; see Crypto.md's cache risk.
                    Compress(w, memory[previous], memory[reference], memory[current], pass != 0U);
                }
                if (progress.Poll != nullptr)
                {
                    progress.Poll(progress.Context);
                }
            }
        }
    }
    ZeroBlock(selected);
    for (UInt32 lane = 0U; lane < p.Lanes; ++lane)
    {
        for (Size i = 0U; i < 128U; ++i)
        {
            selected.Word[i] ^= memory[lane * length + length - 1U].Word[i];
        }
    }
    LongHash(w, {reinterpret_cast<const UInt8*>(&selected), sizeof(selected)}, tag);
    Wipe({reinterpret_cast<UInt8*>(memory), static_cast<Size>(blocks) * sizeof(Argon2Block)});
    return Result::Success;
}
} // namespace Asteria::User::Crypto
