// SPDX-License-Identifier: Apache-2.0

#include "User/Input/HidBoot.hpp"

namespace Asteria::User::Input
{
namespace
{
[[nodiscard]] bool Contains(const UInt8* keys, const Size length, const UInt8 key) noexcept
{
    for (Size i = 0U; i < length; ++i)
    {
        if (keys[i] == key)
        {
            return true;
        }
    }
    return false;
}
} // Namespace

void HidBoot::Release(EventBatch& output) noexcept
{
    const UInt8 empty[8U]{};
    static_cast<void>(Keyboard(empty, sizeof(empty), output));
    // Device retirement balances both key and pointer state. At most eight modifiers + six keys +
    // one button release.
    static_assert(8U + 6U + 1U <= sizeof(EventBatch{}.Events) / sizeof(InputEvent));
    if (m_buttons != 0U)
    {
        InputEvent release{};
        release.Kind = EventKind::Buttons;
        release.Source = m_source;
        output.Events[output.Count++] = release;
    }
    Reset();
}

void HidBoot::Reset() noexcept
{
    for (UInt8& key : m_keys)
    {
        key = 0U;
    }
    m_modifiers = 0U;
    m_buttons = 0U;
}

ReportResult HidBoot::Keyboard(const UInt8* const report, const Size length, EventBatch& output) noexcept
{
    output.Count = 0U;
    if ((report == nullptr) || (length < 8U))
    {
        return ReportResult::Short;
    }
    for (Size i = 2U; i < 8U; ++i)
    {
        if (report[i] == 1U)
        {
            return ReportResult::Rollover;
        }
    }
    if (report[1U] != 0U)
    {
        return ReportResult::Invalid;
    }
    for (Size i = 2U; i < 8U; ++i)
    {
        if ((report[i] == 2U) || (report[i] == 3U) || (report[i] >= 0xE0U))
        {
            return ReportResult::Invalid;
        }
    }
    for (UInt8 bit = 0U; bit < 8U; ++bit)
    {
        const UInt8 mask = static_cast<UInt8>(1U << bit);
        if (((m_modifiers ^ report[0U]) & mask) != 0U)
        {
            AppendKey(output, m_source, static_cast<UInt16>(0xE0U + bit), report[0U],
                      (report[0U] & mask) != 0U ? EventKind::KeyDown : EventKind::KeyUp);
        }
    }
    for (Size i = 0U; i < 6U; ++i)
    {
        if ((m_keys[i] != 0U) && !Contains(m_keys, i, m_keys[i]) && !Contains(report + 2U, 6U, m_keys[i]))
        {
            AppendKey(output, m_source, m_keys[i], report[0U], EventKind::KeyUp);
        }
    }
    for (Size i = 0U; i < 6U; ++i)
    {
        const UInt8 key = report[i + 2U];
        if ((key != 0U) && !Contains(report + 2U, i, key) && !Contains(m_keys, 6U, key))
        {
            AppendKey(output, m_source, key, report[0U], EventKind::KeyDown);
        }
    }
    for (Size i = 0U; i < 6U; ++i)
    {
        m_keys[i] = report[i + 2U];
    }
    m_modifiers = report[0U];
    return ReportResult::Accepted;
}

ReportResult HidBoot::Mouse(const UInt8* const report, const Size length, EventBatch& output) noexcept
{
    output.Count = 0U;
    if ((report == nullptr) || (length < 3U))
    {
        return ReportResult::Short;
    }
    PointerEvents(output, m_source, SignedByte(report[1U]), SignedByte(report[2U]), static_cast<UInt8>(report[0U] & 7U),
                  length > 3U ? SignedByte(report[3U]) : 0, m_buttons);
    return ReportResult::Accepted;
}
} // Namespace Asteria::User::Input
