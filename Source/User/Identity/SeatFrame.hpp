// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_IDENTITY_SEAT_FRAME_HPP_INCLUDED
#define ASTERIA_USER_IDENTITY_SEAT_FRAME_HPP_INCLUDED
#include "User/Identity/TrustedScreen.hpp"
#include "User/Component/EchoFrame.hpp"
namespace Asteria::User::Identity
{
// Four version-1 faces, no credential payload and no client-selected forwarding Endpoint. Submit's
// only attachment is a Map-only password Frame. Query alone attaches a root-owned reply Endpoint.
enum class SeatFace : UInt8
{
    AuthControl,
    AuthSubmit,
    LoginControl,
    TrustedDraw
};
enum class SeatMethod : UInt32
{
    Begin = 1U,
    Submit,
    Cancel,
    Result,
    Copied,
    Ready,
    Query,
    Reply,
    Configure,
    Screen,
    Action,
    Enroll,
    Draw,
    Show,
    Hide,
    CheckRevision,
    OpenSubmit,
    OpenDraw,
    DrawOpened
};
inline constexpr UInt64 SeatInterfaceHigh{0x4153'5449'4430'3031ULL};
inline constexpr UInt64 SeatInterfaceLow{0x5345'4154'0000'0000ULL};
inline constexpr Size SeatMaximumWords{32U};
inline constexpr Size SeatPayloadBytes{SeatMaximumWords * 8U};
inline constexpr Size SeatFrameBytes{Component::EchoHeaderBytes + Component::EchoAttachmentBytes + SeatPayloadBytes};
inline constexpr Size SeatDeliveryBytes{Component::EchoDeliveryHeaderBytes + SeatFrameBytes};
inline constexpr UInt64 AuthControlSelector{SeatInterfaceLow};
inline constexpr UInt64 AuthSubmitSelector{SeatInterfaceLow + 1U};
inline constexpr UInt64 LoginControlSelector{SeatInterfaceLow + 2U};
inline constexpr UInt64 TrustedDrawSelector{SeatInterfaceLow + 3U};
static_assert(SeatPayloadBytes == 256U && SeatFrameBytes <= Component::EchoMaximumFrameBytes);
struct SeatMessage
{
    SeatFace Face{};
    SeatMethod Method{};
    UInt64 Transaction{0U};
    UInt64 Sender{0U};
    UInt64 Selector{0U};
    UInt16 Rights{0U};
    UInt64 Words[SeatMaximumWords]{};
    System::CapabilityHandle Attachment{};
};
[[nodiscard]] Size WriteSeatFrame(UInt8* frame, SeatFace face, SeatMethod method, UInt64 transaction,
                                  const UInt64* words, System::CapabilityHandle attachment = {}) noexcept;
// Failure-atomic, disjoint output. Rejects malformed metadata before the
// service may inspect a password attachment, never reads a password here.
[[nodiscard]] bool ReadSeatFrame(const UInt8* delivery, Size bytes, SeatMessage& output) noexcept;
void DisposeSeatAttachments(const System::SystemCallClient&, const UInt8*, Size) noexcept;

class ScreenEnrollment final
{
public:
    [[nodiscard]] bool Enroll(UInt64 incarnation, UInt64 epoch) noexcept;
    [[nodiscard]] bool Accepts(UInt64 incarnation, UInt64 epoch) const noexcept
    {
        return m_incarnation != 0U && incarnation == m_incarnation && epoch == m_epoch;
    }
    void Retire() noexcept
    {
        m_incarnation = 0U;
    }
    [[nodiscard]] UInt64 Epoch() const noexcept
    {
        return m_epoch;
    }

private:
    UInt64 m_incarnation{0U};
    UInt64 m_epoch{0U};
};
} // Namespace Asteria::User::Identity
#endif
