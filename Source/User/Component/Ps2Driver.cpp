// SPDX-License-Identifier: Apache-2.0

#include "User/Component/Ps2Driver.hpp"
#include "User/Component/Ps2Publication.hpp"
#if defined(ASTERIA_PS2_SHARED_SCRATCH)
#include "User/Component/ServiceScratch.hpp"
#include "Runtime/Placement.hpp"
#endif

#include "User/Component/InputFrame.hpp"
#include "Abi/I8042.hpp"
#include "User/Input/Ps2Controller.hpp"
#include "User/Input/Ps2EventQueue.hpp"
#include "User/Input/Ps2Keyboard.hpp"

namespace Asteria::User::Component
{
namespace
{
struct Driver
{
    Foundation::UInt8 ReadyFrame[EchoHeaderBytes]{};
    const System::SystemCallClient* Client;
    ComponentClock Clock;
    System::CapabilityHandle Data;
    System::CapabilityHandle Command;
    System::CapabilityHandle Source;
    System::CapabilityHandle Reply;
    System::CapabilityHandle Receive;
    System::CapabilityHandle WaitSet;
    System::CapabilityHandle Root;
    System::CapabilityHandle Lines[2U];
    Foundation::UInt64 Epoch;
    bool Registering;
    Input::MouseMode MouseMode;
    System::CapabilityHandle QueryReply;
    Foundation::UInt64 QueryTransaction;
    bool RegistrationSent;
    bool RetryPause;
    bool MouseEnabled;
    bool ReadyPending;
};
#if defined(ASTERIA_PS2_SHARED_SCRATCH)
// Storage configuration: reuse the ordinary service scratch when images combine mutually exclusive
// services.
struct SeatPs2Scratch
{
    Input::Ps2Keyboard Keyboard{};
    Input::Ps2Mouse Mouse{};
    Input::Ps2EventQueue Queue{};
    Input::EventBatch MouseEvents{};
    Foundation::UInt8 Frame[InputMaximumFrameBytes]{};
    Foundation::UInt8 Delivery[EchoDeliveryHeaderBytes + InputMaximumFrameBytes]{};
    Foundation::UInt64 Events{};
    Foundation::UInt8 Request[Abi::ObjectCreateInterruptRequestSizeBytes]{};
    Driver Hardware{};
};
static_assert(sizeof(SeatPs2Scratch) <= ServiceScratch::StorageBytes());
static_assert(alignof(SeatPs2Scratch) <= alignof(ServiceScratch));
constinit SeatPs2Scratch* g_seat{nullptr};
#else
constinit Input::Ps2Keyboard g_keyboard{};
constinit Input::Ps2Mouse g_mouse{};
constinit Input::Ps2EventQueue g_queue{};
constinit Input::EventBatch g_mouseEvents{};
constinit Foundation::UInt8 g_frame[InputMaximumFrameBytes]{};
constinit Foundation::UInt8 g_delivery[EchoDeliveryHeaderBytes + InputMaximumFrameBytes]{};
constinit Foundation::UInt64 g_events{0U};
constinit Foundation::UInt8 g_request[Abi::ObjectCreateInterruptRequestSizeBytes]{};
constinit Driver g_driver{};
#endif
[[nodiscard, gnu::always_inline]] inline auto& Keyboard() noexcept
{
#if defined(ASTERIA_PS2_SHARED_SCRATCH)
    return g_seat->Keyboard;
#else
    return g_keyboard;
#endif
}
[[nodiscard, gnu::always_inline]] inline auto& Mouse() noexcept
{
#if defined(ASTERIA_PS2_SHARED_SCRATCH)
    return g_seat->Mouse;
#else
    return g_mouse;
#endif
}
[[nodiscard, gnu::always_inline]] inline auto& Queue() noexcept
{
#if defined(ASTERIA_PS2_SHARED_SCRATCH)
    return g_seat->Queue;
#else
    return g_queue;
#endif
}
[[nodiscard, gnu::always_inline]] inline auto& MouseEvents() noexcept
{
#if defined(ASTERIA_PS2_SHARED_SCRATCH)
    return g_seat->MouseEvents;
#else
    return g_mouseEvents;
#endif
}
[[nodiscard, gnu::always_inline]] inline auto& Frame() noexcept
{
#if defined(ASTERIA_PS2_SHARED_SCRATCH)
    return g_seat->Frame;
#else
    return g_frame;
#endif
}
[[nodiscard, gnu::always_inline]] inline auto& Delivery() noexcept
{
#if defined(ASTERIA_PS2_SHARED_SCRATCH)
    return g_seat->Delivery;
#else
    return g_delivery;
#endif
}
[[nodiscard, gnu::always_inline]] inline auto& Events() noexcept
{
#if defined(ASTERIA_PS2_SHARED_SCRATCH)
    return g_seat->Events;
#else
    return g_events;
#endif
}
[[nodiscard, gnu::always_inline]] inline auto& Request() noexcept
{
#if defined(ASTERIA_PS2_SHARED_SCRATCH)
    return g_seat->Request;
#else
    return g_request;
#endif
}
[[nodiscard, gnu::always_inline]] inline auto& Hardware() noexcept
{
#if defined(ASTERIA_PS2_SHARED_SCRATCH)
    return g_seat->Hardware;
#else
    return g_driver;
#endif
}

[[nodiscard]] bool Read(void* context, bool status, Foundation::UInt8& value) noexcept
{
    const auto& driver = *static_cast<Driver*>(context);
    const auto response = driver.Client->PortIo(status ? driver.Command : driver.Data, 0U, 1U, 0U);
    value = static_cast<Foundation::UInt8>(response.Value0);
    return response.Error == Abi::SystemCallError::None;
}
[[nodiscard]] bool Write(void* context, bool command, Foundation::UInt8 value) noexcept
{
    const auto& driver = *static_cast<Driver*>(context);
    return driver.Client->PortIo(command ? driver.Command : driver.Data, 0U, 1U, 1U, value).Error ==
           Abi::SystemCallError::None;
}
[[nodiscard]] Foundation::UInt64 Clock(void* context) noexcept
{
    return static_cast<Driver*>(context)->Clock();
}
[[nodiscard]] bool Observe(void* context, Foundation::UInt8 status, Foundation::UInt8 byte) noexcept
{
    if ((status & 0x20U) == 0U)
    {
        const Input::ScanOutput output = Keyboard().Feed(byte);
        if (output.Result != Input::ScanResult::Event)
        {
            return true;
        }
        if (Events() < 0xFFFF'FFFFU)
        {
            ++Events();
        }
        return Queue().Append(output.Event);
    }
    if (!static_cast<Driver*>(context)->MouseEnabled)
    {
        return true;
    }
    static_cast<void>(Mouse().Feed(byte, MouseEvents()));
    for (Foundation::Size index{0U}; index < MouseEvents().Count; ++index)
    {
        if (Events() < 0xFFFF'FFFFU)
        {
            ++Events();
        }
        if (!Queue().Append(MouseEvents().Events[index]))
        {
            return false;
        }
    }
    MouseEvents().Count = 0U;
    return true;
}
[[nodiscard]] System::CapabilityHandle Handle(const Foundation::UInt64* block, Foundation::Size word) noexcept
{
    return System::CapabilityHandle{block[word], block[word + 1U]};
}
[[nodiscard]] bool Send(Driver& driver) noexcept
{
    if (driver.Registering && driver.RegistrationSent)
    {
        return true;
    }
    const Foundation::Size count = driver.Registering ? 0U : Queue().Prepare();
    if (!driver.Registering && (count == 0U))
    {
        return true;
    }
    const Foundation::UInt64 words[4U]{driver.Epoch, count, 0U, 0U};
    const Foundation::Size size =
        WriteInputFrame(Frame(), driver.Registering ? InputRegisterSourceMethod : InputPublishMethod,
                        Queue().GetTransaction(), words, Queue().GetEvents(), count, &driver.Reply);
    const auto sent =
        driver.Client->Send(driver.Registering ? driver.Root : driver.Source, System::AddressOf(Frame()), size);
    if (!driver.Registering &&
        ((sent.Error == Abi::SystemCallError::Revoked) || (sent.Error == Abi::SystemCallError::ObjectNotFound) ||
         (sent.Error == Abi::SystemCallError::InvalidHandle) || (sent.Error == Abi::SystemCallError::Expired)))
    {
        driver.Registering = true;
        Queue().Restart();
        Mouse().Reset(driver.MouseMode);
        return true;
    }
    if (driver.Registering && (sent.Error == Abi::SystemCallError::None))
    {
        driver.RegistrationSent = true;
    }
    return (sent.Error == Abi::SystemCallError::None) || (sent.Error == Abi::SystemCallError::WouldBlock) ||
           (sent.Error == Abi::SystemCallError::ResourceExhausted);
}
[[nodiscard]] bool Replies(Driver& driver) noexcept
{
    // Reply traffic cannot prevent draining/rearming PS/2 IRQs.
    for (Foundation::Size round{0U}; round < 4U; ++round)
    {
        const auto received = driver.Client->Receive(driver.Receive, System::AddressOf(Delivery()), sizeof(Delivery()));
        if (received.Error == Abi::SystemCallError::WouldBlock)
        {
            return true;
        }
        InputMessage message{};
        if ((received.Error != Abi::SystemCallError::None) ||
            !ReadInputFrame(Delivery(), static_cast<Foundation::Size>(received.Value1), message))
        {
            return false;
        }
        if (message.Method == InputQueryMethod)
        {
            if (driver.QueryReply.Selector != 0U)
            {
                static_cast<void>(driver.Client->CloseCapability(message.Reply));
            }
            else
            {
                driver.QueryReply = message.Reply;
                driver.QueryTransaction = message.Transaction;
            }
            continue;
        }
        if ((message.Method != InputReplyMethod) || (message.Transaction != Queue().GetTransaction()))
        {
            if ((message.Reply.Selector != 0U) &&
                (driver.Client->CloseCapability(message.Reply).Error != Abi::SystemCallError::None))
            {
                return false;
            }
            continue;
        }
        const auto answer = static_cast<InputAnswer>(message.Words[1U]);
        if (driver.Registering)
        {
            if ((answer != InputAnswer::Done) || (message.Reply.Selector == 0U))
            {
                return false;
            }
            static_cast<void>(driver.Client->CloseCapability(driver.Source));
            driver.Source = message.Reply;
            driver.Epoch = message.Words[0U];
            driver.Registering = false;
            driver.RegistrationSent = false;
        }
        else if ((message.Reply.Selector != 0U) || (message.Words[2U] != 0U) || (message.Words[3U] != 0U) ||
                 ((answer != InputAnswer::Restarted) && (message.Words[0U] != driver.Epoch)))
        {
            if (message.Reply.Selector != 0U)
            {
                static_cast<void>(driver.Client->CloseCapability(message.Reply));
            }
            return false;
        }
        else
        {
            if (answer == InputAnswer::Done)
            {
                ObservePs2Publication(Queue().GetEvents(), Queue().Prepare());
            }
            if (!Queue().Answer(answer))
            {
                return false;
            }
            if (answer == InputAnswer::Restarted)
            {
                driver.Epoch = message.Words[0U];
                driver.Registering = true;
                driver.RegistrationSent = false;
                Mouse().Reset(driver.MouseMode);
            }
        }
        if ((answer == InputAnswer::Busy) || (answer == InputAnswer::Exhausted))
        {
            driver.RetryPause = true;
        }
    }
    return true;
}
[[nodiscard, gnu::noinline]] bool Connect(Driver& driver, const Foundation::UInt64* block) noexcept
{
    const auto domain = Handle(block, Abi::ComponentAccountWord);
    for (Foundation::Size index{0U}; index < 2U; ++index)
    {
        System::StoreHandle(Request(), Handle(block, Abi::ComponentPs2KeyboardLineWord + index * 2U));
        System::StoreWord(Request() + Abi::ObjectCreateInterruptSourceOffset, Abi::InterruptSourceLine);
        const auto created = driver.Client->CreateObject(domain, Abi::ObjectTypeInterrupt, System::AddressOf(Request()),
                                                         sizeof(Request()));
        if (created.Error != Abi::SystemCallError::None)
        {
            return false;
        }
        driver.Lines[index] = System::CapabilityHandle{created.Value0, created.Value1};
        if (driver.Client->BindWaitSet(driver.WaitSet, driver.Lines[index], 1U + index).Error !=
            Abi::SystemCallError::None)
        {
            return false;
        }
    }
    return true;
}
[[gnu::noinline]] void InitializeDriver(const System::SystemCallClient& client, const Foundation::UInt64* block,
                                        const ComponentClock clock) noexcept
{
    Hardware() = Driver{.Client = &client,
                        .Clock = clock,
                        .Data = Handle(block, Abi::ComponentPs2DataWord),
                        .Command = Handle(block, Abi::ComponentPs2CommandWord),
                        .Source = Handle(block, Abi::ComponentInputServiceWord),
                        .Reply = Handle(block, Abi::ComponentReplyWord),
                        .Receive = Handle(block, Abi::ComponentReceiveWord),
                        .WaitSet = Handle(block, Abi::ComponentWaitSetWord),
                        .Root = Handle(block, Abi::ComponentReportWord),
                        .Lines = {},
                        .Epoch = block[Abi::ComponentInputEpochWord],
                        .Registering = false,
                        .MouseMode = Input::MouseMode::Standard,
                        .QueryReply = {},
                        .QueryTransaction = 0U,
                        .RegistrationSent = false,
                        .RetryPause = false,
                        .MouseEnabled = false,
                        .ReadyPending = false};
}
[[nodiscard]] Foundation::UInt64 TicksPerMillisecond(const ComponentClock clock,
                                                     const Foundation::UInt64* block) noexcept
{
    const Foundation::UInt64 scale = block[Abi::ComponentClockTicksPerMicrosecondWord];
    return ((clock == nullptr) || (scale > ~Foundation::UInt64{0U} / 1000U)) ? 0U : scale * 1000U;
}
// The decoder follows the mode. Bytes observed during bring-up were decoded
// as set 2 and nothing has been published yet, so a fallback mode discards
// their events.
void SelectKeyboard(const Input::Ps2KeyboardMode mode) noexcept
{
    Keyboard().SetScanCodeSet(mode == Input::Ps2KeyboardMode::Set1Translated ? Input::ScanCodeSet::Set1
                                                                             : Input::ScanCodeSet::Set2);
    if (mode != Input::Ps2KeyboardMode::Set2)
    {
        Queue().Restart();
        Events() = 0U;
    }
}
[[nodiscard]] bool Ready(const System::SystemCallClient& client, Driver& driver) noexcept
{
    if (!driver.ReadyPending)
    {
        return true;
    }
    const auto error = client.Send(driver.Root, System::AddressOf(driver.ReadyFrame), sizeof(driver.ReadyFrame)).Error;
    if (error == Abi::SystemCallError::None)
    {
        driver.ReadyPending = false;
    }
    return error == Abi::SystemCallError::None || error == Abi::SystemCallError::WouldBlock;
}
[[nodiscard, gnu::noinline]] bool SendReady(const System::SystemCallClient& client, Driver& driver,
                                            const Input::Ps2Controller& controller) noexcept
{
    // A slow root reader cannot end the physical input service. Keep these startup bytes separate
    // from registration/publish retries.
    WriteEchoFrame(driver.ReadyFrame, nullptr, 0U);
    const Foundation::UInt64 readyMethod =
        controller.GetAuxiliaryState() == Input::AuxiliaryState::Failed ? Ps2ReadyAuxFailedMethod : Ps2ReadyMethod;
    const Input::Ps2KeyboardMode mode = controller.GetKeyboardMode();
    Detail::InputStore32(driver.ReadyFrame + 32U, static_cast<Foundation::UInt32>(readyMethod) |
                                                      (static_cast<Foundation::UInt32>(mode) << Ps2ReadyModeShift));
    if (mode != Input::Ps2KeyboardMode::Set2)
    {
        const Foundation::UInt64 reason = Ps2FailureCode(controller.GetFallbackReason());
        Detail::InputStore32(driver.ReadyFrame + Ps2ReadyTransactionOffset, static_cast<Foundation::UInt32>(reason));
        Detail::InputStore32(driver.ReadyFrame + Ps2ReadyTransactionOffset + 4U,
                             static_cast<Foundation::UInt32>(reason >> 32U));
    }
    driver.ReadyPending = true;
    return Ready(client, driver);
}
} // Namespace
Foundation::UInt64 ServePs2(const System::SystemCallClient& client,
                            const Foundation::UInt64 (&block)[Abi::ComponentBlockWords],
                            const ComponentClock clock) noexcept
{
#if defined(ASTERIA_PS2_SHARED_SCRATCH)
    g_seat = new (Scratch().UninitializedStorage()) SeatPs2Scratch{};
#endif

    InitializeDriver(client, block, clock);
    Driver& driver = Hardware();
    Input::Ps2Controller controller{Input::Ps2Transport{.Context = &driver,
                                                        .Read = Read,
                                                        .Write = Write,
                                                        .Observe = Observe,
                                                        .Clock = (clock != nullptr) ? Clock : nullptr},
                                    65'536U, TicksPerMillisecond(clock, block)};
    if (!Connect(driver, block))
    {
        return Ps2FailureCode(controller.Describe(Input::Ps2Step::Connect, Input::Ps2Reason::Transport));
    }
    const bool mouseMayExist = (block[Abi::ComponentPs2DeclaredWord] & (Abi::I8042FadtFlag | Abi::I8042MouseAml)) != 0U;
    if (!controller.Initialize(mouseMayExist ? Input::AuxiliaryPolicy::Probe : Input::AuxiliaryPolicy::Disabled))
    {
        return Ps2FailureCode(controller.GetFailure());
    }
    driver.MouseEnabled = controller.GetAuxiliaryState() == Input::AuxiliaryState::Ready;
    driver.MouseMode = controller.GetMouseMode();
    Mouse().Reset(driver.MouseMode);
    SelectKeyboard(controller.GetKeyboardMode());
    // Re-arm, then drain until status reports the output buffer empty: a
    // byte that arrived while the line was masked holds the controller's
    // Request until this drain reads it, and one that arrives after the last empty read raises a
    // new request on the unmasked line, edge or level.
    for (const auto line : driver.Lines)
    {
        if (client.ControlInterrupt(line, Abi::InterruptRearm).Error != Abi::SystemCallError::None)
        {
            return Ps2FailureCode(controller.Describe(Input::Ps2Step::Arm, Input::Ps2Reason::Transport));
        }
        if (!controller.Drain())
        {
            return Ps2FailureCode(controller.Describe(Input::Ps2Step::Arm, controller.GetFailure().Reason));
        }
    }
    if (!SendReady(client, driver, controller))
    {
        return Ps2FailureCode(controller.Describe(Input::Ps2Step::Ready, Input::Ps2Reason::Transport));
    }
    for (;;)
    {
        if (!SendPs2Publication(client, driver.Root))
        {
            return Ps2FailureCode(controller.Describe(Input::Ps2Step::Serve, Input::Ps2Reason::Transport));
        }
        if (!Ready(client, driver))
        {
            return Ps2FailureCode(controller.Describe(Input::Ps2Step::Ready, Input::Ps2Reason::Transport));
        }
        if (driver.RetryPause)
        {
            // Receive has emptied the reply Endpoint. Wait once before retry;
            // a device interrupt may wake it early and still needs draining.
            driver.RetryPause = false;
        }
        else if (!Send(driver))
        {
            return Ps2FailureCode(controller.Describe(Input::Ps2Step::Serve, Input::Ps2Reason::Transport));
        }
        const auto waited =
            client.Wait(driver.WaitSet, (Ps2PublicationPending() || driver.ReadyPending || driver.Registering ||
                                         (driver.QueryReply.Selector != 0U) || (Queue().GetCount() != 0U))
                                            ? 10'000U
                                            : 0U);
        if ((waited.Error != Abi::SystemCallError::None) && (waited.Error != Abi::SystemCallError::TimedOut))
        {
            return Ps2FailureCode(controller.Describe(Input::Ps2Step::Serve, Input::Ps2Reason::Transport));
        }
        if (!Replies(driver))
        {
            return Ps2FailureCode(controller.Describe(Input::Ps2Step::Serve, Input::Ps2Reason::Transport));
        }
        if (!controller.Drain())
        {
            return Ps2FailureCode(controller.Describe(Input::Ps2Step::Serve, controller.GetFailure().Reason));
        }
        if ((driver.QueryReply.Selector != 0U) && (Queue().GetCount() == 0U))
        {
            const auto size =
                WriteInputDriverQueryReply(Frame(), driver.QueryTransaction, driver.Epoch, 0U, 0U, Events());
            const auto sent = client.Send(driver.QueryReply, System::AddressOf(Frame()), size);
            if (sent.Error == Abi::SystemCallError::None)
            {
                if (client.CloseCapability(driver.QueryReply).Error != Abi::SystemCallError::None)
                {
                    return Ps2FailureCode(controller.Describe(Input::Ps2Step::Serve, Input::Ps2Reason::Transport));
                }
                driver.QueryReply = {};
            }
            else if (sent.Error != Abi::SystemCallError::WouldBlock)
            {
                return Ps2FailureCode(controller.Describe(Input::Ps2Step::Serve, Input::Ps2Reason::Transport));
            }
        }
        if ((waited.Error == Abi::SystemCallError::None) && (waited.Value0 >= 1U) && (waited.Value0 <= 2U))
        {
            // The same re-arm-then-drain order as at arming (edge or level).
            if (client.ControlInterrupt(driver.Lines[waited.Value0 - 1U], Abi::InterruptRearm).Error !=
                Abi::SystemCallError::None)
            {
                return Ps2FailureCode(controller.Describe(Input::Ps2Step::Serve, Input::Ps2Reason::Transport));
            }
            if (!controller.Drain())
            {
                return Ps2FailureCode(controller.Describe(Input::Ps2Step::Serve, controller.GetFailure().Reason));
            }
        }
    }
}
} // Namespace Asteria::User::Component
