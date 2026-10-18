// SPDX-License-Identifier: Apache-2.0
#include "User/Terminal/Terminal.hpp"

#include <cstdio>
#include <cstring>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

// Seeded tests of the terminal over random byte streams. Each seed picks a
// small screen and writes chunks built from a grammar of text, C0 controls,
// escape and control sequences (valid, malformed and unsupported), control
// strings and UTF-8 (valid and broken). After every chunk:
//
// - a second terminal fed the same bytes one at a time must agree exactly
//   (the parser keeps no state that depends on how output is split);
// - every row whose cells changed, and the rows the drawn cursor left and
//   entered, must be damaged (the renderer redraws only damaged rows);
// - the invariants hold: the cursor and margins inside the screen, cells
//   with valid codes, colours and attributes, the answer queue bounded.
//
// A second phase drives a reference model, a separate and simpler
// implementation of the subset without a scrolling region (text, CR, LF,
// BS, HT, CUP, CUU, CUD, CUF, CUB, ED, EL, ICH, DCH, IL, DL, SU, SD, SGR
// and autowrap), and compares every cell and the cursor. A mismatch prints
// the seed, step and chunk; ASTERIA_TEST_SEED replays one seed.
namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using TestSupport::Random;
using User::Terminal::Terminal;
using User::Terminal::TerminalCellView;
using User::Terminal::TerminalDefaultColor;
using User::Terminal::TerminalRendition;
using User::Terminal::TerminalScreen;

constexpr UInt32 MaximumColumns{14U};
constexpr UInt32 MaximumRows{7U};
constexpr Size StepsPerSeed{150U};
constexpr Size ChunkCapacity{256U};

Terminal g_whole;
Terminal g_split;
TerminalCellView g_before[MaximumRows][MaximumColumns];
UInt8 g_chunk[ChunkCapacity];
Size g_length{0U};

void Append(const UInt8 byte) noexcept
{
    if (g_length < ChunkCapacity)
    {
        g_chunk[g_length++] = byte;
    }
}

void AppendText(const char* const text) noexcept
{
    for (const char* at = text; *at != '\0'; ++at)
    {
        Append(static_cast<UInt8>(*at));
    }
}

void AppendNumber(UInt64 value) noexcept
{
    char digits[24U]{};
    Size count{0U};
    do
    {
        digits[count++] = static_cast<char>('0' + (value % 10U));
        value /= 10U;
    } while ((value != 0U) && (count < sizeof(digits)));
    while (count != 0U)
    {
        Append(static_cast<UInt8>(digits[--count]));
    }
}

[[nodiscard]] UInt64 Parameter(Random& random) noexcept
{
    constexpr UInt64 Interesting[]{0U, 1U, 2U, 3U, 5U, 7U, 8U, 9U, 13U, 25U, 99U, 9999U, 10000U, 4294967296U};
    return random.Chance(3U, 4U) ? random.Below(10U) : Interesting[random.Below(sizeof(Interesting) / 8U)];
}

void AppendCsi(Random& random, const char* const finals) noexcept
{
    AppendText("\x1b[");
    if (random.Chance(1U, 10U))
    {
        Append(static_cast<UInt8>("?><="[random.Below(4U)]));
    }
    const UInt64 count{random.Below(4U)};
    for (UInt64 index{0U}; index < count; ++index)
    {
        if (index != 0U)
        {
            Append(random.Chance(1U, 30U) ? UInt8{':'} : UInt8{';'});
        }
        if (random.Chance(7U, 8U))
        {
            AppendNumber(Parameter(random));
        }
    }
    if (random.Chance(1U, 40U))
    {
        Append(static_cast<UInt8>(0x20U + random.Below(16U)));
    }
    Append(static_cast<UInt8>(finals[random.Below(std::strlen(finals))]));
}

