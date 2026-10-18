// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_GUEST_TAP_HPP_INCLUDED
#define ASTERIA_TESTS_GUEST_TAP_HPP_INCLUDED

// TAP 14 writer for the freestanding guest test program. Each line is built
// in a fixed buffer and handed to the writer whole, ending with LF, so every
// line reaches the host as one DebugWrite call. Text is reduced to printable
// ASCII because the kernel rejects anything else. Numbers are formatted with
// shifts, comparisons and subtraction only: IA-64 multiplies and divides in
// the floating-point register file, which user code may not use (the kernel
// launches it with that file disabled).

#include "Abi/SystemCallAbi.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Guest
{
class Tap final
{
public:
    /// Writes one complete line (the text includes its trailing LF).
    using Writer = bool (*)(void* context, const char* text, Foundation::Size sizeBytes) noexcept;

    constexpr Tap(const Writer writer, void* const context) noexcept : m_writer{writer}, m_context{context}
    {
    }

    void Begin() noexcept
    {
        Append("TAP version 14");
        Flush();
    }

    /// Records one test point; returns `ok`.
    bool Check(const bool ok, const char* const description) noexcept
    {
        ++m_number;
        if (!ok)
        {
            ++m_failures;
            Append("not ");
        }
        Append("ok ");
        AppendDecimal(m_number);
        Append(" - ");
        Append(description);
        Flush();
        return ok;
    }

    /// Records one test point comparing a value; a mismatch adds a diagnostic.
    bool Equal(const char* const description, const Foundation::UInt64 expected,
               const Foundation::UInt64 observed) noexcept
    {
        if (Check(expected == observed, description))
        {
            return true;
        }
        Detail("value", expected, observed);
        return false;
    }

    /// A diagnostic line for the preceding test point.
    void Detail(const char* const name, const Foundation::UInt64 expected, const Foundation::UInt64 observed) noexcept
    {
        Append("# ");
        Append(name);
        Append(": expected ");
        AppendHex(expected);
        Append(" observed ");
        AppendHex(observed);
        Flush();
    }

    void Skip(const char* const description, const char* const reason) noexcept
    {
        ++m_number;
        Append("ok ");
        AppendDecimal(m_number);
        Append(" - ");
        Append(description);
        Append(" # SKIP ");
        Append(reason);
        Flush();
    }

    void Note(const char* const text) noexcept
    {
        Append("# ");
        Append(text);
        Flush();
    }

    /// A diagnostic line stating one capability handle the image received:
    /// `# handle.<name>: selector 0x<16 hex> authenticator 0x<16 hex>`.
    void Handle(const char* const name, const Foundation::UInt64 selector,
                const Foundation::UInt64 authenticator) noexcept
    {
        Append("# handle.");
        Append(name);
        Append(": selector ");
        AppendHex(selector);
        Append(" authenticator ");
        AppendHex(authenticator);
        Flush();
    }

    void End() noexcept
    {
        Append("1..");
        AppendDecimal(m_number);
        Flush();
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetFailures() const noexcept
    {
        return m_failures + m_lostLines;
    }

private:
    static constexpr Foundation::Size Capacity{Abi::DebugWriteMaximumBytes - 1U}; // plus the LF
    static constexpr Foundation::UInt64 PowersOfTen[20]{
        10'000'000'000'000'000'000ULL,
        1'000'000'000'000'000'000ULL,
        100'000'000'000'000'000ULL,
        10'000'000'000'000'000ULL,
        1'000'000'000'000'000ULL,
        100'000'000'000'000ULL,
        10'000'000'000'000ULL,
        1'000'000'000'000ULL,
        100'000'000'000ULL,
        10'000'000'000ULL,
        1'000'000'000ULL,
        100'000'000ULL,
        10'000'000ULL,
        1'000'000ULL,
        100'000ULL,
        10'000ULL,
        1'000ULL,
        100ULL,
        10ULL,
        1ULL,
    };

    void Append(const char* text) noexcept
    {
        for (; (*text != '\0') && (m_length < Capacity); ++text)
        {
            const char character = *text;
            m_line[m_length++] = ((character >= ' ') && (character <= '~')) ? character : '?';
        }
    }

    void AppendDecimal(Foundation::UInt64 value) noexcept
    {
        bool leading{true};
        for (const Foundation::UInt64 power : PowersOfTen)
        {
            char digit{'0'};
            while (value >= power)
            {
                value -= power;
                ++digit;
            }
            if ((digit != '0') || !leading || (power == 1U))
            {
                leading = false;
                AppendCharacter(digit);
            }
        }
    }

    void AppendCharacter(const char character) noexcept
    {
        if (m_length < Capacity)
        {
            m_line[m_length++] = character;
        }
    }

    void AppendHex(const Foundation::UInt64 value) noexcept
    {
        Append("0x");
        for (Foundation::UInt64 shift{64U}; shift != 0U;)
        {
            shift -= 4U;
            AppendCharacter("0123456789abcdef"[(value >> shift) & 0xFU]);
        }
    }

    void Flush() noexcept
    {
        m_line[m_length++] = '\n';
        if (!m_writer(m_context, m_line, m_length))
        {
            ++m_lostLines;
        }
        m_length = 0U;
    }

    Writer m_writer;
    void* m_context;
    char m_line[Capacity + 1U]{};
    Foundation::Size m_length{0U};
    Foundation::UInt64 m_number{0U};
    Foundation::UInt64 m_failures{0U};
    Foundation::UInt64 m_lostLines{0U};
};
} // namespace Asteria::Guest

#endif // ASTERIA_TESTS_GUEST_TAP_HPP_INCLUDED
