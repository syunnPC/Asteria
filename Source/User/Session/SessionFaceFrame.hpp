// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SESSION_SESSION_FACE_FRAME_HPP_INCLUDED
#define ASTERIA_USER_SESSION_SESSION_FACE_FRAME_HPP_INCLUDED
#include "User/Session/Wire.hpp"
#include "User/Session/SessionFrame.hpp"
#include "User/Component/DisplayFrame.hpp"
namespace Asteria::User::Session
{
/// Desktop session, isolated from the VM-tested DisplayFrame v2.
inline constexpr Foundation::UInt64 DisplayFeatureSession{Component::DisplayFeatureSession};
inline constexpr Foundation::UInt64 SessionPresented{3U};
inline constexpr Foundation::UInt64 SessionBusy{4U};
inline constexpr Foundation::UInt64 SessionStale{5U};
inline constexpr Foundation::UInt32 FaceCreateSurface{5U};
inline constexpr Foundation::UInt32 FacePresent{6U};
inline constexpr Foundation::UInt32 FaceDestroySurface{7U};
inline constexpr Foundation::UInt32 FaceAddSurfaceFrames{10U};
inline constexpr Foundation::UInt32 FaceCredit{11U};
inline constexpr Foundation::UInt32 FaceOpen{12U};
inline constexpr Foundation::UInt32 FaceShow{13U};
inline constexpr Foundation::UInt32 FaceDrop{14U};
inline constexpr Foundation::UInt32 FaceDetach{15U};
inline constexpr Foundation::UInt32 FaceSnapshots{16U};
inline constexpr Foundation::UInt32 FaceFence{17U};
using SessionFaceMessage = Wire::Message;
struct SessionFaceCodec
{
    static constexpr auto Low = Component::DisplayInterfaceLow;
    static constexpr auto High = Component::DisplayInterfaceHigh;
    [[nodiscard]] static Foundation::Size Count(Foundation::UInt32 method) noexcept
    {
        switch (method)
        {
        case 3U:
            return 10U;
        case FaceCreateSurface:
            return 12U;
        case FacePresent:
            return 20U;
        case FaceSnapshots:
            return 32U;
        case FaceDestroySurface:
        case FaceAddSurfaceFrames:
        case FaceCredit:
        case FaceOpen:
        case FaceShow:
        case FaceDrop:
        case FaceDetach:
        case FaceFence:
            return 4U;
        default:
            return 0U;
        }
    }
    [[nodiscard]] static Wire::Form Attachment(Foundation::UInt32 method, Foundation::Size i) noexcept
    {
        if ((method == FaceCreateSurface) || (method == FaceAddSurfaceFrames))
        {
            return {Component::EchoFrameType, SessionMapRight};
        }
        // Open's reply carries a Manage face and an Invoke control endpoint.
        const Foundation::UInt16 rights = method == 3U
                                              ? static_cast<Foundation::UInt16>(0x102U | (i == 0U ? 0x800U : 0U))
                                              : Component::EchoInvokeRight;
        return {Component::EchoEndpointType, rights};
    }
    template <class W>
    [[nodiscard]] static bool Valid(Foundation::UInt32 method, const W& w, Foundation::Size attachments,
                                    Foundation::UInt64 txn) noexcept
    {
        using Foundation::Size;
        using Foundation::UInt64;
        if (method == FaceDrop)
        {
            return (attachments == 0U) && Wire::Zero(w, 0U, 4U);
        }
        if (method == FaceShow)
        {
            return (attachments == 0U) && (w[0U] <= 1U) && (w[1U] <= 1U) && Wire::Zero(w, 2U, 4U);
        }
        if (method == 3U)
        {
            return (w[0U] == txn) && (w[1U] >= 1U) && (w[1U] <= SessionStale) &&
                   (w[2U] == Component::DisplayProtocolVersion) && Wire::Pair(w[4U], 1U, SessionMaximumOutputExtent) &&
                   ((w[5U] & 0xFFFF'FFFFU) <= 640U) && ((w[5U] >> 32U) <= 256U) && (w[6U] != 0U) && (w[9U] == 0U) &&
                   ((attachments == 0U) || ((attachments == 2U) && (w[1U] == 1U)));
        }
        if (w[0U] == 0U)
        {
            return false;
        }
        switch (method)
        {
        case FaceOpen:
            return (attachments == 1U) && Wire::Zero(w, 1U, 4U);
        case FaceDestroySurface:
        case FaceDetach:
            return (attachments == 0U) && Wire::Zero(w, 1U, 4U);
        case FaceFence:
            return attachments == 1U && Wire::Zero(w, 1U, 4U);
        case FaceCredit:
            return (attachments == 1U) && (w[1U] >= 1U) && (w[1U] <= SessionMaximumEvents) && Wire::Zero(w, 2U, 4U);
        case FaceAddSurfaceFrames:
            return (attachments >= 1U) && (attachments <= 4U) && (w[1U] < 16U) && (w[1U] + attachments <= 16U) &&
                   Wire::Zero(w, 2U, 4U);
        case FaceCreateSurface:
        {
            if ((attachments < 1U) || (attachments > 4U) || (w[2U] < 1U) || (w[2U] > 16U) ||
                (w[3U] + attachments > w[2U]) || (w[3U] >= w[2U]) || !Wire::Pair(w[4U], 1U, SessionMaximumExtent) ||
                (w[5U] < (w[4U] & 0xFFFF'FFFFU)) || (w[5U] > SessionMaximumExtent) || (w[6U] < 1U) || (w[6U] > 2U) ||
                !Wire::Pair(w[7U], 0U, SessionMaximumOutputExtent))
            {
                return false;
            }
            UInt64 total{0U};
            for (Size i{0U}; i < 16U; ++i)
            {
                const UInt64 pages = (w[8U + (i >> 2U)] >> ((i & 3U) * 16U)) & 0xFFFFU;
                if (i < w[2U] ? ((pages == 0U) || (pages > 512U)) : pages != 0U)
                {
                    return false;
                }
                total += pages;
            }
            return (total == w[1U]) &&
                   SessionImageFits(0U, static_cast<Foundation::UInt32>(w[4U]),
                                    static_cast<Foundation::UInt32>(w[4U] >> 32U),
                                    static_cast<Foundation::UInt32>(w[5U]), total * SessionPageBytes) &&
                   ((w[7U] & 0xFFFF'FFFFU) + (w[4U] & 0xFFFF'FFFFU) <= SessionMaximumOutputExtent) &&
                   ((w[7U] >> 32U) + (w[4U] >> 32U) <= SessionMaximumOutputExtent);
        }
        case FacePresent:
            if ((attachments != 1U) || (w[1U] == 0U) || (w[2U] < 1U) || (w[2U] > 16U) || (w[3U] != 0U))
            {
                return false;
            }
            for (Size i{0U}; i < 16U; ++i)
            {
                const auto r = UnpackSessionDamage(w[4U + i]);
                if (i >= w[2U])
                {
                    if (w[4U + i] != 0U)
                    {
                        return false;
                    }
                }
                else if ((r.Width == 0U) || (r.Height == 0U) ||
                         (static_cast<UInt64>(r.X) + r.Width > SessionMaximumOutputExtent) ||
                         (static_cast<UInt64>(r.Y) + r.Height > SessionMaximumOutputExtent))
                {
                    return false;
                }
            }
            return true;
        case FaceSnapshots:
            if ((attachments != 0U) || (w[1U] < 1U) || (w[1U] > 7U) || !Wire::Zero(w, 2U, 4U))
            {
                return false;
            }
            for (Size i{0U}; i < 7U; ++i)
            {
                const Size first = 4U + 4U * i;
                if (i >= w[1U])
                {
                    if (!Wire::Zero(w, first, first + 4U))
                    {
                        return false;
                    }
                }
                else if ((w[first] == 0U) || !Wire::Pair(w[first + 1U], 0U, SessionMaximumOutputExtent - 1U) ||
                         ((w[first + 2U] & 0xFFFF'FFFFU) > 7U))
                {
                    return false;
                }
            }
            return true;
        default:
            return false;
        }
    }
};
[[nodiscard]] Foundation::Size WriteSessionFaceFrame(Foundation::UInt8* frame, Foundation::UInt32 method,
                                                     Foundation::UInt64 txn, const Foundation::UInt64* words,
                                                     const System::CapabilityHandle* handles,
                                                     Foundation::Size attachments) noexcept;
[[nodiscard]] bool ReadSessionFaceFrame(const Foundation::UInt8* delivery, Foundation::Size bytes,
                                        SessionFaceMessage& message) noexcept;
} // Namespace Asteria::User::Session
#endif
