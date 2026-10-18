// SPDX-License-Identifier: Apache-2.0
#include "User/Toolkit/Fixture.hpp"

namespace Asteria::Test
{
namespace
{
namespace F = ToolkitFixture;
namespace Ui = User::Toolkit;
void Threads(TestSupport::TestContext& test) noexcept
{
    F::Host host;
    Ui::Application app{host, host, Ui::Theme::Classic()};
    Ui::Form form{app};
    Ui::CheckBox check{app};
    static_cast<void>(form.Add(check));
    std::atomic<unsigned int> finished{}, failures{}, callbacks{};
    bool uiOnly{true};
    unsigned int observed[4U]{};
    static_cast<void>(check.On(Ui::EventKind::CheckedChanged,
                               [&](Ui::Control&, Ui::Event&) noexcept
                               {
                                   uiOnly &= app.IsUiThread();
                                   ++callbacks;
                               }));
    struct Shared
    {
        Ui::Application& App;
        Ui::CheckBox& Check;
        std::atomic<unsigned int>& Failures;
        unsigned int* Observed;
    };
    Shared shared{app, check, failures, observed};
    std::thread workers[4U];
    for (unsigned int worker{}; worker < 4U; ++worker)
    {
        workers[worker] =
            std::thread{[&, worker]() noexcept
                        {
                            for (unsigned int sequence{1U}; sequence <= 100U; ++sequence)
                            {
                                if (app.Invoke(
                                        [&shared, worker, sequence]() noexcept
                                        {
                                            if (!shared.App.IsUiThread() || shared.Observed[worker] + 1U != sequence)
                                            {
                                                ++shared.Failures;
                                            }
                                            shared.Observed[worker] = sequence;
                                            const bool next{!shared.Check.State().Checked};
                                            if (shared.Check.SetChecked(next) != Ui::Status::Ok)
                                            {
                                                ++shared.Failures;
                                            }
                                        }) != Ui::Status::Ok)
                                {
                                    ++failures;
                                }
                                if (check.SetText(Ui::Utf8("worker")) != Ui::Status::Ok)
                                {
                                    ++failures;
                                }
                                char text[16U]{};
                                unsigned int length{};
                                if (check.CopyText(text, sizeof(text), length) != Ui::Status::Ok ||
                                    std::strcmp(text, "worker") != 0)
                                {
                                    ++failures;
                                }
                            }
                            ++finished;
                        }};
    }
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{20}};
    while (finished.load() < 4U && std::chrono::steady_clock::now() < deadline)
    {
        static_cast<void>(app.Step());
        std::this_thread::yield();
    }
    const bool timedOut{finished.load() != 4U};
    if (timedOut)
    {
        static_cast<void>(app.Stop());
    }
    for (auto& worker : workers)
    {
        worker.join();
    }
    ASTERIA_EXPECT(test, "concurrent setters getters and callbacks marshal in each worker's order", !timedOut,
                   failures == 0U, callbacks == 400U, uiOnly, observed[0U] == 100U, observed[3U] == 100U);
    unsigned int ordered{};
    for (unsigned int i{}; i < Ui::MaximumQueue; ++i)
    {
        ASTERIA_EXPECT(test, "asynchronous bounded capture enqueued",
                       app.BeginInvoke(
                           [&, i]() noexcept
                           {
                               if (ordered != i)
                               {
                                   ++failures;
                               }
                               ++ordered;
                           }) == Ui::Status::Ok);
    }
    ASTERIA_EXPECT(test, "queue full refuses without overwrite", app.Pending() == Ui::MaximumQueue,
                   app.BeginInvoke(
                       []() noexcept
                       {
                       }) == Ui::Status::QueueFull);
    Ui::Status refused{Ui::Status::Ok};
    std::thread overflow{[&]() noexcept
                         {
                             refused = check.SetText(Ui::Utf8("overflow"));
                         }};
    overflow.join();
    ASTERIA_EXPECT(test, "synchronous property call also refuses full queue", refused == Ui::Status::QueueFull);
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "BeginInvoke runs FIFO outside queue lock", ordered == Ui::MaximumQueue, failures == 0U,
                   app.Pending() == 0U);
    unsigned int reentrant{};
    static_cast<void>(app.BeginInvoke(
        [&]() noexcept
        {
            ++reentrant;
            static_cast<void>(app.BeginInvoke(
                [&]() noexcept
                {
                    ++reentrant;
                }));
        }));
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "drain snapshots queue to bound reentrant work", reentrant == 1U, app.Pending() == 1U);
    static_cast<void>(app.Step());
    ASTERIA_EXPECT(test, "reentrant async call runs next turn", reentrant == 2U);
    Ui::Status cancelled{Ui::Status::Ok};
    std::thread blocked{[&]() noexcept
                        {
                            cancelled = check.SetText(Ui::Utf8("cancelled"));
                        }};
    while (app.Pending() == 0U)
    {
        std::this_thread::yield();
    }
    static_cast<void>(app.Stop());
    blocked.join();
    ASTERIA_EXPECT(test, "shutdown releases pending synchronous calls and rejects future calls",
                   cancelled == Ui::Status::Stopped,
                   app.BeginInvoke(
                       []() noexcept
                       {
                       }) == Ui::Status::Stopped);
}
void Loop(TestSupport::TestContext& test) noexcept
{
    F::Host host;
    Ui::Application app{host, host, Ui::Theme::Classic()};
    Ui::Form form{app};
    Ui::Button button{app};
    static_cast<void>(form.Add(button));
    static_cast<void>(button.SetBounds({0, 0, 80U, 26U}));
    unsigned int clicks{};
    static_cast<void>(button.On(Ui::EventKind::Click,
                                [&](Ui::Control&, Ui::Event&) noexcept
                                {
                                    ++clicks;
                                    static_cast<void>(app.Stop());
                                }));
    std::thread producer{[&]() noexcept
                         {
                             Ui::Event down{Ui::EventKind::MouseDown};
                             down.Position = {4, 4};
                             Ui::Event up{down};
                             up.Kind = Ui::EventKind::MouseUp;
                             static_cast<void>(host.Push(form, down));
                             static_cast<void>(host.Push(form, up));
                         }};
    const Ui::Status result{app.Run()};
    producer.join();
    ASTERIA_EXPECT(test, "injected event source drives Run and wake notifications", result == Ui::Status::Ok,
                   clicks == 1U);
}
} // namespace
} // namespace Asteria::Test
int main()
{
    Asteria::TestSupport::TestContext test{"Toolkit threads"};
    Asteria::Test::Threads(test);
    Asteria::Test::Loop(test);
    return test.Finish();
}
