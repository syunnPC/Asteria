// SPDX-License-Identifier: Apache-2.0
#include "User/Terminal/Terminal.hpp"
#include "User/Text/Utf8.hpp"

#include <cstdio>
#include <cstring>
#include <cstring>

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

// VT conformance tables: a byte stream written to a fresh terminal and the
// grid, cursor, modes, renditions and report answers it must leave. The
// expectations follow the behaviour TerminalScreen.hpp and TerminalParser.hpp
// state (written from ECMA-48 and DEC's VT100 conventions). Rows are given
// trimmed and separated by '|'; a missing row or column is blank.
namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt8;
using User::Terminal::Terminal;
using User::Terminal::TerminalCellView;
using User::Terminal::TerminalDefaultColor;

struct ScreenCase
{
    const char* Name;
    UInt32 Columns;
    UInt32 Rows;
    const char* Input;
    Size InputLength;
    const char* Expected;
    UInt32 CursorRow;
    UInt32 CursorColumn;
    bool WrapPending;
};

#define ASTERIA_VT(name, columns, rows, input, expected, row, column, wrap)                                            \
    ScreenCase                                                                                                         \
    {                                                                                                                  \
        name, columns, rows, input, sizeof(input) - 1U, expected, row, column, wrap                                    \
    }

// Four lines of ten columns: "0123456789", "abcdefghij", "ABCDEFGHIJ", blank;
// the cursor ends at (2, 9) with a wrap pending.
#define ASTERIA_FILLED "0123456789\r\nabcdefghij\r\nABCDEFGHIJ"
#define ASTERIA_FILLED_SCREEN "0123456789|abcdefghij|ABCDEFGHIJ"

