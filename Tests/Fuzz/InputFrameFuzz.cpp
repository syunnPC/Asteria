// SPDX-License-Identifier: Apache-2.0
#include "User/Component/InputFrame.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
namespace C = Asteria::User::Component;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::Size;
UInt8 g_delivery[C::EchoDeliveryHeaderBytes + C::InputMaximumFrameBytes + 8U]{};
UInt8 g_frame[C::InputMaximumFrameBytes]{};
Asteria::User::Input::InputEvent g_events[C::InputMaximumEvents]{};
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > C::InputMaximumFrameBytes + 8U)
    {
        return 0;
    }
    std::memcpy(g_delivery + C::EchoDeliveryHeaderBytes, data, size);
    C::InputMessage message{};
    std::memset(&message, 0xA5, sizeof(message));
    C::InputMessage before{};
    std::memcpy(&before, &message, sizeof(message));
    if (!C::ReadInputFrame(g_delivery, C::EchoDeliveryHeaderBytes + size, message))
    {
        if (std::memcmp(&before, &message, sizeof(message)) != 0)
        {
            std::abort();
        }
        return 0;
    }
    for (Size index{0U}; index < message.Count; ++index)
    {
        g_events[index] = C::ReadInputEvent(message.Events + index * 24U);
        if (!C::IsInputEvent(g_events[index]))
        {
            std::abort();
        }
    }
    const Size rebuilt =
        message.Method == C::InputSetPointerRouteMethod
            ? C::WriteInputPointerRoute(g_frame, message.Transaction, message.Words, message.Reply,
                                        message.PointerSink.Selector == 0U ? nullptr : &message.PointerSink)
            : C::WriteInputFrame(g_frame, message.Method, message.Transaction, message.Words, g_events, message.Count,
                                 message.Reply.Selector == 0U ? nullptr : &message.Reply);
    if ((rebuilt != size) || (std::memcmp(g_frame, data, size) != 0))
    {
        std::abort();
    }
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*emit)(const std::uint8_t*, std::size_t))
{
    const Asteria::User::System::CapabilityHandle reply{1U, 2U};
    const UInt64 route[4U]{3U, 4U, 5U, 0U};
    emit(g_frame, C::WriteInputPointerRoute(g_frame, 8U, route, reply, &reply));
    const UInt64 clear[4U]{3U, 0U, 0U, 0U};
    emit(g_frame, C::WriteInputPointerRoute(g_frame, 9U, clear, reply, nullptr));
    for (Asteria::Foundation::UInt32 method{1U}; method <= C::InputFocusRequestMethod; ++method)
    {
        UInt64 words[4U]{3U, 0U, 0U, 0U};
        const bool events = (method == C::InputPublishMethod) || (method == C::InputEventsMethod);
        if (events || (method == C::InputCreditMethod) || (method == C::InputReplyMethod) ||
            (method >= C::InputAttentionMethod))
        {
            words[1U] = 1U;
        }
        if (method == C::InputEventsMethod)
        {
            words[2U] = 1U;
        }
        g_events[0U] = Asteria::User::Input::InputEvent{.Kind = Asteria::User::Input::EventKind::KeyDown,
                                                        .Modifiers = 5U,
                                                        .Usage = 0x4CU,
                                                        .Source = 1U,
                                                        .Dx = 0,
                                                        .Dy = 0,
                                                        .Buttons = 0U,
                                                        .Reserved = {},
                                                        .Wheel = 0};
        const Size size = C::WriteInputFrame(g_frame, method, 7U, words, g_events, events ? 1U : 0U,
                                             method <= C::InputQueryMethod ? &reply : nullptr);
        emit(g_frame, size);
        g_frame[size - 1U] = 1U;
        emit(g_frame, size);
    }
}
