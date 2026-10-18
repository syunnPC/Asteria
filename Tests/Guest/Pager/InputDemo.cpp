// SPDX-License-Identifier: Apache-2.0
#include "Guest/Pager/RootScenarios.hpp"

#include "User/Component/InputFrame.hpp"
#include "User/Pager/InputTerminal.hpp"
#include "User/Component/Ps2Driver.hpp"
#include "User/Component/XhciDriver.hpp"

namespace Asteria::User::Pager
{
namespace
{
namespace C = Component;
constinit Input::InputEvent g_batch[C::InputMaximumEvents]{};
constinit Foundation::UInt64 g_transaction{0U};
constinit Foundation::UInt64 g_step{0U};
constinit Foundation::UInt64 g_exchange[5U]{};
constinit System::CapabilityHandle g_currentSource{};
constinit C::InputMessage g_hardwareReply{};

[[nodiscard]] PagerServeError Failed(const ScenarioContext& context) noexcept
{
    char line[128U]{};
    constexpr char prefix[]{"# input: failed, step/request/reply/answer/id/value "};
    Foundation::Size length{0U};
    for (Foundation::Size index{0U}; index < sizeof(prefix) - 1U; ++index)
    {
        line[length++] = prefix[index];
    }
    constexpr char digits[]{"0123456789abcdef"};
    for (Foundation::Size index{0U}; index < 6U; ++index)
    {
        const Foundation::UInt64 value = index == 0U ? g_step : g_exchange[index - 1U];
        for (Foundation::UInt64 shift{32U}; shift != 0U; shift -= 4U)
        {
            line[length++] = digits[(value >> (shift - 4U)) & 0xFU];
        }
        line[length++] = index == 5U ? '\n' : ' ';
    }
    WriteLine(context, line, length);
    return PagerServeError::ManagementFailed;
}

[[nodiscard]] bool RegisterAgain(ScenarioContext& context, const ComponentRecord& broker,
                                 const C::InputMessage& request, Abi::SystemCallError& error) noexcept
{
    return RegisterInputAgain(context, broker, request, g_currentSource, g_transaction, error);
}

[[nodiscard, gnu::noinline]] bool Exchange(ScenarioContext& context, const ComponentRecord& broker,
                                           System::CapabilityHandle endpoint, Foundation::UInt32 method,
                                           Foundation::UInt64 argument, Foundation::Size count, C::InputMessage& reply,
                                           Abi::SystemCallError& error) noexcept
{
    g_exchange[0U] = method;
    const Foundation::UInt64 words[4U]{broker.Tag, argument, 0U, 0U};
    const Foundation::UInt64 transaction = ++g_transaction;
    const Foundation::Size size =
        C::WriteInputFrame(context.Frame, method, transaction, words, g_batch, count, &context.Report);
    if ((size == 0U) || !Check(context.Client->Send(endpoint, System::AddressOf(context.Frame), size), error))
    {
        return false;
    }
    Foundation::UInt64 received{0U};
    bool decoded{false};
    const bool driverQuery{method == C::InputQueryMethod && (endpoint.Selector != broker.Service.Selector ||
                                                             endpoint.Authenticator != broker.Service.Authenticator)};
    for (;;)
    {
        received = AwaitMessage(context, broker, context.Report, error);
        decoded = (received != 0U) &&
                  C::ReadInputFrame(context.Delivery, static_cast<Foundation::Size>(received), reply,
                                    driverQuery ? C::InputReplySchema::DriverQuery : C::InputReplySchema::Broker);
        if (!decoded || (reply.Method != C::InputRegisterSourceMethod))
        {
            break;
        }
        if (!RegisterAgain(context, broker, reply, error))
        {
            return false;
        }
    }
    g_exchange[1U] = decoded ? reply.Method : received;
    for (Foundation::Size index{0U}; index < 3U; ++index)
    {
        g_exchange[index + 2U] = decoded ? reply.Words[index + 1U] : 0U;
    }
    return decoded && (reply.Transaction == transaction) && (reply.Words[0U] == broker.Tag);
}
[[nodiscard]] bool Done(const C::InputMessage& message) noexcept
{
    return (message.Method == C::InputReplyMethod) &&
           (message.Words[1U] == static_cast<Foundation::UInt64>(C::InputAnswer::Done));
}
[[nodiscard]] Input::InputEvent Key(Foundation::UInt16 usage, Input::EventKind kind,
                                    Foundation::UInt8 modifiers = 0U) noexcept
{
    // Deliberately forged Source: the focused client must see broker id 1.
    return Input::InputEvent{.Kind = kind,
                             .Modifiers = modifiers,
                             .Usage = usage,
                             .Source = 0xDEAD'BEEFU,
                             .Dx = 0,
                             .Dy = 0,
                             .Buttons = 0U,
                             .Reserved = {},
                             .Wheel = 0};
}
[[nodiscard]] bool Publish(ScenarioContext& context, const ComponentRecord& broker, System::CapabilityHandle source,
                           Foundation::Size count, Abi::SystemCallError& error) noexcept
{
    C::InputMessage reply{};
    return Exchange(context, broker, source, C::InputPublishMethod, count, count, reply, error) && Done(reply);
}
[[nodiscard, gnu::noinline]] bool ReadEvents(ScenarioContext& context, const ComponentRecord& broker,
                                             System::CapabilityHandle control, Foundation::Size count,
                                             Abi::SystemCallError& error) noexcept
{
    C::InputMessage reply{};
    if (!Exchange(context, broker, control, C::InputCreditMethod, C::InputMaximumEvents, 0U, reply, error) ||
        (reply.Method != C::InputEventsMethod) || (reply.Count != count) || (reply.Words[1U] != 1U))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        const Input::InputEvent event = C::ReadInputEvent(reply.Events + index * 24U);
        const Input::InputEvent& expected = g_batch[index];
        if ((event.Source != 1U) || (event.Kind != expected.Kind) || (event.Usage != expected.Usage) ||
            (event.Modifiers != expected.Modifiers) || (event.Dx != expected.Dx) || (event.Dy != expected.Dy) ||
            (event.Buttons != expected.Buttons) || (event.Wheel != expected.Wheel))
        {
            return false;
        }
    }
    return true;
}
[[nodiscard]] bool Probe(ScenarioContext& context, const ComponentRecord& broker, const ComponentRecord& probe,
                         Foundation::UInt64 count, Abi::SystemCallError& error) noexcept
{
    // Query is a fence on the probe's event Endpoint. The first registration
    // Reply may still be in flight; a bounded poll waits for client id 2.
    for (Foundation::Size attempt{0U}; attempt < 100U; ++attempt)
    {
        C::InputMessage reply{};
        if (!Exchange(context, broker, probe.Service, C::InputQueryMethod, 0U, 0U, reply, error) || !Done(reply))
        {
            return false;
        }
        if ((reply.Words[2U] == 2U) && (reply.Words[3U] == count))
        {
            return true;
        }
        if (context.Supervisor->Pause(10'000U, error) != PagerServeError::None)
        {
            return false;
        }
    }
    return false;
}
[[nodiscard]] bool Stop(ScenarioContext& context, ComponentRecord& component, Abi::SystemCallError& error) noexcept
{
    return (context.Supervisor->Pause(100'000U, error) == PagerServeError::None) &&
           (context.Supervisor->Stop(component, error) == PagerServeError::None) &&
           (component.ExitCode == Abi::KilledExitCode) && context.Supervisor->Teardown(component, error);
}

[[nodiscard, gnu::noinline]] bool Script(ScenarioContext& context, const ComponentRecord& broker,
                                         const ComponentRecord& probe, System::CapabilityHandle source,
                                         System::CapabilityHandle control, Abi::SystemCallError& error) noexcept
{
    C::InputMessage reply{};
    g_step = 4U;
    if (!Exchange(context, broker, broker.Service, C::InputSetFocusMethod, 1U, 0U, reply, error) || !Done(reply))
    {
        return false;
    }
    // HID usages spell 'asteria', with one make and one release each.
    constexpr Foundation::UInt16 usages[7U]{4U, 22U, 23U, 8U, 21U, 12U, 4U};
    g_step = 5U;
    for (Foundation::UInt16 usage : usages)
    {
        g_batch[0U] = Key(usage, Input::EventKind::KeyDown);
        g_batch[1U] = Key(usage, Input::EventKind::KeyUp);
        if (!Publish(context, broker, source, 2U, error) || !ReadEvents(context, broker, control, 2U, error))
        {
            return false;
        }
    }
    g_step = 6U;
    g_batch[0U] = Input::InputEvent{.Kind = Input::EventKind::Motion,
                                    .Modifiers = 0U,
                                    .Usage = 0U,
                                    .Source = 99U,
                                    .Dx = 5,
                                    .Dy = -3,
                                    .Buttons = 0U,
                                    .Reserved = {},
                                    .Wheel = 0};
    g_batch[1U] = Input::InputEvent{.Kind = Input::EventKind::Buttons,
                                    .Modifiers = 0U,
                                    .Usage = 0U,
                                    .Source = 99U,
                                    .Dx = 0,
                                    .Dy = 0,
                                    .Buttons = 1U,
                                    .Reserved = {},
                                    .Wheel = 0};
    if (!Publish(context, broker, source, 2U, error) || !ReadEvents(context, broker, control, 2U, error))
    {
        return false;
    }
    g_step = 7U;
    g_batch[0U] = Key(0xE0U, Input::EventKind::KeyDown, 1U);
    g_batch[1U] = Key(0xE2U, Input::EventKind::KeyDown, 5U);
    g_batch[2U] = Key(0x4CU, Input::EventKind::KeyDown, 5U);
    g_batch[3U] = Key(0x4CU, Input::EventKind::KeyUp, 5U);
    g_batch[4U] = Key(0xE2U, Input::EventKind::KeyUp, 1U);
    g_batch[5U] = Key(0xE0U, Input::EventKind::KeyUp, 0U);
    if (!Publish(context, broker, source, 6U, error) ||
        !Exchange(context, broker, broker.Service, C::InputAttentionCreditMethod, 0U, 0U, reply, error) ||
        (reply.Method != C::InputAttentionMethod) || (reply.Words[1U] != 1U) ||
        !Probe(context, broker, probe, 0U, error))
    {
        return false;
    }
    g_step = 8U;
    // No chord event may precede this held key in the client's queue.
    g_batch[0U] = Key(4U, Input::EventKind::KeyDown);
    if (!Publish(context, broker, source, 1U, error) || !ReadEvents(context, broker, control, 1U, error) ||
        !Exchange(context, broker, broker.Service, C::InputSetFocusMethod, 2U, 0U, reply, error) || !Done(reply))
    {
        return false;
    }
    g_step = 9U;
    g_batch[0U] = Key(4U, Input::EventKind::KeyUp);
    if (!ReadEvents(context, broker, control, 1U, error) || !Publish(context, broker, source, 1U, error))
    {
        return false;
    }
    g_step = 10U;
    // The physical release was not delivered to the new focus holder.
    if (!Probe(context, broker, probe, 0U, error))
    {
        return false;
    }
    g_step = 11U;
    g_batch[0U] = Key(5U, Input::EventKind::KeyDown);
    g_batch[1U] = Key(5U, Input::EventKind::KeyUp);
    return Publish(context, broker, source, 2U, error) && Probe(context, broker, probe, 2U, error);
}
// The runner injects a fixed script through QMP once the driver is ready.
// Elsewhere (VMware, a real machine) nothing arrives, or a person types: the
// demonstration then turns into an echo of the real devices instead of
// failing the boot, and the runner, which expects the scripted line, still
// fails its case.
enum class HardwareInput : Foundation::UInt8
{
    Scripted,
    Absent,
    Foreign,
    Failed,
};

[[nodiscard, gnu::noinline]] HardwareInput HardwareEvents(ScenarioContext& context, const ComponentRecord& broker,
                                                          System::CapabilityHandle control,
                                                          Abi::SystemCallError& error) noexcept
{
    constexpr Foundation::UInt16 usages[]{4U, 22U, 23U, 8U, 21U, 12U, 4U, 40U};
    char typed[8U]{};
    Foundation::Size keys{0U};
    Foundation::Size buttons{0U};
    bool motion{false};
    while ((keys != 16U) || (buttons != 2U) || !motion)
    {
        C::InputMessage reply{};
        if (!Exchange(context, broker, control, C::InputCreditMethod, C::InputMaximumEvents, 0U, reply, error) ||
            (reply.Method != C::InputEventsMethod))
        {
            const bool nothing = (g_exchange[1U] == 0U) && (keys == 0U) && (buttons == 0U) && !motion;
            return (nothing && (error == Abi::SystemCallError::None)) ? HardwareInput::Absent : HardwareInput::Failed;
        }
        for (Foundation::Size index{0U}; index < reply.Count; ++index)
        {
            const auto event = C::ReadInputEvent(reply.Events + index * 24U);
            if (event.Source != 1U)
            {
                return HardwareInput::Failed;
            }
            if ((event.Kind == Input::EventKind::KeyDown) || (event.Kind == Input::EventKind::KeyUp))
            {
                if ((keys >= 16U) || (event.Usage != usages[keys / 2U]) || (event.Modifiers != 0U) ||
                    (event.Kind != ((keys % 2U == 0U) ? Input::EventKind::KeyDown : Input::EventKind::KeyUp)))
                {
                    return HardwareInput::Foreign;
                }
                if (event.Kind == Input::EventKind::KeyDown)
                {
                    if (event.Usage == 40U)
                    {
                        typed[7U] = '\n';
                        WriteLine(context, typed, sizeof(typed));
                    }
                    else
                    {
                        typed[keys / 2U] = static_cast<char>('a' + event.Usage - 4U);
                    }
                }
                ++keys;
            }
            else if (event.Kind == Input::EventKind::Motion)
            {
                if (motion || (event.Dx != 5) || (event.Dy != -3))
                {
                    return HardwareInput::Foreign;
                }
                motion = true;
            }
            else if (event.Kind == Input::EventKind::Buttons)
            {
                if ((buttons >= 2U) || (event.Buttons != ((buttons == 0U) ? 1U : 0U)))
                {
                    return HardwareInput::Foreign;
                }
                ++buttons;
            }
            else
            {
                return HardwareInput::Foreign;
            }
        }
    }
    return HardwareInput::Scripted;
}

// The scripted input arrived: the attention credit, the publish fence, the
// second client and the stop.
[[nodiscard, gnu::noinline]] bool WaitUsbInterfaces(ScenarioContext& context, ComponentRecord& broker,
                                                    ComponentRecord& driver, const Foundation::UInt64 interfaces,
                                                    Abi::SystemCallError& error) noexcept
{
    C::InputMessage reply{};
    // Fifteen seconds: a restarted group enumerates every controller again.
    for (Foundation::Size attempt{0U}; attempt < 1500U; ++attempt)
    {
        if (!Exchange(context, broker, driver.Service, C::InputQueryMethod, 0U, 0U, reply, error) || !Done(reply))
        {
            return false;
        }
        if (C::InputQueryInterfaces(reply) == interfaces)
        {
            return true;
        }
        if (context.Supervisor->Pause(10'000U, error) != PagerServeError::None)
        {
            return false;
        }
    }
    return false;
}

// The runner types 'asteria' and Enter, moves the pointer by (5,-3) and
// clicks once the line before this wait was written (Drivers.UsbRecovery):
// every key edge in order, one motion and two button changes, all stamped
// source 1. The credit may already be outstanding; the waits are long, since
// the runner reacts to the line first. The typed line is written at Enter.
[[nodiscard, gnu::noinline]] bool AwaitTyping(ScenarioContext& context, const ComponentRecord& broker,
                                              const System::CapabilityHandle control, bool outstanding,
                                              Abi::SystemCallError& error) noexcept
{
    constexpr Foundation::UInt16 usages[]{4U, 22U, 23U, 8U, 21U, 12U, 4U, 40U};
    char typed[8U]{};
    Foundation::Size keys{0U};
    Foundation::Size buttons{0U};
    bool motion{false};
    while ((keys != 16U) || (buttons != 2U) || !motion)
    {
        if (!outstanding)
        {
            const Foundation::UInt64 words[4U]{broker.Tag, C::InputMaximumEvents, 0U, 0U};
            const auto size = C::WriteInputFrame(context.Frame, C::InputCreditMethod, ++g_transaction, words, nullptr,
                                                 0U, &context.Report);
            if ((size == 0U) || !Check(context.Client->Send(control, System::AddressOf(context.Frame), size), error))
            {
                return false;
            }
        }
        outstanding = false;
        const Foundation::UInt64 received = context.Supervisor->AwaitMessageFor(
            broker, context.Report, context.Delivery, sizeof(context.Delivery), 3000U, error);
        C::InputMessage reply{};
        const bool decoded =
            (received != 0U) && C::ReadInputFrame(context.Delivery, static_cast<Foundation::Size>(received), reply);
        // What arrived, for the failure line.
        g_exchange[0U] = C::InputCreditMethod;
        g_exchange[1U] = decoded ? reply.Method : received;
        for (Foundation::Size index{0U}; index < 3U; ++index)
        {
            g_exchange[index + 2U] = decoded ? reply.Words[index + 1U] : keys;
        }
        if (!decoded || (reply.Method != C::InputEventsMethod) || (reply.Words[0U] != broker.Tag))
        {
            return false;
        }
        for (Foundation::Size index{0U}; index < reply.Count; ++index)
        {
            const auto event = C::ReadInputEvent(reply.Events + index * 24U);
            const bool key = (event.Kind == Input::EventKind::KeyDown) || (event.Kind == Input::EventKind::KeyUp);
            if ((event.Source != 1U) ||
                (key && ((keys >= 16U) || (event.Usage != usages[keys / 2U]) || (event.Modifiers != 0U) ||
                         (event.Kind != ((keys % 2U == 0U) ? Input::EventKind::KeyDown : Input::EventKind::KeyUp)))) ||
                ((event.Kind == Input::EventKind::Motion) && (motion || (event.Dx != 5) || (event.Dy != -3))) ||
                ((event.Kind == Input::EventKind::Buttons) &&
                 ((buttons >= 2U) || (event.Buttons != ((buttons == 0U) ? 1U : 0U)))))
            {
                return false;
            }
            if (key && (event.Kind == Input::EventKind::KeyDown))
            {
                typed[keys / 2U] = (event.Usage == 40U) ? '\n' : static_cast<char>('a' + event.Usage - 4U);
                if (event.Usage == 40U)
                {
                    // The runner moves the pointer once it saw the line.
                    WriteLine(context, typed, sizeof(typed));
                }
            }
            keys += key ? 1U : 0U;
            motion = motion || (event.Kind == Input::EventKind::Motion);
            buttons += (event.Kind == Input::EventKind::Buttons) ? 1U : 0U;
        }
    }
    return true;
}

// The supervisor's kill at an observed point: the driver dies with its interrupt polls queued and
// Its DMA mapped; the product's recovery (InputTerminal.hpp) reports it, proves its controllers
// quiet, and restarts it (restart) or gives it up.
[[nodiscard, gnu::noinline]] bool KillAndRecover(ScenarioContext& context, const ComponentRecord& broker,
                                                 ComponentRecord*& driver, const bool restart,
                                                 Abi::SystemCallError& error) noexcept
{
    if ((driver == nullptr) || (context.Supervisor->Pause(100'000U, error) != PagerServeError::None) ||
        (context.Supervisor->Stop(*driver, error) != PagerServeError::None) ||
        (driver->ExitCode != Abi::KilledExitCode))
    {
        return false;
    }
    const InputRecovery recovery = RecoverInputDrivers(context, &broker, nullptr);
    driver = recovery.Driver;
    return (recovery.Ended == 1U) && (recovery.Quarantined == 0U) &&
           (restart ? ((recovery.Restarted == 1U) && (driver != nullptr))
                    : ((recovery.GaveUp == 1U) && (recovery.Restarted == 0U)));
}

// The boot interfaces a USB host group polls once its enumeration settled:
// at least the runner's keyboard and mouse, and whatever else the machine
// Attaches (itanium2-vpc's own OHCI has QEMU's keyboard when its i8042 is off): the count once it
// held for two seconds.
[[nodiscard, gnu::noinline]] bool SettleUsbInterfaces(ScenarioContext& context, const ComponentRecord& broker,
                                                      const ComponentRecord& driver, Foundation::UInt64& interfaces,
                                                      Abi::SystemCallError& error) noexcept
{
    Foundation::Size stable{0U};
    for (Foundation::Size attempt{0U}; attempt < 1000U; ++attempt)
    {
        C::InputMessage reply{};
        if (!Exchange(context, broker, driver.Service, C::InputQueryMethod, 0U, 0U, reply, error) || !Done(reply))
        {
            return false;
        }
        stable = ((C::InputQueryInterfaces(reply) >= 2U) && (C::InputQueryInterfaces(reply) == interfaces))
                     ? stable + 1U
                     : 0U;
        interfaces = C::InputQueryInterfaces(reply);
        if (stable == 20U)
        {
            return true;
        }
        if (context.Supervisor->Pause(100'000U, error) != PagerServeError::None)
        {
            return false;
        }
    }
    return false;
}

// Kills the restarted driver until the recovery gives it up: restarts
// (first) to InputDriverRestarts, waiting for its interfaces each time, then
// one kill more.
[[nodiscard, gnu::noinline]] bool KillUntilGivenUp(ScenarioContext& context, ComponentRecord& broker,
                                                   ComponentRecord*& driver, const Foundation::UInt8 first,
                                                   const Foundation::UInt64 interfaces,
                                                   Abi::SystemCallError& error) noexcept
{
    for (Foundation::UInt8 restart{first}; restart <= InputDriverRestarts; ++restart)
    {
        if (!KillAndRecover(context, broker, driver, true, error) ||
            !WaitUsbInterfaces(context, broker, *driver, interfaces, error))
        {
            return false;
        }
    }
    return KillAndRecover(context, broker, driver, false, error);
}

[[nodiscard, gnu::noinline]] bool FinishScripted(ScenarioContext& context, ComponentRecord& broker,
                                                 ComponentRecord& driver, ComponentRecord& probe,
                                                 System::CapabilityHandle control, Abi::SystemCallError& error) noexcept
{
    C::InputMessage reply{};
    if (!Exchange(context, broker, broker.Service, C::InputAttentionCreditMethod, 0U, 0U, reply, error) ||
        (reply.Method != C::InputAttentionMethod) || (reply.Words[1U] != 1U))
    {
        return false;
    }
    // Query answers only after the driver's pending Publish is acknowledged.
    // All 25 decoded events include the six secure-attention edges, so this
    // fence proves their breaks drained without relying on elapsed time.
    bool fenced{false};
    for (Foundation::Size attempt{0U}; attempt < 100U; ++attempt)
    {
        if (!Exchange(context, broker, driver.Service, C::InputQueryMethod, 0U, 0U, reply, error) || !Done(reply))
        {
            return false;
        }
        if (reply.Words[3U] == 25U)
        {
            fenced = true;
            break;
        }
        if (context.Supervisor->Pause(10'000U, error) != PagerServeError::None)
        {
            return false;
        }
    }
    if (!fenced)
    {
        return false;
    }
    const Foundation::UInt64 words[4U]{broker.Tag, C::InputMaximumEvents, 0U, 0U};
    const auto size =
        C::WriteInputFrame(context.Frame, C::InputCreditMethod, ++g_transaction, words, nullptr, 0U, &context.Report);
    if (!Check(context.Client->Send(control, System::AddressOf(context.Frame), size), error) ||
        !Exchange(context, broker, broker.Service, C::InputQueryMethod, 0U, 0U, reply, error) || !Done(reply) ||
        !Probe(context, broker, probe, 0U, error))
    {
        return false;
    }
    static constexpr char line[]{
        "# input: typed 'asteria', pointer moved (5,-3), left button, PS/2, secure attention reported, second client empty\n"};
    static constexpr char usbLine[]{
        "# input: typed 'asteria', pointer moved (5,-3), left button, xHCI, secure attention reported, second client empty\n"};
    if (context.Devices->IsPresent(Abi::PagerDeviceXhci))
    {
        WriteLine(context, usbLine, sizeof(usbLine) - 1U);
    }
    else
    {
        WriteLine(context, line, sizeof(line) - 1U);
    }
    if (context.Devices->IsPresent(Abi::PagerDeviceXhci))
    {
        // Killed with its interrupt polls queued; the product's recovery
        // (InputTerminal.hpp) proves the controller quiet, registers its
        // source again and restarts it.
        ComponentRecord* restarted = &driver;
        if (!KillAndRecover(context, broker, restarted, true, error) ||
            !WaitUsbInterfaces(context, broker, *restarted, 2U, error))
        {
            return false;
        }
        static constexpr char unplug[]{"# input: xHCI rebound, QMP may unplug mouse\n"};
        WriteLine(context, unplug, sizeof(unplug) - 1U);
        if (!WaitUsbInterfaces(context, broker, *restarted, 1U, error))
        {
            return false;
        }
        static constexpr char reconnect[]{"# input: xHCI disconnected, QMP may reconnect mouse\n"};
        WriteLine(context, reconnect, sizeof(reconnect) - 1U);
        if (!WaitUsbInterfaces(context, broker, *restarted, 2U, error))
        {
            return false;
        }
        constexpr char recovered[]{
            "# input: xHCI killed with polls queued, quiet, reassigned, two interfaces rebound\n"};
        WriteLine(context, recovered, sizeof(recovered) - 1U);
        // Typing resumes through the restarted driver and its source
        // registered again; the credit sent above is still outstanding.
        constexpr char type[]{"# input: xHCI restarted, QMP may type\n"};
        WriteLine(context, type, sizeof(type) - 1U);
        if (!AwaitTyping(context, broker, control, true, error) ||
            !KillUntilGivenUp(context, broker, restarted, 2U, 2U, error))
        {
            return false;
        }
        constexpr char given[]{"# input: xHCI typed after its restart, killed four times, restarted three, given up\n"};
        WriteLine(context, given, sizeof(given) - 1U);
    }
    // Termination drains/masks both line Interrupts before the Device Manager
    // disables ports and proves the shared output buffer empty.
    else if (!Stop(context, driver, error))
    {
        return false;
    }
    return context.Calls->Close(g_currentSource, error) && context.Calls->Close(control, error) &&
           Stop(context, probe, error) && Stop(context, broker, error);
}

[[nodiscard, gnu::noinline]] bool Hardware(ScenarioContext& context, Abi::SystemCallError& error) noexcept
{
    const bool usb = context.Devices->IsPresent(Abi::PagerDeviceXhci);
    if (!usb && (context.Bootstrap->I8042[0U].Selector == 0U))
    {
        static constexpr char skipped[]{"# input: i8042 not published, PS/2 demo skipped\n"};
        WriteLine(context, skipped, sizeof(skipped) - 1U);
        return true;
    }
    g_step = 20U;
    auto* const broker =
        StartComponent(context, context.Component, context.NextTag++, Abi::ComponentBehaviorInput, error);
    C::InputMessage& reply = g_hardwareReply;
    if ((broker == nullptr) || !SetPointerRoute(context, *broker, error) ||
        !Exchange(context, *broker, broker->Service, C::InputRegisterSourceMethod, 0U, 0U, reply, error) ||
        !Done(reply))
    {
        return false;
    }
    g_step = 21U;
    const auto source = reply.Reply;
    g_currentSource = source;
    if (!Exchange(context, *broker, broker->Service, C::InputRegisterClientMethod, 0U, 0U, reply, error) ||
        !Done(reply))
    {
        return false;
    }
    const auto control = reply.Reply;
    g_step = 22U;
    context.DriverService = broker->Service;
    context.DriverGeneration = broker->Tag;
    auto* const probe =
        StartComponent(context, context.Component, context.NextTag++, Abi::ComponentBehaviorInputClient, error);
    if ((probe == nullptr) || !Probe(context, *broker, *probe, 0U, error) ||
        !Exchange(context, *broker, broker->Service, C::InputSetFocusMethod, 1U, 0U, reply, error) || !Done(reply))
    {
        return false;
    }
    g_step = 23U;
    context.DriverService = source;
    auto* const driver = StartComponent(context, context.Component, context.NextTag++,
                                        usb ? Abi::ComponentBehaviorXhci : Abi::ComponentBehaviorPs2, error);
    context.DriverService = {};
    context.DriverGeneration = 0U;
    if ((probe == nullptr) || (driver == nullptr))
    {
        return false;
    }
    g_step = 24U;
    const auto ready = context.Supervisor->AwaitMessageFor(*driver, context.Report, context.Delivery,
                                                           sizeof(context.Delivery), InputDriverReadyAttempts, error);
    if (ready == 0U)
    {
        ReportUnexpectedEnd(context, driver->Tag, driver->ExitCode, context.Supervisor->HasCrashedSince(*driver));
    }
    if ((ready != C::EchoDeliveryHeaderBytes + C::EchoHeaderBytes) ||
        (C::Detail::InputLoad32(context.Delivery + C::EchoDeliveryHeaderBytes + 32U) !=
         (usb ? C::XhciReadyMethod : C::Ps2ReadyMethod)))
    {
        return false;
    }
    if (usb)
    {
        bool bound{false};
        for (Foundation::Size attempt{0U}; attempt < 400U; ++attempt)
        {
            if (!Exchange(context, *broker, driver->Service, C::InputQueryMethod, 0U, 0U, reply, error) || !Done(reply))
            {
                return false;
            }
            if (C::InputQueryInterfaces(reply) == 2U)
            {
                bound = true;
                break;
            }
            if (context.Supervisor->Pause(10'000U, error) != PagerServeError::None)
            {
                return false;
            }
        }
        if (!bound)
        {
            static constexpr char available[]{"# input: xHCI scripted interfaces unavailable, echoing real devices\n"};
            static constexpr char banner[]{"# input: type or click; keys (US layout) and buttons are echoed here\n"};
            WriteLine(context, available, sizeof(available) - 1U);
            WriteLine(context, banner, sizeof(banner) - 1U);
            context.Supervisor->SetWatchdog(*driver, false);
            context.Supervisor->SetWatchdog(*broker, false);
            return EchoInput(context, false, *broker, control, g_currentSource, g_transaction, error);
        }
        // Restartable as product init's is (RecoverInputDrivers), on the
        // Broker's first source.
        TrackInputDriver(*driver, 0U, source, 1U);
        static constexpr char line[]{"# input: xHCI ready, two boot interfaces, QMP may inject\n"};
        WriteLine(context, line, sizeof(line) - 1U);
    }
    else
    {
        static constexpr char readyLine[]{"# input: PS/2 ready, QMP may inject\n"};
        WriteLine(context, readyLine, sizeof(readyLine) - 1U);
    }
    const HardwareInput input = HardwareEvents(context, *broker, control, error);
    if (input == HardwareInput::Failed)
    {
        return false;
    }
    if (input == HardwareInput::Scripted)
    {
        return FinishScripted(context, *broker, *driver, *probe, control, error);
    }
    static constexpr char absent[]{"# input: no scripted PS/2 input arrived, hardware demo skipped\n"};
    static constexpr char foreign[]{"# input: PS/2 input was not the runner's script, hardware demo ended\n"};
    static constexpr char usbAbsent[]{"# input: no scripted xHCI input arrived, hardware demo skipped\n"};
    static constexpr char usbForeign[]{"# input: xHCI input was not the runner's script, hardware demo ended\n"};
    static constexpr char banner[]{"# input: type or click; keys (US layout) and buttons are echoed here\n"};
    if (usb)
    {
        WriteLine(context, (input == HardwareInput::Absent) ? usbAbsent : usbForeign,
                  ((input == HardwareInput::Absent) ? sizeof(usbAbsent) : sizeof(usbForeign)) - 1U);
    }
    else
    {
        WriteLine(context, (input == HardwareInput::Absent) ? absent : foreign,
                  ((input == HardwareInput::Absent) ? sizeof(absent) : sizeof(foreign)) - 1U);
    }
    context.Supervisor->SetWatchdog(*driver, false);
    context.Supervisor->SetWatchdog(*broker, false);
    WriteLine(context, banner, sizeof(banner) - 1U);
    return EchoInput(context, input == HardwareInput::Absent, *broker, control, g_currentSource, g_transaction, error);
}
} // Namespace

PagerServeError RunUsbHostRecoveryDemo(ScenarioContext& context, Abi::SystemCallError& error) noexcept
{
    g_step = 30U;
    ComponentRecord* const broker =
        StartComponent(context, context.Component, context.NextTag++, Abi::ComponentBehaviorInput, error);
    C::InputMessage& reply = g_hardwareReply;
    if ((broker == nullptr) || !SetPointerRoute(context, *broker, error) ||
        !Exchange(context, *broker, broker->Service, C::InputRegisterSourceMethod, 0U, 0U, reply, error) ||
        !Done(reply) || (reply.Words[2U] != 1U))
    {
        return Failed(context);
    }
    const auto source = reply.Reply;
    g_step = 31U;
    if (!Exchange(context, *broker, broker->Service, C::InputRegisterClientMethod, 0U, 0U, reply, error) ||
        !Done(reply))
    {
        return Failed(context);
    }
    const auto control = reply.Reply;
    if (!Exchange(context, *broker, broker->Service, C::InputSetFocusMethod, 1U, 0U, reply, error) || !Done(reply))
    {
        return Failed(context);
    }
    g_step = 32U;
    // The group driver without progress reports, as the test pager's xHCI
    // driver: its records would interleave with the broker's answers.
    context.ServiceFlags = Abi::UsbHostGroupFlag;
    ComponentRecord* driver = StartInputDriver(context, source, broker->Tag, Abi::ComponentBehaviorXhci, error);
    context.ServiceFlags = 0U;
    Foundation::UInt64 interfaces{0U};
    if ((driver == nullptr) || !SettleUsbInterfaces(context, *broker, *driver, interfaces, error))
    {
        return Failed(context);
    }
    TrackInputDriver(*driver, Abi::UsbHostGroupFlag, source, 1U);
    constexpr char ready[]{"# input: USB host ready, two boot interfaces, QMP may type\n"};
    WriteLine(context, ready, sizeof(ready) - 1U);
    g_step = 33U;
    if (!AwaitTyping(context, *broker, control, false, error))
    {
        return Failed(context);
    }
    // Killed with its interrupt polls queued and its schedules running: the
    // product's recovery proves each controller quiet, registers the source
    // again and restarts the group; typing resumes through it.
    g_step = 34U;
    if (!KillAndRecover(context, *broker, driver, true, error) ||
        !WaitUsbInterfaces(context, *broker, *driver, interfaces, error))
    {
        return Failed(context);
    }
    constexpr char restarted[]{"# input: USB host killed with polls queued, quiet, restarted, QMP may type\n"};
    WriteLine(context, restarted, sizeof(restarted) - 1U);
    g_step = 35U;
    if (!AwaitTyping(context, *broker, control, false, error))
    {
        return Failed(context);
    }
    g_step = 36U;
    if (!KillUntilGivenUp(context, *broker, driver, 2U, interfaces, error))
    {
        return Failed(context);
    }
    constexpr char given[]{"# input: USB host typed after its restart, killed four times, restarted three, given up\n"};
    WriteLine(context, given, sizeof(given) - 1U);
    g_step = 37U;
    // The console's broker outlived its source's driver.
    if ((broker->State != ComponentState::Running) ||
        !Exchange(context, *broker, broker->Service, C::InputQueryMethod, 0U, 0U, reply, error) || !Done(reply) ||
        !context.Calls->Close(source, error) || !context.Calls->Close(control, error) || !Stop(context, *broker, error))
    {
        return Failed(context);
    }
    return PagerServeError::None;
}

PagerServeError RunInputDemo(ScenarioContext& context, Abi::SystemCallError& error) noexcept
{
    g_step = 1U;
    ComponentRecord* const broker =
        StartComponent(context, context.Component, context.NextTag++, Abi::ComponentBehaviorInput, error);
    if ((broker == nullptr) || !SetPointerRoute(context, *broker, error))
    {
        return Failed(context);
    }
    C::InputMessage reply{};
    if (!Exchange(context, *broker, broker->Service, C::InputRegisterSourceMethod, 0U, 0U, reply, error) ||
        !Done(reply) || (reply.Reply.Selector == 0U) || (reply.Words[2U] != 1U))
    {
        return Failed(context);
    }
    const System::CapabilityHandle source = reply.Reply;
    g_step = 2U;
    if (!Exchange(context, *broker, broker->Service, C::InputRegisterClientMethod, 0U, 0U, reply, error) ||
        !Done(reply) || (reply.Reply.Selector == 0U) || (reply.Words[2U] != 1U))
    {
        return Failed(context);
    }
    const System::CapabilityHandle control = reply.Reply;
    g_step = 3U;
    context.DriverService = broker->Service;
    context.DriverGeneration = broker->Tag;
    ComponentRecord* const probe =
        StartComponent(context, context.Component, context.NextTag++, Abi::ComponentBehaviorInputClient, error);
    context.DriverService = {};
    context.DriverGeneration = 0U;
    if ((probe == nullptr) || !Probe(context, *broker, *probe, 0U, error) ||
        !Script(context, *broker, *probe, source, control, error) || !context.Calls->Close(source, error) ||
        !context.Calls->Close(control, error) || !Stop(context, *probe, error) || !Stop(context, *broker, error))
    {
        ReportUnexpectedEnd(context, broker->Tag, broker->ExitCode, context.Supervisor->HasCrashedSince(*broker));
        if (probe != nullptr)
        {
            ReportUnexpectedEnd(context, probe->Tag, probe->ExitCode, context.Supervisor->HasCrashedSince(*probe));
        }
        return Failed(context);
    }
    WriteLine(context, InputDemonstratedLine, sizeof(InputDemonstratedLine) - 1U);
    return Hardware(context, error) ? PagerServeError::None : Failed(context);
}
} // Namespace Asteria::User::Pager