constexpr ScreenCase ScreenCases[]{
    // C0 controls and text.
    ASTERIA_VT("plain text", 10U, 4U, "abc", "abc", 0U, 3U, false),
    ASTERIA_VT("CR returns to column 0", 10U, 4U, "abc\rX", "Xbc", 0U, 1U, false),
    ASTERIA_VT("LF keeps the column", 10U, 4U, "ab\ncd", "ab|  cd", 1U, 4U, false),
    ASTERIA_VT("VT and FF act as LF", 10U, 4U,
               "a\x0b"
               "b\x0c"
               "c",
               "a| b|  c", 2U, 3U, false),
    ASTERIA_VT("CR LF", 10U, 4U, "ab\r\ncd", "ab|cd", 1U, 2U, false),
    ASTERIA_VT("BS moves without erasing", 10U, 4U, "abc\b\bX", "aXc", 0U, 2U, false),
    ASTERIA_VT("BS stops at column 0", 10U, 4U, "\b\bA", "A", 0U, 1U, false),
    ASTERIA_VT("HT to the next stop", 10U, 4U, "a\tb", "a       b", 0U, 9U, false),
    ASTERIA_VT("HT clamps to the last column", 10U, 4U, "\t\t\tX", "         X", 0U, 9U, true),
    ASTERIA_VT("BEL and other C0 are ignored", 10U, 4U,
               "a\x07\x01\x00\x0e"
               "b",
               "ab", 0U, 2U, false),
    ASTERIA_VT("DEL is ignored", 10U, 4U,
               "a\x7f"
               "b",
               "ab", 0U, 2U, false),
    ASTERIA_VT("a full line leaves a pending wrap", 10U, 4U, "0123456789", "0123456789", 0U, 9U, true),
    ASTERIA_VT("the pending wrap happens at the next graphic", 10U, 4U, "0123456789A", "0123456789|A", 1U, 1U, false),
    ASTERIA_VT("CR cancels the pending wrap", 10U, 4U, "0123456789\rX", "X123456789", 0U, 1U, false),
    ASTERIA_VT("BS cancels the pending wrap", 10U, 4U, "0123456789\bX", "01234567X9", 0U, 9U, false),
    ASTERIA_VT("a wrap at the bottom scrolls", 10U, 2U, "0123456789abcdefghijK", "abcdefghij|K", 1U, 1U, false),
    ASTERIA_VT("autowrap off overwrites the last column", 10U, 4U, "\x1b[?7l0123456789AB", "012345678B", 0U, 9U, false),
    ASTERIA_VT("autowrap on again wraps after the next graphic", 10U, 4U, "\x1b[?7l0123456789A\x1b[?7hB", "012345678B",
               0U, 9U, true),
    ASTERIA_VT("LF at the bottom scrolls", 10U, 3U, "a\nb\nc\nd", " b|  c|   d", 2U, 4U, false),
    ASTERIA_VT("EL keeps the pending wrap", 10U, 4U, "0123456789\x1b[KA", "012345678|A", 1U, 1U, false),
    // Escape sequences.
    ASTERIA_VT("DECSC and DECRC", 10U, 4U,
               "ab\x1b"
               "7\x1b[3;5Hx\x1b"
               "8y",
               "aby||    x", 0U, 3U, false),
    ASTERIA_VT("DECRC without DECSC goes home", 10U, 4U,
               "abc\x1b"
               "8X",
               "Xbc", 0U, 1U, false),
    ASTERIA_VT("IND", 10U, 4U,
               "a\x1b"
               "Db",
               "a| b", 1U, 2U, false),
    ASTERIA_VT("NEL", 10U, 4U,
               "a\x1b"
               "Eb",
               "a|b", 1U, 1U, false),
    ASTERIA_VT("RI at the top scrolls down", 10U, 4U, "a\x1bMb", " b|a", 0U, 2U, false),
    ASTERIA_VT("RI moves up", 10U, 4U, "\n\nx\x1bMy", "| y|x", 1U, 2U, false),
    ASTERIA_VT("RIS clears and homes", 10U, 4U,
               "abc\x1b[31m\x1b"
               "cd",
               "d", 0U, 1U, false),
    ASTERIA_VT("an unknown escape final is ignored", 10U, 4U, "a\x1bZb", "ab", 0U, 2U, false),
    ASTERIA_VT("a designation (intermediate) is ignored", 10U, 4U, "a\x1b(Bb", "ab", 0U, 2U, false),
    ASTERIA_VT("a C0 inside an escape acts", 10U, 4U, "a\x1b\rb", "a", 0U, 0U, false),
    ASTERIA_VT("ESC ESC restarts the escape", 10U, 4U,
               "a\x1b\x1b"
               "Db",
               "a| b", 1U, 2U, false),
    // Cursor movement.
    ASTERIA_VT("CUU", 10U, 4U, "\n\n\x1b[Ax", "|x", 1U, 1U, false),
    ASTERIA_VT("CUU stops at the top", 10U, 4U, "\n\x1b[9Ax", "x", 0U, 1U, false),
    ASTERIA_VT("CUU 0 moves one", 10U, 4U, "\n\n\x1b[0Ax", "|x", 1U, 1U, false),
    ASTERIA_VT("CUD stops at the bottom", 10U, 4U, "\x1b[99Bx", "|||x", 3U, 1U, false),
    ASTERIA_VT("CUF", 10U, 4U, "\x1b[3Cx", "   x", 0U, 4U, false),
    ASTERIA_VT("CUF stops at the last column", 10U, 4U, "\x1b[99Cx", "         x", 0U, 9U, true),
    ASTERIA_VT("CUB", 10U, 4U, "abcdef\x1b[2DX", "abcdXf", 0U, 5U, false),
    ASTERIA_VT("CUB stops at column 0", 10U, 4U, "ab\x1b[9DX", "Xb", 0U, 1U, false),
    ASTERIA_VT("CNL", 10U, 4U, "ab\x1b[2Ex", "ab||x", 2U, 1U, false),
    ASTERIA_VT("CPL", 10U, 4U, "\n\n   \x1b[Fx", "|x", 1U, 1U, false),
    ASTERIA_VT("CHA", 10U, 4U, "abc\x1b[7Gx", "abc   x", 0U, 7U, false),
    ASTERIA_VT("CHA clamps", 10U, 4U, "\x1b[99Gx", "         x", 0U, 9U, true),
    ASTERIA_VT("CUP", 10U, 4U, "\x1b[2;3Hx", "|  x", 1U, 3U, false),
    ASTERIA_VT("CUP defaults home", 10U, 4U, "abc\x1b[Hx", "xbc", 0U, 1U, false),
    ASTERIA_VT("CUP with a column only", 10U, 4U, "\x1b[;4Hx", "   x", 0U, 4U, false),
    ASTERIA_VT("CUP clamps", 10U, 4U, "\x1b[99;99Hx", "|||         x", 3U, 9U, true),
    ASTERIA_VT("HVP", 10U, 4U, "\x1b[3;2fx", "|| x", 2U, 2U, false),
    ASTERIA_VT("VPA keeps the column", 10U, 4U, "ab\x1b[3dx", "ab||  x", 2U, 3U, false),
    ASTERIA_VT("CUU stops at the top margin inside the region", 10U, 4U, "\x1b[2;3r\x1b[3;1H\x1b[5Ax", "|x", 1U, 1U,
               false),
    ASTERIA_VT("CUU above the region goes to the first line", 10U, 4U, "\x1b[3;4r\x1b[2;1H\x1b[5Ax", "x", 0U, 1U,
               false),
    ASTERIA_VT("CUD stops at the bottom margin", 10U, 4U, "\x1b[1;2r\x1b[9Bx", "|x", 1U, 1U, false),
    ASTERIA_VT("CUD below the region goes to the last line", 10U, 4U, "\x1b[1;2r\x1b[3;1H\x1b[9Bx", "|||x", 3U, 1U,
               false),
    // Erasing.
    ASTERIA_VT("ED 0", 10U, 4U, ASTERIA_FILLED "\x1b[2;5H\x1b[J", "0123456789|abcd", 1U, 4U, false),
    ASTERIA_VT("ED 1", 10U, 4U, ASTERIA_FILLED "\x1b[2;5H\x1b[1J", "|     fghij|ABCDEFGHIJ", 1U, 4U, false),
    ASTERIA_VT("ED 2 keeps the cursor and a pending wrap", 10U, 4U, ASTERIA_FILLED "\x1b[2J", "", 2U, 9U, true),
    ASTERIA_VT("ED 3 is ignored", 10U, 4U, ASTERIA_FILLED "\x1b[3J", ASTERIA_FILLED_SCREEN, 2U, 9U, true),
    ASTERIA_VT("EL 0", 10U, 4U, ASTERIA_FILLED "\x1b[2;5H\x1b[K", "0123456789|abcd|ABCDEFGHIJ", 1U, 4U, false),
    ASTERIA_VT("EL 1", 10U, 4U, ASTERIA_FILLED "\x1b[2;5H\x1b[1K", "0123456789|     fghij|ABCDEFGHIJ", 1U, 4U, false),
    ASTERIA_VT("EL 2", 10U, 4U, ASTERIA_FILLED "\x1b[2;5H\x1b[2K", "0123456789||ABCDEFGHIJ", 1U, 4U, false),
    ASTERIA_VT("EL 9 is ignored", 10U, 4U, ASTERIA_FILLED "\x1b[2;5H\x1b[9K", ASTERIA_FILLED_SCREEN, 1U, 4U, false),
    ASTERIA_VT("ECH", 10U, 4U, ASTERIA_FILLED "\x1b[2;3H\x1b[3X", "0123456789|ab   fghij|ABCDEFGHIJ", 1U, 2U, false),
    ASTERIA_VT("ECH stops at the line's end", 10U, 4U, ASTERIA_FILLED "\x1b[2;9H\x1b[99X",
               "0123456789|abcdefgh|ABCDEFGHIJ", 1U, 8U, false),
    ASTERIA_VT("ECH erases one by default", 10U, 4U, ASTERIA_FILLED "\x1b[2;1H\x1b[X",
               "0123456789| bcdefghij|ABCDEFGHIJ", 1U, 0U, false),
    // Inserting and deleting.
    ASTERIA_VT("ICH", 10U, 4U, ASTERIA_FILLED "\x1b[2;3H\x1b[2@", "0123456789|ab  cdefgh|ABCDEFGHIJ", 1U, 2U, false),
    ASTERIA_VT("ICH clamps", 10U, 4U, ASTERIA_FILLED "\x1b[2;3H\x1b[99@", "0123456789|ab|ABCDEFGHIJ", 1U, 2U, false),
    ASTERIA_VT("ICH cancels the pending wrap", 10U, 4U, "0123456789\x1b[@X", "012345678X", 0U, 9U, true),
    ASTERIA_VT("DCH", 10U, 4U, ASTERIA_FILLED "\x1b[2;3H\x1b[2P", "0123456789|abefghij|ABCDEFGHIJ", 1U, 2U, false),
    ASTERIA_VT("DCH clamps", 10U, 4U, ASTERIA_FILLED "\x1b[2;3H\x1b[99P", "0123456789|ab|ABCDEFGHIJ", 1U, 2U, false),
    ASTERIA_VT("IL", 10U, 4U, ASTERIA_FILLED "\x1b[2;5H\x1b[L", "0123456789||abcdefghij|ABCDEFGHIJ", 1U, 0U, false),
    ASTERIA_VT("IL clamps", 10U, 4U, ASTERIA_FILLED "\x1b[2;1H\x1b[9L", "0123456789", 1U, 0U, false),
    ASTERIA_VT("DL", 10U, 4U, ASTERIA_FILLED "\x1b[1;5H\x1b[M", "abcdefghij|ABCDEFGHIJ", 0U, 0U, false),
    ASTERIA_VT("IL inside the region", 10U, 4U, ASTERIA_FILLED "\x1b[1;3r\x1b[2;1H\x1b[L", "0123456789||abcdefghij", 1U,
               0U, false),
    ASTERIA_VT("IL outside the region does nothing", 10U, 4U, ASTERIA_FILLED "\x1b[1;2r\x1b[4;3H\x1b[L",
               ASTERIA_FILLED_SCREEN, 3U, 2U, false),
    ASTERIA_VT("DL inside the region", 10U, 4U, ASTERIA_FILLED "\x1b[1;3r\x1b[1;1H\x1b[M", "abcdefghij|ABCDEFGHIJ", 0U,
               0U, false),
    // Scrolling.
    ASTERIA_VT("SU keeps the cursor and a pending wrap", 10U, 4U, ASTERIA_FILLED "\x1b[S", "abcdefghij|ABCDEFGHIJ", 2U,
               9U, true),
    ASTERIA_VT("SD", 10U, 4U, ASTERIA_FILLED "\x1b[2T", "||0123456789|abcdefghij", 2U, 9U, true),
    ASTERIA_VT("SU inside the region", 10U, 4U, ASTERIA_FILLED "\x1b[2;3r\x1b[S", "0123456789|ABCDEFGHIJ", 0U, 0U,
               false),
    ASTERIA_VT("LF scrolls the region at its bottom margin", 10U, 4U, ASTERIA_FILLED "\x1b[1;2r\x1b[2;1H\nx",
               "abcdefghij|x|ABCDEFGHIJ", 1U, 1U, false),
    ASTERIA_VT("LF below the region stops at the last line", 10U, 4U, ASTERIA_FILLED "\x1b[1;2r\x1b[4;1H\nx",
               ASTERIA_FILLED_SCREEN "|x", 3U, 1U, false),
    ASTERIA_VT("RI scrolls the region at its top margin", 10U, 4U, ASTERIA_FILLED "\x1b[2;3r\x1b[2;1H\x1bMx",
               "0123456789|x|abcdefghij", 1U, 1U, false),
    ASTERIA_VT("DECSTBM with top not above bottom is ignored", 10U, 4U, "ab\x1b[3;2rX", "abX", 0U, 3U, false),
    // Parser bounds and malformed sequences.
    ASTERIA_VT("17 parameters ignore the sequence", 10U, 4U, "\x1b[1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1Cx", "x", 0U, 1U,
               false),
    ASTERIA_VT("16 parameters are accepted", 10U, 4U, "\x1b[2;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1Cx", "  x", 0U, 3U, false),
    ASTERIA_VT("a huge parameter is clamped", 10U, 4U, "\x1b[99999999999999999999Cx", "         x", 0U, 9U, true),
    ASTERIA_VT("an intermediate byte ignores the sequence", 10U, 4U, "\x1b[2 Cx", "x", 0U, 1U, false),
    ASTERIA_VT("a parameter byte after an intermediate ignores it", 10U, 4U, "\x1b[ 2Cx", "x", 0U, 1U, false),
    ASTERIA_VT("a sub-parameter ignores the sequence", 10U, 4U, "\x1b[2:3Cx", "x", 0U, 1U, false),
    ASTERIA_VT("a late private marker ignores the sequence", 10U, 4U, "\x1b[2?Cx", "x", 0U, 1U, false),
    ASTERIA_VT("another private marker ignores the sequence", 10U, 4U, "\x1b[>2Cx", "x", 0U, 1U, false),
    ASTERIA_VT("'?' with another final is ignored", 10U, 4U, "\x1b[?2Cx", "x", 0U, 1U, false),
    ASTERIA_VT("an unknown final is ignored", 10U, 4U, "\x1b[2zx", "x", 0U, 1U, false),
    ASTERIA_VT("a C0 inside a control sequence acts", 10U, 4U, "abc\x1b[\r2Cx", "abx", 0U, 3U, false),
    ASTERIA_VT("CAN abandons a control sequence", 10U, 4U,
               "\x1b[2\x18"
               "Cx",
               "Cx", 0U, 2U, false),
    ASTERIA_VT("SUB abandons an escape", 10U, 4U,
               "\x1b\x1a"
               "Dx",
               "Dx", 0U, 2U, false),
    ASTERIA_VT("ESC restarts a control sequence", 10U, 4U, "\x1b[5\x1b[2Cx", "  x", 0U, 3U, false),
    ASTERIA_VT("a byte above 0x7F abandons a control sequence", 10U, 4U, "\x1b[2\xc3\xa9x", "\xc3\xa9x", 0U, 2U, false),
    ASTERIA_VT("DEL inside a control sequence is ignored", 10U, 4U,
               "\x1b[2\x7f"
               "Cx",
               "  x", 0U, 3U, false),
    // Control strings.
    ASTERIA_VT("OSC ends at BEL", 10U, 4U,
               "a\x1b]0;title\x07"
               "b",
               "ab", 0U, 2U, false),
    ASTERIA_VT("OSC ends at ST", 10U, 4U, "a\x1b]0;title\x1b\\b", "ab", 0U, 2U, false),
    ASTERIA_VT("DCS ignores BEL and ends at ST", 10U, 4U, "a\x1bPq#0;1\x07still\x1b\\b", "ab", 0U, 2U, false),
    ASTERIA_VT("SOS, PM and APC are ignored", 10U, 4U, "a\x1bXsos\x1b\\b\x1b^pm\x1b\\c\x1b_apc\x1b\\d", "abcd", 0U, 4U,
               false),
    ASTERIA_VT("ESC that is not ST ends a string", 10U, 4U, "a\x1b]0;t\x1b[2Cx", "a  x", 0U, 4U, false),
    ASTERIA_VT("a string's bytes above 0x7F are ignored", 10U, 4U,
               "a\x1b]\xc3\xa9\x07"
               "b",
               "ab", 0U, 2U, false),
    ASTERIA_VT("CAN abandons a string", 10U, 4U,
               "a\x1b]0;t\x18"
               "b",
               "ab", 0U, 2U, false),
    // UTF-8 and code page 437.
    ASTERIA_VT("two-byte character", 10U, 4U, "\xc3\xa9", "\xc3\xa9", 0U, 1U, false),
    ASTERIA_VT("box drawing", 10U, 4U, "\xe2\x94\x80\xe2\x95\x94", "\xe2\x94\x80\xe2\x95\x94", 0U, 2U, false),
    ASTERIA_VT("a symbol of the control area", 10U, 4U, "\xe2\x98\xba\xe2\x8c\x82", "\xe2\x98\xba\xe2\x8c\x82", 0U, 2U,
               false),
    ASTERIA_VT("degree, NBSP and block", 10U, 4U, "\xc2\xb0\xc2\xa0\xe2\x96\xa0", "\xc2\xb0\xc2\xa0\xe2\x96\xa0", 0U,
               3U, false),
    ASTERIA_VT("a code point without a glyph", 10U, 4U, "\xe2\x82\xac", "\xe2\x82\xac", 0U, 1U, false),
    ASTERIA_VT("a four-byte character takes one cell", 10U, 4U, "\xf0\x9f\x98\x80x", "\xf0\x9f\x98\x80x", 0U, 2U,
               false),
    ASTERIA_VT("an overlong two-byte form", 10U, 4U, "\xc0\xafx", "\xef\xbf\xbd\xef\xbf\xbdx", 0U, 3U, false),
    ASTERIA_VT("an overlong three-byte form", 10U, 4U, "\xe0\x80\xafx", "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbdx", 0U, 4U,
               false),
    ASTERIA_VT("a surrogate", 10U, 4U, "\xed\xa0\x80x", "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbdx", 0U, 4U, false),
    ASTERIA_VT("above U+10FFFF", 10U, 4U, "\xf4\x90\x80\x80x", "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbdx", 0U,
               5U, false),
    ASTERIA_VT("a byte that starts nothing", 10U, 4U, "\xf5x\xffy", "\xef\xbf\xbdx\xef\xbf\xbdy", 0U, 4U, false),
    ASTERIA_VT("a character cut by ASCII", 10U, 4U, "\xc3x", "\xef\xbf\xbdx", 0U, 2U, false),
    ASTERIA_VT("a character cut by ESC", 10U, 4U, "\xe2\x94\x1b[2Cx", "\xef\xbf\xbd  x", 0U, 4U, false),
    ASTERIA_VT("a character cut by CAN", 10U, 4U, "\xe2\x18x", "\xef\xbf\xbdx", 0U, 2U, false),
    ASTERIA_VT("a C1 code point is ignored", 10U, 4U,
               "a\xc2\x85"
               "b",
               "ab", 0U, 2U, false),
    ASTERIA_VT("a raw C1 byte is malformed", 10U, 4U,
               "a\x9b"
               "b",
               "a\xef\xbf\xbd"
               "b",
               0U, 3U, false),
};

