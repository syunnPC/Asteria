// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_CRYPTO_CRYPTO_HPP_INCLUDED
#define ASTERIA_USER_CRYPTO_CRYPTO_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Crypto/Sha256.hpp"

namespace Asteria::User::Crypto
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;

// Bounded cryptographic primitives and authentication profiles.
enum class Result : UInt8
{
    Success,
    InvalidArgument,
    BufferSize,
    AuthenticationFailed,
    ReseedRequired,
    NotInitialized
};
enum class HashAlgorithm : UInt8
{
    Sha256,
    Sha512
};
enum class AeadAlgorithm : UInt8
{
    AesGcm,
    ChaCha20Poly1305
};
struct Bytes
{
    const UInt8* Data;
    Size Length;
};
struct MutableBytes
{
    UInt8* Data;
    Size Length;
};
inline constexpr Size MaximumAeadBytes{16U * 1024U * 1024U};
inline constexpr Size MaximumArgonBlocks{65536U};
inline constexpr Size MaximumDrbgRequest{65536U};

namespace Detail
{
struct Sha512State
{
    UInt64 State[8U];
    UInt8 Block[128U];
    Size Used;
    UInt64 Total;
};
struct HashState
{
    Crypto::Detail::Sha256 Sha256{};
    Sha512State Sha512{};
};
struct Field
{
    UInt32 Limb[16U];
};
struct Point
{
    Field X;
    Field Y;
    Field Z;
    Field T;
};
struct BlakeState
{
    UInt64 H[8U];
    UInt64 V[16U];
    UInt64 M[16U];
    UInt8 Block[128U];
    Size Used;
    UInt64 Total;
    Size Output;
};
} // Namespace Detail
struct Argon2Block
{
    UInt64 Word[128U];
};
static_assert(sizeof(Argon2Block) == 128U * sizeof(UInt64));
static_assert(sizeof(Argon2Block) == 1024U);
static_assert(sizeof(Detail::Field) == 16U * sizeof(UInt32));
static_assert(sizeof(Detail::Point) == 4U * sizeof(Detail::Field));
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);

/// Exclusive caller-owned scratch, kept off the target stack. Zero initialized
/// before first use. Operations clear it before returning, except rejection
/// of buffers overlapping scratch (which precedes any reads/writes).
struct Workspace final
{
    Detail::HashState Hash{};
    Detail::HashState Outer{};
    UInt8 Pad[128U]{};
    UInt8 Digest[64U]{};
    UInt8 Temporary[64U]{};
    UInt8 Expansion[64U]{};
    UInt8 AesKey[240U]{};
    UInt8 AesState[16U]{};
    UInt8 AesTemp[16U]{};
    UInt8 Gcm[6U][16U]{};
    UInt32 Cha[32U]{};
    UInt8 ChaBlock[64U]{};
    UInt8 PolyKey[32U]{};
    UInt32 PolyR[5U]{};
    UInt32 PolyH[5U]{};
    UInt32 PolyS[4U]{};
    UInt64 PolyProduct[5U]{};
    Detail::Field Field[24U]{};
    Detail::Point Point[6U]{};
    UInt64 Product[31U]{};
    UInt32 Scalar[16U]{};
    UInt32 ScalarTemp[16U]{};
    UInt8 CurveBytes[5U][64U]{};
    Detail::BlakeState Blake{};
    Argon2Block Argon[6U]{};
    UInt8 ArgonSeed[72U]{};
    UInt8 ArgonHash[64U]{};
};

static_assert(sizeof(Workspace) <= 16U * 1024U);

struct Argon2Parameters
{
    UInt32 MemoryKiB;
    UInt32 Passes;
    UInt32 Lanes;
};

// RFC 9106 §4 item 2 / §7.4, second option.
// The login consumer provides the memory and a fresh salt; no service wiring.
inline constexpr Argon2Parameters LoginArgon2Defaults{64U * 1024U, 3U, 4U};
inline constexpr Size LoginArgon2SaltBytes{128U / 8U};
inline constexpr Size LoginArgon2TagBytes{256U / 8U};
static_assert(LoginArgon2Defaults.MemoryKiB <= MaximumArgonBlocks);
static_assert(LoginArgon2Defaults.MemoryKiB % (4U * LoginArgon2Defaults.Lanes) == 0U);

