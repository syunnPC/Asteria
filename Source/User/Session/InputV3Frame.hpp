// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SESSION_INPUT_V3_FRAME_HPP_INCLUDED
#define ASTERIA_USER_SESSION_INPUT_V3_FRAME_HPP_INCLUDED
#include "User/Session/Wire.hpp"
#include "User/Component/InputFrame.hpp"
#include "User/Component/DisplayFrame.hpp"
namespace Asteria::User::Session
{
/// Field codec for desktop session. InputFrame.hpp and this codec share the v3 contract. Interface
/// identifiers stay unchanged; a registration reply reports version 3. Wire event size remains 24
/// bytes.
inline constexpr Foundation::UInt64 InputV3ProtocolVersion{Component::InputProtocolVersion};
inline constexpr Foundation::UInt64 InputV3BrokerPointer{Component::InputBrokerPointer};
using InputV3Message = Wire::Message;
struct InputV3Codec
{
    static constexpr auto Low = Component::InputInterfaceLow;
    static constexpr auto High = Component::InputInterfaceHigh;
    [[nodiscard]] static Foundation::Size Count(Foundation::UInt32 method) noexcept
    {
        return (method >= 1U) && (method <= Component::InputBaselineMethod) ? 28U : 0U;
    }
    [[nodiscard]] static Wire::Form Attachment(Foundation::UInt32 method, Foundation::Size) noexcept
    {
        return {Component::EchoEndpointType,
                static_cast<Foundation::UInt16>(Component::EchoInvokeRight | (method == 9U ? 0x100U : 0U))};
    }
    template <class W>
    [[nodiscard]] static bool Valid(Foundation::UInt32 method, const W& w, Foundation::Size attachments,
                                    Foundation::UInt64) noexcept
    {
        using Foundation::Size;
        using Foundation::UInt64;
        if (w[0U] == 0U)
        {
            return false;
        }
        Size events{0U};
        if (method == Component::InputSetPointerRouteMethod)
        {
            if ((w[3U] != 0U) ||
                !(((attachments == 2U) && (w[1U] != 0U)) || ((attachments == 1U) && (w[1U] == 0U) && (w[2U] == 0U))))
            {
                return false;
            }
        }
        else if (method == Component::InputRebindClientMethod || method == Component::InputResumeMethod)
        {
            if (attachments != 1U || w[2U] > (method == Component::InputRebindClientMethod ? 1U : 0U) || w[3U] != 0U ||
                (method == Component::InputResumeMethod ? w[1U] != 0U
                                                        : w[1U] < 1U || w[1U] > Component::InputMaximumClients))
            {
                return false;
            }
        }
        else if (method == Component::InputBaselineMethod)
        {
            if (attachments != 0U || w[3U] > 7U)
            {
                return false;
            }
        }
        else if (method <= Component::InputQueryMethod)
        {
            if ((attachments != 1U) || (w[3U] != 0U) || (w[2U] > (method == Component::InputSetFocusMethod ? 1U : 0U)))
            {
                return false;
            }
            switch (method)
            {
            case Component::InputRegisterSourceMethod:
                if (w[1U] > Component::InputMaximumSources)
                {
                    return false;
                }
                break;
            case Component::InputRegisterClientMethod:
                if (w[1U] > InputV3BrokerPointer)
                {
                    return false;
                }
                break;
            case Component::InputSetFocusMethod:
                if (w[1U] > Component::InputMaximumClients)
                {
                    return false;
                }
                break;
            case Component::InputPublishMethod:
            case Component::InputCreditMethod:
                if ((w[1U] < 1U) || (w[1U] > Component::InputMaximumEvents))
                {
                    return false;
                }
                if (method == Component::InputPublishMethod)
                {
                    events = static_cast<Size>(w[1U]);
                }
                break;
            default:
                if (w[1U] != 0U)
                {
                    return false;
                }
                break;
            }
        }
        else if (method == Component::InputReplyMethod)
        {
            if ((w[1U] < 1U) || (w[1U] > 5U) || (w[2U] > Component::InputMaximumSources) || (w[3U] > 0xFFFF'FFFFU) ||
                ((w[1U] != 1U) && ((w[2U] != 0U) || (w[3U] != 0U))) || (attachments > 1U) ||
                ((attachments == 1U) && ((w[1U] != 1U) || (w[2U] == 0U) || (w[3U] != InputV3ProtocolVersion))))
            {
                return false;
            }
        }
        else if (method == Component::InputEventsMethod)
        {
            if ((attachments != 0U) || (w[1U] < 1U) || (w[1U] > Component::InputMaximumClients) || (w[2U] < 1U) ||
                (w[2U] > Component::InputMaximumEvents))
            {
                return false;
            }
            events = static_cast<Size>(w[2U]); // w3 is the pointer horizon.
        }
        else if ((attachments != 0U) || (w[1U] < 1U) ||
                 (w[1U] > (method == 11U ? Component::InputMaximumSources : Component::InputMaximumClients)) ||
                 (w[3U] != 0U) ||
                 (w[2U] != 0U && !(method == Component::InputAttentionMethod && w[2U] >= 66U && w[2U] <= 68U)))
        {
            return false;
        }
        for (Size i{0U}; i < Component::InputMaximumEvents; ++i)
        {
            const Size first = 4U + 3U * i;
            if (i >= events)
            {
                if (!Wire::Zero(w, first, first + 3U))
                {
                    return false;
                }
                continue;
            }
            const UInt64 a = w[first];
            const UInt64 b = w[first + 1U];
            const UInt64 c = w[first + 2U];
            const Input::InputEvent event{static_cast<Input::EventKind>(a & 0xFFU),
                                          static_cast<Foundation::UInt8>(a >> 8U),
                                          static_cast<Foundation::UInt16>(a >> 16U),
                                          static_cast<Foundation::UInt32>(a >> 32U),
                                          Component::Detail::InputSigned(static_cast<Foundation::UInt32>(b)),
                                          Component::Detail::InputSigned(static_cast<Foundation::UInt32>(b >> 32U)),
                                          static_cast<Foundation::UInt8>(c),
                                          {static_cast<Foundation::UInt8>(c >> 8U),
                                           static_cast<Foundation::UInt8>(c >> 16U),
                                           static_cast<Foundation::UInt8>(c >> 24U)},
                                          Component::Detail::InputSigned(static_cast<Foundation::UInt32>(c >> 32U))};
            if (!Component::IsInputEvent(event) ||
                ((method == Component::InputEventsMethod) &&
                 ((event.Source == 0U) || (event.Source > Component::InputMaximumSources))))
            {
                return false;
            }
        }
        return true;
    }
};
/// Pointer route word 7 carries the queued key count. This
/// codec matches PointerRoute's immutable v3 route entries.
struct InputV3PointerCodec
{
    static constexpr auto Low = Component::DisplayInterfaceLow;
    static constexpr auto High = Component::DisplayInterfaceHigh;
    [[nodiscard]] static Foundation::Size Count(Foundation::UInt32 method) noexcept
    {
        return method == Component::DisplayPointerMethod ? 8U : 0U;
    }
    [[nodiscard]] static Wire::Form Attachment(Foundation::UInt32, Foundation::Size) noexcept
    {
        return {0U, 0U};
    }
    template <class W>
    [[nodiscard]] static bool Valid(Foundation::UInt32, const W& w, Foundation::Size attachments,
                                    Foundation::UInt64) noexcept
    {
        return (attachments == 0U) && (w[0U] != 0U) && (w[1U] != 0U) && (w[2U] >= 1U) && (w[2U] <= 3U) &&
               (w[4U] <= 7U) && (w[5U] >= 1U) && (w[5U] <= Component::InputMaximumSources) && (w[6U] <= 0xFFFF'FFFFU) &&
               ((w[2U] != Component::DisplayAbsoluteMotion) || Wire::Pair(w[3U], 0U, 65535U)) &&
               ((w[2U] != Component::DisplayPointerSnapshot) || (w[3U] == 0U));
    }
};
[[nodiscard]] Foundation::Size WriteInputV3Frame(Foundation::UInt8* frame, Foundation::UInt32 method,
                                                 Foundation::UInt64 txn, const Foundation::UInt64* words,
                                                 const System::CapabilityHandle* handles,
                                                 Foundation::Size attachments) noexcept;
[[nodiscard]] bool ReadInputV3Frame(const Foundation::UInt8* delivery, Foundation::Size bytes,
                                    InputV3Message& message) noexcept;
[[nodiscard]] Foundation::Size WriteInputV3Pointer(Foundation::UInt8* frame, Foundation::UInt64 txn,
                                                   const Foundation::UInt64* words) noexcept;
[[nodiscard]] bool ReadInputV3Pointer(const Foundation::UInt8* delivery, Foundation::Size bytes,
                                      InputV3Message& message) noexcept;
} // Namespace Asteria::User::Session
#endif