// Writes input to the terminal and reports whether the grid matches expected.
[[nodiscard]] bool GridMatches(const Terminal& terminal, const char* const expected) noexcept
{
    const auto& screen = terminal.Screen();
    UInt32 row{0U};
    UInt32 column{0U};
    bool match{true};
    const char* const end{expected + std::strlen(expected)};
    for (const char* at = expected;; ++at)
    {
        if ((*at == '|') || (*at == '\0'))
        {
            for (; column < screen.Columns(); ++column)
            {
                match = match && (screen.CellAt(row, column).CodePoint == 0x20U);
            }
            ++row;
            column = 0U;
            if (*at == '\0')
            {
                break;
            }
            continue;
        }
        Size consumed{0U};
        const UInt32 point{
            User::Text::NextCodePoint(reinterpret_cast<const UInt8*>(at), static_cast<Size>(end - at), consumed)};
        match = match && (screen.CellAt(row, column).CodePoint == point);
        at += consumed - 1U;
        ++column;
    }
    for (; row < screen.Rows(); ++row)
    {
        for (UInt32 blank{0U}; blank < screen.Columns(); ++blank)
        {
            match = match && (screen.CellAt(row, blank).CodePoint == 0x20U);
        }
    }
    return match;
}

void DumpGrid(const Terminal& terminal) noexcept
{
    const auto& screen = terminal.Screen();
    for (UInt32 row{0U}; row < screen.Rows(); ++row)
    {
        static_cast<void>(std::fprintf(stderr, "  |"));
        for (UInt32 column{0U}; column < screen.Columns(); ++column)
        {
            const UInt32 code{screen.CellAt(row, column).CodePoint};
            if ((code >= 0x20U) && (code < 0x7FU))
            {
                static_cast<void>(std::fprintf(stderr, "%c", code));
            }
            else
            {
                static_cast<void>(std::fprintf(stderr, "\\x%02x", code));
            }
        }
        static_cast<void>(std::fprintf(stderr, "|\n"));
    }
}

