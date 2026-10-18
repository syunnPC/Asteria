// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_SUPPORT_RANDOM_HPP_INCLUDED
#define ASTERIA_TESTS_SUPPORT_RANDOM_HPP_INCLUDED

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace Asteria::TestSupport
{
/// Deterministic pseudo-random generator for model-based tests (xoshiro256**
/// seeded through SplitMix64). The same seed always yields the same operation
/// sequence, so every failure is reproducible from its printed seed.
class Random final
{
public:
    explicit constexpr Random(const std::uint64_t seed) noexcept
    {
        std::uint64_t state = seed;
        for (std::uint64_t& word : m_state)
        {
            word = SplitMix64(state);
        }
    }

    [[nodiscard]] constexpr std::uint64_t Next() noexcept
    {
        const std::uint64_t result = RotateLeft(m_state[1] * 5U, 7U) * 9U;
        const std::uint64_t shifted = m_state[1] << 17U;
        m_state[2] ^= m_state[0];
        m_state[3] ^= m_state[1];
        m_state[1] ^= m_state[2];
        m_state[0] ^= m_state[3];
        m_state[2] ^= shifted;
        m_state[3] = RotateLeft(m_state[3], 45U);
        return result;
    }

    /// Returns a value uniformly distributed in [0, bound); bound must be nonzero.
    [[nodiscard]] constexpr std::uint64_t Below(const std::uint64_t bound) noexcept
    {
        const std::uint64_t limit = ~std::uint64_t{0U} - (~std::uint64_t{0U} % bound);
        std::uint64_t value = Next();
        while (value >= limit)
        {
            value = Next();
        }
        return value % bound;
    }

    /// Returns true with probability numerator/denominator.
    [[nodiscard]] constexpr bool Chance(const std::uint64_t numerator, const std::uint64_t denominator) noexcept
    {
        return Below(denominator) < numerator;
    }

private:
    [[nodiscard]] static constexpr std::uint64_t RotateLeft(const std::uint64_t value,
                                                            const unsigned int distance) noexcept
    {
        return (value << distance) | (value >> (64U - distance));
    }

    [[nodiscard]] static constexpr std::uint64_t SplitMix64(std::uint64_t& state) noexcept
    {
        state += 0x9E37'79B9'7F4A'7C15ULL;
        std::uint64_t value = state;
        value = (value ^ (value >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D0'49BB'1331'11EBULL;
        return value ^ (value >> 31U);
    }

    std::uint64_t m_state[4]{};
};

/// Seeds for a model-based test run: ASTERIA_TEST_SEED selects one seed to
/// reproduce a failure; otherwise the fixed seed range [first, first + count)
/// runs, so ordinary runs are deterministic.
struct SeedRange
{
    std::uint64_t First;
    std::uint64_t Count;
};

[[nodiscard]] inline SeedRange GetSeedRange(const std::uint64_t defaultFirst, const std::uint64_t defaultCount) noexcept
{
    const char* const requested = std::getenv("ASTERIA_TEST_SEED");
    if (requested == nullptr)
    {
        return SeedRange{defaultFirst, defaultCount};
    }
    char* end{nullptr};
    const unsigned long long seed = std::strtoull(requested, &end, 0);
    if ((end == requested) || (*end != '\0'))
    {
        static_cast<void>(std::fprintf(stderr, "ASTERIA_TEST_SEED is not a number: %s\n", requested));
        std::abort();
    }
    return SeedRange{static_cast<std::uint64_t>(seed), 1U};
}
} // namespace Asteria::TestSupport

#endif // ASTERIA_TESTS_SUPPORT_RANDOM_HPP_INCLUDED
