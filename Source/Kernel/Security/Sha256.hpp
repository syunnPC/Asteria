// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SECURITY_SHA256_HPP_INCLUDED
#define ASTERIA_KERNEL_SECURITY_SHA256_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Security
{
inline constexpr Foundation::Size Sha256BlockBytes{64U};
inline constexpr Foundation::Size Sha256DigestBytes{32U};

/// SHA-256 (FIPS 180-4) over a byte stream. Update absorbs input incrementally; Finish pads the
/// message, writes its digest, and returns the object to its initial state, clearing the buffered
/// input. FIPS 180-4 limits a message to 2^64 - 1 bits; no caller approaches that bound. Execution:
/// - Callable in any context; no locking, no allocation, no firmware call. - Every call is bounded
/// by the number of bytes it receives. - Uses only 32-bit additions, rotations, and logical
/// operations (no multiplication, so no IA-64 floating-point register use).
class Sha256 final
{
public:
    constexpr Sha256() noexcept = default;

    /// Absorbs sizeBytes bytes. bytes may be null only when sizeBytes is zero.
    void Update(const Foundation::UInt8* bytes, Foundation::Size sizeBytes) noexcept;

    /// Writes the digest of every byte absorbed since construction or the
    /// previous Finish, then restarts the object.
    void Finish(Foundation::UInt8 (&digest)[Sha256DigestBytes]) noexcept;

private:
    void Compress(const Foundation::UInt8* block) noexcept;
    void Restart() noexcept;

    Foundation::UInt32 m_state[8U]{
        0x6A09'E667U, 0xBB67'AE85U, 0x3C6E'F372U, 0xA54F'F53AU, 0x510E'527FU, 0x9B05'688CU, 0x1F83'D9ABU, 0x5BE0'CD19U,
    };
    Foundation::UInt8 m_block[Sha256BlockBytes]{};
    Foundation::Size m_blockBytes{0U};
    Foundation::UInt64 m_messageBytes{0U};
};

/// HMAC-SHA256 (RFC 2104) with incremental message input. One object authenticates one message:
/// Finish ends it. It is also the HKDF-SHA256 building block (RFC 5869): HKDF-Extract is HMAC(salt,
/// input keying material), and the first HKDF-Expand block is HMAC(pseudorandom key, info || 0x01).
/// Execution: - Same as Sha256. Keys longer than one block are hashed first.
class HmacSha256 final
{
public:
    /// Keys the MAC. key may be null only when keyBytes is zero.
    HmacSha256(const Foundation::UInt8* key, Foundation::Size keyBytes) noexcept;

    HmacSha256(const HmacSha256&) = delete;
    HmacSha256& operator=(const HmacSha256&) = delete;

    void Update(const Foundation::UInt8* bytes, Foundation::Size sizeBytes) noexcept;

    /// Writes the tag of the message; the object must not be updated
    /// afterwards.
    void Finish(Foundation::UInt8 (&tag)[Sha256DigestBytes]) noexcept;

private:
    Sha256 m_inner{};
    Foundation::UInt8 m_outerPad[Sha256BlockBytes]{};
};

static_assert(sizeof(Sha256) <= 128U);
static_assert(sizeof(HmacSha256) <= 192U);
} // Namespace Asteria::Kernel::Security

#endif // ASTERIA_KERNEL_SECURITY_SHA256_HPP_INCLUDED
