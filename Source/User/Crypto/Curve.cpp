// SPDX-License-Identifier: Apache-2.0

#include "User/Crypto/Internal.hpp"

namespace Asteria::User::Crypto
{
namespace
{
using Detail::Field;
using Detail::Point;
using Detail::Cleanup;
using Detail::Copy;
using Detail::DisjointWork;
using Detail::HashFinish;
using Detail::HashStart;
using Detail::HashUpdate;
using Detail::Input;
using Detail::Mask;
using Detail::MaximumHashBytes;
using Detail::Overlap;
using Detail::Read;
using Detail::Valid;
using Detail::Zero;
// RFC 7748 §4.1 / RFC 8032 §5.1. Field p=2^255-19, radix 2^16.
constexpr UInt32 Prime[16U]{65517U, 65535U, 65535U, 65535U, 65535U, 65535U, 65535U, 65535U,
                            65535U, 65535U, 65535U, 65535U, 65535U, 65535U, 65535U, 32767U};
constexpr UInt32 D[16U]{30883U, 4953U,  19914U, 30187U, 55467U, 16705U, 2637U,  112U,
                        59544U, 30585U, 16505U, 36039U, 65139U, 11119U, 27886U, 20995U};
constexpr UInt32 BaseX[16U]{54554U, 36645U, 11616U, 51542U, 42930U, 38181U, 51040U, 26924U,
                            56412U, 64982U, 57905U, 49316U, 21502U, 52590U, 14035U, 8553U};
constexpr UInt32 BaseY[16U]{26200U, 26214U, 26214U, 26214U, 26214U, 26214U, 26214U, 26214U,
                            26214U, 26214U, 26214U, 26214U, 26214U, 26214U, 26214U, 26214U};
constexpr UInt32 SqrtMinusOne[16U]{41136U, 18958U, 6951U, 50414U, 58488U, 44335U, 6150U, 12099U,
                                   55207U, 15867U, 153U,  11085U, 57099U, 20417U, 9344U, 11139U};
constexpr UInt32 Order[16U]{54253U, 23797U, 25370U, 22546U, 40150U, 41719U, 63966U, 5342U,
                            0U,     0U,     0U,     0U,     0U,     0U,     0U,     4096U};
void Set(Field& f, UInt32 n) noexcept
{
    for (Size i = 0U; i < 16U; ++i)
    {
        f.Limb[i] = 0U;
    }
    f.Limb[0] = n;
}
void Constant(Field& f, const UInt32* n) noexcept
{
    for (Size i = 0U; i < 16U; ++i)
    {
        f.Limb[i] = n[i];
    }
}
void Select(Field& out, const Field& a, const Field& b, UInt32 bit) noexcept
{
    const UInt32 mask = Mask(bit);
    for (Size i = 0U; i < 16U; ++i)
    {
        out.Limb[i] = (a.Limb[i] & ~mask) | (b.Limb[i] & mask);
    }
}
void Swap(Field& a, Field& b, UInt32 bit) noexcept
{
    const UInt32 mask = Mask(bit);
    for (Size i = 0U; i < 16U; ++i)
    {
        const UInt32 x = (a.Limb[i] ^ b.Limb[i]) & mask;
        a.Limb[i] ^= x;
        b.Limb[i] ^= x;
    }
}
void Normalize(Field& f) noexcept
{
    for (Size pass = 0U; pass < 3U; ++pass)
    {
        for (Size i = 0U; i < 15U; ++i)
        {
            f.Limb[i + 1U] += f.Limb[i] >> 16U;
            f.Limb[i] &= 65535U;
        }
        const UInt32 carry = f.Limb[15] >> 15U;
        f.Limb[15] &= 32767U;
        f.Limb[0] += 19U * carry;
    }
    UInt32 t[16U]{};
    UInt32 borrow = 0U;
    for (Size i = 0U; i < 16U; ++i)
    {
        const UInt32 x = f.Limb[i] - Prime[i] - borrow;
        t[i] = x & 65535U;
        borrow = x >> 31U;
    }
    const UInt32 mask = Mask(borrow ^ 1U);
    for (Size i = 0U; i < 16U; ++i)
    {
        f.Limb[i] = (f.Limb[i] & ~mask) | (t[i] & mask);
    }
    Wipe({reinterpret_cast<UInt8*>(t), sizeof(t)});
}
void Add(Field& out, const Field& a, const Field& b) noexcept
{
    for (Size i = 0U; i < 16U; ++i)
    {
        out.Limb[i] = a.Limb[i] + b.Limb[i];
    }
    Normalize(out);
}
void Subtract(Field& out, const Field& a, const Field& b) noexcept
{
    for (Size i = 0U; i < 16U; ++i)
    {
        out.Limb[i] = a.Limb[i] + 2U * Prime[i] - b.Limb[i];
    }
    Normalize(out);
}
void Mul(Workspace& w, Field& out, const Field& a, const Field& b) noexcept
{
    for (Size i = 0U; i < 31U; ++i)
    {
        w.Product[i] = 0U;
    }
    for (Size i = 0U; i < 16U; ++i)
    {
        for (Size j = 0U; j < 16U; ++j)
        {
            w.Product[i + j] += static_cast<UInt64>(a.Limb[i]) * b.Limb[j];
        }
    }
    for (Size i = 16U; i < 31U; ++i)
    {
        w.Product[i - 16U] += 38U * w.Product[i];
    }
    for (Size pass = 0U; pass < 3U; ++pass)
    {
        for (Size i = 0U; i < 15U; ++i)
        {
            w.Product[i + 1U] += w.Product[i] >> 16U;
            w.Product[i] &= 65535U;
        }
        const UInt64 carry = w.Product[15] >> 15U;
        w.Product[15] &= 32767U;
        w.Product[0] += 19U * carry;
    }
    for (Size i = 0U; i < 16U; ++i)
    {
        out.Limb[i] = static_cast<UInt32>(w.Product[i]);
    }
    Normalize(out);
}
void Decode(Field& out, const UInt8* input) noexcept
{
    for (Size i = 0U; i < 16U; ++i)
    {
        out.Limb[i] = static_cast<UInt32>(input[2U * i]) | (static_cast<UInt32>(input[2U * i + 1U]) << 8U);
    }
    out.Limb[15] &= 32767U;
    Normalize(out);
}
void Encode(const Field& f, UInt8* out) noexcept
{
    for (Size i = 0U; i < 16U; ++i)
    {
        out[2U * i] = static_cast<UInt8>(f.Limb[i]);
        out[2U * i + 1U] = static_cast<UInt8>(f.Limb[i] >> 8U);
    }
}
[[nodiscard]] UInt32 FieldEqual(const Field& a, const Field& b) noexcept
{
    UInt32 d = 0U;
    for (Size i = 0U; i < 16U; ++i)
    {
        d |= a.Limb[i] ^ b.Limb[i];
    }
    return Zero(d);
}
[[nodiscard]] UInt32 FieldZero(const Field& a) noexcept
{
    UInt32 d = 0U;
    for (Size i = 0U; i < 16U; ++i)
    {
        d |= a.Limb[i];
    }
    return Zero(d);
}
// Public binary exponent schedules: p-2 or (p+3)/8. No secret branches.
void Power(Workspace& w, Field& out, const Field& input, bool inverse) noexcept
{
    auto& base = w.Field[20];
    auto& acc = w.Field[21];
    base = input;
    Set(acc, 1U);
    const UInt32 bits = inverse ? 255U : 252U;
    for (UInt32 bit = bits; bit > 0U; --bit)
    {
        Mul(w, acc, acc, acc);
        const UInt32 index = bit - 1U;
        const bool set = inverse ? (index >= 5U || ((0x0bU >> index) & 1U) != 0U) : (index >= 1U);
        if (set)
        {
            Mul(w, acc, acc, base);
        }
    }
    out = acc;
}
void Identity(Point& p) noexcept
{
    Set(p.X, 0U);
    Set(p.Y, 1U);
    Set(p.Z, 1U);
    Set(p.T, 0U);
}
void Base(Point& p) noexcept
{
    Constant(p.X, BaseX);
    Constant(p.Y, BaseY);
    Set(p.Z, 1U);
}
// RFC 8032 §5.1.4 complete extended-coordinate addition (a=-1).
void PointAdd(Workspace& w, Point& out, const Point& p, const Point& q) noexcept
{
    auto& a = w.Field[0];
    auto& b = w.Field[1];
    auto& c = w.Field[2];
    auto& d = w.Field[3];
    auto& e = w.Field[4];
    auto& f = w.Field[5];
    auto& g = w.Field[6];
    auto& h = w.Field[7];
    auto& tmp = w.Field[8];
    Subtract(a, p.Y, p.X);
    Subtract(tmp, q.Y, q.X);
    Mul(w, a, a, tmp);
    Add(b, p.Y, p.X);
    Add(tmp, q.Y, q.X);
    Mul(w, b, b, tmp);
    Constant(tmp, D);
    Mul(w, c, p.T, q.T);
    Mul(w, c, c, tmp);
    Add(c, c, c);
    Mul(w, d, p.Z, q.Z);
    Add(d, d, d);
    Subtract(e, b, a);
    Subtract(f, d, c);
    Add(g, d, c);
    Add(h, b, a);
    Mul(w, out.X, e, f);
    Mul(w, out.Y, g, h);
    Mul(w, out.T, e, h);
    Mul(w, out.Z, f, g);
}
void PointSelect(Point& out, const Point& a, const Point& b, UInt32 bit) noexcept
{
    Select(out.X, a.X, b.X, bit);
    Select(out.Y, a.Y, b.Y, bit);
    Select(out.Z, a.Z, b.Z, bit);
    Select(out.T, a.T, b.T, bit);
}
void ScalarPoint(Workspace& w, Point& out, const Point& p, const UInt8* scalar) noexcept
{
    auto& acc = w.Point[4];
    auto& tmp = w.Point[5];
    Identity(acc);
    for (UInt32 bit = 256U; bit > 0U; --bit)
    {
        PointAdd(w, acc, acc, acc);
        PointAdd(w, tmp, acc, p);
        PointSelect(acc, acc, tmp, (static_cast<UInt32>(scalar[(bit - 1U) / 8U]) >> ((bit - 1U) % 8U)) & 1U);
    }
    out = acc;
}
void PointEncode(Workspace& w, const Point& p, UInt8* out) noexcept
{
    auto& inverse = w.Field[10];
    auto& x = w.Field[11];
    auto& y = w.Field[12];
    Power(w, inverse, p.Z, true);
    Mul(w, x, p.X, inverse);
    Mul(w, y, p.Y, inverse);
    Encode(y, out);
    out[31] |= static_cast<UInt8>((x.Limb[0] & 1U) << 7U);
}
[[nodiscard]] UInt32 PointIdentity(const Point& p) noexcept
{
    return FieldZero(p.X) & FieldEqual(p.Y, p.Z);
}
[[nodiscard]] UInt32 PointDecode(Workspace& w, Point& p, const UInt8* input) noexcept
{
    auto& y2 = w.Field[10];
    auto& u = w.Field[11];
    auto& v = w.Field[12];
    auto& x2 = w.Field[13];
    auto& check = w.Field[14];
    auto& tmp = w.Field[15];
    Decode(p.Y, input);
    Encode(p.Y, w.CurveBytes[4]);
    UInt32 difference = 0U;
    for (Size i = 0U; i < 32U; ++i)
    {
        difference |= w.CurveBytes[4][i] ^ (input[i] & (i == 31U ? 127U : 255U));
    }
    UInt32 valid = Zero(difference);
    Set(p.Z, 1U);
    Mul(w, y2, p.Y, p.Y);
    Subtract(u, y2, p.Z);
    Constant(tmp, D);
    Mul(w, v, y2, tmp);
    Add(v, v, p.Z);
    Power(w, tmp, v, true);
    Mul(w, x2, u, tmp);
    Power(w, p.X, x2, false);
    Mul(w, check, p.X, p.X);
    const UInt32 first = FieldEqual(check, x2);
    Constant(tmp, SqrtMinusOne);
    Mul(w, tmp, p.X, tmp);
    Select(p.X, tmp, p.X, first);
    Mul(w, check, p.X, p.X);
    valid &= FieldEqual(check, x2);
    const UInt32 sign = input[31] >> 7U;
    valid &= 1U ^ (FieldZero(p.X) & sign);
    Set(tmp, 0U);
    Subtract(tmp, tmp, p.X);
    Select(p.X, p.X, tmp, (p.X.Limb[0] & 1U) ^ sign);
    Mul(w, p.T, p.X, p.Y);
    // Reject small-order encodings with a fixed three doublings.
    w.Point[3] = p;
    for (Size i = 0U; i < 3U; ++i)
    {
        PointAdd(w, w.Point[3], w.Point[3], w.Point[3]);
    }
    valid &= PointIdentity(w.Point[3]) ^ 1U;
    return valid;
}
// Fixed bitwise reduction modulo L (RFC 8032 §5.1.5/6), at most 512 bits.
void ScalarReduce(Workspace& w, const UInt8* in, Size bytes, UInt8* out) noexcept
{
    for (Size i = 0U; i < 16U; ++i)
    {
        w.Scalar[i] = 0U;
    }
    for (Size bit = bytes * 8U; bit > 0U; --bit)
    {
        UInt32 carry = (static_cast<UInt32>(in[(bit - 1U) / 8U]) >> ((bit - 1U) % 8U)) & 1U;
        for (Size i = 0U; i < 16U; ++i)
        {
            const UInt32 x = (w.Scalar[i] << 1U) | carry;
            w.Scalar[i] = x & 65535U;
            carry = x >> 16U;
        }
        UInt32 borrow = 0U;
        for (Size i = 0U; i < 16U; ++i)
        {
            const UInt32 x = w.Scalar[i] - Order[i] - borrow;
            w.ScalarTemp[i] = x & 65535U;
            borrow = x >> 31U;
        }
        const UInt32 mask = Mask(borrow ^ 1U);
        for (Size i = 0U; i < 16U; ++i)
        {
            w.Scalar[i] = (w.Scalar[i] & ~mask) | (w.ScalarTemp[i] & mask);
        }
    }
    for (Size i = 0U; i < 16U; ++i)
    {
        out[i * 2U] = static_cast<UInt8>(w.Scalar[i]);
        out[i * 2U + 1U] = static_cast<UInt8>(w.Scalar[i] >> 8U);
    }
}
void ScalarMulAdd(Workspace& w, const UInt8* k, const UInt8* s, const UInt8* r, UInt8* out) noexcept
{
    // Byte radix gives bounded 64-bit sums, no native wide integers.
    for (Size i = 0U; i < 64U; ++i)
    {
        UInt64 sum = i < 32U ? r[i] : 0U;
        const Size low = i < 32U ? 0U : i - 31U;
        const Size high = i < 32U ? i : 31U;
        for (Size j = low; j <= high; ++j)
        {
            sum += static_cast<UInt64>(k[j]) * s[i - j];
        }
        // carry is held in Scalar[0], not in Product (field scratch).
        if (i == 0U)
        {
            w.Scalar[0] = 0U;
        }
        sum += w.Scalar[0];
        w.CurveBytes[3][i] = static_cast<UInt8>(sum);
        w.Scalar[0] = static_cast<UInt32>(sum >> 8U);
    }
    ScalarReduce(w, w.CurveBytes[3], 64U, out);
}
[[nodiscard]] bool CurveBuffers(Workspace& w, Bytes key, Bytes input, MutableBytes output) noexcept
{
    return Input(key) && Input(input) && Valid(output) && DisjointWork(w, key) && DisjointWork(w, input) &&
           DisjointWork(w, Read(output)) && !Overlap(key, Read(output)) && !Overlap(input, Read(output));
}
void SeedExpand(Workspace& w, Bytes seed) noexcept
{
    HashStart(w.Hash, HashAlgorithm::Sha512);
    HashUpdate(w.Hash, HashAlgorithm::Sha512, seed);
    HashFinish(w.Hash, HashAlgorithm::Sha512, w.CurveBytes[0]);
    w.CurveBytes[0][0] &= 248U;
    w.CurveBytes[0][31] &= 63U;
    w.CurveBytes[0][31] |= 64U;
}
} // namespace
Result X25519(Workspace& w, Bytes scalar, Bytes coordinate, MutableBytes shared) noexcept
{
    if (!DisjointWork(w, scalar) || !DisjointWork(w, coordinate) || !DisjointWork(w, Read(shared)))
    {
        return Result::InvalidArgument;
    }
    Cleanup cleanup{w};
    if (!CurveBuffers(w, scalar, coordinate, shared))
    {
        return Result::InvalidArgument;
    }
    if (scalar.Length != 32U || coordinate.Length != 32U || shared.Length != 32U)
    {
        return Result::BufferSize;
    }
    Copy(w.CurveBytes[0], scalar.Data, 32U);
    w.CurveBytes[0][0] &= 248U;
    w.CurveBytes[0][31] &= 127U;
    w.CurveBytes[0][31] |= 64U;
    auto& x1 = w.Field[0];
    auto& x2 = w.Field[1];
    auto& z2 = w.Field[2];
    auto& x3 = w.Field[3];
    auto& z3 = w.Field[4];
    auto& a = w.Field[5];
    auto& aa = w.Field[6];
    auto& b = w.Field[7];
    auto& bb = w.Field[8];
    auto& e = w.Field[9];
    auto& c = w.Field[10];
    auto& d = w.Field[11];
    auto& da = w.Field[12];
    auto& cb = w.Field[13];
    auto& tmp = w.Field[14];
    Decode(x1, coordinate.Data);
    Set(x2, 1U);
    Set(z2, 0U);
    x3 = x1;
    Set(z3, 1U);
    UInt32 swap = 0U;
    // RFC 7748 §5 Montgomery ladder, a24=121665.
    for (UInt32 bit = 255U; bit > 0U; --bit)
    {
        const UInt32 k = (static_cast<UInt32>(w.CurveBytes[0][(bit - 1U) / 8U]) >> ((bit - 1U) % 8U)) & 1U;
        swap ^= k;
        Swap(x2, x3, swap);
        Swap(z2, z3, swap);
        swap = k;
        Add(a, x2, z2);
        Mul(w, aa, a, a);
        Subtract(b, x2, z2);
        Mul(w, bb, b, b);
        Subtract(e, aa, bb);
        Add(c, x3, z3);
        Subtract(d, x3, z3);
        Mul(w, da, d, a);
        Mul(w, cb, c, b);
        Add(tmp, da, cb);
        Mul(w, x3, tmp, tmp);
        Subtract(tmp, da, cb);
        Mul(w, tmp, tmp, tmp);
        Mul(w, z3, x1, tmp);
        Mul(w, x2, aa, bb);
        Set(tmp, 121665U);
        Normalize(tmp);
        Mul(w, tmp, tmp, e);
        Add(tmp, aa, tmp);
        Mul(w, z2, e, tmp);
    }
    Swap(x2, x3, swap);
    Swap(z2, z3, swap);
    Power(w, tmp, z2, true);
    Mul(w, x2, x2, tmp);
    Encode(x2, w.CurveBytes[1]);
    const UInt32 zero = FieldZero(x2);
    if (zero != 0U)
    {
        return Result::AuthenticationFailed;
    }
    Copy(shared.Data, w.CurveBytes[1], 32U);
    return Result::Success;
}
Result Ed25519PublicKey(Workspace& w, Bytes seed, MutableBytes publicKey) noexcept
{
    if (!DisjointWork(w, seed) || !DisjointWork(w, Read(publicKey)))
    {
        return Result::InvalidArgument;
    }
    Cleanup cleanup{w};
    if (!CurveBuffers(w, seed, {nullptr, 0U}, publicKey))
    {
        return Result::InvalidArgument;
    }
    if (seed.Length != 32U || publicKey.Length != 32U)
    {
        return Result::BufferSize;
    }
    SeedExpand(w, seed);
    Base(w.Point[0]);
    Mul(w, w.Point[0].T, w.Point[0].X, w.Point[0].Y);
    ScalarPoint(w, w.Point[1], w.Point[0], w.CurveBytes[0]);
    PointEncode(w, w.Point[1], publicKey.Data);
    return Result::Success;
}
Result Ed25519Sign(Workspace& w, Bytes seed, Bytes message, MutableBytes signature) noexcept
{
    if (!DisjointWork(w, seed) || !DisjointWork(w, message) || !DisjointWork(w, Read(signature)))
    {
        return Result::InvalidArgument;
    }
    Cleanup cleanup{w};
    if (!CurveBuffers(w, seed, message, signature) || message.Length > MaximumHashBytes - 128U)
    {
        return Result::InvalidArgument;
    }
    if (seed.Length != 32U || signature.Length != 64U)
    {
        return Result::BufferSize;
    }
    SeedExpand(w, seed);
    Base(w.Point[0]);
    Mul(w, w.Point[0].T, w.Point[0].X, w.Point[0].Y);
    ScalarPoint(w, w.Point[1], w.Point[0], w.CurveBytes[0]);
    PointEncode(w, w.Point[1], w.CurveBytes[3]);
    HashStart(w.Hash, HashAlgorithm::Sha512);
    HashUpdate(w.Hash, HashAlgorithm::Sha512, {w.CurveBytes[0] + 32U, 32U});
    HashUpdate(w.Hash, HashAlgorithm::Sha512, message);
    HashFinish(w.Hash, HashAlgorithm::Sha512, w.CurveBytes[1]);
    ScalarReduce(w, w.CurveBytes[1], 64U, w.CurveBytes[1]);
    ScalarPoint(w, w.Point[1], w.Point[0], w.CurveBytes[1]);
    PointEncode(w, w.Point[1], signature.Data);
    HashStart(w.Hash, HashAlgorithm::Sha512);
    HashUpdate(w.Hash, HashAlgorithm::Sha512, {signature.Data, 32U});
    HashUpdate(w.Hash, HashAlgorithm::Sha512, {w.CurveBytes[3], 32U});
    HashUpdate(w.Hash, HashAlgorithm::Sha512, message);
    HashFinish(w.Hash, HashAlgorithm::Sha512, w.CurveBytes[2]);
    ScalarReduce(w, w.CurveBytes[2], 64U, w.CurveBytes[2]);
    ScalarMulAdd(w, w.CurveBytes[2], w.CurveBytes[0], w.CurveBytes[1], signature.Data + 32U);
    return Result::Success;
}
Result Ed25519Verify(Workspace& w, Bytes publicKey, Bytes message, Bytes signature) noexcept
{
    if (!DisjointWork(w, publicKey) || !DisjointWork(w, message) || !DisjointWork(w, signature))
    {
        return Result::InvalidArgument;
    }
    Cleanup cleanup{w};
    if (!Input(publicKey) || !Input(message) || !Input(signature) || message.Length > MaximumHashBytes - 128U)
    {
        return Result::InvalidArgument;
    }
    if (publicKey.Length != 32U || signature.Length != 64U)
    {
        return Result::BufferSize;
    }
    ScalarReduce(w, signature.Data + 32U, 32U, w.CurveBytes[1]);
    UInt32 valid = Equal({w.CurveBytes[1], 32U}, {signature.Data + 32U, 32U}) ? 1U : 0U;
    valid &= PointDecode(w, w.Point[0], publicKey.Data);
    valid &= PointDecode(w, w.Point[1], signature.Data);
    HashStart(w.Hash, HashAlgorithm::Sha512);
    HashUpdate(w.Hash, HashAlgorithm::Sha512, {signature.Data, 32U});
    HashUpdate(w.Hash, HashAlgorithm::Sha512, publicKey);
    HashUpdate(w.Hash, HashAlgorithm::Sha512, message);
    HashFinish(w.Hash, HashAlgorithm::Sha512, w.CurveBytes[2]);
    ScalarReduce(w, w.CurveBytes[2], 64U, w.CurveBytes[2]);
    ScalarPoint(w, w.Point[2], w.Point[0], w.CurveBytes[2]);
    PointAdd(w, w.Point[2], w.Point[1], w.Point[2]);
    Base(w.Point[0]);
    Mul(w, w.Point[0].T, w.Point[0].X, w.Point[0].Y);
    ScalarPoint(w, w.Point[1], w.Point[0], signature.Data + 32U);
    // Projective equality avoids inversions, checks the uncofactored equation.
    Mul(w, w.Field[10], w.Point[1].X, w.Point[2].Z);
    Mul(w, w.Field[11], w.Point[2].X, w.Point[1].Z);
    valid &= FieldEqual(w.Field[10], w.Field[11]);
    Mul(w, w.Field[10], w.Point[1].Y, w.Point[2].Z);
    Mul(w, w.Field[11], w.Point[2].Y, w.Point[1].Z);
    valid &= FieldEqual(w.Field[10], w.Field[11]);
    return valid != 0U ? Result::Success : Result::AuthenticationFailed;
}
} // namespace Asteria::User::Crypto
