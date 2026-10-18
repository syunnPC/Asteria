// SPDX-License-Identifier: Apache-2.0

#include "User/Identity/SeatFrame.hpp"
namespace Asteria::User::Identity
{
namespace
{
[[nodiscard]] UInt64 Load(const UInt8* p, Size n = 8U) noexcept
{
    UInt64 value{0U};
    for (Size i{0U}; i < n; ++i)
    {
        value |= static_cast<UInt64>(p[i]) << (8U * i);
    }
    return value;
}
void Store(UInt8* p, UInt64 value, Size n = 8U) noexcept
{
    for (Size i{0U}; i < n; ++i)
    {
        p[i] = static_cast<UInt8>(value >> (8U * i));
    }
}
struct ByteWords
{
    const UInt8* Bytes;
    [[nodiscard]] UInt64 operator[](Size i) const noexcept
    {
        return Load(Bytes + 8U * i);
    }
};
template <class W> [[nodiscard]] bool Valid(SeatFace face, SeatMethod method, const W& w, bool attached) noexcept
{
    const bool control = face == SeatFace::AuthControl || face == SeatFace::LoginControl;
    if (static_cast<UInt8>(face) > static_cast<UInt8>(SeatFace::TrustedDraw))
    {
        return false;
    }
    bool shape{false};
    Size used{0U};
    switch (method)
    {
    case SeatMethod::Begin:
        shape = face == SeatFace::AuthControl && w[0U] != 0U && w[1U] != 0U && w[2U] <= 1U && w[4U] != 0U;
        used = 5U;
        break;
    case SeatMethod::Submit:
        shape = face == SeatFace::AuthSubmit && w[0U] != 0U && w[1U] != 0U && w[2U] != 0U && w[3U] >= 1U &&
                w[3U] <= MaximumNameBytes && w[4U] >= 1U && w[4U] <= MaximumPasswordBytes;
        used = 13U;
        for (Size i{0U}; i < 64U; ++i)
        {
            const UInt8 b = static_cast<UInt8>(w[5U + i / 8U] >> (8U * (i % 8U)));
            if (i >= w[3U] && b != 0U)
            {
                shape = false;
            }
        }
        break;
    case SeatMethod::Cancel:
        shape = (control || face == SeatFace::AuthSubmit) && w[0U] != 0U && w[1U] != 0U;
        used = 2U;
        break;
    case SeatMethod::Result:
        shape = face == SeatFace::AuthControl && w[0U] != 0U && w[1U] != 0U && w[2U] != 0U && w[3U] <= 1U &&
                w[4U] <= static_cast<UInt64>(Status::Cancelled);
        used = 32U;
        break;
    case SeatMethod::Copied:
        shape = (face == SeatFace::AuthSubmit || face == SeatFace::LoginControl) && w[0U] != 0U && w[1U] != 0U &&
                w[2U] != 0U;
        used = 4U;
        break;
    case SeatMethod::Ready:
    case SeatMethod::Reply:
        shape = control && w[0U] != 0U;
        used = 4U;
        break;
    case SeatMethod::Query:
        shape = control && w[0U] != 0U;
        used = 1U;
        break;
    case SeatMethod::Configure:
        shape = control && w[0U] != 0U && w[1U] != 0U;
        used = 4U;
        break;
    case SeatMethod::Screen:
        shape = face == SeatFace::LoginControl && w[0U] != 0U && w[1U] <= static_cast<UInt64>(ScreenState::Recovery) &&
                w[2U] <= static_cast<UInt64>(EditState::RetryAt) && w[9U] <= MaximumNameBytes;
        used = 18U;
        break;
    case SeatMethod::Action:
        shape = face == SeatFace::LoginControl && w[0U] != 0U && w[1U] <= 5U && w[4U] <= MaximumNameBytes;
        used = 13U;
        break;
    case SeatMethod::Enroll:
        shape = face == SeatFace::TrustedDraw && w[0U] != 0U && w[1U] != 0U;
        used = 2U;
        break;
    case SeatMethod::Draw:
        shape = face == SeatFace::TrustedDraw && w[0U] != 0U && w[1U] != 0U && w[2U] != 0U;
        used = 3U;
        break;
    case SeatMethod::Show:
    case SeatMethod::Hide:
        shape = face == SeatFace::TrustedDraw && w[0U] != 0U;
        used = 1U;
        break;
    case SeatMethod::CheckRevision:
        shape = face == SeatFace::AuthControl && w[0U] != 0U && w[1U] != 0U;
        used = 2U;
        break;
    case SeatMethod::OpenSubmit:
        shape = face == SeatFace::AuthControl && w[0U] != 0U;
        used = 1U;
        break;
    case SeatMethod::OpenDraw:
    case SeatMethod::DrawOpened:
        shape = face == SeatFace::TrustedDraw && w[0U] != 0U;
        used = 1U;
        break;
    default:
        return false;
    }
    const bool wants = method == SeatMethod::Submit || method == SeatMethod::Query || method == SeatMethod::Draw ||
                       method == SeatMethod::Configure || method == SeatMethod::OpenSubmit ||
                       method == SeatMethod::Enroll || method == SeatMethod::OpenDraw ||
                       method == SeatMethod::DrawOpened;
    if (!shape || attached != wants)
    {
        return false;
    }
    for (Size i{used}; i < SeatMaximumWords; ++i)
    {
        if (w[i] != 0U)
        {
            return false;
        }
    }
    return true;
}
[[nodiscard]] Component::EchoAttachment Form(SeatMethod method, System::CapabilityHandle handle) noexcept
{
    const bool endpoint = method == SeatMethod::Query || method == SeatMethod::Configure ||
                          method == SeatMethod::OpenSubmit || method == SeatMethod::Enroll ||
                          method == SeatMethod::OpenDraw || method == SeatMethod::DrawOpened;
    return {handle,
            static_cast<UInt16>(endpoint ? (method == SeatMethod::DrawOpened   ? (Component::EchoManageRight | 0x0102U)
                                            : method == SeatMethod::OpenSubmit ? 0x0102U
                                                                               : Component::EchoInvokeRight)
                                         : 0x0010U),
            Component::EchoCopy, endpoint ? Component::EchoEndpointType : Component::EchoFrameType};
}
} // namespace
Size WriteSeatFrame(UInt8* frame, SeatFace face, SeatMethod method, UInt64 transaction, const UInt64* words,
                    System::CapabilityHandle attachment) noexcept
{
    const bool attached = attachment.Selector != 0U;
    if (frame == nullptr || words == nullptr || transaction == 0U || !Valid(face, method, words, attached))
    {
        return 0U;
    }
    const auto form = Form(method, attachment);
    Component::WriteEchoFrame(frame, &form, attached ? 1U : 0U);
    const Size total = Component::EchoHeaderBytes + (attached ? Component::EchoAttachmentBytes : 0U) + SeatPayloadBytes;
    Store(frame + 8U, total, 4U);
    Store(frame + 12U, SeatPayloadBytes, 2U);
    Store(frame + 14U, SeatPayloadBytes, 2U);
    Store(frame + 16U, SeatInterfaceLow + static_cast<UInt8>(face));
    Store(frame + 24U, SeatInterfaceHigh);
    Store(frame + 32U, static_cast<UInt32>(method), 4U);
    Store(frame + 40U, transaction);
    auto* p = frame + Component::EchoHeaderBytes + (attached ? Component::EchoAttachmentBytes : 0U);
    for (Size i{0U}; i < SeatMaximumWords; ++i)
    {
        Store(p + 8U * i, words[i]);
    }
    return total;
}
bool ReadSeatFrame(const UInt8* delivery, Size bytes, SeatMessage& out) noexcept
{
    if (delivery == nullptr ||
        bytes < Component::EchoDeliveryHeaderBytes + Component::EchoHeaderBytes + SeatPayloadBytes)
    {
        return false;
    }
    const auto* f = delivery + Component::EchoDeliveryHeaderBytes;
    const auto low = Load(f + 16U);
    const Size attached = f[81U];
    const Size total = Component::EchoHeaderBytes + attached * Component::EchoAttachmentBytes + SeatPayloadBytes;
    if (low < SeatInterfaceLow || low > SeatInterfaceLow + 3U || Load(f + 24U) != SeatInterfaceHigh || attached > 1U ||
        bytes != Component::EchoDeliveryHeaderBytes + total || Load(f + 8U, 4U) != total ||
        Load(f + 12U, 2U) != SeatPayloadBytes || Load(f + 14U, 2U) != SeatPayloadBytes || Load(f + 40U) == 0U)
    {
        return false;
    }
    for (Size i{0U}; i < Component::EchoHeaderBytes; ++i)
    {
        if ((i >= 8U && i < 36U) || (i >= 40U && i < 48U) || i == 81U)
        {
            continue;
        }
        if (f[i] != Abi::CanonicalAsynchronousFrame[i])
        {
            return false;
        }
    }
    const auto face = static_cast<SeatFace>(low - SeatInterfaceLow);
    const auto method = static_cast<SeatMethod>(Load(f + 32U, 4U));
    if (attached != 0U)
    {
        const auto* a = f + Component::EchoHeaderBytes;
        const auto form = Form(method, {});
        if (Load(a) == 0U || Load(a + 16U) != MaximumCounter || Load(a + 24U, 2U) != form.Rights ||
            a[26U] != form.Mode || a[27U] != form.Type || Load(a + 28U, 4U) != 0U)
        {
            return false;
        }
    }
    const auto* p = f + Component::EchoHeaderBytes + attached * Component::EchoAttachmentBytes;
    // Validate directly through the fixed wire view before publishing output.
    const UInt64 start = reinterpret_cast<UInt64>(delivery);
    const UInt64 target = reinterpret_cast<UInt64>(&out);
    if (target >= start ? target - start < bytes : start - target < sizeof(out))
    {
        return false;
    }
    if (!Valid(face, method, ByteWords{p}, attached != 0U))
    {
        return false;
    }
    out.Face = face;
    out.Method = method;
    out.Transaction = Load(f + 40U);
    out.Sender = Load(delivery + 24U);
    out.Selector = Load(delivery + 32U);
    out.Rights = static_cast<UInt16>(Load(delivery + 14U, 2U));
    for (Size i{0U}; i < SeatMaximumWords; ++i)
    {
        out.Words[i] = Load(p + 8U * i);
    }
    out.Attachment = attached != 0U ? Component::ReadEchoHandle(delivery, 0U) : System::CapabilityHandle{};
    return true;
}
bool ScreenEnrollment::Enroll(UInt64 incarnation, UInt64 epoch) noexcept
{
    if (incarnation == 0U || epoch == 0U || epoch <= m_epoch)
    {
        return false;
    }
    m_incarnation = incarnation;
    m_epoch = epoch;
    return true;
}
void DisposeSeatAttachments(const System::SystemCallClient& client, const UInt8* delivery, Size bytes) noexcept
{
    if (delivery == nullptr || bytes < Component::EchoDeliveryHeaderBytes + Component::EchoHeaderBytes)
    {
        return;
    }
    const Size count = Component::GetEchoAttachmentCount(delivery);
    for (Size i{0U}; i < count && i < (Component::EchoMaximumFrameBytes - Component::EchoHeaderBytes) /
                                          Component::EchoAttachmentBytes;
         ++i)
    {
        const Size end =
            Component::EchoDeliveryHeaderBytes + Component::EchoHeaderBytes + (i + 1U) * Component::EchoAttachmentBytes;
        if (bytes < end)
        {
            return;
        }
        const auto handle = Component::ReadEchoHandle(delivery, i);
        if (handle.Selector != 0U)
        {
            static_cast<void>(client.CloseCapability(handle));
        }
    }
}
} // namespace Asteria::User::Identity