void AppendSgr(Random& random) noexcept
{
    constexpr UInt64 Values[]{0U,  1U,  4U,  7U,  22U,  24U,  27U,  30U, 31U, 33U, 37U, 39U,  40U, 42U, 47U,
                              49U, 90U, 95U, 97U, 100U, 103U, 107U, 38U, 48U, 5U,  2U,  196U, 8U,  9U};
    AppendText("\x1b[");
    const UInt64 count{random.Below(5U)};
    for (UInt64 index{0U}; index < count; ++index)
    {
        if (index != 0U)
        {
            Append(';');
        }
        AppendNumber(Values[random.Below(sizeof(Values) / 8U)]);
    }
    Append('m');
}

// One token of the full grammar.
void AppendToken(Random& random) noexcept
{
    switch (random.Below(14U))
    {
    case 0U:
    case 1U:
    case 2U:
    {
        const UInt64 count{1U + random.Below(12U)};
        for (UInt64 index{0U}; index < count; ++index)
        {
            Append(static_cast<UInt8>(0x20U + random.Below(0x5FU)));
        }
        break;
    }
    case 3U:
    {
        constexpr UInt8 Controls[]{0x00U, 0x07U, 0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU,
                                   0x0DU, 0x0DU, 0x0AU, 0x0EU, 0x18U, 0x1AU, 0x7FU};
        Append(Controls[random.Below(sizeof(Controls))]);
        break;
    }
    case 4U:
    case 5U:
        AppendCsi(random, "@ABCDEFGHJKLMPSTXcdfmnrhlz");
        break;
    case 6U:
        AppendSgr(random);
        break;
    case 7U:
        Append(0x1BU);
        if (random.Chance(1U, 6U))
        {
            Append(static_cast<UInt8>(0x20U + random.Below(16U)));
        }
        Append(static_cast<UInt8>("78DMEcZ\\("[random.Below(9U)]));
        break;
    case 8U:
    {
        constexpr const char* Strings[]{"\x1b]0;title\x07",  "\x1b]2;x\x1b\\", "\x1bPq#1\x07more\x1b\\",
                                        "\x1bXa\x1b\\",      "\x1b^b\x1b\\",   "\x1b_c\x1b\\",
                                        "\x1b]unterminated", "\x1b]a\x1b[1C"};
        AppendText(Strings[random.Below(sizeof(Strings) / sizeof(Strings[0U]))]);
        break;
    }
    case 9U:
    {
        constexpr const char* Characters[]{"\xc3\xa9",         "\xe2\x94\x80", "\xe2\x98\xba", "\xe2\x82\xac",
                                           "\xf0\x9f\x98\x80", "\xc2\x85",     "\xc2\xa0",     "\xe2\x96\x88"};
        AppendText(Characters[random.Below(sizeof(Characters) / sizeof(Characters[0U]))]);
        break;
    }
    case 10U:
    {
        const UInt64 count{1U + random.Below(3U)};
        for (UInt64 index{0U}; index < count; ++index)
        {
            Append(static_cast<UInt8>(0x80U + random.Below(0x80U)));
        }
        break;
    }
    case 11U:
        AppendText("\x1b[");
        AppendNumber(random.Below(MaximumRows + 2U));
        Append(';');
        AppendNumber(random.Below(MaximumRows + 2U));
        Append('r');
        break;
    case 12U:
        AppendText(random.Chance(1U, 2U) ? "\x1b[?" : "\x1b[");
        AppendNumber(random.Chance(1U, 3U) ? 7U : (random.Chance(1U, 2U) ? 25U : 1U));
        Append(random.Chance(1U, 2U) ? UInt8{'h'} : UInt8{'l'});
        break;
    default:
        AppendText(random.Chance(1U, 2U) ? "\r\n" : "\n");
        break;
    }
}

[[nodiscard]] bool ValidColor(const UInt8 color) noexcept
{
    return (color < 16U) || (color == TerminalDefaultColor);
}

