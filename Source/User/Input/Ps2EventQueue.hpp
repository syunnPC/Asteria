// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_INPUT_PS2_EVENT_QUEUE_HPP_INCLUDED
#define ASTERIA_USER_INPUT_PS2_EVENT_QUEUE_HPP_INCLUDED

#include "User/Component/InputFrame.hpp"

namespace Asteria::User::Input
{
/// Fixed FIFO. The exposed head is frozen until Done; Busy/Exhausted and a
/// lost answer keep identical bytes and transaction. Restart discards replay.
class Ps2EventQueue final
{
public:
    [[nodiscard]] bool Append(const InputEvent& event) noexcept;
    [[nodiscard]] Size Prepare() noexcept;
    [[nodiscard]] bool Answer(Component::InputAnswer answer) noexcept;
    void Restart() noexcept;
    [[nodiscard]] const InputEvent* GetEvents() const noexcept
    {
        return m_events;
    }
    [[nodiscard]] Foundation::UInt64 GetTransaction() const noexcept
    {
        return m_transaction + 1U;
    }
    [[nodiscard]] Size GetCount() const noexcept
    {
        return m_count;
    }

private:
    InputEvent m_events[64U]{};
    Size m_count{0U};
    Size m_pending{0U};
    Foundation::UInt64 m_transaction{0U};
};
}
#endif
