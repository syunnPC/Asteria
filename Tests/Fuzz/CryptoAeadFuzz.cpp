// SPDX-License-Identifier: Apache-2.0
// Raw AEAD buffers, not a persisted format. Length fields exercise refusal;
// successful opens must reseal identically; failures must preserve output.
#include "User/Crypto/Crypto.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
namespace
{
using Asteria::User::Crypto::AeadAlgorithm;
using Asteria::User::Crypto::Bytes;
using Asteria::User::Crypto::Equal;
using Asteria::User::Crypto::Open;
using Asteria::User::Crypto::Result;
using Asteria::User::Crypto::Seal;
using Asteria::User::Crypto::Size;
using Asteria::User::Crypto::UInt8;
using Asteria::User::Crypto::Workspace;
Workspace Work{};
UInt8 Plain[256U]{};
UInt8 Cipher[256U]{};
UInt8 Tag[16U]{};
UInt8 Seed[128U]{};
void Require(bool condition) noexcept
{
    if (!condition)
    {
        std::abort();
    }
}
[[nodiscard]] bool Cleared() noexcept
{
    const auto* p = reinterpret_cast<const UInt8*>(&Work);
    for (Size i = 0U; i < sizeof(Work); ++i)
    {
        if (p[i] != 0U)
        {
            return false;
        }
    }
    return true;
}
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 5U || size > 512U)
    {
        return 0;
    }
    const Size keyBytes = data[1];
    const Size nonceBytes = data[2];
    const Size aadBytes = data[3];
    const Size tagBytes = data[4];
    const Size header = 5U + keyBytes + nonceBytes + aadBytes + tagBytes;
    if (size < header || size - header > sizeof(Plain))
    {
        return 0;
    }
    const AeadAlgorithm algorithm = data[0] == 0U ? AeadAlgorithm::AesGcm : AeadAlgorithm::ChaCha20Poly1305;
    const Bytes key{data + 5U, keyBytes};
    const Bytes nonce{data + 5U + keyBytes, nonceBytes};
    const Bytes aad{data + 5U + keyBytes + nonceBytes, aadBytes};
    const Bytes tag{data + header - tagBytes, tagBytes};
    const Bytes cipher{data + header, size - header};
    std::memset(Plain, 0xa5, sizeof(Plain));
    const Result result = Open(Work, algorithm, key, nonce, aad, cipher, tag, {Plain, cipher.Length});
    Require(Cleared());
    if (result != Result::Success)
    {
        for (auto b : Plain)
        {
            Require(b == 0xa5U);
        }
    }
    else
    {
        Require(Seal(Work, algorithm, key, nonce, aad, {Plain, cipher.Length}, {Cipher, cipher.Length}, {Tag, 16U}) ==
                Result::Success);
        Require(Equal({Cipher, cipher.Length}, cipher) && Equal({Tag, 16U}, tag));
        Tag[0] ^= 1U;
        std::memset(Plain, 0xa5, sizeof(Plain));
        Require(Open(Work, algorithm, key, nonce, aad, cipher, {Tag, 16U}, {Plain, cipher.Length}) ==
                Result::AuthenticationFailed);
        for (auto b : Plain)
        {
            Require(b == 0xa5U);
        }
    }
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*emit)(const std::uint8_t*, std::size_t))
{
    for (UInt8 algorithm = 0U; algorithm < 2U; ++algorithm)
    {
        std::memset(Seed, 0, sizeof(Seed));
        Seed[0] = algorithm;
        Seed[1] = 32U;
        Seed[2] = 12U;
        Seed[3] = 7U;
        Seed[4] = 16U;
        const Bytes key{Seed + 5U, 32U};
        const Bytes nonce{Seed + 37U, 12U};
        const Bytes aad{Seed + 49U, 7U};
        Require(Seal(Work, algorithm == 0U ? AeadAlgorithm::AesGcm : AeadAlgorithm::ChaCha20Poly1305, key, nonce, aad,
                     {Plain, 33U}, {Seed + 72U, 33U}, {Seed + 56U, 16U}) == Result::Success);
        emit(Seed, 105U);
        Seed[56] ^= 1U;
        emit(Seed, 105U);
        emit(Seed, 4U);
    }
}