Terminal g_terminal;

void Write(Terminal& terminal, const char* const text, const Size length) noexcept
{
    terminal.Write(reinterpret_cast<const UInt8*>(text), length);
}

void TestScreenCases(TestContext& runner) noexcept
{
    for (const ScreenCase& test : ScreenCases)
    {
        g_terminal.Configure(test.Columns, test.Rows);
        Write(g_terminal, test.Input, test.InputLength);
        const auto& screen = g_terminal.Screen();
        const bool grid{GridMatches(g_terminal, test.Expected)};
        if (!ASTERIA_EXPECT(runner, test.Name, grid, screen.CursorRow() == test.CursorRow,
                            screen.CursorColumn() == test.CursorColumn, screen.WrapPending() == test.WrapPending))
        {
            static_cast<void>(std::fprintf(stderr, "  cursor (%u, %u) wrap %d\n", screen.CursorRow(),
                                           screen.CursorColumn(), screen.WrapPending() ? 1 : 0));
            DumpGrid(g_terminal);
        }
    }
}

struct CellCase
{
    const char* Name;
    const char* Input;
    Size InputLength;
    UInt32 Row;
    UInt32 Column;
    TerminalCellView Expected;
};

#define ASTERIA_CELL(name, input, row, column, code, attributes, foreground, background)                               \
    CellCase                                                                                                           \
    {                                                                                                                  \
        name, input, sizeof(input) - 1U, row, column, TerminalCellView                                                 \
        {                                                                                                              \
            code, attributes, foreground, background                                                                   \
        }                                                                                                              \
    }

