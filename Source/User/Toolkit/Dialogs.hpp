// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TOOLKIT_DIALOGS_HPP_INCLUDED
#define ASTERIA_USER_TOOLKIT_DIALOGS_HPP_INCLUDED

#include "User/Toolkit/Extended.hpp"

namespace Asteria::User::Toolkit
{
struct DialogStrings
{
    TextView Accept{};
    TextView Cancel{};
    TextView Prompt{};
};
enum class DialogResult : UInt8
{
    Pending,
    Accepted,
    Cancelled,
    Error
};
class MessageBox final
{
public:
    explicit MessageBox(Application& app) noexcept;
    [[nodiscard]] Form& Window() noexcept
    {
        return m_form;
    }
    [[nodiscard]] Status Show(TextView message, const DialogStrings& strings, Handler completed = {}) noexcept;
    [[nodiscard]] DialogResult Result() noexcept;

private:
    void Finish(DialogResult result) noexcept;
    Form m_form;
    Label m_message;
    Button m_accept;
    Button m_cancel;
    Handler m_completed{};
    DialogResult m_result{DialogResult::Pending};
};
// Tokens are opaque to the toolkit. The service owns enumeration tokens and
// mints the selected grant; ACL/view checks stay on the service side.
struct FileCapability
{
    UInt64 Token{};
};
struct FileEntry
{
    char Name[MaximumItemText + 1U]{};
    UInt32 Length{};
    bool Directory{};
    FileCapability Handle{};
};
class IFileDialogService
{
public:
    [[nodiscard]] virtual Status Enumerate(FileCapability directory, FileEntry* entries, UInt32 capacity,
                                           UInt32& count) noexcept = 0;
    [[nodiscard]] virtual Status Select(FileCapability directory, TextView name, bool save,
                                        FileCapability& selected) noexcept = 0;

protected:
    ~IFileDialogService() = default;
};
class FileDialog final
{
public:
    FileDialog(Application& app, IFileDialogService& service) noexcept;
    [[nodiscard]] Form& Window() noexcept
    {
        return m_form;
    }
    [[nodiscard]] Status Show(FileCapability directory, bool save, const DialogStrings& strings,
                              Handler completed = {}) noexcept;
    [[nodiscard]] Status Selected(FileCapability& capability, DialogResult& result) noexcept;

private:
    [[nodiscard]] Status Load(FileCapability directory) noexcept;
    void Accept() noexcept;
    void Finish(DialogResult result) noexcept;
    IFileDialogService& m_service;
    Form m_form;
    Label m_prompt;
    ListBox m_list;
    TextBox m_name;
    Button m_accept;
    Button m_cancel;
    FileEntry m_entries[MaximumItems]{};
    UInt32 m_count{};
    FileCapability m_directory{};
    FileCapability m_selected{};
    Handler m_completed{};
    DialogResult m_result{DialogResult::Pending};
    bool m_save{};
};
} // namespace Asteria::User::Toolkit
#endif
