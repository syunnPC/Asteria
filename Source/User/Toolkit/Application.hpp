// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TOOLKIT_APPLICATION_HPP_INCLUDED
#define ASTERIA_USER_TOOLKIT_APPLICATION_HPP_INCLUDED

#include "User/Toolkit/Theme.hpp"

namespace Asteria::User::Toolkit
{
class Form;
struct Completion
{
    UInt32 Done{};
    Status Result{Status::Ok};
};
class IThreadHost
{
public:
    [[nodiscard]] virtual bool IsUiThread() noexcept = 0;
    virtual void LockQueue() noexcept = 0;
    virtual void UnlockQueue() noexcept = 0;
    // Wait must acquire Done, Finish must release it and persistently notify.
    virtual void Wait(Completion& completion) noexcept = 0;
    virtual void Finish(Completion& completion) noexcept = 0;
    virtual void WakeUi() noexcept = 0;

protected:
    ~IThreadHost() = default;
};
struct SourceEvent
{
    Form* Target{};
    Event Input{};
};
class IEventSource
{
public:
    [[nodiscard]] virtual bool Poll(SourceEvent& event) noexcept = 0;
    // Wait until input, a persistent WakeUi notification, or the deadline.
    virtual void WaitUntil(UInt64 deadline) noexcept = 0;
    [[nodiscard]] virtual UInt64 Now() noexcept = 0;

protected:
    ~IEventSource() = default;
};
class Application final
{
public:
    Application(IThreadHost& threads, IEventSource& source, const Theme& theme) noexcept;
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    [[nodiscard]] Status Invoke(Action action) noexcept;
    [[nodiscard]] Status BeginInvoke(Action action) noexcept;
    template <typename T> [[nodiscard]] Status Invoke(const T& function) noexcept
    {
        return Invoke(Action::Bind(function));
    }
    template <typename T> [[nodiscard]] Status BeginInvoke(const T& function) noexcept
    {
        return BeginInvoke(Action::Bind(function));
    }
    template <typename T> [[nodiscard]] Status Call(const T& function) noexcept
    {
        Status result{Status::Invalid};
        const Status transport{Invoke(
            [&]() noexcept
            {
                result = function();
            })};
        return transport == Status::Ok ? result : transport;
    }
    [[nodiscard]] Status Step() noexcept;
    [[nodiscard]] Status Run() noexcept;
    [[nodiscard]] Status Stop() noexcept;
    [[nodiscard]] Status SetModal(Form* form) noexcept;
    [[nodiscard]] Status SetTheme(const Theme& theme) noexcept;
    [[nodiscard]] Status StartTimer(UInt32& id, UInt64 interval, Action callback) noexcept;
    [[nodiscard]] Status StopTimer(UInt32 id) noexcept;
    [[nodiscard]] Status Dispatch(Form& form, Event event) noexcept;
    [[nodiscard]] Status Focus(Control* control) noexcept;
    [[nodiscard]] Status ReleaseInput(Form& form) noexcept;
    [[nodiscard]] Status Register(Control& control) noexcept;
    void Unregister(Control& control) noexcept;
    [[nodiscard]] bool IsUiThread() noexcept
    {
        return m_threads.IsUiThread();
    }
    [[nodiscard]] const Theme& UiTheme() const noexcept
    {
        return m_theme;
    }
    [[nodiscard]] Control* UiFocus() const noexcept
    {
        return m_focus;
    }
    [[nodiscard]] UInt32 Pending() noexcept;
    [[nodiscard]] bool Invariants() noexcept;

private:
    struct Work
    {
        Action Callback{};
        Completion* Reply{};
    };
    struct Timer
    {
        Action Callback{};
        UInt64 Interval{};
        UInt64 Next{};
        UInt32 Id{};
    };
    [[nodiscard]] Status Enqueue(Action action, Completion* completion) noexcept;
    void Drain() noexcept;
    void Tick() noexcept;
    void MoveFocus(Form& form, bool reverse) noexcept;
    [[nodiscard]] bool Known(const Control* control) const noexcept;
    IThreadHost& m_threads;
    IEventSource& m_source;
    Theme m_theme{};
    Work m_queue[MaximumQueue]{};
    Timer m_timers[MaximumTimers]{};
    Control* m_controls[MaximumControls]{};
    UInt32 m_head{};
    UInt32 m_count{};
    UInt32 m_controlCount{};
    UInt32 m_nextTimer{1U};
    Form* m_modal{};
    Control* m_focus{};
    Control* m_hot{};
    Control* m_capture{};
    Control* m_clickTarget{};
    Point m_clickPosition{};
    UInt64 m_clickTime{};
    bool m_stopped{};
    bool m_valid{};
    bool m_stepping{};
};
} // namespace Asteria::User::Toolkit
#endif
