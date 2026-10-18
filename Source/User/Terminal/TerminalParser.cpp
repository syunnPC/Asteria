// SPDX-License-Identifier: Apache-2.0

#include "User/Terminal/TerminalParser.hpp"

#include "User/Text/Utf8.hpp"

namespace Asteria::User::Terminal
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt8;

constexpr UInt8 Bell{0x07U};
constexpr UInt8 BackspaceCode{0x08U};
constexpr UInt8 TabCode{0x09U};
constexpr UInt8 LineFeedCode{0x0AU};
constexpr UInt8 VerticalTabCode{0x0BU};
constexpr UInt8 FormFeedCode{0x0CU};
constexpr UInt8 CarriageReturnCode{0x0DU};
constexpr UInt8 Cancel{0x18U};
constexpr UInt8 Substitute{0x1AU};
constexpr UInt8 EscapeCode{0x1BU};
constexpr UInt8 Delete{0x7FU};
constexpr UInt8 StringTerminatorFinal{0x5CU};

/// Executes a C0 control: the movements of the subset; the others (BEL
/// among them) do nothing.
void Execute(const UInt8 byte, TerminalScreen& screen) noexcept
{
    switch (byte)
    {
    case BackspaceCode:
        screen.Backspace();
        break;
    case TabCode:
        screen.HorizontalTab();
        break;
    case LineFeedCode:
    case VerticalTabCode:
    case FormFeedCode:
        screen.LineFeed();
        break;
    case CarriageReturnCode:
        screen.CarriageReturn();
        break;
    default:
        break;
    }
}

/// Writes value in decimal at out[at...]; returns the new position. The
/// values are at most TerminalMaximumRows or TerminalMaximumColumns.
[[nodiscard]] Size AppendDecimal(UInt8* const out, Size at, const UInt32 value) noexcept
{
    UInt32 hundreds{0U};
    UInt32 rest{value};
    while ((rest >= 100U) && (hundreds < 9U))
    {
        rest -= 100U;
        ++hundreds;
    }
    UInt32 tens{0U};
    while (rest >= 10U)
    {
        rest -= 10U;
        ++tens;
    }
    if (hundreds != 0U)
    {
        out[at++] = static_cast<UInt8>('0' + hundreds);
    }
    if ((hundreds != 0U) || (tens != 0U))
    {
        out[at++] = static_cast<UInt8>('0' + tens);
    }
    out[at++] = static_cast<UInt8>('0' + rest);
    return at;
}
} // namespace

void TerminalResponses::Clear() noexcept
{
    m_count = 0U;
}

void TerminalResponses::Append(const UInt8* const bytes, const Size count) noexcept
{
    if ((bytes == nullptr) || (count > (TerminalResponseCapacity - m_count)))
    {
        ++m_dropped;
        return;
    }
    for (Size index{0U}; index < count; ++index)
    {
        m_bytes[m_count + index] = bytes[index];
    }
    m_count += count;
}

Size TerminalResponses::Take(UInt8* const out, const Size capacity) noexcept
{
    if (out == nullptr)
    {
        return 0U;
    }
    const Size taken{(capacity < m_count) ? capacity : m_count};
    for (Size index{0U}; index < taken; ++index)
    {
        out[index] = m_bytes[index];
    }
    for (Size index{taken}; index < m_count; ++index)
    {
        m_bytes[index - taken] = m_bytes[index];
    }
    m_count -= taken;
    return taken;
}

Size TerminalResponses::Pending() const noexcept
{
    return m_count;
}

Foundation::UInt64 TerminalResponses::Dropped() const noexcept
{
    return m_dropped;
}

void TerminalParser::Reset() noexcept
{
    m_state = State::Ground;
    m_private = 0U;
    m_count = 0U;
    m_remaining = 0U;
    m_codePoint = 0U;
}