[[nodiscard]] bool SameScreen(const TerminalScreen& first, const TerminalScreen& second) noexcept
{
    if ((first.CursorRow() != second.CursorRow()) || (first.CursorColumn() != second.CursorColumn()) ||
        (first.WrapPending() != second.WrapPending()) || (first.Autowrap() != second.Autowrap()) ||
        (first.CursorVisible() != second.CursorVisible()) ||
        (first.ApplicationCursorKeys() != second.ApplicationCursorKeys()) ||
        (first.MarginTop() != second.MarginTop()) || (first.MarginBottom() != second.MarginBottom()))
    {
        return false;
    }
    const TerminalRendition a{first.Rendition()};
    const TerminalRendition b{second.Rendition()};
    if ((a.Attributes != b.Attributes) || (a.Foreground != b.Foreground) || (a.Background != b.Background))
    {
        return false;
    }
    for (UInt32 row{0U}; row < first.Rows(); ++row)
    {
        for (UInt32 column{0U}; column < first.Columns(); ++column)
        {
            const TerminalCellView x{first.CellAt(row, column)};
            const TerminalCellView y{second.CellAt(row, column)};
            if ((x.CodePoint != y.CodePoint) || (x.Attributes != y.Attributes) || (x.Foreground != y.Foreground) ||
                (x.Background != y.Background))
            {
                return false;
            }
        }
    }
    return true;
}

void Snapshot(const TerminalScreen& screen) noexcept
{
    for (UInt32 row{0U}; row < screen.Rows(); ++row)
    {
        for (UInt32 column{0U}; column < screen.Columns(); ++column)
        {
            g_before[row][column] = screen.CellAt(row, column);
        }
    }
}

/// Every changed row is damaged; empty when so, else the first offending row + 1.
[[nodiscard]] UInt32 UndamagedChange(const TerminalScreen& screen, const UInt32 cursorRow, const UInt32 cursorColumn,
                                     const bool cursorVisible) noexcept
{
    for (UInt32 row{0U}; row < screen.Rows(); ++row)
    {
        bool changed{false};
        for (UInt32 column{0U}; column < screen.Columns(); ++column)
        {
            const TerminalCellView cell{screen.CellAt(row, column)};
            const TerminalCellView old{g_before[row][column]};
            changed = changed || (cell.CodePoint != old.CodePoint) || (cell.Attributes != old.Attributes) ||
                      (cell.Foreground != old.Foreground) || (cell.Background != old.Background);
        }
        const bool cursorMoved{
            (cursorVisible != screen.CursorVisible()) ||
            (screen.CursorVisible() && ((cursorRow != screen.CursorRow()) || (cursorColumn != screen.CursorColumn())))};
        const bool cursorRowNow{cursorMoved && ((cursorVisible && (row == cursorRow)) ||
                                                (screen.CursorVisible() && (row == screen.CursorRow())))};
        if ((changed || cursorRowNow) && !screen.IsRowDamaged(row))
        {
            return row + 1U;
        }
    }
    return 0U;
}

[[nodiscard]] bool Invariants(const TerminalScreen& screen) noexcept
{
    bool valid{(screen.CursorRow() < screen.Rows()) && (screen.CursorColumn() < screen.Columns()) &&
               (screen.MarginTop() <= screen.MarginBottom()) && (screen.MarginBottom() < screen.Rows()) &&
               ((screen.MarginTop() < screen.MarginBottom()) || (screen.Rows() == 1U)) &&
               (!screen.WrapPending() || (screen.CursorColumn() == (screen.Columns() - 1U)))};
    for (UInt32 row{0U}; row < screen.Rows(); ++row)
    {
        for (UInt32 column{0U}; column < screen.Columns(); ++column)
        {
            const TerminalCellView cell{screen.CellAt(row, column)};
            valid = valid && (cell.CodePoint != 0U) && ((cell.Attributes & ~0x07U) == 0U) &&
                    ValidColor(cell.Foreground) && ValidColor(cell.Background);
        }
    }
    const TerminalRendition rendition{screen.Rendition()};
    return valid && ((rendition.Attributes & ~0x07U) == 0U) && ValidColor(rendition.Foreground) &&
           ValidColor(rendition.Background);
}

