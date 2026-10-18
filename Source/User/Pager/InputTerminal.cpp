// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/InputTerminal.hpp"

#include "Abi/Clock.hpp"
#include "User/Pager/PowerOff.hpp"
#include "User/Pager/ProductDesktop.hpp"
#include "User/Pager/ProductLogin.hpp"

#include "User/Component/Ps2Driver.hpp"
#include "User/Component/XhciDriver.hpp"
#include "Inventory/ReportText.hpp"
#include "User/Input/KeyRepeat.hpp"
#include "User/Pager/DeviceInventory.hpp"
#include "User/Pager/UsbProgress.hpp"
#include "User/Component/DriverSetup.hpp"

namespace Asteria::User::Pager
{
struct TerminalEcho
{
    /// The pointer position the terminal accumulates from motion.
    Foundation::Int64 Pointer[2U];
    Input::KeyRepeat Repeat;
    const ComponentRecord* Broker;
    PagerThreadClock Clock;
    Foundation::UInt64 Scale;
    Foundation::UInt64 Origin;
    /// Whether the terminal's credit is outstanding with the broker.
    bool Outstanding;
};

namespace
{
namespace C = Component;
// The supervisor's ordinary answer wait: two seconds of ten-millisecond polls.
constexpr Foundation::UInt64 BrokerReplyAttempts{200U};
constinit Foundation::UInt64 g_registrationTransaction{0U};
constinit Foundation::UInt64 g_registrationId{0U};

// DiagnosticChannel accepts complete printable lines. Keep the edited line
// here while the display receives each key immediately; long lines are emitted
// in bounded chunks on serial. These buffers belong to the pager thread.
constinit char g_line[128U]{};
constinit Foundation::Size g_length{0U};
constinit Foundation::UInt8 g_powerRequest{0U};

// "<step> (cmd 0xNN, status 0xNN, byte 0xNN|none, <reason>)".
void AppendPs2Diagnostic(Inventory::ReportText& text, const Input::Ps2Diagnostic& diagnostic) noexcept
{
    text.Text(Input::Ps2StepText(diagnostic.Step));
    text.Text(" (cmd 0x");
    text.Hex(diagnostic.Command, 2U);
    text.Text(", status 0x");
    text.Hex(diagnostic.Status, 2U);
    text.Text(", byte ");
    if (diagnostic.DataValid)
    {
        text.Text("0x");
        text.Hex(diagnostic.Data, 2U);
    }
    else
    {
        text.Text("none");
    }
    text.Text(", ");
    text.Text(Input::Ps2ReasonText(diagnostic.Reason));
    text.Character(')');
}

void ReportInputFailure(ScenarioContext& context, const Foundation::UInt64 code) noexcept
{
    char line[Ps2LineBytes]{};
    // A driver may end while the terminal holds a partial edited line.
    WriteBetweenLines(context, line, FormatPs2Failure(line, code));
}

// A USB input driver's restart record : its start block's flags, the broker source it publishes on
// and the restarts spent; one per xHCI catalog index and one for the USB host group.
struct TrackedInputDriver
{
    System::CapabilityHandle Source;
    Foundation::UInt64 SourceId;
    Foundation::UInt64 Flags;
    Foundation::UInt8 Restarts;
    bool Tracked;
};
constinit TrackedInputDriver g_tracked[Abi::PagerXhciControllers + 1U]{};
constinit Foundation::UInt64 g_recoveryTransaction{0U};

[[nodiscard]] TrackedInputDriver* TrackedOf(const Foundation::UInt8 device) noexcept
{
    if (device == ComponentUsbHostGroup)
    {
        return &g_tracked[Abi::PagerXhciControllers];
    }
    return ((device >= Abi::PagerDeviceXhci) && (device - Abi::PagerDeviceXhci < Abi::PagerXhciControllers))
               ? &g_tracked[device - Abi::PagerDeviceXhci]
               : nullptr;
}

// "# init: <driver> <what>[ (n of 3)]", or for a driver given up "# init:
// <driver> gave up after 3 restarts".
void ReportRecovery(const ScenarioContext& context, const Foundation::UInt8 device, const char* const what,
                    const Foundation::UInt8 restart) noexcept
{
    char line[96U]{};
    Inventory::ReportText text{line, sizeof(line)};
    text.Text("# init: ");
    AppendInputDriver(text, device);
    text.Character(' ');
    if (what == nullptr)
    {
        text.Text("gave up after ");
        text.Decimal(InputDriverRestarts);
        text.Text(" restarts");
    }
    else
    {
        text.Text(what);
    }
    if ((what != nullptr) && (restart != 0U))
    {
        text.Text(" (");
        text.Decimal(restart);
        text.Text(" of ");
        text.Decimal(InputDriverRestarts);
        text.Character(')');
    }
    text.Character('\n');
    WriteBetweenLines(context, line, text.Bytes());
}

// "# usb: <controller> quiet (...), released" or "# usb: <controller> not
// quiet: <why>, quarantined": the Device Manager's proof for one controller.
void ReportQuiesce(const ScenarioContext& context, const Foundation::Size device) noexcept
{
    const DeviceQuiesce& quiesce = context.Devices->GetQuiesce(device);
    char line[112U]{};
    Inventory::ReportText text{line, sizeof(line)};
    const bool xhci = device < Abi::PagerDeviceUsbHost;
    constexpr const char* Kinds[]{"usb", "ehci", "ohci", "uhci"};
    text.Text("# usb: ");
    text.Text(xhci ? "xhci" : Kinds[static_cast<Foundation::UInt8>(quiesce.Kind) & 3U]);
    text.Decimal(xhci ? device - Abi::PagerDeviceXhci : device - Abi::PagerDeviceUsbHost);
    switch (quiesce.Outcome)
    {
    case DeviceQuiesceOutcome::Quiet:
        text.Text(quiesce.Running ? " quiet (was running, halted and reset, " : " quiet (halted and reset, ");
        text.Decimal(quiesce.Polls);
        text.Text(" polls, status 0x");
        text.Hex(quiesce.Status, 8U);
        text.Text("), released");
        break;
    case DeviceQuiesceOutcome::FirmwareOwned:
        text.Text(" not quiet: firmware owns it, quarantined");
        break;
    case DeviceQuiesceOutcome::TimedOut:
        text.Text(" not quiet: timed out, quarantined");
        break;
    case DeviceQuiesceOutcome::None:
    case DeviceQuiesceOutcome::Invalid:
        text.Text(" not quiet: unreadable, quarantined");
        break;
    }
    text.Character('\n');
    WriteBetweenLines(context, line, text.Bytes());
}

// One request to the broker on endpoint, answered on the root's reply
// Endpoint, apart from the terminal's events on its report Endpoint; the
// answer is Done for broker's epoch. An Endpoint copy the answer carries is
// closed.
[[nodiscard, gnu::noinline]] bool ExchangeOnReply(ScenarioContext& context, const ComponentRecord& broker,
                                                  const System::CapabilityHandle endpoint,
                                                  const Foundation::UInt32 method, const Foundation::UInt64 argument,
                                                  Abi::SystemCallError& error) noexcept
{
    const Foundation::UInt64 words[4U]{broker.Tag, argument, 0U, 0U};
    const Foundation::UInt64 transaction = ++g_recoveryTransaction;
    const auto size = C::WriteInputFrame(context.Frame, method, transaction, words, nullptr, 0U, &context.Reply);
    if ((size == 0U) || (context.Reply.Selector == 0U) ||
        !Check(context.Client->Send(endpoint, System::AddressOf(context.Frame), size), error))
    {
        return false;
    }
    const auto received = AwaitMessage(context, broker, context.Reply, error);
    C::InputMessage reply{};
    if ((received == 0U) || !C::ReadInputFrame(context.Delivery, static_cast<Foundation::Size>(received), reply) ||
        (reply.Method != C::InputReplyMethod) || (reply.Transaction != transaction))
    {
        return false;
    }
    const bool closed = (reply.Reply.Selector == 0U) || context.Calls->Close(reply.Reply, error);
    return closed && (reply.Words[0U] == broker.Tag) &&
           (reply.Words[1U] == static_cast<Foundation::UInt64>(C::InputAnswer::Done));
}

// Registers a restarted driver's source again (InputBroker.md: RegisterSource
// naming the source): first a Query on the source itself, which the broker
// answers only after whatever the dead instance queued there, so nothing of
// it is processed after the registration; then the registration, which
// releases the dead instance's held keys and clears the replay fence. The
// root keeps its own source capability.
[[nodiscard]] bool RegisterSourceAgain(ScenarioContext& context, const ComponentRecord& broker,
                                       const TrackedInputDriver& tracked, Abi::SystemCallError& error) noexcept
{
    return ExchangeOnReply(context, broker, tracked.Source, C::InputQueryMethod, 0U, error) &&
           ExchangeOnReply(context, broker, broker.Service, C::InputRegisterSourceMethod, tracked.SourceId, error);
}

// Restarts a tracked driver whose devices were proven quiet: the new
// instance once it reported ready, null otherwise. A failed start leaves its
// record ended, which the caller's next pass reports and tears down.
[[nodiscard, gnu::noinline]] ComponentRecord*
RestartInputDriver(ScenarioContext& context, const ComponentRecord& broker, const Foundation::UInt8 device,
                   TrackedInputDriver& tracked, TerminalEcho* const echo) noexcept
{
    ++tracked.Restarts;
    ReportRecovery(context, device, "restarting", tracked.Restarts);
    Abi::SystemCallError error{Abi::SystemCallError::None};
    if (!RegisterSourceAgain(context, broker, tracked, error))
    {
        ReportRecovery(context, device, "not restarted: the broker refused its source", 0U);
        tracked.Tracked = false;
        return nullptr;
    }
    context.ServiceFlags = tracked.Flags;
    ComponentRecord* const restarted =
        StartInputDriver(context, tracked.Source, broker.Tag, Abi::ComponentBehaviorXhci, error, echo);
    context.ServiceFlags = 0U;
    if (restarted != nullptr)
    {
        ReportRecovery(context, device, "restarted", tracked.Restarts);
    }
    return restarted;
}

[[nodiscard]] bool RetireInputDrivers(ScenarioContext& context) noexcept
{
    return RecoverInputDrivers(context, nullptr, nullptr).Ended != 0U;
}

void WriteTerminal(const ScenarioContext& context, const char* const text, const Foundation::Size bytes) noexcept
{
    WriteConsole(context, text, bytes);
    for (Foundation::Size index{0U}; index < bytes; ++index)
    {
        const char character = text[index];
        if (character == '\b')
        {
            if (g_length != 0U)
            {
                --g_length;
            }
            continue;
        }
        g_line[g_length++] = (character == '\t') ? ' ' : character;
        if ((character == '\n') || (g_length == sizeof(g_line) - 1U))
        {
            if (character != '\n')
            {
                g_line[g_length++] = '\n';
            }
            static_cast<void>(context.Client->DebugWrite(context.Diagnostic, System::AddressOf(g_line), g_length));
            g_length = 0U;
        }
    }
    // Lines written between edited lines erase and redraw this partial line.
    if (context.Display != nullptr)
    {
        context.Display->PartialLine = g_line;
        context.Display->PartialBytes = g_length;
    }
}

void EchoEvents(const ScenarioContext& context, const C::InputMessage& reply, Foundation::Int64 (&pointer)[2U],
                Input::KeyRepeat& repeat, Foundation::UInt64 now) noexcept;

[[nodiscard]] Foundation::UInt64 Elapsed(const TerminalEcho& echo) noexcept
{
    return ((echo.Clock == nullptr) || (echo.Scale == 0U))
               ? 0U
               : (Abi::ElapsedClockTicks(echo.Clock(), echo.Origin)) / echo.Scale;
}

// The broker's answer to the terminal's outstanding credit, which arrives on
// the report Endpoint while a restarted driver's readiness is awaited there:
// echoed, the credit spent. False for any other delivery.
[[nodiscard]] bool EchoDelivery(const ScenarioContext& context, TerminalEcho& echo,
                                const Foundation::UInt64 received) noexcept
{
    C::InputMessage reply{};
    if ((echo.Broker == nullptr) ||
        !C::ReadInputFrame(context.Delivery, static_cast<Foundation::Size>(received), reply) ||
        (reply.Method != C::InputEventsMethod) || (reply.Words[0U] != echo.Broker->Tag))
    {
        return false;
    }
    echo.Outstanding = false;
    EchoEvents(context, reply, echo.Pointer, echo.Repeat, Elapsed(echo));
    return true;
}

// AwaitMessageFor on the report Endpoint, printing the xHCI diagnostics that
// arrive before the awaited message, and with echo the broker's events. Each
// driver bounds its diagnostics, and so does this wait.
[[nodiscard]] Foundation::UInt64 AwaitReport(ScenarioContext& context, const ComponentRecord& component,
                                             const System::CapabilityHandle endpoint, const Foundation::UInt64 attempts,
                                             Abi::SystemCallError& error, TerminalEcho* const echo = nullptr) noexcept
{
    for (Foundation::Size printed{0U}; printed < 4096U; ++printed)
    {
        const Foundation::UInt64 received = context.Supervisor->AwaitMessageFor(
            component, endpoint, context.Delivery, sizeof(context.Delivery), attempts, error);
        if ((received == 0U) ||
            (!PrintUsbProgress(context, received) && ((echo == nullptr) || !EchoDelivery(context, *echo, received))))
        {
            return received;
        }
    }
    return 0U;
}

// The character a key-down event types on a US layout, or 0: letters,
// digits, Enter, Backspace, Tab, Space and punctuation, with Shift only.
[[nodiscard]] char Character(const Input::InputEvent& event) noexcept
{
    static constexpr char plain[]{"abcdefghijklmnopqrstuvwxyz1234567890\n\0\b\t -=[]\\\0;'`,./"};
    static constexpr char shifted[]{"ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\0\b\t _+{}|\0:\"~<>?"};
    static_assert((sizeof(plain) == 54U) && (sizeof(shifted) == 54U));
    constexpr Foundation::UInt8 Shift{0x22U};
    if ((event.Usage < 4U) || (event.Usage >= 4U + sizeof(plain) - 1U) || ((event.Modifiers & ~Shift) != 0U))
    {
        return '\0';
    }
    return ((event.Modifiers & Shift) != 0U) ? shifted[event.Usage - 4U] : plain[event.Usage - 4U];
}

void AppendSigned(char* const text, Foundation::Size& length, const Foundation::Int64 value) noexcept
{
    Foundation::UInt64 magnitude =
        value < 0 ? 0U - static_cast<Foundation::UInt64>(value) : static_cast<Foundation::UInt64>(value);
    if (value < 0)
    {
        text[length++] = '-';
    }
    char digits[20U]{};
    Foundation::Size count{0U};
    do
    {
        digits[count++] = static_cast<char>('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude != 0U);
    while (count != 0U)
    {
        text[length++] = digits[--count];
    }
}

// Writes what one delivery of events types: characters, Backspace as an
// erase, and each button change with the pointer's accumulated position.
[[gnu::noinline]] void EchoEvents(const ScenarioContext& context, const C::InputMessage& reply,
                                  Foundation::Int64 (&pointer)[2U], Input::KeyRepeat& repeat,
                                  const Foundation::UInt64 now) noexcept
{
    char text[112U]{};
    Foundation::Size length{0U};
    for (Foundation::Size index{0U}; index < reply.Count; ++index)
    {
        if (length > sizeof(text) - 56U)
        {
            WriteTerminal(context, text, length);
            length = 0U;
        }
        const auto event = C::ReadInputEvent(reply.Events + index * 24U);
        // PowerOff.md: Ctrl+Alt+F10/F11 test path, never Super.
        if (event.Kind == Input::EventKind::KeyDown && (event.Modifiers & 0x11U) != 0U &&
            (event.Modifiers & 0x44U) != 0U && (event.Modifiers & 0xAAU) == 0U &&
            (event.Usage >= 66U && event.Usage <= 68U))
        {
            g_powerRequest = event.Usage == 66U ? 3U : (event.Usage == 68U ? 2U : 1U);
            continue;
        }
        repeat.Feed(event, now);
        if (event.Kind == Input::EventKind::KeyDown)
        {
            const char character = Character(event);
            if (character == '\b')
            {
                text[length++] = '\b';
                text[length++] = ' ';
            }
            if (character != '\0')
            {
                text[length++] = character;
            }
        }
        else if (event.Kind == Input::EventKind::Motion)
        {
            pointer[0U] += event.Dx;
            pointer[1U] += event.Dy;
        }
        else if (event.Kind == Input::EventKind::Buttons)
        {
            text[length++] = '[';
            text[length++] = ((event.Buttons & 1U) != 0U) ? 'L' : '-';
            text[length++] = ((event.Buttons & 4U) != 0U) ? 'M' : '-';
            text[length++] = ((event.Buttons & 2U) != 0U) ? 'R' : '-';
            text[length++] = ' ';
            AppendSigned(text, length, pointer[0U]);
            text[length++] = ',';
            AppendSigned(text, length, pointer[1U]);
            text[length++] = ']';
            text[length++] = '\n';
        }
    }
    if (length != 0U)
    {
        WriteTerminal(context, text, length);
    }
}

void EchoRepeats(const ScenarioContext& context, Input::KeyRepeat& repeat, const Foundation::UInt64 now) noexcept
{
    Input::InputEvent event{};
    const Foundation::UInt64 count = repeat.TakeDue(now, event);
    const char character = Character(event);
    if (character == '\0')
    {
        return;
    }
    const char erase[3U]{'\b', ' ', '\b'};
    for (Foundation::UInt64 index{0U}; index < count; ++index)
    {
        WriteTerminal(context, character == '\b' ? erase : &character, character == '\b' ? 3U : 1U);
    }
}

[[nodiscard, gnu::noinline]] bool Exchange(ScenarioContext& context, const ComponentRecord& broker,
                                           System::CapabilityHandle endpoint, Foundation::UInt32 method,
                                           Foundation::UInt64 argument, Foundation::UInt64& transaction,
                                           Component::InputMessage& reply, Abi::SystemCallError& error) noexcept
{
    const Foundation::UInt64 words[4U]{broker.Tag, argument, 0U, 0U};
    const Foundation::Size size =
        Component::WriteInputFrame(context.Frame, method, ++transaction, words, nullptr, 0U, &context.Report);
    if ((size == 0U) || !Check(context.Client->Send(endpoint, System::AddressOf(context.Frame), size), error))
    {
        return false;
    }
    // A second controller registers while the first one's diagnostics arrive.
    const Foundation::UInt64 received = AwaitReport(context, broker, context.Report, BrokerReplyAttempts, error);
    return (received != 0U) &&
           Component::ReadInputFrame(context.Delivery, static_cast<Foundation::Size>(received), reply) &&
           (reply.Method == Component::InputReplyMethod) && (reply.Transaction == transaction) &&
           (reply.Words[0U] == broker.Tag) &&
           (reply.Words[1U] == static_cast<Foundation::UInt64>(Component::InputAnswer::Done));
}
} // Namespace

[[nodiscard, gnu::noinline]] bool RegisterInputAgain(ScenarioContext& context, const ComponentRecord& broker,
                                                     const C::InputMessage& request, System::CapabilityHandle& source,
                                                     Foundation::UInt64& nextTransaction,
                                                     Abi::SystemCallError& error) noexcept
{
    const auto replyEndpoint = request.Reply;
    const auto transaction = request.Transaction;
    if ((source.Selector == 0U))
    {
        return false;
    }
    if (transaction != g_registrationTransaction)
    {
        const Foundation::UInt64 words[4U]{broker.Tag, 0U, 0U, 0U};
        const auto size = C::WriteInputFrame(context.Frame, C::InputRegisterSourceMethod, ++nextTransaction, words,
                                             nullptr, 0U, &context.Reply);
        if (!Check(context.Client->Send(broker.Service, System::AddressOf(context.Frame), size), error))
        {
            return false;
        }
        const auto received = AwaitMessage(context, broker, context.Reply, error);
        C::InputMessage registered{};
        if ((received == 0U) ||
            !C::ReadInputFrame(context.Delivery, static_cast<Foundation::Size>(received), registered) ||
            (registered.Method != C::InputReplyMethod) || (registered.Transaction != nextTransaction) ||
            (registered.Words[0U] != broker.Tag) || (registered.Words[1U] != 1U) || (registered.Reply.Selector == 0U) ||
            !context.Calls->Close(source, error))
        {
            return false;
        }
        source = registered.Reply;
        g_registrationTransaction = transaction;
        g_registrationId = registered.Words[2U];
    }
    const Foundation::UInt64 words[4U]{broker.Tag, 1U, g_registrationId, C::InputProtocolVersion};
    const auto size = C::WriteInputFrame(context.Frame, C::InputReplyMethod, transaction, words, nullptr, 0U, &source);
    for (Foundation::Size attempt{0U}; attempt < 100U; ++attempt)
    {
        const auto sent = context.Client->Send(replyEndpoint, System::AddressOf(context.Frame), size);
        if (sent.Error == Abi::SystemCallError::None)
        {
            return context.Calls->Close(replyEndpoint, error);
        }
        if ((sent.Error != Abi::SystemCallError::WouldBlock) ||
            (context.Supervisor->Pause(10'000U, error) != PagerServeError::None))
        {
            return false;
        }
    }
    return false;
}

[[nodiscard, gnu::noinline]] bool EchoInput(ScenarioContext& context, const bool creditOutstanding,
                                            const ComponentRecord& broker, System::CapabilityHandle control,
                                            System::CapabilityHandle& source, Foundation::UInt64& transaction,
                                            Abi::SystemCallError& error) noexcept
{
    TerminalEcho echo{.Pointer = {},
                      .Repeat = Input::KeyRepeat{},
                      .Broker = &broker,
                      .Clock = context.Launch == nullptr ? nullptr : context.Launch->Component.Clock,
                      .Scale = context.Bootstrap == nullptr ? 0U : context.Bootstrap->ClockTicksPerMicrosecond,
                      .Origin = 0U,
                      .Outstanding = creditOutstanding};
    echo.Origin = echo.Clock == nullptr ? 0U : echo.Clock();
    for (;;)
    {
        if (!(context.Bootstrap != nullptr && context.Bootstrap->LoginEnabled != 0U
                  ? StepProductLogin(context, error)
                  : StepProductDesktop(context, error)))
        {
            return false;
        }
        if (g_powerRequest != 0U)
        {
            const auto request = g_powerRequest;
            g_powerRequest = 0U;
            if (request == 3U)
            {
                if (!PreparePowerTestVolume(context, error))
                {
                    constexpr char text[]{"# power: test volume unavailable\n"};
                    WriteBetweenLines(context, text, sizeof(text) - 1U);
                }
            }
            else if (!RequestPower(context, request == 2U, error))
            {
                return false;
            }
        }
        if (!echo.Outstanding)
        {
            const Foundation::UInt64 words[4U]{broker.Tag, C::InputMaximumEvents, 0U, 0U};
            const auto size = C::WriteInputFrame(context.Frame, C::InputCreditMethod, ++transaction, words, nullptr, 0U,
                                                 &context.Report);
            if ((size == 0U) || !Check(context.Client->Send(control, System::AddressOf(context.Frame), size), error))
            {
                return false;
            }
            echo.Outstanding = true;
            // Drawing the last delivery may have outlasted the repeat delay (a
            // scroll on a slow scanout). Releases queued meanwhile come on this
            // credit: a repeat that fell due waits for them instead of firing.
            echo.Repeat.Settle(Elapsed(echo), RepeatSettleMicroseconds);
        }
        // Keep the supervisor's fault-serving/reap step, but never sleep past
        // The next repeat. A due deadline uses a positive one-microsecond wait; WaitSet timeout
        // zero means an indefinite wait in this ABI.
        const Foundation::UInt64 wait = echo.Repeat.WaitMicroseconds(Elapsed(echo), 10'000U);
        if (context.Supervisor->Step(wait == 0U ? 1U : wait, error) != PagerServeError::None)
        {
            return false;
        }
        // Recovery reuses the delivery buffer, so it precedes the Receive.
        const bool sourceGone = RecoverInputDrivers(context, &broker, &echo).Ended != 0U;
        if (sourceGone)
        {
            echo.Repeat.Reset();
        }
        const auto delivery =
            context.Client->Receive(context.Report, System::AddressOf(context.Delivery), sizeof(context.Delivery));
        if ((delivery.Error != Abi::SystemCallError::None) && (delivery.Error != Abi::SystemCallError::WouldBlock))
        {
            error = delivery.Error;
            return false;
        }
        Foundation::UInt64 received = delivery.Error == Abi::SystemCallError::WouldBlock ? 0U : delivery.Value1;
        const Foundation::UInt64 now = Elapsed(echo);
        if ((received != 0U) && PrintUsbProgress(context, received))
        {
            received = 0U;
        }
        if (received != 0U)
        {
            C::InputMessage reply{};
            if (!C::ReadInputFrame(context.Delivery, static_cast<Foundation::Size>(received), reply))
            {
                return false;
            }
            if (reply.Method == C::InputRegisterSourceMethod)
            {
                echo.Repeat.Reset();
                if (!RegisterInputAgain(context, broker, reply, source, transaction, error))
                {
                    return false;
                }
                continue;
            }
            if ((reply.Method != C::InputEventsMethod) || (reply.Words[0U] != broker.Tag))
            {
                return false;
            }
            echo.Outstanding = false;
            EchoEvents(context, reply, echo.Pointer, echo.Repeat, now);
            if (sourceGone)
            {
                // A delivery queued before retirement must not restart a held key.
                echo.Repeat.Reset();
            }
        }
        if (received == 0U)
        {
            if ((error != Abi::SystemCallError::None) || (broker.State != ComponentState::Running))
            {
                return false;
            }
            EchoRepeats(context, echo.Repeat, now);
            continue;
        }
        EchoRepeats(context, echo.Repeat, now);
        Foundation::UInt64 epoch{0U};
        Foundation::UInt64 sequence{0U};
        if (!FencePointer(context, epoch, sequence, error))
        {
            return false;
        }
    }
}

char InputTerminalCharacter(const Input::InputEvent& event) noexcept
{
    return Character(event);
}

Foundation::Size FormatPs2Failure(char (&line)[Ps2LineBytes], const Foundation::UInt64 code) noexcept
{
    Inventory::ReportText text{line, sizeof(line)};
    text.Text("# init: ps2 failed, continuing");
    if ((code & 0xFFFF'FFFFU) == Abi::ComponentInputFailedExitCode)
    {
        text.Text(": ");
        AppendPs2Diagnostic(text, Input::DecodePs2Diagnostic(static_cast<Foundation::UInt32>(code >> 32U)));
    }
    if (code != 0U)
    {
        text.Text(" code=0x");
        text.Hex(code, 16U);
    }
    text.Character('\n');
    return text.Bytes();
}

Foundation::Size FormatPs2Mode(char (&line)[Ps2LineBytes], const Foundation::UInt32 method,
                               const Foundation::UInt64 transaction) noexcept
{
    const Foundation::UInt32 value = (method >> C::Ps2ReadyModeShift) & 15U;
    const bool known = value <= static_cast<Foundation::UInt32>(Input::Ps2KeyboardMode::Set2Unnegotiated);
    const auto mode = known ? static_cast<Input::Ps2KeyboardMode>(value) : Input::Ps2KeyboardMode::Set2;
    Inventory::ReportText text{line, sizeof(line)};
    text.Text("# init: ps2 keyboard=");
    text.Text(known ? Input::Ps2KeyboardModeText(mode) : "unknown");
    if (known && (mode != Input::Ps2KeyboardMode::Set2) &&
        ((transaction & 0xFFFF'FFFFU) == Abi::ComponentInputFailedExitCode))
    {
        text.Text("; set 2 failed at ");
        AppendPs2Diagnostic(text, Input::DecodePs2Diagnostic(static_cast<Foundation::UInt32>(transaction >> 32U)));
    }
    text.Character('\n');
    return text.Bytes();
}

ComponentRecord* StartInputDriver(ScenarioContext& context, const System::CapabilityHandle source,
                                  const Foundation::UInt64 epoch, const Foundation::UInt64 behavior,
                                  Abi::SystemCallError& error, TerminalEcho* const echo) noexcept
{
    context.DriverService = source;
    context.DriverGeneration = epoch;
    // The USB host group driver is the `usb` image's.
    const bool group = (context.ServiceFlags & Abi::UsbHostGroupFlag) != 0U;
    ComponentRecord* const driver =
        StartComponent(context, group ? context.UsbHost : context.Component, context.NextTag++, behavior, error);
    context.DriverService = {};
    context.DriverGeneration = 0U;
    if (driver == nullptr)
    {
        return nullptr;
    }
    // Handoff, reset and the five-second enumeration settle precede readiness.
    // An xHCI driver's enumeration diagnostics precede its readiness.
    const Foundation::UInt64 ready =
        AwaitReport(context, *driver, context.Report, InputDriverReadyAttempts, error, echo);
    const bool ps2 = behavior == Abi::ComponentBehaviorPs2;
    const auto method = ps2 ? Component::Ps2ReadyMethod : Component::XhciReadyMethod;
    const Foundation::UInt32 reported =
        ready == Component::EchoDeliveryHeaderBytes + Component::EchoHeaderBytes
            ? Component::Detail::InputLoad32(context.Delivery + Component::EchoDeliveryHeaderBytes + 32U)
            : 0U;
    // A PS/2 report's mode bits are checked by the mode line, not here.
    const Foundation::UInt32 receivedMethod = ps2 ? (reported & Component::Ps2ReadyMethodMask) : reported;
    const bool auxFailed = ps2 && receivedMethod == Component::Ps2ReadyAuxFailedMethod;
    if ((ready != Component::EchoDeliveryHeaderBytes + Component::EchoHeaderBytes) ||
        (receivedMethod != method && !auxFailed))
    {
        if (driver->State == ComponentState::Running)
        {
            Abi::SystemCallError stopped{Abi::SystemCallError::None};
            static_cast<void>(context.Supervisor->Stop(*driver, stopped));
        }
        return nullptr;
    }
    if (ps2)
    {
        const Foundation::UInt8* const frame = context.Delivery + Component::EchoDeliveryHeaderBytes;
        const Foundation::UInt64 transaction =
            Foundation::UInt64{Component::Detail::InputLoad32(frame + Component::Ps2ReadyTransactionOffset)} |
            (Foundation::UInt64{Component::Detail::InputLoad32(frame + Component::Ps2ReadyTransactionOffset + 4U)}
             << 32U);
        char line[Ps2LineBytes]{};
        WriteLine(context, line, FormatPs2Mode(line, reported, transaction));
    }
    if (auxFailed)
    {
        constexpr char message[]{"# init: ps2 aux failed; keyboard remains active\n"};
        WriteLine(context, message, sizeof(message) - 1U);
    }
    context.Supervisor->SetWatchdog(*driver, false);
    return driver;
}

void TrackInputDriver(const ComponentRecord& driver, const Foundation::UInt64 flags,
                      const System::CapabilityHandle source, const Foundation::UInt64 sourceId) noexcept
{
    TrackedInputDriver* const tracked =
        (driver.Behavior == Abi::ComponentBehaviorXhci) ? TrackedOf(driver.Device) : nullptr;
    if ((tracked == nullptr) || (source.Selector == 0U) || (sourceId == 0U))
    {
        return;
    }
    // A driver tracked again (its scenario started it afresh) keeps the
    // restarts it already spent.
    tracked->Source = source;
    tracked->SourceId = sourceId;
    tracked->Flags = flags;
    tracked->Tracked = true;
}

// the
// teardown's drain revokes the dead driver's DMA, interrupts and assignments, and the Device
// Manager proves each device quiet before it is assigned again; only then is a tracked driver
// restarted, at most InputDriverRestarts times. Each pass handles one ended record, a failed
// restart's included, so the loop ends once the restarts are spent.
InputRecovery RecoverInputDrivers(ScenarioContext& context, const ComponentRecord* const broker,
                                  TerminalEcho* const echo) noexcept
{
    InputRecovery recovery{};
    constexpr Foundation::Size Passes{MaximumComponents * (InputDriverRestarts + 2U)};
    for (Foundation::Size pass{0U}; pass < Passes; ++pass)
    {
        ComponentRecord* const driver = context.Supervisor->FindStoppedInputDriver();
        if (driver == nullptr)
        {
            break;
        }
        driver->FailureReported = true;
        ++recovery.Ended;
        const Foundation::UInt8 device = driver->Device;
        const bool usb = driver->Behavior == Abi::ComponentBehaviorXhci;
        if (usb)
        {
            ReportXhciFailure(context, device, driver->ExitCode);
        }
        else
        {
            ReportInputFailure(context, driver->ExitCode);
        }
        // The devices the drain will quarantine, for the proof's report.
        bool pending[Abi::PagerBootstrapDevices]{};
        for (Foundation::Size index{0U}; index < Abi::PagerBootstrapDevices; ++index)
        {
            pending[index] =
                (context.Devices != nullptr) && context.Devices->IsPending(index) &&
                ((index == device) || ((device == ComponentUsbHostGroup) && (index >= Abi::PagerDeviceUsbHost)));
        }
        // Failure to prove hardware quiet retains quarantine and its pins.
        // Optional recovery cannot tear down unrelated resident services.
        Abi::SystemCallError cleanup{Abi::SystemCallError::None};
        const bool quiet = context.Supervisor->Teardown(*driver, cleanup);
        for (Foundation::Size index{Abi::PagerDeviceXhci}; usb && (index < Abi::PagerBootstrapDevices); ++index)
        {
            if (pending[index])
            {
                ReportQuiesce(context, index);
            }
        }
        TrackedInputDriver* const tracked = usb ? TrackedOf(device) : nullptr;
        if ((tracked == nullptr) || !tracked->Tracked || (broker == nullptr) ||
            (broker->State != ComponentState::Running))
        {
            continue;
        }
        if (!quiet)
        {
            ReportRecovery(context, device, "not restarted: its controller stays quarantined", 0U);
            tracked->Tracked = false;
            ++recovery.Quarantined;
            continue;
        }
        if (tracked->Restarts >= InputDriverRestarts)
        {
            ReportRecovery(context, device, nullptr, 0U);
            tracked->Tracked = false;
            ++recovery.GaveUp;
            continue;
        }
        ComponentRecord* const restarted = RestartInputDriver(context, *broker, device, *tracked, echo);
        if (restarted != nullptr)
        {
            ++recovery.Restarted;
            recovery.Driver = restarted;
        }
        else if (tracked->Tracked && (context.Supervisor->FindStoppedInputDriver() == nullptr))
        {
            // No record to report: the build found no slot to start in.
            ReportRecovery(context, device, "not restarted: no component slot", 0U);
            tracked->Tracked = false;
        }
    }
    return recovery;
}

namespace
{
// Product init's USB input drivers beside display, broker and PS/2 (ps2,
// started; source and sourceId its broker source, or the first USB driver's
// when no PS/2 driver took it): xHCI and xhci, the USB host group and
// usbHost, each tracked for recovery. False only when the broker refused a
// registration.
[[nodiscard, gnu::noinline]] bool StartUsbDrivers(ScenarioContext& context, const ComponentRecord& broker,
                                                  const System::CapabilityHandle source,
                                                  const Foundation::UInt64 sourceId, Foundation::UInt64& transaction,
                                                  bool& xhci, bool& usbHost, Abi::SystemCallError& error) noexcept
{
    const bool ps2Present = context.Bootstrap->I8042[0U].Selector != 0U;
    Component::InputMessage reply{};
    Foundation::Size usbDrivers{0U};
    Foundation::UInt64 sourcesUsed{1U};
    const Foundation::UInt64 capacity = context.Bootstrap->SessionMode == Abi::SessionDesktop
                                            ? Component::InputMaximumSources
                                            : Abi::ConsoleInputSources;
    // The xHCI controllers in PCI catalog order, then (controller 4) one
    // driver for every USB 1.1/2.0 host controller of the USB host table.
    for (Foundation::Size controller{0U}; controller <= Abi::PagerXhciControllers; ++controller)
    {
        const bool group = controller == Abi::PagerXhciControllers;
        if (group ? (!context.Devices->IsPresent(Abi::PagerDeviceUsbHost) || (context.UsbHost.Bytes == nullptr))
                  : !context.Devices->IsPresent(Abi::PagerDeviceXhci + controller))
        {
            continue;
        }
        // The desktop admits every published seat source. Console startup retains its
        // three-source admission bound; the USB host group follows the catalog xHCI functions.
        if ((ps2Present || usbDrivers != 0U) && sourcesUsed == capacity)
        {
            constexpr char deferred[]{"# init: usb deferred, source capacity\n"};
            WriteLine(context, deferred, sizeof(deferred) - 1U);
            break;
        }
        if (ps2Present || (usbDrivers != 0U))
        {
            if (!Exchange(context, broker, broker.Service, Component::InputRegisterSourceMethod, 0U, transaction, reply,
                          error))
            {
                return false;
            }
            ++sourcesUsed;
        }
        const bool registered = ps2Present || (usbDrivers != 0U);
        const auto usbSource = registered ? reply.Reply : source;
        const Foundation::UInt64 usbSourceId = registered ? reply.Words[2U] : sourceId;
        // Product init prints the driver's enumeration diagnostics (`# usb:`).
        const Foundation::UInt64 flags = (group ? Abi::UsbHostGroupFlag : controller) | Abi::XhciReportProgressFlag;
        context.ServiceFlags = flags;
        auto* const driver = StartInputDriver(context, usbSource, broker.Tag, Abi::ComponentBehaviorXhci, error);
        context.ServiceFlags = 0U;
        if (driver != nullptr)
        {
            // A failure while it serves restarts it (RecoverInputDrivers).
            TrackInputDriver(*driver, flags, usbSource, usbSourceId);
        }
        if (driver == nullptr)
        {
            if (!RetireInputDrivers(context))
            {
                ReportXhciFailure(context, group ? ComponentNoDevice : Abi::PagerDeviceXhci + controller,
                                  C::DriverFailure(Abi::ComponentDriverIdentityStep, error));
            }
            error = Abi::SystemCallError::None;
            continue;
        }
        ++usbDrivers;
        xhci = xhci || !group;
        usbHost = usbHost || group;
    }
    return true;
}
} // Namespace

PagerServeError StartInputTerminal(ScenarioContext& context, Abi::SystemCallError& error) noexcept
{
    ComponentRecord* const broker =
        StartComponent(context, context.Component, context.NextTag++, Abi::ComponentBehaviorInput, error);
    Component::InputMessage reply{};
    Foundation::UInt64 transaction{0U};
    if ((broker == nullptr) || !SetPointerRoute(context, *broker, error) ||
        !Exchange(context, *broker, broker->Service, Component::InputRegisterSourceMethod, 0U, transaction, reply,
                  error))
    {
        return PagerServeError::ManagementFailed;
    }
    System::CapabilityHandle source = reply.Reply;
    const Foundation::UInt64 sourceId = reply.Words[2U];
    if (!Exchange(context, *broker, broker->Service, Component::InputRegisterClientMethod, 0U, transaction, reply,
                  error))
    {
        return PagerServeError::ManagementFailed;
    }
    const System::CapabilityHandle control = reply.Reply;
    if (!Exchange(context, *broker, broker->Service, Component::InputSetFocusMethod, 1U, transaction, reply, error))
    {
        return PagerServeError::ManagementFailed;
    }
    context.OptionalInputDrivers = true;
    const bool ps2Present = context.Bootstrap->I8042[0U].Selector != 0U;
    const bool ps2 =
        ps2Present && (StartInputDriver(context, source, broker->Tag, Abi::ComponentBehaviorPs2, error) != nullptr);
    if (ps2Present && !ps2)
    {
        if (!RetireInputDrivers(context))
        {
            ReportInputFailure(context, 0U);
        }
        error = Abi::SystemCallError::None;
    }
    bool xhci{false};
    bool usbHost{false};
    if (!StartUsbDrivers(context, *broker, source, sourceId, transaction, xhci, usbHost, error))
    {
        return PagerServeError::ManagementFailed;
    }
    context.Supervisor->SetWatchdog(*broker, false);
    if (!context.Supervisor->WatchService(*broker, error))
    {
        return PagerServeError::ManagementFailed;
    }
    char readyLine[64U]{};
    Inventory::ReportText ready{readyLine, sizeof(readyLine)};
    ready.Text("# init: ready: display, input");
    ready.Text(ps2 ? ", ps2" : "");
    ready.Text(xhci ? ", xhci" : "");
    ready.Text(usbHost ? ", usb" : "");
    ready.Character('\n');
    WriteLine(context, readyLine, ready.Bytes());
    if (context.Bootstrap->LoginEnabled != 0U)
    {
        StartProductLogin(context, *broker);
    }
    else
    {
        StartProductDesktop(context, *broker);
    }
    error = Abi::SystemCallError::None;
    return EchoInput(context, false, *broker, control, source, transaction, error) ? PagerServeError::None
                                                                                   : PagerServeError::ManagementFailed;
}
} // Namespace Asteria::User::Pager