void TerminalParser::Feed(const UInt8 byte, TerminalScreen& screen, TerminalResponses& responses) noexcept
{
    if ((m_state != State::Ground) && ((byte == Cancel) || (byte == Substitute)))
    {
        m_state = State::Ground;
        return;
    }
    switch (m_state)
    {
    case State::Ground:
        FeedGround(byte, screen);
        break;
    case State::Escape:
        FeedEscape(byte, screen);
        break;
    case State::EscapeIntermediate:
        FeedEscapeIntermediate(byte, screen);
        break;
    case State::CsiEntry:
    case State::CsiParameter:
        FeedCsi(byte, screen, responses);
        break;
    case State::CsiIntermediate:
        FeedCsiIntermediate(byte, screen);
        break;
    case State::CsiIgnore:
        FeedCsiIgnore(byte, screen);
        break;
    case State::OscString:
    case State::IgnoredString:
        FeedString(byte);
        break;
    case State::StringEscape:
        FeedStringEscape(byte, screen);
        break;
    }
}

void TerminalParser::FeedGround(const UInt8 byte, TerminalScreen& screen) noexcept
{
    if (m_remaining != 0U)
    {
        if ((byte >= m_lower) && (byte <= m_upper))
        {
            m_codePoint = (m_codePoint << 6U) | (byte & 0x3FU);
            m_lower = 0x80U;
            m_upper = 0xBFU;
            --m_remaining;
            if (m_remaining == 0U)
            {
                PrintCodePoint(m_codePoint, screen);
            }
            return;
        }
        // Cut short: one replacement for the partial character, then the
        // interrupting byte as if no character had been pending.
        m_remaining = 0U;
        screen.Print(Text::ReplacementCodePoint);
    }
    if (byte == EscapeCode)
    {
        m_state = State::Escape;
        return;
    }
    if (byte < 0x20U)
    {
        Execute(byte, screen);
        return;
    }
    if (byte < Delete)
    {
        screen.Print(byte);
        return;
    }
    if (byte == Delete)
    {
        return;
    }
    StartUtf8(byte, screen);
}

void TerminalParser::StartUtf8(const UInt8 byte, TerminalScreen& screen) noexcept
{
    // The well-formed UTF-8 byte sequences: the first continuation byte's
    // range excludes overlong forms, surrogates and values above U+10FFFF.
    m_lower = 0x80U;
    m_upper = 0xBFU;
    if ((byte >= 0xC2U) && (byte <= 0xDFU))
    {
        m_codePoint = byte & 0x1FU;
        m_remaining = 1U;
    }
    else if ((byte >= 0xE0U) && (byte <= 0xEFU))
    {
        m_codePoint = byte & 0x0FU;
        m_remaining = 2U;
        if (byte == 0xE0U)
        {
            m_lower = 0xA0U;
        }
        else if (byte == 0xEDU)
        {
            m_upper = 0x9FU;
        }
    }
    else if ((byte >= 0xF0U) && (byte <= 0xF4U))
    {
        m_codePoint = byte & 0x07U;
        m_remaining = 3U;
        if (byte == 0xF0U)
        {
            m_lower = 0x90U;
        }
        else if (byte == 0xF4U)
        {
            m_upper = 0x8FU;
        }
    }
    else
    {
        screen.Print(Text::ReplacementCodePoint);
    }
}

void TerminalParser::PrintCodePoint(const UInt32 codePoint, TerminalScreen& screen) noexcept
{
    if ((codePoint >= 0x80U) && (codePoint <= 0x9FU))
    {
        return;
    }
    screen.Print(codePoint);
}

