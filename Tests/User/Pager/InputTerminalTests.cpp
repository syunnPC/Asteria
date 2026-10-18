// SPDX-License-Identifier: Apache-2.0
#include "User/Pager/InputTerminal.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "Abi/ComponentBootstrap.hpp"
#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "User/Component/DriverSetup.hpp"
#include "User/Component/Ps2Driver.hpp"
#include "User/Component/UsbProgressFrame.hpp"
#include "User/Pager/UsbProgress.hpp"

namespace Asteria::Test
{
namespace
{
namespace C = User::Component;
namespace P = User::Pager;
namespace S = User::System;
using Foundation::Size;
using Foundation::UInt64;

struct TerminalModel
{
    std::vector<User::Input::InputEvent> Events{};
    // Deliveries that are an xHCI driver's diagnostic, not input events.
    std::vector<bool> Progress{};
    std::vector<std::string> SerialAfter{};
    std::vector<std::string> ScreenAfter{};
    std::string Serial{};
    std::string Screen{};
    Size Next{0U};
    Size Waits{0U};
    Size Credits{0U};
    bool Equal{true};
    std::vector<UInt64> DeliveryAt{};
    UInt64 Now{0U};
    UInt64 End{0U};
    bool Timed{false};
    bool ShortWait{false};
    Size Mismatch{0U};
    // The broker's answer to a credit arrives Latency after it is posted.
    UInt64 Latency{0U};
    UInt64 CreditAt{0U};
    // Drawing this character once takes Stall (a scroll on a slow scanout).
    char StallOn{'\0'};
    UInt64 Stall{0U};
};

// The diagnostic delivered at a step: each kind in turn, varied fields.
[[nodiscard]] User::Input::UsbDiagnostic ProgressRecord(const Size step) noexcept
{
    return {.Kind = static_cast<User::Input::UsbDiagnosticKind>(1U + step % User::Input::LastUsbDiagnosticKind),
            .Port = static_cast<Foundation::UInt8>(step % 5U),
            .Detail = static_cast<Foundation::UInt8>(step),
            .Code = static_cast<Foundation::UInt8>(step % 7U),
            .Value0 = static_cast<Foundation::UInt32>(step * 977U),
            .Value1 = static_cast<Foundation::UInt32>(step),
            .Data = step * 0x0101'0101ULL};
}

TerminalModel* g_clockModel{nullptr};
UInt64 Clock() noexcept
{
    return ~UInt64{0U} - 300'000U + g_clockModel->Now * 3U;
}

void Invoke(void* const state, const Abi::SystemCallRequest* const request,
            Abi::SystemCallResponse* const response) noexcept
{
    auto& model = *static_cast<TerminalModel*>(state);
    *response = {.Error = Abi::SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
    const auto number = static_cast<Abi::SystemCallNumber>(request->Number);
    if (number == Abi::SystemCallNumber::WaitSetWait)
    {
        ++model.Waits;
        if (model.Timed)
        {
            const UInt64 wait = request->Arguments[2U];
            model.Equal = model.Equal && (wait != 0U) && (wait <= 10'000U);
            model.ShortWait = model.ShortWait || (wait < 10'000U);
            // Early fault wakes must not count as whole elapsed waits.
            UInt64 elapsed = model.Waits % 5U == 0U ? 100U : wait;
            if (model.Next < model.DeliveryAt.size())
            {
                const UInt64 until =
                    model.Now < model.DeliveryAt[model.Next] ? model.DeliveryAt[model.Next] - model.Now : 1U;
                elapsed = until < elapsed ? until : elapsed;
            }
            model.Now += elapsed;
        }
        response->Error = Abi::SystemCallError::TimedOut;
    }
    else if (number == Abi::SystemCallNumber::ObjectDestroy)
    {
        response->Error = Abi::SystemCallError::WouldBlock;
    }
    else if (number == Abi::SystemCallNumber::DebugWrite)
    {
        model.Serial.append(reinterpret_cast<const char*>(request->Arguments[2U]), request->Arguments[3U]);
    }
    else if (number == Abi::SystemCallNumber::IpcSend)
    {
        const auto* const frame = reinterpret_cast<const Foundation::UInt8*>(request->Arguments[2U]);
        if (request->Arguments[0U] == 7U)
        {
            const Size count = S::LoadWord(frame + C::EchoHeaderBytes);
            const std::string text{reinterpret_cast<const char*>(frame + C::EchoHeaderBytes + C::DisplayPayloadBytes),
                                   count};
            model.Screen += text;
            if ((model.StallOn != '\0') && (text.find(model.StallOn) != std::string::npos))
            {
                model.Now += model.Stall;
                model.StallOn = '\0';
            }
        }
        else
        {
            ++model.Credits;
            model.CreditAt = model.Now;
        }
    }
    else if (number == Abi::SystemCallNumber::IpcReceive)
    {
        if (!model.Timed && (model.Next != 0U))
        {
            if (model.Equal && ((model.Serial != model.SerialAfter[model.Next - 1U]) ||
                                (model.Screen != model.ScreenAfter[model.Next - 1U])))
            {
                model.Equal = false;
                model.Mismatch = model.Next - 1U;
            }
        }
        if (model.Timed && ((model.Next == model.Events.size()) || (model.Now < model.DeliveryAt[model.Next]) ||
                            (model.Now < model.CreditAt + model.Latency)))
        {
            if (model.Now < model.End)
            {
                response->Error = Abi::SystemCallError::WouldBlock;
                return;
            }
        }
        if (model.Next == model.Events.size())
        {
            response->Error = Abi::SystemCallError::InvalidConfiguration;
            return;
        }
        auto* const delivery = reinterpret_cast<Foundation::UInt8*>(request->Arguments[2U]);
        for (Size index{0U}; index < C::EchoDeliveryHeaderBytes; ++index)
        {
            delivery[index] = 0U;
        }
        if ((model.Next < model.Progress.size()) && model.Progress[model.Next])
        {
            response->Value1 =
                C::EchoDeliveryHeaderBytes + C::WriteXhciProgress(delivery + C::EchoDeliveryHeaderBytes,
                                                                  model.Next + 1U, 0U, ProgressRecord(model.Next));
            ++model.Next;
            return;
        }
        const UInt64 words[4U]{1U, 1U, 1U, 0U};
        response->Value1 =
            C::EchoDeliveryHeaderBytes + C::WriteInputFrame(delivery + C::EchoDeliveryHeaderBytes, C::InputEventsMethod,
                                                            model.Next + 1U, words, &model.Events[model.Next], 1U);
        ++model.Next;
    }
    else
    {
        model.Equal = false;
        response->Error = Abi::SystemCallError::InvalidConfiguration;
    }
}

void Generate(TerminalModel& model, const UInt64 seed)
{
    TestSupport::Random random{seed};
    std::string serial{};
    std::string screen{};
    std::string pending{};
    Foundation::Int64 x{0};
    Foundation::Int64 y{0};
    for (Size step{0U}; step < 300U; ++step)
    {
        if ((step != 299U) && random.Chance(1U, 6U))
        {
            // A diagnostic goes between edited lines: the partial line is
            // erased, the line written whole, and the partial line redrawn.
            char line[P::UsbProgressLineBytes]{};
            const std::string text{line, P::FormatUsbProgress(0U, ProgressRecord(step), line)};
            serial += text;
            for (Size index{0U}; index < pending.size(); ++index)
            {
                screen += "\b \b";
            }
            screen += text + pending;
            model.Events.push_back({});
            model.Progress.push_back(true);
            model.SerialAfter.push_back(serial);
            model.ScreenAfter.push_back(screen);
            continue;
        }
        User::Input::InputEvent event{};
        event.Source = 1U;
        event.Kind = User::Input::EventKind::KeyDown;
        const UInt64 choice = (step == 299U) ? 3U : random.Below(8U);
        std::string typed{};
        if (choice <= 1U)
        {
            const auto letter = static_cast<Foundation::UInt16>(random.Below(26U));
            event.Usage = 4U + letter;
            event.Modifiers = choice == 1U ? 2U : 0U;
            typed += static_cast<char>((choice == 1U ? 'A' : 'a') + letter);
        }
        else if (choice == 2U)
        {
            event.Usage = 42U;
            typed = "\b \b";
            if (!pending.empty())
            {
                pending.pop_back();
            }
        }
        else if (choice == 3U)
        {
            event.Usage = 40U;
            typed = "\n";
        }
        else if (choice == 4U)
        {
            event.Kind = User::Input::EventKind::Motion;
            event.Dx = static_cast<Foundation::Int32>(random.Below(21U)) - 10;
            event.Dy = static_cast<Foundation::Int32>(random.Below(21U)) - 10;
            x += event.Dx;
            y += event.Dy;
        }
        else if (choice == 5U)
        {
            event.Kind = User::Input::EventKind::Buttons;
            event.Buttons = static_cast<Foundation::UInt8>(random.Below(8U));
            typed = "[";
            typed += (event.Buttons & 1U) != 0U ? 'L' : '-';
            typed += (event.Buttons & 4U) != 0U ? 'M' : '-';
            typed += (event.Buttons & 2U) != 0U ? 'R' : '-';
            typed += " " + std::to_string(x) + "," + std::to_string(y) + "]\n";
        }
        else
        {
            event.Usage = 4U;
            event.Kind = choice == 6U ? User::Input::EventKind::KeyUp : User::Input::EventKind::KeyDown;
            event.Modifiers = choice == 7U ? 1U : 0U;
        }
        screen += typed;
        if (choice != 2U)
        {
            pending += typed;
        }
        if (!pending.empty() && pending.back() == '\n')
        {
            serial += pending;
            pending.clear();
        }
        model.Events.push_back(event);
        model.Progress.push_back(false);
        model.SerialAfter.push_back(serial);
        model.ScreenAfter.push_back(screen);
    }
}

void Run(TestSupport::TestContext& test, const UInt64 seed, const bool outstanding, const UInt64 failedBehavior = 0U,
         const bool killed = false)
{
    auto model = std::make_unique<TerminalModel>();
    Generate(*model, seed);
    const S::SystemCallClient client{{.Function = Invoke, .Context = model.get()}};
    const P::PagerBootstrap bootstrap{};
    auto pager = std::make_unique<P::PagerProgram>(client, bootstrap);
    auto supervisor = std::make_unique<P::ComponentSupervisor>();
    P::ObjectCalls calls{};
    P::ComponentLoader loader{};
    P::DeviceManagerRole devices{};
    const P::ComponentPlatform platform{};
    calls.Configure(client);
    supervisor->Configure(calls, loader, devices, *pager, bootstrap, platform);
    P::ComponentRecord* const broker = supervisor->Allocate();
    broker->State = P::ComponentState::Running;
    broker->Tag = 1U;
    auto context = std::make_unique<P::ScenarioContext>();
    P::DisplayMirror display{.Console = {7U, 0U}, .Transaction = 0U, .Dropped = false};
    context->Client = &client;
    context->Supervisor = supervisor.get();
    context->Display = &display;
    P::ComponentRecord* failed{nullptr};
    if (failedBehavior != 0U)
    {
        failed = supervisor->Allocate();
        failed->State = P::ComponentState::Ended;
        failed->Behavior = failedBehavior;
        if (failedBehavior == Abi::ComponentBehaviorXhci)
        {
            failed->Device = static_cast<Foundation::UInt8>(Abi::PagerDeviceXhci);
            failed->ExitCode =
                killed ? Abi::KilledExitCode
                       : C::DriverFailure(Abi::ComponentDriverScratchpadStep, Abi::SystemCallError::ResourceExhausted);
        }
        else
        {
            failed->ExitCode = C::Ps2FailureCode({.Step = Asteria::User::Input::Ps2Step::KeyboardGetSet,
                                                  .Reason = Asteria::User::Input::Ps2Reason::Timeout,
                                                  .DataValid = true,
                                                  .Command = 0U,
                                                  .Status = 0x14U,
                                                  .Data = 0xFAU});
        }
        // Refused cleanup retains this record; reporting must still happen once.
        failed->WaitSet = {8U, 0U};
        context->OptionalInputDrivers = true;
        const char* line =
            (failedBehavior == Abi::ComponentBehaviorXhci)
                ? "# init: xhci failed, continuing 0000:00:00.0 step=24 err=0x06 code=0x00000618c0de00e9\n"
                : "# init: ps2 failed, continuing: keyboard F0 00 (cmd 0x00, status 0x14, byte 0xfa, timeout)"
                  " code=0xfa1400aac0de00f0\n";
        if (killed)
        {
            // A tracked xHCI driver whose teardown left its controller
            // quarantined (the record's WaitSet refuses to go) is not
            // restarted.
            P::TrackInputDriver(*failed, 0U, {11U, 0U}, 1U);
            line = "# init: xhci failed, continuing 0000:00:00.0: killed\n"
                   "# init: xhci 0000:00:00.0 not restarted: its controller stays quarantined\n";
        }
        for (Size step{0U}; step < model->Events.size(); ++step)
        {
            model->SerialAfter[step].insert(0U, line);
            model->ScreenAfter[step].insert(0U, line);
        }
    }
    S::CapabilityHandle source{};
    UInt64 transaction{0U};
    Abi::SystemCallError error{Abi::SystemCallError::None};
    Size deliveries{0U};
    for (const bool progress : model->Progress)
    {
        deliveries += progress ? 0U : 1U;
    }
    const bool echoed = P::EchoInput(*context, outstanding, *broker, {3U, 0U}, source, transaction, error);
    if (!model->Equal || echoed || (error != Abi::SystemCallError::InvalidConfiguration) ||
        (model->Waits != model->Events.size() + 1U) || (model->Credits != deliveries + (outstanding ? 0U : 1U)) ||
        display.Dropped)
    {
        static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu operation=terminal-delivery\n",
                                       static_cast<unsigned long long>(seed),
                                       model->Equal ? model->Next : model->Mismatch));
    }
    ASTERIA_EXPECT(test, "input deliveries match edited serial lines and immediate display bytes while serving",
                   !echoed, error == Abi::SystemCallError::InvalidConfiguration, model->Equal,
                   model->Waits == model->Events.size() + 1U, model->Credits == deliveries + (outstanding ? 0U : 1U),
                   !display.Dropped);
    if (failed != nullptr)
    {
        ASTERIA_EXPECT(test, "optional driver exit reports once while broker and echo stay alive",
                       failed->FailureReported, failed->State == P::ComponentState::Ended,
                       broker->State == P::ComponentState::Running);
    }
}
void TimedTerminal(TestSupport::TestContext& test)
{
    auto model = std::make_unique<TerminalModel>();
    model->Timed = true;
    model->End = 3'000'000U;
    const auto add = [&](const UInt64 at, const User::Input::EventKind kind, const Foundation::UInt16 usage,
                         const Foundation::UInt8 modifiers = 0U)
    {
        model->Events.push_back({.Kind = kind,
                                 .Modifiers = modifiers,
                                 .Usage = usage,
                                 .Source = 1U,
                                 .Dx = 0,
                                 .Dy = 0,
                                 .Buttons = 0U,
                                 .Reserved = {},
                                 .Wheel = 0});
        model->DeliveryAt.push_back(at);
    };
    add(10'000U, User::Input::EventKind::KeyDown, 4U);
    add(610'000U, User::Input::EventKind::KeyDown, 0xE1U, 2U);
    add(710'000U, User::Input::EventKind::KeyUp, 0xE1U);
    add(850'000U, User::Input::EventKind::KeyUp, 4U);
    add(1'100'000U, User::Input::EventKind::KeyDown, 40U);
    add(1'110'000U, User::Input::EventKind::KeyUp, 40U);
    for (UInt64 index{0U}; index < 32U; ++index)
    {
        add(1'120'000U + index * 10'000U, User::Input::EventKind::KeyDown, 27U);
        add(1'125'000U + index * 10'000U, User::Input::EventKind::KeyUp, 27U);
    }
    add(1'500'000U, User::Input::EventKind::KeyDown, 42U);
    add(2'400'000U, User::Input::EventKind::KeyUp, 42U);
    add(2'650'000U, User::Input::EventKind::KeyDown, 40U);
    add(2'660'000U, User::Input::EventKind::KeyUp, 40U);
    const S::SystemCallClient client{{.Function = Invoke, .Context = model.get()}};
    P::PagerBootstrap bootstrap{};
    bootstrap.ClockTicksPerMicrosecond = 3U;
    auto pager = std::make_unique<P::PagerProgram>(client, bootstrap);
    auto supervisor = std::make_unique<P::ComponentSupervisor>();
    P::ObjectCalls calls{};
    P::ComponentLoader loader{};
    P::DeviceManagerRole devices{};
    P::ComponentPlatform platform{};
    platform.Clock = Clock;
    calls.Configure(client);
    supervisor->Configure(calls, loader, devices, *pager, bootstrap, platform);
    P::ComponentRecord* const broker = supervisor->Allocate();
    broker->State = P::ComponentState::Running;
    broker->Tag = 1U;
    auto context = std::make_unique<P::ScenarioContext>();
    auto launch = std::make_unique<P::PagerThreadLaunch>();
    launch->Component = platform;
    P::DisplayMirror display{.Console = {7U, 0U}, .Transaction = 0U, .Dropped = false};
    context->Client = &client;
    context->Supervisor = supervisor.get();
    context->Display = &display;
    context->Bootstrap = &bootstrap;
    context->Launch = launch.get();
    g_clockModel = model.get();
    S::CapabilityHandle source{};
    UInt64 transaction{0U};
    Abi::SystemCallError error{Abi::SystemCallError::None};
    static_cast<void>(P::EchoInput(*context, false, *broker, {3U, 0U}, source, transaction, error));
    const auto newline = model->Screen.find('\n');
    const auto first = model->Screen.substr(0U, newline + 1U);
    const auto upper = first.find("AAA");
    const Size erases = (model->Screen.size() - first.size() - 33U) / 3U;
    ASTERIA_EXPECT(test, "elapsed clock repeats through fault wakes with current Shift and stops on release",
                   model->Equal, model->ShortWait, first.size() >= 10U, first.size() <= 14U, upper != std::string::npos,
                   first.back() == '\n', model->Serial.starts_with(first),
                   error == Abi::SystemCallError::InvalidConfiguration);
    ASTERIA_EXPECT(
        test, "held Backspace edits display and serial identically then stops", erases >= 12U, erases <= 14U,
        model->Serial == first + std::string(32U - erases, 'x') + "\n",
        model->Screen == first + std::string(32U, 'x') +
                             [&]()
                             {
                                 std::string erased{};
                                 for (Size index{0U}; index < erases; ++index)
                                 {
                                     erased += "\b \b";
                                 }
                                 return erased;
                             }() +
                             "\n");
    g_clockModel = nullptr;
}

} // namespace
} // namespace Asteria::Test

namespace Asteria::Test
{
namespace
{
// Drawing can outlast the repeat delay while a release waits in the broker.
// The terminal must consume the release before generating catch-up repeats.
void StalledDrawing(TestSupport::TestContext& test)
{
    auto model = std::make_unique<TerminalModel>();
    model->Timed = true;
    model->End = 1'500'000U;
    model->Latency = 2'000U;
    model->StallOn = 'h';
    model->Stall = 700'000U;
    model->Events.push_back({.Kind = User::Input::EventKind::KeyDown,
                             .Modifiers = 0U,
                             .Usage = 0x0BU,
                             .Source = 1U,
                             .Dx = 0,
                             .Dy = 0,
                             .Buttons = 0U,
                             .Reserved = {},
                             .Wheel = 0});
    model->DeliveryAt.push_back(10'000U);
    model->Events.push_back({.Kind = User::Input::EventKind::KeyUp,
                             .Modifiers = 0U,
                             .Usage = 0x0BU,
                             .Source = 1U,
                             .Dx = 0,
                             .Dy = 0,
                             .Buttons = 0U,
                             .Reserved = {},
                             .Wheel = 0});
    model->DeliveryAt.push_back(90'000U);
    const S::SystemCallClient client{{.Function = Invoke, .Context = model.get()}};
    P::PagerBootstrap bootstrap{};
    bootstrap.ClockTicksPerMicrosecond = 3U;
    auto pager = std::make_unique<P::PagerProgram>(client, bootstrap);
    auto supervisor = std::make_unique<P::ComponentSupervisor>();
    P::ObjectCalls calls{};
    P::ComponentLoader loader{};
    P::DeviceManagerRole devices{};
    P::ComponentPlatform platform{};
    platform.Clock = Clock;
    calls.Configure(client);
    supervisor->Configure(calls, loader, devices, *pager, bootstrap, platform);
    P::ComponentRecord* const broker = supervisor->Allocate();
    broker->State = P::ComponentState::Running;
    broker->Tag = 1U;
    auto context = std::make_unique<P::ScenarioContext>();
    auto launch = std::make_unique<P::PagerThreadLaunch>();
    launch->Component = platform;
    P::DisplayMirror display{.Console = {7U, 0U}, .Transaction = 0U, .Dropped = false};
    context->Client = &client;
    context->Supervisor = supervisor.get();
    context->Display = &display;
    context->Bootstrap = &bootstrap;
    context->Launch = launch.get();
    g_clockModel = model.get();
    S::CapabilityHandle source{};
    UInt64 transaction{0U};
    Abi::SystemCallError error{Abi::SystemCallError::None};
    static_cast<void>(P::EchoInput(*context, false, *broker, {3U, 0U}, source, transaction, error));
    ASTERIA_EXPECT(test, "a draw outlasting the delay emits no repeat before the queued release", model->Equal,
                   model->Screen == "h", model->Next == 2U, error == Abi::SystemCallError::InvalidConfiguration);
    g_clockModel = nullptr;
}
} // namespace
} // namespace Asteria::Test

namespace Asteria::Test
{
// The PS/2 lines as the user reads them on a machine without serial.
void Ps2Lines(TestSupport::TestContext& test)
{
    namespace In = User::Input;
    using Foundation::UInt32;
    char line[P::Ps2LineBytes]{};
    const auto text = [&line](const Size bytes)
    {
        return std::string(line, bytes);
    };
    const UInt64 fallback = C::Ps2FailureCode({.Step = In::Ps2Step::VerifyConfiguration,
                                               .Reason = In::Ps2Reason::Unexpected,
                                               .DataValid = true,
                                               .Command = 0x20U,
                                               .Status = 0x1DU,
                                               .Data = 0x65U});
    const UInt32 set1 = static_cast<UInt32>(C::Ps2ReadyMethod) | (1U << C::Ps2ReadyModeShift);
    const UInt32 set2Firmware = static_cast<UInt32>(C::Ps2ReadyAuxFailedMethod) | (2U << C::Ps2ReadyModeShift);
    ASTERIA_EXPECT(
        test, "mode lines name the decoding and the abandoned set-2 step",
        text(P::FormatPs2Mode(line, static_cast<UInt32>(C::Ps2ReadyMethod), 1U)) == "# init: ps2 keyboard=set2\n",
        text(P::FormatPs2Mode(line, set1, fallback)) ==
            "# init: ps2 keyboard=set1-translated; set 2 failed at controller 20 read-back"
            " (cmd 0x20, status 0x1d, byte 0x65, unexpected)\n",
        text(P::FormatPs2Mode(line, set2Firmware,
                              C::Ps2FailureCode({.Step = In::Ps2Step::KeyboardSetSet2,
                                                 .Reason = In::Ps2Reason::Resend,
                                                 .DataValid = false,
                                                 .Command = 2U,
                                                 .Status = 0U,
                                                 .Data = 0U}))) ==
            "# init: ps2 keyboard=set2-unnegotiated; set 2 failed at keyboard F0 02"
            " (cmd 0x02, status 0x00, byte none, resend exhausted)\n",
        text(P::FormatPs2Mode(line, set1, 1U)) == "# init: ps2 keyboard=set1-translated\n",
        text(P::FormatPs2Mode(line, static_cast<UInt32>(C::Ps2ReadyMethod) | (9U << C::Ps2ReadyModeShift), fallback)) ==
            "# init: ps2 keyboard=unknown\n");
    ASTERIA_EXPECT(test, "failure lines decode the step or show a foreign code",
                   text(P::FormatPs2Failure(line, C::Ps2FailureCode({.Step = In::Ps2Step::Connect,
                                                                     .Reason = In::Ps2Reason::Transport,
                                                                     .DataValid = false,
                                                                     .Command = 0U,
                                                                     .Status = 0U,
                                                                     .Data = 0U}))) ==
                       "# init: ps2 failed, continuing: interrupt setup (cmd 0x00, status 0x00, byte none, transport)"
                       " code=0x00000061c0de00f0\n",
                   text(P::FormatPs2Failure(line, Abi::ComponentCrashReturnedExitCode)) ==
                       "# init: ps2 failed, continuing code=0x00000000c0de00e3\n",
                   text(P::FormatPs2Failure(line, 0U)) == "# init: ps2 failed, continuing\n");
}
} // namespace Asteria::Test

int main()
{
    Asteria::TestSupport::TestContext test{"InputTerminalTests"};
    Asteria::Test::Ps2Lines(test);
    const auto seeds = Asteria::TestSupport::GetSeedRange(1U, 8U);
    for (Asteria::Foundation::UInt64 index{0U}; index < seeds.Count; ++index)
    {
        Asteria::Test::Run(test, seeds.First + index, false);
        Asteria::Test::Run(test, seeds.First + index, true);
        Asteria::Test::Run(test, seeds.First + index, false, Asteria::Abi::ComponentBehaviorXhci);
        Asteria::Test::Run(test, seeds.First + index, false, Asteria::Abi::ComponentBehaviorPs2);
        // A supervisor's kill names no driver step.
        Asteria::Test::Run(test, seeds.First + index, false, Asteria::Abi::ComponentBehaviorXhci, true);
    }
    Asteria::Test::TimedTerminal(test);
    Asteria::Test::StalledDrawing(test);
    return test.Finish();
}