constexpr UInt8 Default{TerminalDefaultColor};
constexpr UInt8 Bold{User::Terminal::TerminalBold};
constexpr UInt8 Underline{User::Terminal::TerminalUnderline};
constexpr UInt8 Reverse{User::Terminal::TerminalReverse};

constexpr CellCase CellCases[]{
    ASTERIA_CELL("a fresh cell", "A", 0U, 0U, 'A', 0U, Default, Default),
    ASTERIA_CELL("SGR 1 4 7", "\x1b[1;4;7mA", 0U, 0U, 'A', Bold | Underline | Reverse, Default, Default),
    ASTERIA_CELL("SGR 31 42", "\x1b[31;42mA", 0U, 0U, 'A', 0U, 1U, 2U),
    ASTERIA_CELL("SGR 37 47", "\x1b[37;47mA", 0U, 0U, 'A', 0U, 7U, 7U),
    ASTERIA_CELL("SGR 91 102", "\x1b[91;102mA", 0U, 0U, 'A', 0U, 9U, 10U),
    ASTERIA_CELL("SGR 97 107", "\x1b[97;107mA", 0U, 0U, 'A', 0U, 15U, 15U),
    ASTERIA_CELL("SGR 22", "\x1b[1;4m\x1b[22mA", 0U, 0U, 'A', Underline, Default, Default),
    ASTERIA_CELL("SGR 24", "\x1b[4;7m\x1b[24mA", 0U, 0U, 'A', Reverse, Default, Default),
    ASTERIA_CELL("SGR 27", "\x1b[7;1m\x1b[27mA", 0U, 0U, 'A', Bold, Default, Default),
    ASTERIA_CELL("SGR 39", "\x1b[31;41m\x1b[39mA", 0U, 0U, 'A', 0U, Default, 1U),
    ASTERIA_CELL("SGR 49", "\x1b[31;41m\x1b[49mA", 0U, 0U, 'A', 0U, 1U, Default),
    ASTERIA_CELL("SGR with no parameter resets", "\x1b[1;31;41m\x1b[mA", 0U, 0U, 'A', 0U, Default, Default),
    ASTERIA_CELL("SGR 0 among others", "\x1b[1;31;0;4mA", 0U, 0U, 'A', Underline, Default, Default),
    ASTERIA_CELL("SGR 38;5 is skipped", "\x1b[38;5;196;4mA", 0U, 0U, 'A', Underline, Default, Default),
    ASTERIA_CELL("SGR 48;2 is skipped", "\x1b[48;2;1;2;3;1mA", 0U, 0U, 'A', Bold, Default, Default),
    ASTERIA_CELL("SGR 38 with another form skips itself", "\x1b[38;4mA", 0U, 0U, 'A', Underline, Default, Default),
    ASTERIA_CELL("SGR 38 at the end", "\x1b[1;38mA", 0U, 0U, 'A', Bold, Default, Default),
    ASTERIA_CELL("unknown SGR values are ignored", "\x1b[5;8;9;38;5mA", 0U, 0U, 'A', 0U, Default, Default),
    ASTERIA_CELL("SGR 30 is not the default", "\x1b[30;40mA", 0U, 0U, 'A', 0U, 0U, 0U),
    ASTERIA_CELL("a sub-parameter ignores SGR", "\x1b[31:1mA", 0U, 0U, 'A', 0U, Default, Default),
    ASTERIA_CELL("ED erases with the current background", "\x1b[1;4;31;44m\x1b[2J", 3U, 9U, 0x20U, 0U, Default, 4U),
    ASTERIA_CELL("EL erases with the current background", "ab\x1b[43m\x1b[1;1H\x1b[K", 0U, 1U, 0x20U, 0U, Default, 3U),
    ASTERIA_CELL("ECH erases with the current background", "ab\x1b[42m\x1b[1;1H\x1b[X", 0U, 0U, 0x20U, 0U, Default, 2U),
    ASTERIA_CELL("ICH blanks take the current background", "ab\x1b[45m\x1b[1;1H\x1b[@", 0U, 0U, 0x20U, 0U, Default, 5U),
    ASTERIA_CELL("ICH shifts cells with their rendition", "\x1b[31ma\x1b[m\x1b[1;1H\x1b[@", 0U, 1U, 'a', 0U, 1U,
                 Default),
    ASTERIA_CELL("DCH blanks take the current background", "ab\x1b[46m\x1b[1;1H\x1b[P", 0U, 9U, 0x20U, 0U, Default, 6U),
    ASTERIA_CELL("scrolled-in lines take the current background", "\x1b[46m\x1b[S", 3U, 0U, 0x20U, 0U, Default, 6U),
    ASTERIA_CELL("IL blanks take the current background", "\x1b[41m\x1b[L", 0U, 4U, 0x20U, 0U, Default, 1U),
    ASTERIA_CELL("DECRC restores the rendition",
                 "\x1b[1;32m\x1b"
                 "7\x1b[0m\x1b"
                 "8A",
                 0U, 0U, 'A', Bold, 2U, Default),
    ASTERIA_CELL("RIS resets the rendition",
                 "\x1b[1;32m\x1b"
                 "cA",
                 0U, 0U, 'A', 0U, Default, Default),
    ASTERIA_CELL("RIS blanks in the default colours",
                 "\x1b[44m\x1b"
                 "c",
                 2U, 2U, 0x20U, 0U, Default, Default),
};