void TerminalParser::FeedEscape(const UInt8 byte, TerminalScreen& screen) noexcept
{
    if (byte == EscapeCode)
    {
        return;
    }
    if (byte < 0x20U)
    {
        Execute(byte, screen);
        return;
    }
    if (byte < 0x30U)
    {
        m_state = State::EscapeIntermediate;
        return;
    }
    if (byte == Delete)
    {
        return;
    }
    if (byte > Delete)
    {
        m_state = State::Ground;
        FeedGround(byte, screen);
        return;
    }
    m_state = State::Ground;
    switch (byte)
    {
    case '[':
        StartCsi();
        m_state = State::CsiEntry;
        break;
    case ']':
        m_state = State::OscString;
        break;
    case 'P':
    case 'X':
    case '^':
    case '_':
        m_state = State::IgnoredString;
        break;
    case '7':
        screen.SaveCursor();
        break;
    case '8':
        screen.RestoreCursor();
        break;
    case 'D':
        screen.LineFeed();
        break;
    case 'M':
        screen.ReverseIndex();
        break;
    case 'E':
        screen.CarriageReturn();
        screen.LineFeed();
        break;
    case 'c':
        screen.Reset();
        break;
    default:
        break;
    }
}

void TerminalParser::FeedEscapeIntermediate(const UInt8 byte, TerminalScreen& screen) noexcept
{
    if (byte == EscapeCode)
    {
        m_state = State::Escape;
        return;
    }
    if (byte < 0x20U)
    {
        Execute(byte, screen);
        return;
    }
    if ((byte < 0x30U) || (byte == Delete))
    {
        return;
    }
    m_state = State::Ground;
    if (byte > Delete)
    {
        FeedGround(byte, screen);
    }
}

void TerminalParser::StartCsi() noexcept
{
    m_private = 0U;
    m_count = 0U;
}

void TerminalParser::AddDigit(const UInt8 digit) noexcept
{
    if (m_count == 0U)
    {
        m_count = 1U;
        m_parameters[0U] = 0U;
    }
    UInt32& value = m_parameters[m_count - 1U];
    const UInt32 next{(value * 10U) + (digit - static_cast<UInt32>('0'))};
    value = (next > TerminalMaximumParameterValue) ? TerminalMaximumParameterValue : next;
}

void TerminalParser::AddSeparator() noexcept
{
    if (m_count == 0U)
    {
        m_count = 1U;
        m_parameters[0U] = 0U;
    }
    if (m_count == TerminalMaximumParameters)
    {
        m_state = State::CsiIgnore;
        return;
    }
    m_parameters[m_count] = 0U;
    ++m_count;
}

void TerminalParser::FeedCsi(const UInt8 byte, TerminalScreen& screen, TerminalResponses& responses) noexcept
{
    if (byte == EscapeCode)
    {
        m_state = State::Escape;
        return;
    }
    if (byte < 0x20U)
    {
        Execute(byte, screen);
        return;
    }
    if (byte == Delete)
    {
        return;
    }
    if (byte > Delete)
    {
        m_state = State::Ground;
        FeedGround(byte, screen);
        return;
    }
    if ((byte >= '0') && (byte <= '9'))
    {
        AddDigit(byte);
        m_state = State::CsiParameter;
        return;
    }
    if (byte == ';')
    {
        m_state = State::CsiParameter;
        AddSeparator();
        return;
    }
    if (byte == ':')
    {
        m_state = State::CsiIgnore;
        return;
    }
    if ((byte >= '<') && (byte <= '?'))
    {
        if (m_state == State::CsiEntry)
        {
            m_private = byte;
            m_state = State::CsiParameter;
        }
        else
        {
            m_state = State::CsiIgnore;
        }
        return;
    }
    if (byte < 0x30U)
    {
        m_state = State::CsiIntermediate;
        return;
    }
    m_state = State::Ground;
    Dispatch(byte, screen, responses);
}

void TerminalParser::FeedCsiIntermediate(const UInt8 byte, TerminalScreen& screen) noexcept
{
    if (byte == EscapeCode)
    {
        m_state = State::Escape;
        return;
    }
    if (byte < 0x20U)
    {
        Execute(byte, screen);
        return;
    }
    if ((byte < 0x30U) || (byte == Delete))
    {
        return;
    }
    if (byte < 0x40U)
    {
        m_state = State::CsiIgnore;
        return;
    }
    // No function of the subset has an intermediate byte: the final ends an
    // ignored sequence.
    m_state = State::Ground;
    if (byte > Delete)
    {
        FeedGround(byte, screen);
    }
}

