// SPDX-License-Identifier: Apache-2.0

#include "User/Toolkit/Application.hpp"

#include "User/Toolkit/Form.hpp"

namespace Asteria::User::Toolkit
{
Application::Application(IThreadHost& threads, IEventSource& source, const Theme& theme) noexcept
    : m_threads{threads}, m_source{source}, m_theme{theme}, m_valid{theme.Valid() && threads.IsUiThread()}
{
}
Status Application::Enqueue(Action action, Completion* completion) noexcept
{
    m_threads.LockQueue();
    Status status{Status::Ok};
    if (!m_valid)
    {
        status = Status::Invalid;
    }
    else if (m_stopped)
    {
        status = Status::Stopped;
    }
    else if (m_count == MaximumQueue)
    {
        status = Status::QueueFull;
    }
    else
    {
        m_queue[(m_head + m_count) % MaximumQueue] = {action, completion};
        ++m_count;
    }
    m_threads.UnlockQueue();
    if (status == Status::Ok)
    {
        m_threads.WakeUi();
    }
    return status;
}
Status Application::Invoke(Action action) noexcept
{
    if (IsUiThread())
    {
        if (!m_valid)
        {
            return Status::Invalid;
        }
        if (m_stopped)
        {
            return Status::Stopped;
        }
        action();
        return Status::Ok;
    }
    Completion completion{};
    const Status result{Enqueue(action, &completion)};
    if (result != Status::Ok)
    {
        return result;
    }
    m_threads.Wait(completion);
    return completion.Result;
}
Status Application::BeginInvoke(Action action) noexcept
{
    return Enqueue(action, nullptr);
}
void Application::Drain() noexcept
{
    const UInt32 count{Pending()};
    for (UInt32 index{}; index < count; ++index)
    {
        m_threads.LockQueue();
        if (m_count == 0U)
        {
            m_threads.UnlockQueue();
            break;
        }
        Work work{m_queue[m_head]};
        m_head = (m_head + 1U) % MaximumQueue;
        --m_count;
        const bool stopped{m_stopped};
        m_threads.UnlockQueue();
        if (!stopped)
        {
            work.Callback();
        }
        if (work.Reply != nullptr)
        {
            work.Reply->Result = stopped ? Status::Stopped : Status::Ok;
            m_threads.Finish(*work.Reply);
        }
    }
}
UInt32 Application::Pending() noexcept
{
    m_threads.LockQueue();
    const UInt32 result{m_count};
    m_threads.UnlockQueue();
    return result;
}
Status Application::Stop() noexcept
{
    if (!IsUiThread())
    {
        return Invoke(
            [this]() noexcept
            {
                static_cast<void>(Stop());
            });
    }
    m_threads.LockQueue();
    m_stopped = true;
    m_threads.UnlockQueue();
    Drain();
    m_threads.WakeUi();
    return Status::Ok;
}
Status Application::Register(Control& control) noexcept
{
    if (!IsUiThread())
    {
        return Status::WrongThread;
    }
    if (!m_valid || m_stopped)
    {
        return Status::Invalid;
    }
    if (Known(&control))
    {
        return Status::Invalid;
    }
    if (m_controlCount == MaximumControls)
    {
        return Status::Limit;
    }
    m_controls[m_controlCount++] = &control;
    return Status::Ok;
}
bool Application::Known(const Control* control) const noexcept
{
    for (UInt32 i{}; i < m_controlCount; ++i)
    {
        if (m_controls[i] == control)
        {
            return true;
        }
    }
    return false;
}
void Application::Unregister(Control& control) noexcept
{
    if (!IsUiThread())
    {
        return;
    }
    if (m_modal == &control)
    {
        m_modal = nullptr;
    }
    if (m_focus != nullptr && m_focus->UiDescendantOf(&control))
    {
        m_focus = nullptr;
    }
    if (m_hot != nullptr && m_hot->UiDescendantOf(&control))
    {
        m_hot = nullptr;
    }
    if (m_capture != nullptr && m_capture->UiDescendantOf(&control))
    {
        m_capture = nullptr;
    }
    if (m_clickTarget != nullptr && m_clickTarget->UiDescendantOf(&control))
    {
        m_clickTarget = nullptr;
    }
    for (UInt32 i{}; i < m_controlCount; ++i)
    {
        if (m_controls[i] != &control)
        {
            continue;
        }
        for (UInt32 j{i + 1U}; j < m_controlCount; ++j)
        {
            m_controls[j - 1U] = m_controls[j];
        }
        --m_controlCount;
        break;
    }
}
Status Application::ReleaseInput(Form& form) noexcept
{
    return Call(
        [&]() noexcept
        {
            if (m_capture != nullptr && m_capture->UiForm() == &form)
            {
                Event release{EventKind::MouseUp};
                release.Position = {-static_cast<Int32>(MaximumAxis), -static_cast<Int32>(MaximumAxis)};
                m_capture->UiInput(release);
                m_capture = nullptr;
            }
            if (m_hot != nullptr && m_hot->UiForm() == &form)
            {
                Event leave{EventKind::MouseLeave};
                m_hot->UiInput(leave);
                m_hot = nullptr;
            }
            if (m_focus != nullptr && m_focus->UiForm() == &form)
            {
                static_cast<void>(Focus(nullptr));
            }
            m_clickTarget = nullptr;
            return Status::Ok;
        });
}
Status Application::Focus(Control* control) noexcept
{
    return Call(
        [&]() noexcept
        {
            if (control != nullptr &&
                (!Known(control) || !control->UiEffective() || !control->UiTabStop() || control->UiForm() == nullptr ||
                 (m_modal != nullptr && control->UiForm() != m_modal)))
            {
                return Status::Invalid;
            }
            if (m_focus == control)
            {
                return Status::Ok;
            }
            Control* old{m_focus};
            m_focus = control;
            if (old != nullptr)
            {
                old->UiInvalidate();
                Event event{EventKind::LostFocus};
                old->UiInput(event);
            }
            if (control != nullptr && m_focus == control)
            {
                control->UiInvalidate();
                Event event{EventKind::GotFocus};
                control->UiInput(event);
            }
            return Status::Ok;
        });
}
void Application::MoveFocus(Form& form, bool reverse) noexcept
{
    UInt64 current{};
    bool hasCurrent{};
    for (UInt32 i{}; i < m_controlCount; ++i)
    {
        if (m_controls[i] == m_focus)
        {
            current = static_cast<UInt64>(m_focus->UiTabOrder()) * MaximumControls + i;
            hasCurrent = true;
        }
    }
    Control* next{};
    Control* wrap{};
    UInt64 best{};
    UInt64 wrapRank{};
    for (UInt32 i{}; i < m_controlCount; ++i)
    {
        Control* candidate{m_controls[i]};
        if (candidate->UiForm() != &form || !candidate->UiEffective() || !candidate->UiTabStop())
        {
            continue;
        }
        const UInt64 rank{static_cast<UInt64>(candidate->UiTabOrder()) * MaximumControls + i};
        if (wrap == nullptr || (reverse ? rank > wrapRank : rank < wrapRank))
        {
            wrap = candidate;
            wrapRank = rank;
        }
        if ((!hasCurrent || (reverse ? rank < current : rank > current)) &&
            (next == nullptr || (reverse ? rank > best : rank < best)))
        {
            next = candidate;
            best = rank;
        }
    }
    static_cast<void>(Focus(next != nullptr ? next : wrap));
}
Status Application::Dispatch(Form& form, Event event) noexcept
{
    return Call(
        [&]() noexcept
        {
            if ((m_modal != nullptr && m_modal != &form) || !Known(&form) || !form.UiEffective() ||
                event.Kind >= EventKind::Count)
            {
                return Status::Invalid;
            }
            if (m_focus != nullptr && !m_focus->UiEffective())
            {
                static_cast<void>(Focus(nullptr));
            }
            if (m_capture != nullptr && (!m_capture->UiEffective() || m_capture->UiForm() != &form))
            {
                m_capture = nullptr;
            }
            if (event.Kind == EventKind::KeyPress && !User::Text::IsScalar(event.Scalar))
            {
                return Status::Invalid;
            }
            if (event.Kind == EventKind::KeyDown && event.Code == Key::Tab)
            {
                MoveFocus(form, event.Shift);
                return Status::Ok;
            }
            if (event.Alt && event.Scalar != 0U && event.Kind == EventKind::KeyDown)
            {
                for (UInt32 i{}; i < m_controlCount; ++i)
                {
                    Control* control{m_controls[i]};
                    if (control->UiForm() == &form && control->UiEffective() && control->UiMnemonic() == event.Scalar)
                    {
                        static_cast<void>(Focus(control));
                        control->UiActivate(event);
                        return Status::Ok;
                    }
                }
            }
            const bool mouse{event.Kind >= EventKind::MouseDown && event.Kind <= EventKind::MouseWheel};
            Control* target{};
            if (mouse)
            {
                Control* hit{form.UiHit(event.Position)};
                if (event.Kind == EventKind::MouseMove && hit != m_hot)
                {
                    Control* old{m_hot};
                    m_hot = hit;
                    if (old != nullptr)
                    {
                        Event leave{event};
                        leave.Kind = EventKind::MouseLeave;
                        old->UiInput(leave);
                    }
                    if (hit != nullptr)
                    {
                        Event enter{event};
                        enter.Kind = EventKind::MouseEnter;
                        hit->UiInput(enter);
                    }
                }
                target = m_capture != nullptr ? m_capture : hit;
                if (event.Kind == EventKind::MouseDown && event.Button == 1U && target != nullptr)
                {
                    m_capture = target;
                    static_cast<void>(Focus(target));
                }
            }
            else
            {
                target = m_focus != nullptr && m_focus->UiForm() == &form ? m_focus : &form;
            }
            if (target != nullptr && target->UiEffective())
            {
                if ((event.Kind == EventKind::MouseDown || event.Kind == EventKind::MouseUp) && event.Button != 1U)
                {
                    target->UiEmit(event);
                }
                else
                {
                    target->UiInput(event);
                }
                if (event.Kind == EventKind::MouseDown && event.Button == 1U)
                {
                    const UInt64 now{m_source.Now()};
                    const Foundation::Int64 dx{static_cast<Foundation::Int64>(event.Position.X) - m_clickPosition.X};
                    const Foundation::Int64 dy{static_cast<Foundation::Int64>(event.Position.Y) - m_clickPosition.Y};
                    const Foundation::Int64 distance{m_theme.DoubleClickDistance};
                    if (m_clickTarget == target && now >= m_clickTime &&
                        now - m_clickTime <= m_theme.DoubleClickPeriod && dx >= -distance && dx <= distance &&
                        dy >= -distance && dy <= distance)
                    {
                        Event twice{event};
                        twice.Kind = EventKind::DoubleClick;
                        target->UiInput(twice);
                        m_clickTarget = nullptr;
                    }
                    else
                    {
                        m_clickTarget = target;
                        m_clickTime = now;
                        m_clickPosition = event.Position;
                    }
                }
            }
            if (event.Kind == EventKind::MouseWheel && target != nullptr)
            {
                Control* parent{target->UiParent()};
                for (UInt32 depth{}; !event.Handled && parent != nullptr && depth < MaximumDepth; ++depth)
                {
                    if (parent->UiEffective())
                    {
                        parent->UiInput(event);
                    }
                    parent = parent->UiParent();
                }
            }
            if (event.Kind == EventKind::MouseUp && event.Button == 1U)
            {
                m_capture = nullptr;
            }
            if (!event.Handled && event.Kind == EventKind::KeyDown &&
                (event.Code == Key::Enter || event.Code == Key::Escape))
            {
                Control* button{form.UiDefault(event.Code == Key::Escape)};
                if (button != nullptr && Known(button) && button->UiEffective())
                {
                    button->UiActivate(event);
                }
            }
            return Status::Ok;
        });
}
Status Application::SetModal(Form* form) noexcept
{
    return Call(
        [&]() noexcept
        {
            if (form != nullptr && (!Known(form) || !form->UiEffective()))
            {
                return Status::Invalid;
            }
            if (form != nullptr && m_modal != nullptr && m_modal != form)
            {
                return Status::Busy;
            }
            m_modal = form;
            return Status::Ok;
        });
}
Status Application::SetTheme(const Theme& theme) noexcept
{
    return Call(
        [&]() noexcept
        {
            if (!theme.Valid())
            {
                return Status::Invalid;
            }
            m_theme = theme;
            for (UInt32 i{}; i < m_controlCount; ++i)
            {
                if (m_controls[i]->Type() == Kind::Form)
                {
                    m_controls[i]->UiLayout();
                    m_controls[i]->UiInvalidate();
                }
            }
            return Status::Ok;
        });
}
Status Application::StartTimer(UInt32& id, UInt64 interval, Action callback) noexcept
{
    return Call(
        [&]() noexcept
        {
            if (interval == 0U || interval > 0xFFFFFFFFU || m_nextTimer == 0U)
            {
                return Status::Invalid;
            }
            for (auto& timer : m_timers)
            {
                if (timer.Id != 0U)
                {
                    continue;
                }
                id = m_nextTimer++;
                timer = {callback, interval, m_source.Now() + interval, id};
                return Status::Ok;
            }
            return Status::Limit;
        });
}
Status Application::StopTimer(UInt32 id) noexcept
{
    return Call(
        [&]() noexcept
        {
            if (id == 0U)
            {
                return Status::Invalid;
            }
            for (auto& timer : m_timers)
            {
                if (timer.Id == id)
                {
                    timer.Id = 0U;
                    return Status::Ok;
                }
            }
            return Status::Invalid;
        });
}
void Application::Tick() noexcept
{
    const UInt64 now{m_source.Now()};
    for (auto& timer : m_timers)
    {
        if (timer.Id == 0U || timer.Next > now)
        {
            continue;
        }
        timer.Next = now + timer.Interval;
        Action callback{timer.Callback};
        callback();
        if (m_stopped)
        {
            break;
        }
    }
}
Status Application::Step() noexcept
{
    if (!IsUiThread())
    {
        return Status::WrongThread;
    }
    if (!m_valid)
    {
        return Status::Invalid;
    }
    if (m_stopped)
    {
        return Status::Stopped;
    }
    if (m_stepping)
    {
        return Status::Busy;
    }
    m_stepping = true;
    Drain();
    SourceEvent event{};
    if (!m_stopped && m_source.Poll(event) && event.Target != nullptr && Known(event.Target))
    {
        static_cast<void>(Dispatch(*event.Target, event.Input));
    }
    if (!m_stopped)
    {
        Tick();
    }
    for (UInt32 i{}; !m_stopped && i < m_controlCount; ++i)
    {
        if (m_controls[i]->Type() == Kind::Form)
        {
            static_cast<void>(static_cast<Form*>(m_controls[i])->Render());
        }
    }
    m_stepping = false;
    return m_stopped ? Status::Stopped : Status::Ok;
}
Status Application::Run() noexcept
{
    if (!IsUiThread())
    {
        return Status::WrongThread;
    }
    while (Step() == Status::Ok)
    {
        if (Pending() != 0U)
        {
            continue;
        }
        UInt64 deadline{~UInt64{0U}};
        for (const auto& timer : m_timers)
        {
            if (timer.Id != 0U && timer.Next < deadline)
            {
                deadline = timer.Next;
            }
        }
        m_source.WaitUntil(deadline);
    }
    return m_stopped ? Status::Ok : Status::Invalid;
}
bool Application::Invariants() noexcept
{
    bool valid{};
    static_cast<void>(Invoke(
        [&]() noexcept
        {
            valid = Pending() <= MaximumQueue && m_controlCount <= MaximumControls &&
                    (m_focus == nullptr || (Known(m_focus) && m_focus->UiEffective() && m_focus->UiForm() != nullptr));
            for (UInt32 i{}; valid && i < m_controlCount; ++i)
            {
                UInt32 depth{};
                const Control* at{m_controls[i]};
                for (; at != nullptr && depth <= MaximumDepth; ++depth)
                {
                    at = at->UiParent();
                }
                valid = at == nullptr && depth <= MaximumDepth;
            }
        }));
    return valid;
}
} // namespace Asteria::User::Toolkit
