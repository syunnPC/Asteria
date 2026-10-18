// SPDX-License-Identifier: Apache-2.0
// RFC 8032 point/signature decoding and RFC 7748 coordinate parsing. Invalid
// encodings and lengths must leave scratch clear and output unchanged.
#include "User/Crypto/Crypto.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
namespace
{
using Asteria::User::Crypto::Ed25519Verify;
using Asteria::User::Crypto::Result;
using Asteria::User::Crypto::Size;
using Asteria::User::Crypto::UInt8;
using Asteria::User::Crypto::Workspace;
using Asteria::User::Crypto::X25519;
Workspace Work{};
UInt8 Output[32U]{};
UInt8 Seed[129U]{};
void Require(bool x) noexcept
{
    if (!x)
    {
        std::abort();
    }
}
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 33U || size > sizeof(Seed))
    {
        return 0;
    }
    Result result = Result::InvalidArgument;
    if (data[0] == 0U && size >= 33U)
    {
        const Size signatureBytes = size - 33U < 64U ? size - 33U : 64U;
        const Size messageBytes = size - 33U - signatureBytes;
        result = Ed25519Verify(Work, {data + 1U, 32U}, {data + 33U + signatureBytes, messageBytes},
                               {data + 33U, signatureBytes});
        Require(result == Result::Success || result == Result::AuthenticationFailed || result == Result::BufferSize);
    }
    else if (data[0] != 0U && size >= 33U)
    {
        std::memset(Output, 0xa5, sizeof(Output));
        result = X25519(Work, {data + 1U, 32U}, {data + 33U, size - 33U}, {Output, 32U});
        if (result != Result::Success)
        {
            for (auto x : Output)
            {
                Require(x == 0xa5U);
            }
        }
    }
    const auto* p = reinterpret_cast<const UInt8*>(&Work);
    for (Size i = 0U; i < sizeof(Work); ++i)
    {
        Require(p[i] == 0U);
    }
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*emit)(const std::uint8_t*, std::size_t))
{
    // Ed25519 RFC 8032 §7.1 test 1, empty message.
    const UInt8 key[32U]{0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe,
                         0xd3, 0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6,
                         0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};
    const UInt8 signature[64U]{0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72, 0x90, 0x86, 0xe2, 0xcc, 0x80,
                               0x6e, 0x82, 0x8a, 0x84, 0x87, 0x7f, 0x1e, 0xb8, 0xe5, 0xd9, 0x74, 0xd8, 0x73,
                               0xe0, 0x65, 0x22, 0x49, 0x01, 0x55, 0x5f, 0xb8, 0x82, 0x15, 0x90, 0xa3, 0x3b,
                               0xac, 0xc6, 0x1e, 0x39, 0x70, 0x1c, 0xf9, 0xb4, 0x6b, 0xd2, 0x5b, 0xf5, 0xf0,
                               0x59, 0x5b, 0xbe, 0x24, 0x65, 0x51, 0x41, 0x43, 0x8e, 0x7a, 0x10, 0x0b};
    std::memcpy(Seed + 1U, key, 32U);
    std::memcpy(Seed + 33U, signature, 64U);
    emit(Seed, 97U);
    Seed[33] ^= 1U;
    emit(Seed, 97U);
    emit(Seed, 33U);
    Seed[0] = 1U;
    Seed[33] = 9U;
    std::memset(Seed + 34U, 0, 31U);
    emit(Seed, 65U);
}