void TerminalParser::FeedCsiIgnore(const UInt8 byte, TerminalScreen& screen) noexcept
{
    if (byte == EscapeCode)
    {
        m_state = State::Escape;
        return;
    }
    if (byte < 0x20U)
    {
        Execute(byte, screen);
        return;
    }
    if ((byte < 0x40U) || (byte == Delete))
    {
        return;
    }
    m_state = State::Ground;
    if (byte > Delete)
    {
        FeedGround(byte, screen);
    }
}

void TerminalParser::FeedString(const UInt8 byte) noexcept
{
    if (byte == EscapeCode)
    {
        m_state = State::StringEscape;
        return;
    }
    if ((byte == Bell) && (m_state == State::OscString))
    {
        m_state = State::Ground;
    }
}

void TerminalParser::FeedStringEscape(const UInt8 byte, TerminalScreen& screen) noexcept
{
    if (byte == StringTerminatorFinal)
    {
        m_state = State::Ground;
        return;
    }
    // Not ST: the string ends and the ESC starts an escape sequence.
    m_state = State::Escape;
    FeedEscape(byte, screen);
}

UInt32 TerminalParser::Count(const Size index) const noexcept
{
    if ((index >= m_count) || (m_parameters[index] == 0U))
    {
        return 1U;
    }
    return m_parameters[index];
}

UInt32 TerminalParser::Selector(const Size index) const noexcept
{
    return (index < m_count) ? m_parameters[index] : 0U;
}

void TerminalParser::Dispatch(const UInt8 final, TerminalScreen& screen, TerminalResponses& responses) noexcept
{
    if (m_private == '?')
    {
        if ((final == 'h') || (final == 'l'))
        {
            DispatchModes(final, screen);
        }
        return;
    }
    if (m_private != 0U)
    {
        return;
    }
    switch (final)
    {
    case '@':
        screen.InsertCharacters(Count(0U));
        break;
    case 'A':
        screen.CursorUp(Count(0U));
        break;
    case 'B':
        screen.CursorDown(Count(0U));
        break;
    case 'C':
        screen.CursorForward(Count(0U));
        break;
    case 'D':
        screen.CursorBackward(Count(0U));
        break;
    case 'E':
        screen.CursorDown(Count(0U));
        screen.CarriageReturn();
        break;
    case 'F':
        screen.CursorUp(Count(0U));
        screen.CarriageReturn();
        break;
    case 'G':
        screen.SetColumn(Count(0U) - 1U);
        break;
    case 'H':
    case 'f':
        screen.SetCursor(Count(0U) - 1U, Count(1U) - 1U);
        break;
    case 'J':
        screen.EraseInDisplay(Selector(0U));
        break;
    case 'K':
        screen.EraseInLine(Selector(0U));
        break;
    case 'L':
        screen.InsertLines(Count(0U));
        break;
    case 'M':
        screen.DeleteLines(Count(0U));
        break;
    case 'P':
        screen.DeleteCharacters(Count(0U));
        break;
    case 'S':
        screen.ScrollUp(Count(0U));
        break;
    case 'T':
        screen.ScrollDown(Count(0U));
        break;
    case 'X':
        screen.EraseCharacters(Count(0U));
        break;
    case 'd':
        screen.SetRow(Count(0U) - 1U);
        break;
    case 'm':
        SelectGraphicRendition(screen);
        break;
    case 'c':
    case 'n':
        Report(final, screen, responses);
        break;
    case 'r':
    {
        // DECSTBM: a missing or zero bottom margin is the last line; one past
        // the screen is clamped to it.
        const UInt32 rows{screen.Rows()};
        UInt32 bottom{Selector(1U)};
        if ((bottom == 0U) || (bottom > rows))
        {
            bottom = rows;
        }
        screen.SetMargins(Count(0U) - 1U, bottom - 1U);
        break;
    }
    default:
        break;
    }
}