void PrintChunk() noexcept
{
    static_cast<void>(std::fprintf(stderr, "  chunk:"));
    for (Size index{0U}; index < g_length; ++index)
    {
        static_cast<void>(std::fprintf(stderr, " %02x", g_chunk[index]));
    }
    static_cast<void>(std::fprintf(stderr, "\n"));
}

[[nodiscard]] bool RunGrammarSeed(TestContext& runner, const UInt64 seed) noexcept
{
    Random random{seed};
    const UInt32 columns{static_cast<UInt32>(1U + random.Below(MaximumColumns))};
    const UInt32 rows{static_cast<UInt32>(1U + random.Below(MaximumRows))};
    g_whole.Configure(columns, rows);
    g_split.Configure(columns, rows);
    for (Size step{0U}; step < StepsPerSeed; ++step)
    {
        g_length = 0U;
        const UInt64 tokens{1U + random.Below(8U)};
        for (UInt64 token{0U}; token < tokens; ++token)
        {
            AppendToken(random);
        }
        const TerminalScreen& screen = g_whole.Screen();
        Snapshot(screen);
        const UInt32 cursorRow{screen.CursorRow()};
        const UInt32 cursorColumn{screen.CursorColumn()};
        const bool cursorVisible{screen.CursorVisible()};
        g_whole.Write(g_chunk, g_length);
        for (Size index{0U}; index < g_length; ++index)
        {
            g_split.Write(g_chunk + index, 1U);
        }
        UInt8 first[160U]{};
        UInt8 second[160U]{};
        const Size firstCount{g_whole.TakeResponses(first, sizeof(first))};
        const Size secondCount{g_split.TakeResponses(second, sizeof(second))};
        const UInt32 undamaged{UndamagedChange(screen, cursorRow, cursorColumn, cursorVisible)};
        const bool same{SameScreen(screen, g_split.Screen()) && (firstCount == secondCount) &&
                        (std::memcmp(first, second, firstCount) == 0)};
        const bool valid{Invariants(screen) && (firstCount <= User::Terminal::TerminalResponseCapacity)};
        if (!same || (undamaged != 0U) || !valid)
        {
            static_cast<void>(std::fprintf(stderr, "seed %llu step %zu (%ux%u): same %d, undamaged row %u, valid %d\n",
                                           static_cast<unsigned long long>(seed), step, columns, rows, same ? 1 : 0,
                                           undamaged, valid ? 1 : 0));
            PrintChunk();
            ASTERIA_FAIL(runner, "the terminal diverged from the split stream, its damage or its invariants");
            return false;
        }
        g_whole.ClearDamage();
        g_split.ClearDamage();
        if (random.Chance(1U, 40U))
        {
            const UInt32 newColumns{static_cast<UInt32>(1U + random.Below(MaximumColumns))};
            const UInt32 newRows{static_cast<UInt32>(1U + random.Below(MaximumRows))};
            g_whole.Resize(newColumns, newRows);
            g_split.Resize(newColumns, newRows);
            g_whole.ClearDamage();
            g_split.ClearDamage();
        }
    }
    return true;
}

/// The reference model: the subset on a whole-screen region, written plainly.
class Reference final
{
public:
    void Configure(const UInt32 columns, const UInt32 rows) noexcept
    {
        m_columns = columns;
        m_rows = rows;
        m_row = 0U;
        m_column = 0U;
        m_pending = false;
        m_autowrap = true;
        m_rendition = TerminalRendition{0U, TerminalDefaultColor, TerminalDefaultColor};
        for (UInt32 row{0U}; row < rows; ++row)
        {
            for (UInt32 column{0U}; column < columns; ++column)
            {
                m_cells[row][column] = TerminalCellView{0x20U, 0U, TerminalDefaultColor, TerminalDefaultColor};
            }
        }
    }