void TestCellCases(TestContext& runner) noexcept
{
    for (const CellCase& test : CellCases)
    {
        g_terminal.Configure(10U, 4U);
        Write(g_terminal, test.Input, test.InputLength);
        const TerminalCellView cell{g_terminal.Screen().CellAt(test.Row, test.Column)};
        if (!ASTERIA_EXPECT(runner, test.Name, cell.CodePoint == test.Expected.CodePoint,
                            cell.Attributes == test.Expected.Attributes, cell.Foreground == test.Expected.Foreground,
                            cell.Background == test.Expected.Background))
        {
            static_cast<void>(std::fprintf(stderr, "  cell {0x%02x, 0x%02x, 0x%02x, 0x%02x}\n", cell.CodePoint,
                                           cell.Attributes, cell.Foreground, cell.Background));
        }
    }
}

struct ModeCase
{
    const char* Name;
    const char* Input;
    Size InputLength;
    bool ApplicationCursorKeys;
    bool Autowrap;
    bool CursorVisible;
    UInt32 MarginTop;
    UInt32 MarginBottom;
};

#define ASTERIA_MODE(name, input, keys, wrap, visible, top, bottom)                                                    \
    ModeCase                                                                                                           \
    {                                                                                                                  \
        name, input, sizeof(input) - 1U, keys, wrap, visible, top, bottom                                              \
    }

