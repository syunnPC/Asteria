// SPDX-License-Identifier: Apache-2.0
#include "User/Terminal/Terminal.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// The terminal's parser and screen over arbitrary output. The first two
// bytes choose the grid (1-40 columns, 1-12 rows), the rest is written. The
// properties: no undefined behaviour; the cursor, margins and every cell
// stay valid; the answer queue stays bounded; and the same bytes written in
// one call and one byte at a time leave identical terminals.
namespace
{
namespace T = Asteria::User::Terminal;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt8;

T::Terminal g_whole;
T::Terminal g_split;

[[nodiscard]] bool ValidColor(const UInt8 color) noexcept
{
    return (color < 16U) || (color == T::TerminalDefaultColor);
}

[[nodiscard]] bool Same(const T::TerminalScreen& first, const T::TerminalScreen& second) noexcept
{
    if ((first.CursorRow() != second.CursorRow()) || (first.CursorColumn() != second.CursorColumn()) ||
        (first.WrapPending() != second.WrapPending()) || (first.MarginTop() != second.MarginTop()) ||
        (first.MarginBottom() != second.MarginBottom()) || (first.Autowrap() != second.Autowrap()) ||
        (first.CursorVisible() != second.CursorVisible()) ||
        (first.ApplicationCursorKeys() != second.ApplicationCursorKeys()))
    {
        return false;
    }
    for (UInt32 row{0U}; row < first.Rows(); ++row)
    {
        for (UInt32 column{0U}; column < first.Columns(); ++column)
        {
            const T::TerminalCellView a{first.CellAt(row, column)};
            const T::TerminalCellView b{second.CellAt(row, column)};
            if ((a.CodePoint != b.CodePoint) || (a.Attributes != b.Attributes) || (a.Foreground != b.Foreground) ||
                (a.Background != b.Background) || !Asteria::User::Text::IsScalar(a.CodePoint) ||
                ((a.Attributes & ~0x07U) != 0U) || !ValidColor(a.Foreground) || !ValidColor(a.Background))
            {
                return false;
            }
        }
    }
    return true;
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 2U)
    {
        return 0;
    }
    const UInt32 columns{1U + (data[0U] % 40U)};
    const UInt32 rows{1U + (data[1U] % 12U)};
    g_whole.Configure(columns, rows);
    g_split.Configure(columns, rows);
    g_whole.Write(data + 2U, size - 2U);
    for (Size index{2U}; index < size; ++index)
    {
        g_split.Write(data + index, 1U);
    }
    const T::TerminalScreen& screen = g_whole.Screen();
    if ((screen.Columns() != columns) || (screen.Rows() != rows) || (screen.CursorRow() >= rows) ||
        (screen.CursorColumn() >= columns) || (screen.MarginBottom() >= rows) ||
        (screen.MarginTop() > screen.MarginBottom()) || !Same(screen, g_split.Screen()))
    {
        std::abort();
    }
    UInt8 first[T::TerminalResponseCapacity + 8U]{};
    UInt8 second[T::TerminalResponseCapacity + 8U]{};
    const Size firstCount{g_whole.TakeResponses(first, sizeof(first))};
    const Size secondCount{g_split.TakeResponses(second, sizeof(second))};
    if ((firstCount > T::TerminalResponseCapacity) || (firstCount != secondCount) ||
        (std::memcmp(first, second, firstCount) != 0) || (g_whole.DroppedResponses() != g_split.DroppedResponses()))
    {
        std::abort();
    }
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*emit)(const std::uint8_t*, std::size_t))
{
    static constexpr const char* Seeds[]{
        "\x09\x03"
        "hello, world\r\n\x1b[1;31mred\x1b[0m",
        "\x4f\x18"
        "\x1b[2;3r\x1b[5;1H\x1b[2L\x1b[3M\x1b[S\x1b[T\x1b"
        "D\x1bM\x1b"
        "E",
        "\x09\x03"
        "0123456789abc\x1b[?7l0123456789\x1b[?25l\x1b[?1h",
        "\x13\x05"
        "\x1b[6n\x1b[5n\x1b[c\x1b[0c\x1b[>c",
        "\x09\x03"
        "\x1b]0;title\x07\x1bPq\x1b\\\x1bXa\x1b\\\x1b^b\x1b\\\x1b_c\x1b\\",
        "\x09\x03"
        "\xc3\xa9\xe2\x94\x80\xf0\x9f\x98\x80\xed\xa0\x80\xc0\xaf\xc2\x85",
        "\x09\x03"
        "\x1b[38;5;196;48;2;1;2;3;1;4;7m\x1b[1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1m",
        "\x09\x03"
        "abc\x1b[2@\x1b[2P\x1b[3X\x1b[J\x1b[1K\x1b[2J\x1b"
        "7\x1b[3;3H\x1b"
        "8\x1b"
        "c",
    };
    for (const char* seed : Seeds)
    {
        emit(reinterpret_cast<const std::uint8_t*>(seed), std::strlen(seed));
    }
}
