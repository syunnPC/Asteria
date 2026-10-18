// SPDX-License-Identifier: Apache-2.0

#include "User/Toolkit/Controls.hpp"

namespace Asteria::User::Toolkit
{
TextBox::~TextBox() noexcept
{
    if (m_caretTimer != 0U)
    {
        static_cast<void>(m_application.StopTimer(m_caretTimer));
    }
}
bool TextBox::UiAcceptText(TextView text) const noexcept
{
    for (UInt32 i{}; i < text.Length; ++i)
    {
        const UInt8 byte{static_cast<UInt8>(text.Data[i])};
        if (byte < 0x20U && !(m_multiline && byte == 0x0AU))
        {
            return false;
        }
        if (byte == 0x7FU)
        {
            return false;
        }
    }
    return true;
}
bool TextBox::UiBoundary(UInt32 index) const noexcept
{
    return index <= m_textLength && (index == m_textLength || (static_cast<UInt8>(m_text[index]) & 0xC0U) != 0x80U);
}
UInt32 TextBox::UiPrevious(UInt32 index) const noexcept
{
    if (index == 0U)
    {
        return 0U;
    }
    --index;
    while (index > 0U && !UiBoundary(index))
    {
        --index;
    }
    return index;
}
UInt32 TextBox::UiNext(UInt32 index) const noexcept
{
    if (index == m_textLength)
    {
        return index;
    }
    ++index;
    while (index < m_textLength && !UiBoundary(index))
    {
        ++index;
    }
    return index;
}
void TextBox::UiTextChanged() noexcept
{
    m_caret = Min(m_caret, m_textLength);
    m_selection = Min(m_selection, m_textLength);
    while (!UiBoundary(m_caret))
    {
        --m_caret;
    }
    while (!UiBoundary(m_selection))
    {
        --m_selection;
    }
    m_caretOn = true;
}
Status TextBox::Select(UInt32 start, UInt32 end) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (!UiBoundary(start) || !UiBoundary(end))
            {
                return Status::Invalid;
            }
            m_selection = start;
            m_caret = end;
            m_caretOn = true;
            UiInvalidate();
            return Status::Ok;
        });
}
Status TextBox::Selection(UInt32& start, UInt32& end) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            start = m_selection;
            end = m_caret;
            return Status::Ok;
        });
}
Status TextBox::SetPassword(bool password) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            m_password = password;
            UiInvalidate();
            return Status::Ok;
        });
}
Status TextBox::SetMultiline(bool multiline) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            if (!multiline)
            {
                for (UInt32 i{}; i < m_textLength; ++i)
                {
                    if (m_text[i] == '\n')
                    {
                        return Status::Invalid;
                    }
                }
            }
            m_multiline = multiline;
            UiInvalidate();
            return Status::Ok;
        });
}
Status TextBox::SetReadOnly(bool readOnly) noexcept
{
    return m_application.Call(
        [&]() noexcept
        {
            m_readOnly = readOnly;
            UiInvalidate();
            return Status::Ok;
        });
}
void TextBox::UiMove(UInt32 index, bool shift) noexcept
{
    m_caret = index;
    if (!shift)
    {
        m_selection = index;
    }
    m_caretOn = true;
    UiInvalidate();
}
void TextBox::UiEdit(UInt32 scalar, bool erase) noexcept
{
    if (m_readOnly)
    {
        return;
    }
    if (!erase && !User::Text::IsPrintable(scalar) && !(m_multiline && scalar == 0x0AU))
    {
        return;
    }
    UInt8 encoded[4U]{};
    const UInt32 count{erase ? 0U : static_cast<UInt32>(User::Text::EncodeUtf8(scalar, encoded))};
    const UInt32 first{Min(m_caret, m_selection)};
    const UInt32 last{m_caret > m_selection ? m_caret : m_selection};
    if (m_textLength - (last - first) + count > MaximumText || (erase && first == last))
    {
        return;
    }
    __builtin_memmove(m_text + first + count, m_text + last, m_textLength - last + 1U);
    __builtin_memcpy(m_text + first, encoded, count);
    m_textLength = m_textLength - (last - first) + count;
    m_caret = first + count;
    m_selection = m_caret;
    m_caretOn = true;
    UiInvalidate();
    Event changed{EventKind::TextChanged};
    UiEmit(changed);
}
UInt32 TextBox::UiLineStart(UInt32 index) const noexcept
{
    while (index > 0U && m_text[index - 1U] != '\n')
    {
        --index;
    }
    return index;
}
UInt32 TextBox::UiWidth(UInt32 start, UInt32 end) noexcept
{
    Graphics graphics{{}, {}, m_application.UiTheme()};
    if (!m_password)
    {
        return graphics.Measure({m_text + start, end - start});
    }
    UInt8 encoded[4U]{};
    const UInt32 bytes{static_cast<UInt32>(User::Text::EncodeUtf8(m_application.UiTheme().PasswordScalar, encoded))};
    const UInt32 width{graphics.Measure({reinterpret_cast<const char*>(encoded), bytes})};
    UInt32 count{};
    for (UInt32 at{start}; at < end; at = UiNext(at))
    {
        ++count;
    }
    return count * width;
}
UInt32 TextBox::UiIndexAt(Point point) noexcept
{
    const auto& theme{m_application.UiTheme()};
    const Rect inner{Inset(UiBounds(), theme.Padding)};
    const Int32 localX{point.X - inner.X + static_cast<Int32>(m_scrollX)};
    const Int32 localY{point.Y - inner.Y + static_cast<Int32>(m_scrollY)};
    const UInt32 row{
        m_multiline ? static_cast<UInt32>(Clamp(localY, 0, static_cast<Int32>(MaximumAxis))) / theme.LineHeight : 0U};
    UInt32 start{};
    UInt32 currentRow{};
    while (start < m_textLength && currentRow < row)
    {
        if (m_text[start++] == '\n')
        {
            ++currentRow;
        }
    }
    UInt32 at{start};
    while (at < m_textLength && m_text[at] != '\n')
    {
        const UInt32 next{UiNext(at)};
        const UInt32 left{UiWidth(start, at)};
        const UInt32 right{UiWidth(start, next)};
        if (localX < static_cast<Int32>((left + right) / 2U))
        {
            break;
        }
        at = next;
    }
    return at;
}
void TextBox::UiDraw(Graphics& graphics, Rect bounds) noexcept
{
    const auto& theme{graphics.Palette()};
    graphics.FillRect(bounds, UiEffective() && !m_readOnly ? theme.Window : theme.Face);
    graphics.Bevel(bounds, true);
    const Rect inner{Inset(bounds, theme.Padding)};
    UInt32 caretRow{};
    for (UInt32 i{}; i < m_caret; ++i)
    {
        if (m_text[i] == '\n')
        {
            ++caretRow;
        }
    }
    const UInt32 caretX{UiWidth(UiLineStart(m_caret), m_caret)};
    if (caretX < m_scrollX)
    {
        m_scrollX = caretX;
    }
    if (caretX + theme.CaretWidth > m_scrollX + inner.Width)
    {
        m_scrollX = caretX + theme.CaretWidth - Min(caretX + theme.CaretWidth, inner.Width);
    }
    const UInt32 caretY{caretRow * theme.LineHeight};
    if (caretY < m_scrollY)
    {
        m_scrollY = caretY;
    }
    if (caretY + theme.LineHeight > m_scrollY + inner.Height)
    {
        m_scrollY = caretY + theme.LineHeight - Min(caretY + theme.LineHeight, inner.Height);
    }
    const Rect oldClip{graphics.Clip()};
    graphics.SetClip(User::Graphics::Intersect(oldClip, inner));
    UInt32 lineStart{};
    UInt32 row{};
    for (UInt32 work{}; work <= MaximumText; ++work)
    {
        UInt32 end{lineStart};
        while (end < m_textLength && m_text[end] != '\n')
        {
            ++end;
        }
        const Int32 y{inner.Y + static_cast<Int32>(row * theme.LineHeight) - static_cast<Int32>(m_scrollY)};
        const Int32 x{inner.X - static_cast<Int32>(m_scrollX)};
        const UInt32 selectedStart{Min(m_caret, m_selection)},
            selectedEnd{m_caret > m_selection ? m_caret : m_selection};
        const UInt32 first{selectedStart > lineStart ? Min(selectedStart, end) : lineStart};
        const UInt32 last{selectedEnd < end ? (selectedEnd > lineStart ? selectedEnd : lineStart) : end};
        const UInt32 left{UiWidth(lineStart, first)};
        const UInt32 right{UiWidth(lineStart, last)};
        if (first < last)
        {
            graphics.FillRect({x + static_cast<Int32>(left), y, right - left, theme.LineHeight}, theme.Selection);
        }
        if (!m_password)
        {
            const Color color{UiEffective() ? theme.Text : theme.DisabledText};
            graphics.Text({m_text + lineStart, end - lineStart}, {x, y, UiWidth(lineStart, end), theme.LineHeight},
                          color, Align::Start, false);
            if (first < last)
            {
                const Rect previous{graphics.Clip()};
                graphics.SetClip(User::Graphics::Intersect(
                    previous, {x + static_cast<Int32>(left), y, right - left, theme.LineHeight}));
                graphics.Text({m_text + lineStart, end - lineStart}, {x, y, UiWidth(lineStart, end), theme.LineHeight},
                              theme.SelectedText, Align::Start, false);
                graphics.SetClip(previous);
            }
        }
        else
        {
            UInt8 encoded[4U]{};
            const UInt32 bytes{static_cast<UInt32>(User::Text::EncodeUtf8(theme.PasswordScalar, encoded))};
            for (UInt32 at{lineStart}; at < end; at = UiNext(at))
            {
                const UInt32 width{UiWidth(at, UiNext(at))};
                graphics.Text({reinterpret_cast<const char*>(encoded), bytes},
                              {x + static_cast<Int32>(UiWidth(lineStart, at)), y, width, theme.LineHeight},
                              !UiEffective() ? theme.DisabledText
                                             : (at >= first && at < last ? theme.SelectedText : theme.Text),
                              Align::Start, false);
            }
        }
        if (end == m_textLength)
        {
            break;
        }
        lineStart = end + 1U;
        ++row;
    }
    if (m_application.UiFocus() == this && m_caretOn)
    {
        graphics.FillRect({inner.X + static_cast<Int32>(caretX) - static_cast<Int32>(m_scrollX),
                           inner.Y + static_cast<Int32>(caretY) - static_cast<Int32>(m_scrollY), theme.CaretWidth,
                           theme.LineHeight},
                          theme.Text);
    }
    graphics.SetClip(oldClip);
}
void TextBox::UiInput(Event& event) noexcept
{
    if (event.Kind == EventKind::GotFocus)
    {
        m_caretOn = true;
        if (m_caretTimer == 0U)
        {
            static_cast<void>(m_application.StartTimer(m_caretTimer, m_application.UiTheme().CaretPeriod,
                                                       Action::Bind(
                                                           [this]() noexcept
                                                           {
                                                               m_caretOn = !m_caretOn;
                                                               UiInvalidate();
                                                           })));
        }
    }
    if (event.Kind == EventKind::LostFocus)
    {
        if (m_caretTimer != 0U)
        {
            static_cast<void>(m_application.StopTimer(m_caretTimer));
            m_caretTimer = 0U;
        }
        m_dragging = false;
    }
    if (event.Kind == EventKind::MouseDown)
    {
        UiMove(UiIndexAt(event.Position), event.Shift);
        m_dragging = true;
    }
    if (event.Kind == EventKind::MouseMove && m_dragging)
    {
        UiMove(UiIndexAt(event.Position), true);
    }
    if (event.Kind == EventKind::MouseUp)
    {
        m_dragging = false;
    }
    if (event.Kind == EventKind::KeyPress)
    {
        UiEdit(event.Scalar);
        event.Handled = true;
    }
    if (event.Kind == EventKind::KeyDown)
    {
        if (event.Code == Key::Left || event.Code == Key::Right)
        {
            UInt32 next{event.Code == Key::Left ? UiPrevious(m_caret) : UiNext(m_caret)};
            if (!event.Shift && m_caret != m_selection)
            {
                next = event.Code == Key::Left ? Min(m_caret, m_selection)
                                               : (m_caret > m_selection ? m_caret : m_selection);
            }
            UiMove(next, event.Shift);
            event.Handled = true;
        }
        else if (event.Code == Key::Home || event.Code == Key::End)
        {
            UInt32 next{event.ControlKey ? 0U : UiLineStart(m_caret)};
            if (event.Code == Key::End)
            {
                next = event.ControlKey ? m_textLength : m_caret;
                while (next < m_textLength && m_text[next] != '\n')
                {
                    ++next;
                }
            }
            UiMove(next, event.Shift);
            event.Handled = true;
        }
        else if ((event.Code == Key::Up || event.Code == Key::Down) && m_multiline)
        {
            const UInt32 start{UiLineStart(m_caret)};
            const UInt32 column{m_caret - start};
            UInt32 next{event.Code == Key::Up ? (start == 0U ? 0U : UiLineStart(start - 1U)) : m_caret};
            if (event.Code == Key::Down)
            {
                while (next < m_textLength && m_text[next] != '\n')
                {
                    ++next;
                }
                if (next < m_textLength)
                {
                    ++next;
                }
            }
            UInt32 end{next};
            while (end < m_textLength && m_text[end] != '\n')
            {
                ++end;
            }
            next = Min(next + column, end);
            while (!UiBoundary(next))
            {
                --next;
            }
            UiMove(next, event.Shift);
            event.Handled = true;
        }
        else if (event.Code == Key::Backspace || event.Code == Key::Delete)
        {
            if (!m_readOnly)
            {
                if (m_caret == m_selection)
                {
                    m_selection = event.Code == Key::Backspace ? UiPrevious(m_caret) : UiNext(m_caret);
                }
                UiEdit(0U, true);
            }
            event.Handled = true;
        }
        else if (event.Code == Key::A && event.ControlKey)
        {
            m_selection = 0U;
            UiMove(m_textLength, true);
            event.Handled = true;
        }
        else if (event.Code == Key::Enter && m_multiline)
        {
            UiEdit(0x0AU);
            event.Handled = true;
        }
        else if (event.Code == Key::Space)
        {
            event.Handled = true;
        }
    }
    // Editing consumes keyboard activation; all named input events still fire.
    UiEmit(event);
}
} // namespace Asteria::User::Toolkit