    void Print(const UInt8 code) noexcept
    {
        if (m_pending && m_autowrap)
        {
            m_column = 0U;
            LineFeed();
        }
        m_pending = false;
        m_cells[m_row][m_column] =
            TerminalCellView{code, m_rendition.Attributes, m_rendition.Foreground, m_rendition.Background};
        if ((m_column + 1U) == m_columns)
        {
            m_pending = m_autowrap;
        }
        else
        {
            ++m_column;
        }
    }

    void LineFeed() noexcept
    {
        m_pending = false;
        if ((m_row + 1U) == m_rows)
        {
            Shift(0U, 1U, true);
        }
        else
        {
            ++m_row;
        }
    }

    void Control(const UInt8 byte) noexcept
    {
        if ((byte == '\n') || (byte == 0x0BU) || (byte == 0x0CU))
        {
            LineFeed();
            return;
        }
        m_pending = false;
        if (byte == '\r')
        {
            m_column = 0U;
        }
        else if (byte == '\b')
        {
            m_column = (m_column == 0U) ? 0U : (m_column - 1U);
        }
        else if (byte == '\t')
        {
            const UInt32 stop{((m_column / 8U) + 1U) * 8U};
            m_column = (stop < m_columns) ? stop : (m_columns - 1U);
        }
    }

    /// A control sequence of the subset: final, first and second parameter
    /// (zero when absent).
    void Sequence(const char final, const UInt32 first, const UInt32 second, const UInt32* const all,
                  const Size count) noexcept
    {
        const UInt32 n{(first == 0U) ? 1U : first};
        const UInt32 lastColumn{m_columns - 1U};
        const UInt32 lastRow{m_rows - 1U};
        const TerminalCellView blank{0x20U, 0U, TerminalDefaultColor, m_rendition.Background};
        switch (final)
        {
        case 'H':
            m_pending = false;
            m_row = (n - 1U < lastRow) ? (n - 1U) : lastRow;
            m_column = (((second == 0U) ? 0U : (second - 1U)) < lastColumn) ? ((second == 0U) ? 0U : (second - 1U))
                                                                            : lastColumn;
            break;
        case 'A':
            m_pending = false;
            m_row = (m_row > n) ? (m_row - n) : 0U;
            break;
        case 'B':
            m_pending = false;
            m_row = ((lastRow - m_row) > n) ? (m_row + n) : lastRow;
            break;
        case 'C':
            m_pending = false;
            m_column = ((lastColumn - m_column) > n) ? (m_column + n) : lastColumn;
            break;
        case 'D':
            m_pending = false;
            m_column = (m_column > n) ? (m_column - n) : 0U;
            break;
        case 'K':
        case 'J':
            for (UInt32 row{0U}; row < m_rows; ++row)
            {
                for (UInt32 column{0U}; column < m_columns; ++column)
                {
                    const bool before{(row < m_row) || ((row == m_row) && (column <= m_column))};
                    const bool after{(row > m_row) || ((row == m_row) && (column >= m_column))};
                    const bool line{row == m_row};
                    const bool erase{
                        (final == 'J')
                            ? (((first == 0U) && after) || ((first == 1U) && before) || (first == 2U))
                            : (line && (((first == 0U) && after) || ((first == 1U) && before) || (first == 2U)))};
                    if (erase)
                    {
                        m_cells[row][column] = blank;
                    }
                }
            }
            break;
        case '@':
        case 'P':
        {
            m_pending = false;
            TerminalCellView line[MaximumColumns]{};
            for (UInt32 column{0U}; column < m_columns; ++column)
            {
                line[column] = m_cells[m_row][column];
            }
            for (UInt32 column{m_column}; column < m_columns; ++column)
            {
                const UInt64 from{(final == '@') ? (static_cast<UInt64>(column) - n)
                                                 : (static_cast<UInt64>(column) + n)};
                const bool inside{(final == '@') ? ((column >= m_column + n) && (column >= n)) : (from < m_columns)};
                m_cells[m_row][column] = inside ? line[from] : blank;
            }
            break;
        }
        case 'L':
        case 'M':
            m_pending = false;
            for (UInt32 step{0U}; (step < n) && (step < m_rows); ++step)
            {
                Shift(m_row, 1U, final == 'M');
            }
            m_column = 0U;
            break;
        case 'S':
        case 'T':
            for (UInt32 step{0U}; (step < n) && (step < m_rows); ++step)
            {
                Shift(0U, 1U, final == 'S');
            }
            break;
        case 'm':
            Rendition(all, count);
            break;
        default:
            break;
        }
    }

