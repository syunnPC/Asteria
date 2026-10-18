// SPDX-License-Identifier: Apache-2.0

#include "User/Toolkit/Dialogs.hpp"

namespace Asteria::User::Toolkit
{
MessageBox::MessageBox(Application& app) noexcept : m_form{app}, m_message{app}, m_accept{app}, m_cancel{app}
{
    static_cast<void>(m_form.Add(m_message));
    static_cast<void>(m_form.Add(m_accept));
    static_cast<void>(m_form.Add(m_cancel));
    static_cast<void>(m_accept.On(EventKind::Click,
                                  [this](Control&, Event&) noexcept
                                  {
                                      Finish(DialogResult::Accepted);
                                  }));
    static_cast<void>(m_cancel.On(EventKind::Click,
                                  [this](Control&, Event&) noexcept
                                  {
                                      Finish(DialogResult::Cancelled);
                                  }));
    static_cast<void>(m_form.SetDefaultButtons(&m_accept, &m_cancel));
    static_cast<void>(m_form.SetVisible(false));
}
Status MessageBox::Show(TextView message, const DialogStrings& strings, Handler completed) noexcept
{
    return m_form.App().Call(
        [&]() noexcept
        {
            if (!ValidText(message, MaximumText) || !ValidText(strings.Accept, MaximumText) ||
                !ValidText(strings.Cancel, MaximumText))
            {
                return Status::Invalid;
            }
            const Rect bounds{m_form.State().Bounds};
            const auto& theme{m_form.App().UiTheme()};
            static_cast<void>(m_message.SetText(message));
            static_cast<void>(m_accept.SetText(strings.Accept));
            static_cast<void>(m_cancel.SetText(strings.Cancel));
            static_cast<void>(m_message.SetBounds(
                Inset({0, 0, bounds.Width, bounds.Height > theme.ItemHeight ? bounds.Height - theme.ItemHeight : 0U},
                      theme.Padding)));
            const UInt32 half{bounds.Width / 2U};
            static_cast<void>(
                m_accept.SetBounds({0, static_cast<Int32>(bounds.Height - Min(bounds.Height, theme.ItemHeight)), half,
                                    Min(bounds.Height, theme.ItemHeight)}));
            static_cast<void>(m_cancel.SetBounds(
                {static_cast<Int32>(half), static_cast<Int32>(bounds.Height - Min(bounds.Height, theme.ItemHeight)),
                 bounds.Width - half, Min(bounds.Height, theme.ItemHeight)}));
            m_completed = completed;
            m_result = DialogResult::Pending;
            static_cast<void>(m_form.SetVisible(true));
            static_cast<void>(m_form.App().SetModal(&m_form));
            return m_accept.Focus();
        });
}
void MessageBox::Finish(DialogResult result) noexcept
{
    if (m_result != DialogResult::Pending)
    {
        return;
    }
    m_result = result;
    static_cast<void>(m_form.App().SetModal(nullptr));
    static_cast<void>(m_form.SetVisible(false));
    Event event{EventKind::Click};
    event.Item = static_cast<Int32>(result);
    m_completed(m_form, event);
}
DialogResult MessageBox::Result() noexcept
{
    DialogResult result{};
    static_cast<void>(m_form.App().Invoke(
        [&]() noexcept
        {
            result = m_result;
        }));
    return result;
}
FileDialog::FileDialog(Application& app, IFileDialogService& service) noexcept
    : m_service{service}, m_form{app}, m_prompt{app}, m_list{app}, m_name{app}, m_accept{app}, m_cancel{app}
{
    static_cast<void>(m_form.Add(m_prompt));
    static_cast<void>(m_form.Add(m_list));
    static_cast<void>(m_form.Add(m_name));
    static_cast<void>(m_form.Add(m_accept));
    static_cast<void>(m_form.Add(m_cancel));
    static_cast<void>(m_accept.On(EventKind::Click,
                                  [this](Control&, Event&) noexcept
                                  {
                                      Accept();
                                  }));
    static_cast<void>(m_cancel.On(EventKind::Click,
                                  [this](Control&, Event&) noexcept
                                  {
                                      Finish(DialogResult::Cancelled);
                                  }));
    static_cast<void>(m_list.On(EventKind::SelectedIndexChanged,
                                [this](Control&, Event& event) noexcept
                                {
                                    if (event.Item >= 0 && event.Item < static_cast<Int32>(m_count))
                                    {
                                        const auto& entry{m_entries[static_cast<UInt32>(event.Item)]};
                                        static_cast<void>(m_name.SetText({entry.Name, entry.Length}));
                                    }
                                }));
    static_cast<void>(m_list.On(EventKind::KeyDown,
                                [this](Control&, Event& event) noexcept
                                {
                                    if (event.Code == Key::Enter)
                                    {
                                        event.Handled = true;
                                        Accept();
                                    }
                                }));
    static_cast<void>(m_form.SetDefaultButtons(&m_accept, &m_cancel));
    static_cast<void>(m_form.SetVisible(false));
}
Status FileDialog::Load(FileCapability directory) noexcept
{
    UInt32 count{};
    const Status result{m_service.Enumerate(directory, m_entries, MaximumItems, count)};
    if (result != Status::Ok || count > MaximumItems)
    {
        return result == Status::Ok ? Status::Limit : result;
    }
    for (UInt32 i{}; i < count; ++i)
    {
        if (!ValidText({m_entries[i].Name, m_entries[i].Length}, MaximumItemText))
        {
            return Status::Invalid;
        }
    }
    m_count = count;
    m_directory = directory;
    static_cast<void>(m_list.ClearItems());
    for (UInt32 i{}; i < count; ++i)
    {
        static_cast<void>(m_list.AddItem({m_entries[i].Name, m_entries[i].Length}));
    }
    static_cast<void>(m_name.SetText(Utf8("")));
    return Status::Ok;
}
Status FileDialog::Show(FileCapability directory, bool save, const DialogStrings& strings, Handler completed) noexcept
{
    return m_form.App().Call(
        [&]() noexcept
        {
            if (!ValidText(strings.Accept, MaximumText) || !ValidText(strings.Cancel, MaximumText) ||
                !ValidText(strings.Prompt, MaximumText))
            {
                return Status::Invalid;
            }
            const Status status{Load(directory)};
            if (status != Status::Ok)
            {
                return status;
            }
            const Rect bounds{m_form.State().Bounds};
            const auto& theme{m_form.App().UiTheme()};
            const UInt32 row{Min(theme.ItemHeight, bounds.Height / 4U)};
            static_cast<void>(m_prompt.SetText(strings.Prompt));
            static_cast<void>(m_accept.SetText(strings.Accept));
            static_cast<void>(m_cancel.SetText(strings.Cancel));
            static_cast<void>(m_prompt.SetBounds({0, 0, bounds.Width, row}));
            static_cast<void>(m_list.SetBounds({0, static_cast<Int32>(row), bounds.Width, bounds.Height - 3U * row}));
            static_cast<void>(m_name.SetBounds({0, static_cast<Int32>(bounds.Height - 2U * row), bounds.Width, row}));
            static_cast<void>(m_accept.SetBounds({0, static_cast<Int32>(bounds.Height - row), bounds.Width / 2U, row}));
            static_cast<void>(
                m_cancel.SetBounds({static_cast<Int32>(bounds.Width / 2U), static_cast<Int32>(bounds.Height - row),
                                    bounds.Width - bounds.Width / 2U, row}));
            m_completed = completed;
            m_save = save;
            m_result = DialogResult::Pending;
            m_selected = {};
            static_cast<void>(m_form.SetVisible(true));
            static_cast<void>(m_form.App().SetModal(&m_form));
            return m_name.Focus();
        });
}
void FileDialog::Accept() noexcept
{
    const TextView name{m_name.UiText()};
    if (name.Length == 0U)
    {
        return;
    }
    for (UInt32 i{}; i < m_count; ++i)
    {
        const auto& entry{m_entries[i]};
        bool same{entry.Length == name.Length};
        for (UInt32 j{}; same && j < name.Length; ++j)
        {
            same = entry.Name[j] == name.Data[j];
        }
        if (same && entry.Directory)
        {
            if (Load(entry.Handle) != Status::Ok)
            {
                Finish(DialogResult::Error);
            }
            else
            {
                static_cast<void>(m_name.Focus());
            }
            return;
        }
    }
    if (m_service.Select(m_directory, name, m_save, m_selected) != Status::Ok)
    {
        Finish(DialogResult::Error);
        return;
    }
    Finish(DialogResult::Accepted);
}
void FileDialog::Finish(DialogResult result) noexcept
{
    if (m_result != DialogResult::Pending)
    {
        return;
    }
    m_result = result;
    static_cast<void>(m_form.App().SetModal(nullptr));
    static_cast<void>(m_form.SetVisible(false));
    Event event{EventKind::Click};
    event.Item = static_cast<Int32>(result);
    m_completed(m_form, event);
}
Status FileDialog::Selected(FileCapability& capability, DialogResult& result) noexcept
{
    return m_form.App().Call(
        [&]() noexcept
        {
            capability = m_selected;
            result = m_result;
            return Status::Ok;
        });
}
} // namespace Asteria::User::Toolkit
