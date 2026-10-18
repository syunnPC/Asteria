// SPDX-License-Identifier: Apache-2.0

#include "User/Terminal/Terminal.hpp"

namespace Asteria::User::Terminal
{
void Terminal::Configure(const Foundation::UInt32 columns, const Foundation::UInt32 rows) noexcept
{
    m_screen.Configure(columns, rows);
    m_parser.Reset();
    m_responses.Clear();
}

void Terminal::Resize(const Foundation::UInt32 columns, const Foundation::UInt32 rows) noexcept
{
    m_screen.Resize(columns, rows);
}

void Terminal::Write(const Foundation::UInt8* const bytes, const Foundation::Size count) noexcept
{
    if ((bytes == nullptr) || (m_screen.Columns() == 0U))
    {
        return;
    }
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        m_parser.Feed(bytes[index], m_screen, m_responses);
    }
}

Foundation::Size Terminal::TakeResponses(Foundation::UInt8* const out, const Foundation::Size capacity) noexcept
{
    return m_responses.Take(out, capacity);
}

Foundation::UInt64 Terminal::DroppedResponses() const noexcept
{
    return m_responses.Dropped();
}

const TerminalScreen& Terminal::Screen() const noexcept
{
    return m_screen;
}

void Terminal::ClearDamage() noexcept
{
    m_screen.ClearDamage();
}
} // namespace Asteria::User::Terminal