    void SetAutowrap(const bool on) noexcept
    {
        m_autowrap = on;
    }

    [[nodiscard]] bool Matches(const TerminalScreen& screen) const noexcept
    {
        bool same{(screen.CursorRow() == m_row) && (screen.CursorColumn() == m_column) &&
                  (screen.WrapPending() == m_pending)};
        for (UInt32 row{0U}; row < m_rows; ++row)
        {
            for (UInt32 column{0U}; column < m_columns; ++column)
            {
                const TerminalCellView a{screen.CellAt(row, column)};
                const TerminalCellView b{m_cells[row][column]};
                same = same && (a.CodePoint == b.CodePoint) && (a.Attributes == b.Attributes) &&
                       (a.Foreground == b.Foreground) && (a.Background == b.Background);
            }
        }
        return same;
    }

private:
    /// Removes one line at first (up: lines below move up, a blank enters at
    /// the bottom) or inserts one there (down).
    void Shift(const UInt32 first, const UInt32, const bool up) noexcept
    {
        const TerminalCellView blank{0x20U, 0U, TerminalDefaultColor, m_rendition.Background};
        if (up)
        {
            for (UInt32 row{first}; (row + 1U) < m_rows; ++row)
            {
                for (UInt32 column{0U}; column < m_columns; ++column)
                {
                    m_cells[row][column] = m_cells[row + 1U][column];
                }
            }
            for (UInt32 column{0U}; column < m_columns; ++column)
            {
                m_cells[m_rows - 1U][column] = blank;
            }
            return;
        }
        for (UInt32 row{m_rows - 1U}; row > first; --row)
        {
            for (UInt32 column{0U}; column < m_columns; ++column)
            {
                m_cells[row][column] = m_cells[row - 1U][column];
            }
        }
        for (UInt32 column{0U}; column < m_columns; ++column)
        {
            m_cells[first][column] = blank;
        }
    }

    void Rendition(const UInt32* const all, const Size count) noexcept
    {
        if (count == 0U)
        {
            m_rendition = TerminalRendition{0U, TerminalDefaultColor, TerminalDefaultColor};
            return;
        }
        for (Size index{0U}; index < count; ++index)
        {
            const UInt32 value{all[index]};
            if (value == 0U)
            {
                m_rendition = TerminalRendition{0U, TerminalDefaultColor, TerminalDefaultColor};
            }
            else if (value == 1U)
            {
                m_rendition.Attributes = static_cast<UInt8>(m_rendition.Attributes | 1U);
            }
            else if (value == 7U)
            {
                m_rendition.Attributes = static_cast<UInt8>(m_rendition.Attributes | 4U);
            }
            else if ((value >= 30U) && (value <= 37U))
            {
                m_rendition.Foreground = static_cast<UInt8>(value - 30U);
            }
            else if ((value >= 40U) && (value <= 47U))
            {
                m_rendition.Background = static_cast<UInt8>(value - 40U);
            }
            else if (value == 39U)
            {
                m_rendition.Foreground = TerminalDefaultColor;
            }
            else if (value == 49U)
            {
                m_rendition.Background = TerminalDefaultColor;
            }
        }
    }