void TerminalParser::DispatchModes(const UInt8 final, TerminalScreen& screen) noexcept
{
    const bool set{final == 'h'};
    for (Size index{0U}; index < m_count; ++index)
    {
        switch (m_parameters[index])
        {
        case 1U:
            screen.SetApplicationCursorKeys(set);
            break;
        case 7U:
            screen.SetAutowrap(set);
            break;
        case 25U:
            screen.SetCursorVisible(set);
            break;
        default:
            break;
        }
    }
}

void TerminalParser::SelectGraphicRendition(TerminalScreen& screen) const noexcept
{
    TerminalRendition rendition{screen.Rendition()};
    const Size count{(m_count == 0U) ? 1U : m_count};
    for (Size index{0U}; index < count; ++index)
    {
        const UInt32 value{Selector(index)};
        if (value == 0U)
        {
            rendition = TerminalRendition{
                .Attributes = 0U, .Foreground = TerminalDefaultColor, .Background = TerminalDefaultColor};
        }
        else if (value == 1U)
        {
            rendition.Attributes = static_cast<UInt8>(rendition.Attributes | TerminalBold);
        }
        else if (value == 4U)
        {
            rendition.Attributes = static_cast<UInt8>(rendition.Attributes | TerminalUnderline);
        }
        else if (value == 7U)
        {
            rendition.Attributes = static_cast<UInt8>(rendition.Attributes | TerminalReverse);
        }
        else if (value == 22U)
        {
            rendition.Attributes = static_cast<UInt8>(rendition.Attributes & ~TerminalBold);
        }
        else if (value == 24U)
        {
            rendition.Attributes = static_cast<UInt8>(rendition.Attributes & ~TerminalUnderline);
        }
        else if (value == 27U)
        {
            rendition.Attributes = static_cast<UInt8>(rendition.Attributes & ~TerminalReverse);
        }
        else if ((value >= 30U) && (value <= 37U))
        {
            rendition.Foreground = static_cast<UInt8>(value - 30U);
        }
        else if (value == 39U)
        {
            rendition.Foreground = TerminalDefaultColor;
        }
        else if ((value >= 40U) && (value <= 47U))
        {
            rendition.Background = static_cast<UInt8>(value - 40U);
        }
        else if (value == 49U)
        {
            rendition.Background = TerminalDefaultColor;
        }
        else if ((value >= 90U) && (value <= 97U))
        {
            rendition.Foreground = static_cast<UInt8>(value - 90U + 8U);
        }
        else if ((value >= 100U) && (value <= 107U))
        {
            rendition.Background = static_cast<UInt8>(value - 100U + 8U);
        }
        else if ((value == 38U) || (value == 48U))
        {
            // Extended colours are not in the subset: skip their arguments.
            const UInt32 form{Selector(index + 1U)};
            if (form == 5U)
            {
                index += 2U;
            }
            else if (form == 2U)
            {
                index += 4U;
            }
        }
    }
    screen.SetRendition(rendition);
}

void TerminalParser::Report(const UInt8 final, TerminalScreen& screen, TerminalResponses& responses) noexcept
{
    const UInt32 selector{Selector(0U)};
    Size length{0U};
    m_answer[length++] = EscapeCode;
    m_answer[length++] = '[';
    if (final == 'c')
    {
        if ((m_count > 1U) || (selector != 0U))
        {
            return;
        }
        m_answer[length++] = '?';
        m_answer[length++] = '1';
        m_answer[length++] = ';';
        m_answer[length++] = '2';
        m_answer[length++] = 'c';
    }
    else if (selector == 5U)
    {
        m_answer[length++] = '0';
        m_answer[length++] = 'n';
    }
    else if (selector == 6U)
    {
        length = AppendDecimal(m_answer, length, screen.CursorRow() + 1U);
        m_answer[length++] = ';';
        length = AppendDecimal(m_answer, length, screen.CursorColumn() + 1U);
        m_answer[length++] = 'R';
    }
    else
    {
        return;
    }
    responses.Append(m_answer, length);
}
} // namespace Asteria::User::Terminal