constexpr ModeCase ModeCases[]{
    ASTERIA_MODE("the defaults", "", false, true, true, 0U, 3U),
    ASTERIA_MODE("?25 l", "\x1b[?25l", false, true, false, 0U, 3U),
    ASTERIA_MODE("?25 l then h", "\x1b[?25l\x1b[?25h", false, true, true, 0U, 3U),
    ASTERIA_MODE("?1 h", "\x1b[?1h", true, true, true, 0U, 3U),
    ASTERIA_MODE("?1 h then l", "\x1b[?1h\x1b[?1l", false, true, true, 0U, 3U),
    ASTERIA_MODE("?7 l", "\x1b[?7l", false, false, true, 0U, 3U),
    ASTERIA_MODE("several modes at once", "\x1b[?1h\x1b[?7;25;1l", false, false, false, 0U, 3U),
    ASTERIA_MODE("unknown modes are ignored", "\x1b[?25;99;1049l", false, true, false, 0U, 3U),
    ASTERIA_MODE("a mode without '?' is ignored", "\x1b[25l\x1b[4h", false, true, true, 0U, 3U),
    ASTERIA_MODE("another private marker is ignored", "\x1b[>25l", false, true, true, 0U, 3U),
    ASTERIA_MODE("?7 with no parameter changes nothing", "\x1b[?l", false, true, true, 0U, 3U),
    ASTERIA_MODE("DECSTBM", "\x1b[2;3r", false, true, true, 1U, 2U),
    ASTERIA_MODE("DECSTBM clamps the bottom", "\x1b[2;99r", false, true, true, 1U, 3U),
    ASTERIA_MODE("DECSTBM defaults", "\x1b[2;3r\x1b[r", false, true, true, 0U, 3U),
    ASTERIA_MODE("DECSTBM refuses top equal to bottom", "\x1b[2;3r\x1b[3;3r", false, true, true, 1U, 2U),
    ASTERIA_MODE("DECSTBM refuses a top below the screen", "\x1b[9;99r", false, true, true, 0U, 3U),
    ASTERIA_MODE("RIS resets the modes and region",
                 "\x1b[?1h\x1b[?7;25l\x1b[2;3r\x1b"
                 "c",
                 false, true, true, 0U, 3U),
};

void TestModeCases(TestContext& runner) noexcept
{
    for (const ModeCase& test : ModeCases)
    {
        g_terminal.Configure(10U, 4U);
        Write(g_terminal, test.Input, test.InputLength);
        const auto& screen = g_terminal.Screen();
        ASTERIA_EXPECT(runner, test.Name, screen.ApplicationCursorKeys() == test.ApplicationCursorKeys,
                       screen.Autowrap() == test.Autowrap, screen.CursorVisible() == test.CursorVisible,
                       screen.MarginTop() == test.MarginTop, screen.MarginBottom() == test.MarginBottom);
    }
}

struct ReportCase
{
    const char* Name;
    UInt32 Columns;
    UInt32 Rows;
    const char* Input;
    Size InputLength;
    const char* Answer;
    Size AnswerLength;
};

#define ASTERIA_REPORT(name, columns, rows, input, answer)                                                             \
    ReportCase{name, columns, rows, input, sizeof(input) - 1U, answer, sizeof(answer) - 1U}

