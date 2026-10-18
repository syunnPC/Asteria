// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_CRYPTO_INTERNAL_HPP_INCLUDED
#define ASTERIA_USER_CRYPTO_INTERNAL_HPP_INCLUDED
#include "User/Crypto/Crypto.hpp"

namespace Asteria::User::Crypto::Detail
{
// FIPS 180-4 §5.3.5; also RFC 7693 §2.6 Blake2b IV.
inline constexpr UInt64 Sha512IV[8U]{0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
                                     0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                                     0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};

[[nodiscard]] inline bool Valid(Bytes b) noexcept
{
    return b.Length == 0U || b.Data != nullptr;
}
[[nodiscard]] inline bool Valid(MutableBytes b) noexcept
{
    return b.Length == 0U || b.Data != nullptr;
}
[[nodiscard]] inline Bytes Read(MutableBytes b) noexcept
{
    return {b.Data, b.Length};
}
[[nodiscard]] inline UInt32 Mask(UInt32 bit) noexcept
{
    // An opaque volatile round trip prevents the optimizer recognizing a
    // secret 0/-1 select and replacing fixed-address reads with predicated
    // loads or a branch. Erase the temporary before returning the mask.
    volatile UInt32 opaque = 0U - bit;
    const UInt32 mask = opaque;
    opaque = 0U;
    return mask;
}
[[nodiscard]] inline UInt64 Mask64(UInt64 bit) noexcept
{
    volatile UInt64 opaque = 0ULL - bit;
    const UInt64 mask = opaque;
    opaque = 0U;
    return mask;
}
[[nodiscard]] inline UInt32 Zero(UInt32 x) noexcept
{
    return ((x | (0U - x)) >> 31U) ^ 1U;
}
[[nodiscard]] inline UInt32 Same(UInt32 x, UInt32 y) noexcept
{
    return Zero(x ^ y);
}
[[nodiscard]] inline UInt32 Load32(const UInt8* p) noexcept
{
    return static_cast<UInt32>(p[0]) | (static_cast<UInt32>(p[1]) << 8U) | (static_cast<UInt32>(p[2]) << 16U) |
           (static_cast<UInt32>(p[3]) << 24U);
}
[[nodiscard]] inline UInt64 Load64(const UInt8* p) noexcept
{
    return Load32(p) | (static_cast<UInt64>(Load32(p + 4U)) << 32U);
}
inline void Store32(UInt8* p, UInt32 x) noexcept
{
    for (Size i = 0; i < 4U; ++i)
    {
        p[i] = static_cast<UInt8>(x >> (8U * i));
    }
}
inline void Store64(UInt8* p, UInt64 x) noexcept
{
    for (Size i = 0; i < 8U; ++i)
    {
        p[i] = static_cast<UInt8>(x >> (8U * i));
    }
}
[[nodiscard]] inline UInt64 Ror(UInt64 x, UInt32 n) noexcept
{
    return (x >> n) | (x << (64U - n));
}
[[nodiscard]] inline UInt32 Rol(UInt32 x, UInt32 n) noexcept
{
    return (x << n) | (x >> (32U - n));
}
inline void Copy(UInt8* out, const UInt8* in, Size n) noexcept
{
    for (Size i = 0; i < n; ++i)
    {
        out[i] = in[i];
    }
}
inline void Clear(Workspace& w) noexcept
{
    Wipe({reinterpret_cast<UInt8*>(&w), sizeof(w)});
}
struct Cleanup final
{
    Workspace& Work;
    ~Cleanup() noexcept
    {
        Clear(Work);
    }
};
[[nodiscard]] inline Size DigestBytes(HashAlgorithm a) noexcept
{
    return a == HashAlgorithm::Sha256 ? 32U : 64U;
}
[[nodiscard]] inline bool Algorithm(HashAlgorithm a) noexcept
{
    return a == HashAlgorithm::Sha256 || a == HashAlgorithm::Sha512;
}
inline constexpr UInt64 MaximumHashBytes{(1ULL << 61U) - 1U};
[[nodiscard]] inline bool Input(Bytes b) noexcept
{
    return Valid(b) && b.Length <= MaximumHashBytes;
}
void HashStart(HashState& s, HashAlgorithm a) noexcept;
void HashUpdate(HashState& s, HashAlgorithm a, Bytes input) noexcept;
void HashFinish(HashState& s, HashAlgorithm a, UInt8* digest) noexcept;
void HmacStart(Workspace& w, HashAlgorithm a, Bytes key) noexcept;
void HmacFinish(Workspace& w, HashAlgorithm a, UInt8* tag) noexcept;
[[nodiscard]] bool Overlap(Bytes a, Bytes b) noexcept;
[[nodiscard]] bool DisjointWork(Workspace& w, Bytes b) noexcept;
void AesSetup(Workspace& w, Bytes key) noexcept;
void AesBlock(Workspace& w, Size keyBytes, const UInt8* input, UInt8* output) noexcept;
void ChaBlock(Workspace& w, Bytes key, Bytes nonce, UInt32 counter) noexcept;
void PolyStart(Workspace& w, const UInt8* key) noexcept;
void PolyBlocks(Workspace& w, Bytes input, bool padded) noexcept;
void PolyFinish(Workspace& w, UInt8* tag) noexcept;
} // namespace Asteria::User::Crypto::Detail
#endif
