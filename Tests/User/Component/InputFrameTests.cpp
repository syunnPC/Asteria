// SPDX-License-Identifier: Apache-2.0
#include "User/Component/InputFrame.hpp"

#include <cstring>
#include <initializer_list>
#include "Support/TestContext.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"

namespace
{
namespace C = Asteria::User::Component;
namespace F = Asteria::Foundation;
using Asteria::TestSupport::TestContext;
constexpr Asteria::User::System::CapabilityHandle Reply{1U, 2U};
F::UInt8 g_delivery[C::EchoDeliveryHeaderBytes + C::InputMaximumFrameBytes + 1U]{};

void Codecs(TestContext& context)
{
    for (const F::UInt64 status : {7ULL, 8ULL, 0x1'0000'0002ULL})
    {
        const F::UInt64 query[4U]{9U, 1U, status, 0x1'0000'0000ULL};
        const auto bytes =
            C::WriteInputFrame(g_delivery + C::EchoDeliveryHeaderBytes, C::InputReplyMethod, 12U, query, nullptr, 0U);
        C::InputMessage decoded{};
        ASTERIA_EXPECT(context, "USB query preserves interfaces, power bits and u64 counter",
                       C::ReadInputFrame(g_delivery, C::EchoDeliveryHeaderBytes + bytes, decoded,
                                         C::InputReplySchema::DriverQuery),
                       decoded.Words[2U] == status, decoded.Words[3U] == query[3U]);
    }
    const F::UInt64 query[4U]{9U, 1U, 0xFFFF'FFFF'0000'0008ULL, 0xFFFF'FFFF'FFFF'FFFFULL};
    C::InputMessage status{};
    auto queryBytes =
        C::WriteInputDriverQueryReply(g_delivery + C::EchoDeliveryHeaderBytes, 12U, 9U, 8U, 0xFFFF'FFFFU, query[3U]);
    ASTERIA_EXPECT(context, "driver query schema preserves full unsigned fields and does not widen registration",
                   C::ReadInputFrame(g_delivery, C::EchoDeliveryHeaderBytes + queryBytes, status,
                                     C::InputReplySchema::DriverQuery),
                   status.Words[2U] == query[2U], status.Words[3U] == query[3U], C::InputQueryInterfaces(status) == 8U,
                   C::InputQueryPoweredOff(status) == 0xFFFF'FFFFU,
                   !C::ReadInputFrame(g_delivery, C::EchoDeliveryHeaderBytes + queryBytes, status));
    queryBytes = C::WriteInputFrame(g_delivery + C::EchoDeliveryHeaderBytes, C::InputReplyMethod, 12U, query, nullptr,
                                    0U, &Reply);
    ASTERIA_EXPECT(context, "driver query cannot import a registration endpoint",
                   !C::ReadInputFrame(g_delivery, C::EchoDeliveryHeaderBytes + queryBytes, status,
                                      C::InputReplySchema::DriverQuery));
    const F::UInt64 rejectedQuery[4U]{9U, 5U, 8U, 1U};
    queryBytes = C::WriteInputFrame(g_delivery + C::EchoDeliveryHeaderBytes, C::InputReplyMethod, 12U, rejectedQuery,
                                    nullptr, 0U);
    ASTERIA_EXPECT(context, "failed driver query requires zero status fields",
                   !C::ReadInputFrame(g_delivery, C::EchoDeliveryHeaderBytes + queryBytes, status,
                                      C::InputReplySchema::DriverQuery));
    const F::UInt64 route[4U]{9U, 256U, 123U, 0U};
    const F::Size routeBytes =
        C::WriteInputPointerRoute(g_delivery + C::EchoDeliveryHeaderBytes, 12U, route, Reply, &Reply);
    C::InputMessage routed{};
    ASTERIA_EXPECT(context, "pointer route carries two Invoke-only endpoint copies",
                   C::ReadInputFrame(g_delivery, C::EchoDeliveryHeaderBytes + routeBytes, routed),
                   routed.Method == C::InputSetPointerRouteMethod, routed.PointerSink.Selector == Reply.Selector,
                   routed.Words[1U] == 256U, routed.Words[2U] == 123U);
    const F::Size rightsAt = C::EchoDeliveryHeaderBytes + C::EchoHeaderBytes + C::EchoAttachmentBytes + 25U;
    g_delivery[rightsAt] = 8U;
    ASTERIA_EXPECT(context, "route never imports display Manage",
                   !C::ReadInputFrame(g_delivery, C::EchoDeliveryHeaderBytes + routeBytes, routed));
    g_delivery[rightsAt] = 0U;
    const F::UInt64 words[4U]{9U, 1U, 0U, 0U};
    const Asteria::User::Input::InputEvent motion{.Kind = Asteria::User::Input::EventKind::Motion,
                                                  .Modifiers = 0U,
                                                  .Usage = 0U,
                                                  .Source = 0xFFFF'FFFFU,
                                                  .Dx = -2'147'483'647 - 1,
                                                  .Dy = 2'147'483'647,
                                                  .Buttons = 0U,
                                                  .Reserved = {},
                                                  .Wheel = 0};
    const F::Size size = C::WriteInputFrame(g_delivery + C::EchoDeliveryHeaderBytes, C::InputPublishMethod, 13U, words,
                                            &motion, 1U, &Reply);
    C::InputMessage message{};
    ASTERIA_EXPECT(context, "bounded publish decodes and preserves all signed motion bits",
                   C::ReadInputFrame(g_delivery, C::EchoDeliveryHeaderBytes + size, message), message.Count == 1U,
                   message.Transaction == 13U, C::ReadInputEvent(message.Events).Dx == motion.Dx,
                   C::ReadInputEvent(message.Events).Dy == motion.Dy,
                   C::ReadInputEvent(message.Events).Source == 0xFFFF'FFFFU);
    Asteria::Kernel::Ipc::MessageEnvelope envelope{};
    ASTERIA_EXPECT(context, "the native IPC codec accepts the protocol's canonical frame",
                   Asteria::Kernel::Ipc::MessageCodec::Decode(
                       Asteria::Kernel::Ipc::ReadOnlyByteView{g_delivery + C::EchoDeliveryHeaderBytes, size},
                       envelope) == Asteria::Kernel::Ipc::MessageCodecError::None);
    const C::InputMessage before = message;
    bool atomic{true};
    for (F::Size cut{0U}; cut < C::EchoDeliveryHeaderBytes + size; ++cut)
    {
        atomic = atomic && !C::ReadInputFrame(g_delivery, cut, message) &&
                 std::memcmp(&before, &message, sizeof(message)) == 0;
    }
    ASTERIA_EXPECT(context, "every truncated delivery fails atomically", atomic);
    // Canonical fixed header and reserved bytes, every attachment field,
    // reserved words, irrelevant fields, invalid kind, event tail padding.
    constexpr F::Size offsets[]{
        0U,         4U,         6U,        8U,        12U,       13U,        14U,        15U,        16U,
        24U,        32U,        36U,       48U,       56U,       64U,        72U,        80U,        81U,
        82U,        84U,        88U + 16U, 88U + 24U, 88U + 25U, 88U + 26U,  88U + 27U,  88U + 28U,  120U + 8U,
        120U + 16U, 120U + 24U, 152U,      152U + 1U, 152U + 2U, 152U + 16U, 152U + 17U, 152U + 20U, 152U + 24U};
    bool malformed{true};
    for (F::Size offset : offsets)
    {
        g_delivery[C::EchoDeliveryHeaderBytes + offset] ^= 0x80U;
        malformed = malformed && !C::ReadInputFrame(g_delivery, C::EchoDeliveryHeaderBytes + size, message);
        g_delivery[C::EchoDeliveryHeaderBytes + offset] ^= 0x80U;
    }
    ASTERIA_EXPECT(context, "malformed fields and noncanonical padding are refused", malformed);
    for (F::UInt32 method{C::InputRegisterSourceMethod}; method <= C::InputFocusRequestMethod; ++method)
    {
        F::UInt64 values[4U]{9U, 0U, 0U, 0U};
        const bool request = method <= C::InputQueryMethod;
        const bool events = (method == C::InputPublishMethod) || (method == C::InputEventsMethod);
        if ((method == C::InputCreditMethod) || events)
        {
            values[1U] = 1U;
        }
        if (method == C::InputReplyMethod)
        {
            values[1U] = 5U;
        }
        if (method >= C::InputEventsMethod)
        {
            values[1U] = 1U;
            values[2U] = method == C::InputEventsMethod ? 1U : 0U;
        }
        auto event = motion;
        event.Source = 1U;
        const F::Size bytes =
            C::WriteInputFrame(g_delivery + C::EchoDeliveryHeaderBytes, method, 3U, values, events ? &event : nullptr,
                               events ? 1U : 0U, request ? &Reply : nullptr);
        ASTERIA_EXPECT(context, "every method has a validated frame shape",
                       C::ReadInputFrame(g_delivery, C::EchoDeliveryHeaderBytes + bytes, message),
                       message.Method == method);
    }
    // Source and client ids share the larger bound, three sources (the
    // broker refuses a client it does not have). RegisterSource may name a
    // registered source to register it again; RegisterClient may not.
    struct Bound
    {
        F::UInt32 Method;
        F::Size Word;
        F::UInt64 Last;
        bool Attached;
    };
    constexpr Bound bounds[]{
        {C::InputRegisterSourceMethod, 1U, C::InputMaximumSources, true},
        {C::InputSetFocusMethod, 1U, C::InputMaximumClients, true},
        {C::InputAttentionMethod, 1U, C::InputMaximumSources, false},
        {C::InputFocusRequestMethod, 1U, C::InputMaximumClients, false},
        {C::InputReplyMethod, 2U, C::InputMaximumSources, false},
    };
    for (const Bound& bound : bounds)
    {
        for (F::UInt64 value{bound.Last}; value <= bound.Last + 1U; ++value)
        {
            F::UInt64 values[4U]{9U, 0U, 0U, 0U};
            values[1U] = (bound.Method == C::InputReplyMethod) ? 1U : value;
            values[2U] = (bound.Method == C::InputReplyMethod) ? value : 0U;
            values[3U] = (bound.Method == C::InputReplyMethod) ? C::InputProtocolVersion : 0U;
            const F::Size bytes = C::WriteInputFrame(g_delivery + C::EchoDeliveryHeaderBytes, bound.Method, 3U, values,
                                                     nullptr, 0U, bound.Attached ? &Reply : nullptr);
            ASTERIA_EXPECT(context, "the last source or client id is accepted, the next refused",
                           C::ReadInputFrame(g_delivery, C::EchoDeliveryHeaderBytes + bytes, message) ==
                               (value == bound.Last));
        }
    }
    const F::UInt64 client[4U]{9U, 1U, 0U, 0U};
    const F::Size registered = C::WriteInputFrame(g_delivery + C::EchoDeliveryHeaderBytes, C::InputRegisterClientMethod,
                                                  3U, client, nullptr, 0U, &Reply);
    ASTERIA_EXPECT(context, "v3 flag registers broker-routed pointer delivery",
                   C::ReadInputFrame(g_delivery, C::EchoDeliveryHeaderBytes + registered, message));
    for (F::UInt32 source{static_cast<F::UInt32>(C::InputMaximumSources)}; source <= C::InputMaximumSources + 1U;
         ++source)
    {
        const F::UInt64 delivered[4U]{9U, 1U, 1U, 0U};
        auto event = motion;
        event.Source = source;
        const F::Size bytes = C::WriteInputFrame(g_delivery + C::EchoDeliveryHeaderBytes, C::InputEventsMethod, 3U,
                                                 delivered, &event, 1U);
        ASTERIA_EXPECT(context, "delivered events carry a stamp of one of three sources",
                       C::ReadInputFrame(g_delivery, C::EchoDeliveryHeaderBytes + bytes, message) ==
                           (source == C::InputMaximumSources));
    }
    F::UInt8 scratch[C::InputMaximumFrameBytes]{};
    ASTERIA_EXPECT(context, "writer bounds leave output unchanged",
                   C::WriteInputFrame(scratch, 0U, 1U, words, nullptr, 0U) == 0U,
                   C::WriteInputFrame(scratch, 3U, 1U, words, &motion, 9U) == 0U, scratch[0U] == 0U);
}
} // namespace
int main()
{
    TestContext context{"InputFrameTests"};
    Codecs(context);
    return context.Finish();
}
