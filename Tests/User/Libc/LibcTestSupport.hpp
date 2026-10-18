// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TEST_LIBC_SUPPORT_HPP_INCLUDED
#define ASTERIA_TEST_LIBC_SUPPORT_HPP_INCLUDED

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstdio>

#include "Support/TestContext.hpp"

namespace Asteria::Test::Libc
{
using TestSupport::TestContext;
class Random final
{
public:
    Random() noexcept
    {
        const char* const replay = std::getenv("ASTERIA_TEST_SEED");
        if (replay != nullptr)
        {
            m_state = std::strtoull(replay, nullptr, 0);
        }
        if (m_state == 0U)
        {
            m_state = 1U;
        }
        static_cast<void>(std::printf("libc seed=%llu\n", static_cast<unsigned long long>(m_state)));
    }
    [[nodiscard]] std::uint64_t Next() noexcept
    {
        // Reproducible xorshift64 with intentional unsigned wraparound.
        m_state ^= m_state << 13U;
        m_state ^= m_state >> 7U;
        m_state ^= m_state << 17U;
        return m_state;
    }

private:
    std::uint64_t m_state{0xA57E21A123456789U};
};
} // namespace Asteria::Test::Libc
#endif