/// Utilities: Wipe requires a valid buffer; Equal accepts null only at length 0.
void Wipe(MutableBytes bytes) noexcept;
[[nodiscard]] bool Equal(Bytes left, Bytes right) noexcept;
[[nodiscard]] Result Hash(Workspace& work, HashAlgorithm algorithm, Bytes input, MutableBytes digest) noexcept;
[[nodiscard]] Result Hmac(Workspace& work, HashAlgorithm algorithm, Bytes key, Bytes message,
                          MutableBytes tag) noexcept;
[[nodiscard]] Result HkdfExtract(Workspace& work, HashAlgorithm algorithm, Bytes salt, Bytes input,
                                 MutableBytes key) noexcept;
[[nodiscard]] Result HkdfExpand(Workspace& work, HashAlgorithm algorithm, Bytes key, Bytes info,
                                MutableBytes output) noexcept;
[[nodiscard]] Result AesEncrypt(Workspace& work, Bytes key, Bytes block, MutableBytes output) noexcept;
[[nodiscard]] Result ChaCha20(Workspace& work, Bytes key, Bytes nonce, UInt32 counter, Bytes input,
                              MutableBytes output) noexcept;
[[nodiscard]] Result Poly1305(Workspace& work, Bytes key, Bytes input, MutableBytes tag) noexcept;
/// Nonces are exactly 12 bytes; tags exactly 16. Caller guarantees nonce
/// uniqueness per key. Payload may be exactly in-place; partial overlap fails.
[[nodiscard]] Result Seal(Workspace& work, AeadAlgorithm algorithm, Bytes key, Bytes nonce, Bytes aad, Bytes plaintext,
                          MutableBytes ciphertext, MutableBytes tag) noexcept;
/// On every failure, plaintext is unchanged. Tag verification precedes writes.
[[nodiscard]] Result Open(Workspace& work, AeadAlgorithm algorithm, Bytes key, Bytes nonce, Bytes aad, Bytes ciphertext,
                          Bytes tag, MutableBytes plaintext) noexcept;
[[nodiscard]] Result X25519(Workspace& work, Bytes scalar, Bytes coordinate, MutableBytes shared) noexcept;
[[nodiscard]] Result Ed25519PublicKey(Workspace& work, Bytes seed, MutableBytes publicKey) noexcept;
[[nodiscard]] Result Ed25519Sign(Workspace& work, Bytes seed, Bytes message, MutableBytes signature) noexcept;
[[nodiscard]] Result Ed25519Verify(Workspace& work, Bytes publicKey, Bytes message, Bytes signature) noexcept;
/// Memory is exclusive and disjoint, counted in 1024-byte blocks. The used rounded memory cost is
/// wiped on success; refusal preserves caller memory. RFC 9106 §3.4.1.3 hybrid addressing uses
/// data-dependent memory accesses; see Documentation/Architecture/Crypto.md.
// Service only, at public segment boundaries (RFC 9106 section 3.4). No credential or hash state
// reaches the callback.
struct Argon2Progress
{
    void* Context{nullptr};
    void (*Poll)(void*) noexcept {nullptr};
};
[[nodiscard]] Result Argon2id(Workspace& work, Argon2Parameters parameters, Bytes password, Bytes salt, Bytes secret,
                              Bytes associatedData, Argon2Block* memory, Size memoryBlocks, MutableBytes tag,
                              Argon2Progress progress = {}) noexcept;

/// Caller serializes access and supplies credited entropy. Reseed immediately
/// before Generate when prediction resistance is required (SP 800-90A §9.3).
class HmacDrbg final
{
public:
    constexpr HmacDrbg() noexcept = default;
    ~HmacDrbg() noexcept
    {
        Clear();
    }
    HmacDrbg(const HmacDrbg&) = delete;
    HmacDrbg& operator=(const HmacDrbg&) = delete;
    [[nodiscard]] Result Instantiate(Workspace& work, Bytes entropy, Bytes nonce, Bytes personalization) noexcept;
    [[nodiscard]] Result Reseed(Workspace& work, Bytes entropy, Bytes additional) noexcept;
    [[nodiscard]] Result Generate(Workspace& work, Bytes additional, MutableBytes output) noexcept;
    void Clear() noexcept;

private:
    void Update(Workspace& work, Bytes first, Bytes second, Bytes third) noexcept;
    UInt8 m_key[32U]{};
    UInt8 m_value[32U]{};
    UInt64 m_reseedCounter{0U};
};
} // Namespace Asteria::User::Crypto
#endif
