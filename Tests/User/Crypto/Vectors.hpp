// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TEST_USER_CRYPTO_VECTORS_HPP_INCLUDED
#define ASTERIA_TEST_USER_CRYPTO_VECTORS_HPP_INCLUDED
#include "User/Crypto/Crypto.hpp"
#include "Support/TestContext.hpp"
#include <cstring>

namespace CryptoTest
{
using Asteria::User::Crypto::Bytes;
using Asteria::User::Crypto::Equal;
using Asteria::User::Crypto::Size;
using Asteria::User::Crypto::UInt8;
using Asteria::User::Crypto::Workspace;
inline Workspace Work{};
inline UInt8 Output[2048U]{};
inline UInt8 Expected[2048U]{};
inline constexpr Bytes Empty{nullptr, 0U};
struct Hex final
{
    UInt8 Data[2048U]{};
    Size Length{0U};
    explicit Hex(const char* text) noexcept
    {
        const auto nibble = [](char x) noexcept -> UInt8
        {
            return static_cast<UInt8>(x <= '9' ? x - '0' : x - 'a' + 10);
        };
        for (Size i = 0U; text[i] != '\0'; i += 2U)
        {
            Data[Length++] =
                static_cast<UInt8>((static_cast<unsigned int>(nibble(text[i])) << 4U) | nibble(text[i + 1U]));
        }
    }
    [[nodiscard]] Bytes View() const noexcept
    {
        return {Data, Length};
    }
};
[[nodiscard]] inline Bytes Text(const char* text) noexcept
{
    return {reinterpret_cast<const UInt8*>(text), std::strlen(text)};
}
[[nodiscard]] inline bool IsHex(const UInt8* output, Size n, const char* hex) noexcept
{
    const Hex expected{hex};
    return Equal({output, n}, expected.View());
}
[[nodiscard]] inline bool Cleared() noexcept
{
    const auto* bytes = reinterpret_cast<const UInt8*>(&Work);
    for (Size i = 0U; i < sizeof(Work); ++i)
    {
        if (bytes[i] != 0U)
        {
            return false;
        }
    }
    return true;
}
} // namespace CryptoTest
#endif
