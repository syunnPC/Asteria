// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_SUPPORT_TEST_CONTEXT_HPP_INCLUDED
#define ASTERIA_TESTS_SUPPORT_TEST_CONTEXT_HPP_INCLUDED

#include <cstddef>
#include <cstdio>
#include <type_traits>

#include "Support/TestExpansion.hpp"

namespace Asteria::TestSupport
{
/// Records the expectations of one hosted test executable.
///
/// Tests normally use the macros below rather than calling these members.
/// ASTERIA_EXPECT evaluates its terms left to right with the short-circuit
/// semantics of `&&`, so a term that the scenario guards with an earlier term
/// is never evaluated after that guard fails. A failure reports the source
/// location, the scenario description, and the first false term.
///
/// The context is single-threaded: concurrent tests record results after
/// joining their worker threads. It performs no allocation.
class TestContext final
{
public:
    explicit TestContext(const char* const suiteName) noexcept : m_suiteName{suiteName}
    {
    }

    TestContext(const TestContext&) = delete;
    TestContext& operator=(const TestContext&) = delete;
    TestContext(TestContext&&) = delete;
    TestContext& operator=(TestContext&&) = delete;

    /// Clears the first-false-term record; returns true to start a term chain.
    [[nodiscard]] bool BeginExpectation() noexcept
    {
        m_failedTermText = nullptr;
        m_failedTermIndex = 0;
        return true;
    }

    /// Records one term of the current expectation and returns its value.
    [[nodiscard]] bool Term(const bool value, const char* const text, const int index) noexcept
    {
        if (!value && (m_failedTermText == nullptr))
        {
            m_failedTermText = text;
            m_failedTermIndex = index;
        }
        return value;
    }

    /// Records one expectation and returns its result, so a caller may guard
    /// dependent expectations with it.
    bool Expect(const bool result, const char* const description, const char* const file, const int line,
                const int termCount) noexcept
    {
        ++m_expectationCount;
        if (!result)
        {
            ++m_failureCount;
            static_cast<void>(std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, description));
            if (m_failedTermText != nullptr)
            {
                static_cast<void>(std::fprintf(stderr, "  term %d of %d is false: %s\n", m_failedTermIndex, termCount,
                                               m_failedTermText));
            }
        }
        m_failedTermText = nullptr;
        m_failedTermIndex = 0;
        return result;
    }

    /// Records an equality expectation and prints both operands on failure.
    template <typename TActual, typename TExpected>
    bool ExpectEqual(const TActual& actual, const TExpected& expected, const char* const actualText,
                     const char* const expectedText, const char* const file, const int line) noexcept
    {
        const bool equal = actual == expected;
        ++m_expectationCount;
        if (!equal)
        {
            ++m_failureCount;
            static_cast<void>(
                std::fprintf(stderr, "FAIL %s:%d: %s == %s\n  actual:   ", file, line, actualText, expectedText));
            PrintValue(actual);
            static_cast<void>(std::fprintf(stderr, "\n  expected: "));
            PrintValue(expected);
            static_cast<void>(std::fprintf(stderr, "\n"));
        }
        return equal;
    }

    /// Records a failure that has no boolean expression, such as an
    /// unexpected fixture state.
    void Fail(const char* const description, const char* const file, const int line) noexcept
    {
        static_cast<void>(Expect(false, description, file, line, 0));
    }

    [[nodiscard]] std::size_t GetFailureCount() const noexcept
    {
        return m_failureCount;
    }

    /// Prints the summary and returns the process exit status.
    [[nodiscard]] int Finish() const noexcept
    {
        return Finish(nullptr);
    }

    /// Prints the summary for one named variant of a multi-process suite, such
    /// as a subprocess mode selected by argv, and returns the exit status.
    [[nodiscard]] int Finish(const char* const variant) const noexcept
    {
        const char* const separator = (variant != nullptr) ? " " : "";
        const char* const name = (variant != nullptr) ? variant : "";
        if (m_failureCount == 0U)
        {
            static_cast<void>(std::fprintf(stdout, "PASS: %s%s%s: %zu expectations\n", m_suiteName, separator, name,
                                           m_expectationCount));
            return 0;
        }

        static_cast<void>(std::fprintf(stderr, "FAIL: %s%s%s: %zu of %zu expectations failed\n", m_suiteName, separator,
                                       name, m_failureCount, m_expectationCount));
        return 1;
    }

private:
    template <typename TValue> static void PrintValue(const TValue& value) noexcept
    {
        if constexpr (std::is_same_v<TValue, bool>)
        {
            static_cast<void>(std::fprintf(stderr, "%s", value ? "true" : "false"));
        }
        else if constexpr (std::is_enum_v<TValue>)
        {
            PrintValue(static_cast<std::underlying_type_t<TValue>>(value));
        }
        else if constexpr (std::is_integral_v<TValue> && std::is_signed_v<TValue>)
        {
            static_cast<void>(std::fprintf(stderr, "%lld", static_cast<long long>(value)));
        }
        else if constexpr (std::is_integral_v<TValue>)
        {
            static_cast<void>(std::fprintf(stderr, "%llu (0x%llx)", static_cast<unsigned long long>(value),
                                           static_cast<unsigned long long>(value)));
        }
        else if constexpr (std::is_pointer_v<TValue>)
        {
            static_cast<void>(std::fprintf(stderr, "%p", static_cast<const volatile void*>(value)));
        }
        else
        {
            static_cast<void>(value);
            static_cast<void>(std::fprintf(stderr, "<unprintable>"));
        }
    }

    const char* m_suiteName;
    std::size_t m_expectationCount{0U};
    std::size_t m_failureCount{0U};
    const char* m_failedTermText{nullptr};
    int m_failedTermIndex{0};
};
} // namespace Asteria::TestSupport

/// Records one expectation. Each variadic argument is one term; the terms are
/// evaluated left to right and evaluation stops at the first false term.
/// Wrap a term that contains a top-level comma in parentheses.
#define ASTERIA_EXPECT(context, description, ...)                                                                      \
    (context).Expect((context).BeginExpectation() && ASTERIA_TEST_DETAIL_TERMS(context, __VA_ARGS__), (description),   \
                     __FILE__, __LINE__, ASTERIA_TEST_DETAIL_COUNT(__VA_ARGS__))

/// Records one boolean expectation described by its own expression text.
#define ASTERIA_CHECK(context, expression) ASTERIA_EXPECT(context, #expression, expression)

/// Records an equality expectation and prints both operands on failure.
#define ASTERIA_CHECK_EQ(context, actual, expected)                                                                    \
    (context).ExpectEqual((actual), (expected), #actual, #expected, __FILE__, __LINE__)

/// Records an unconditional failure.
#define ASTERIA_FAIL(context, description) (context).Fail((description), __FILE__, __LINE__)

#endif // ASTERIA_TESTS_SUPPORT_TEST_CONTEXT_HPP_INCLUDED