    TerminalCellView m_cells[MaximumRows][MaximumColumns]{};
    UInt32 m_columns{0U};
    UInt32 m_rows{0U};
    UInt32 m_row{0U};
    UInt32 m_column{0U};
    bool m_pending{false};
    bool m_autowrap{true};
    TerminalRendition m_rendition{};
};

Reference g_reference;

[[nodiscard]] bool RunReferenceSeed(TestContext& runner, const UInt64 seed) noexcept
{
    Random random{seed ^ 0x5EED'7E57U};
    const UInt32 columns{static_cast<UInt32>(1U + random.Below(MaximumColumns))};
    const UInt32 rows{static_cast<UInt32>(1U + random.Below(MaximumRows))};
    g_whole.Configure(columns, rows);
    g_reference.Configure(columns, rows);
    constexpr char Finals[]{"HABCDKJ@PLMSTm"};
    for (Size step{0U}; step < StepsPerSeed; ++step)
    {
        g_length = 0U;
        const UInt64 kind{random.Below(8U)};
        if (kind < 3U)
        {
            const UInt8 code{static_cast<UInt8>(0x21U + random.Below(0x5EU))};
            Append(code);
            g_reference.Print(code);
        }
        else if (kind == 3U)
        {
            constexpr UInt8 Controls[]{'\r', '\n', '\b', '\t', 0x0BU, 0x0CU};
            const UInt8 control{Controls[random.Below(sizeof(Controls))]};
            Append(control);
            g_reference.Control(control);
        }
        else if (kind == 4U)
        {
            const bool on{random.Chance(1U, 2U)};
            AppendText(on ? "\x1b[?7h" : "\x1b[?7l");
            g_reference.SetAutowrap(on);
        }
        else
        {
            const char final{Finals[random.Below(sizeof(Finals) - 1U)]};
            UInt32 parameters[4U]{};
            const Size count{(final == 'H') ? 2U : ((final == 'm') ? random.Below(4U) : random.Below(2U))};
            constexpr UInt32 Sgr[]{0U, 1U, 7U, 30U, 33U, 37U, 39U, 40U, 44U, 47U, 49U};
            AppendText("\x1b[");
            for (Size index{0U}; index < count; ++index)
            {
                if (index != 0U)
                {
                    Append(';');
                }
                parameters[index] = (final == 'm') ? Sgr[random.Below(sizeof(Sgr) / sizeof(Sgr[0U]))]
                                    : ((final == 'J') || (final == 'K'))
                                        ? static_cast<UInt32>(random.Below(3U))
                                        : static_cast<UInt32>(random.Below(MaximumColumns + 2U));
                AppendNumber(parameters[index]);
            }
            Append(static_cast<UInt8>(final));
            g_reference.Sequence(final, parameters[0U], parameters[1U], parameters, count);
        }
        g_whole.Write(g_chunk, g_length);
        if (!g_reference.Matches(g_whole.Screen()))
        {
            static_cast<void>(std::fprintf(stderr, "seed %llu step %zu (%ux%u): the reference model differs\n",
                                           static_cast<unsigned long long>(seed), step, columns, rows));
            PrintChunk();
            ASTERIA_FAIL(runner, "the terminal differs from the reference model");
            return false;
        }
    }
    return true;
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"TerminalModelTests"};
    const Asteria::TestSupport::SeedRange seeds = Asteria::TestSupport::GetSeedRange(1U, 200U);
    for (Asteria::Foundation::UInt64 seed = seeds.First; seed < seeds.First + seeds.Count; ++seed)
    {
        const bool grammar{Asteria::Test::RunGrammarSeed(runner, seed)};
        const bool reference{Asteria::Test::RunReferenceSeed(runner, seed)};
        ASTERIA_EXPECT(runner, "the seed's grammar and reference phases passed", grammar, reference);
    }
    return runner.Finish();
}