constexpr ReportCase ReportCases[]{
    ASTERIA_REPORT("DSR 5", 80U, 25U, "\x1b[5n", "\x1b[0n"),
    ASTERIA_REPORT("DSR 6 at home", 80U, 25U, "\x1b[6n", "\x1b[1;1R"),
    ASTERIA_REPORT("DSR 6 after CUP", 80U, 25U, "\x1b[12;34H\x1b[6n", "\x1b[12;34R"),
    ASTERIA_REPORT("DSR 6 at the largest position", 320U, 90U, "\x1b[99;999H\x1b[6n", "\x1b[90;320R"),
    ASTERIA_REPORT("DSR 6 with a pending wrap", 10U, 4U, "0123456789\x1b[6n", "\x1b[1;10R"),
    ASTERIA_REPORT("DSR 6 after a clamped CUP", 80U, 25U, "\x1b[9;100H\x1b[6n", "\x1b[9;80R"),
    ASTERIA_REPORT("DA", 80U, 25U, "\x1b[c", "\x1b[?1;2c"),
    ASTERIA_REPORT("DA 0", 80U, 25U, "\x1b[0c", "\x1b[?1;2c"),
    ASTERIA_REPORT("DA 1 is not answered", 80U, 25U, "\x1b[1c", ""),
    ASTERIA_REPORT("DA with two parameters is not answered", 80U, 25U, "\x1b[0;0c", ""),
    ASTERIA_REPORT("secondary DA is not answered", 80U, 25U, "\x1b[>c", ""),
    ASTERIA_REPORT("DECXCPR is not answered", 80U, 25U, "\x1b[?6n", ""),
    ASTERIA_REPORT("DSR 7 is not answered", 80U, 25U, "\x1b[7n", ""),
    ASTERIA_REPORT("answers queue in order", 80U, 25U, "\x1b[5n\x1b[2;3H\x1b[6n\x1b[c", "\x1b[0n\x1b[2;3R\x1b[?1;2c"),
};

void TestReportCases(TestContext& runner) noexcept
{
    for (const ReportCase& test : ReportCases)
    {
        g_terminal.Configure(test.Columns, test.Rows);
        Write(g_terminal, test.Input, test.InputLength);
        UInt8 answer[64U]{};
        const Size length{g_terminal.TakeResponses(answer, sizeof(answer))};
        ASTERIA_EXPECT(runner, test.Name, length == test.AnswerLength,
                       std::memcmp(answer, test.Answer, test.AnswerLength) == 0, g_terminal.DroppedResponses() == 0U);
    }

    // The queue holds 128 bytes; an answer that does not fit is dropped
    // whole and counted, and a partial take keeps the rest in order.
    g_terminal.Configure(80U, 25U);
    for (Size index{0U}; index < 20U; ++index)
    {
        Write(g_terminal, "\x1b[c", 3U);
    }
    UInt8 head[5U]{};
    const Size first{g_terminal.TakeResponses(head, sizeof(head))};
    UInt8 rest[200U]{};
    const Size second{g_terminal.TakeResponses(rest, sizeof(rest))};
    ASTERIA_EXPECT(runner, "a full answer queue drops whole answers", first == 5U, second == 121U,
                   g_terminal.DroppedResponses() == 2U, std::memcmp(head, "\x1b[?1;", 5U) == 0,
                   std::memcmp(rest, "2c\x1b[?1;2c", 9U) == 0, std::memcmp(rest + 114U, "\x1b[?1;2c", 7U) == 0,
                   g_terminal.TakeResponses(rest, sizeof(rest)) == 0U);
    ASTERIA_EXPECT(runner, "a null output takes nothing", g_terminal.TakeResponses(nullptr, 8U) == 0U);
    Write(g_terminal, "\x1b[5n", 4U);
    g_terminal.Configure(80U, 25U);
    ASTERIA_EXPECT(runner, "Configure discards queued answers", g_terminal.TakeResponses(rest, sizeof(rest)) == 0U);
}

void TestStreamSplitting(TestContext& runner) noexcept
{
    // Every sequence may arrive split across writes, one byte at a time.
    for (const ScreenCase& test : ScreenCases)
    {
        g_terminal.Configure(test.Columns, test.Rows);
        for (Size index{0U}; index < test.InputLength; ++index)
        {
            Write(g_terminal, test.Input + index, 1U);
        }
        const auto& screen = g_terminal.Screen();
        ASTERIA_EXPECT(runner, test.Name, GridMatches(g_terminal, test.Expected), screen.CursorRow() == test.CursorRow,
                       screen.CursorColumn() == test.CursorColumn);
    }
}

void TestUnconfigured(TestContext& runner) noexcept
{
    static Terminal fresh;
    fresh.Write(reinterpret_cast<const UInt8*>("abc\x1b[6n"), 7U);
    UInt8 answer[8U]{};
    ASTERIA_EXPECT(runner, "an unconfigured terminal ignores writes", fresh.Screen().Columns() == 0U,
                   fresh.TakeResponses(answer, sizeof(answer)) == 0U, fresh.Screen().CellAt(0U, 0U).CodePoint == 0x20U);
    g_terminal.Configure(10U, 4U);
    g_terminal.Write(nullptr, 5U);
    ASTERIA_EXPECT(runner, "a null write is ignored", g_terminal.Screen().CursorColumn() == 0U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"TerminalConformanceTests"};
    Asteria::Test::TestScreenCases(runner);
    Asteria::Test::TestCellCases(runner);
    Asteria::Test::TestModeCases(runner);
    Asteria::Test::TestReportCases(runner);
    Asteria::Test::TestStreamSplitting(runner);
    Asteria::Test::TestUnconfigured(runner);
    return runner.Finish();
}
