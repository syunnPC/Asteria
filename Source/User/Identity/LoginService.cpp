// SPDX-License-Identifier: Apache-2.0

#include "User/Identity/LoginService.hpp"
#include "User/Identity/LoginRenderer.hpp"
#include "User/Component/InputFrame.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "User/Input/Keymap.hpp"
#include "User/Text/Utf8.hpp"
#include "User/System/DeviceAccess.hpp"
namespace
{
struct LoginPlacement
{
    void* Address;
};
} // Namespace
[[nodiscard]] inline void* operator new(Asteria::Foundation::Size, LoginPlacement place) noexcept
{
    return place.Address;
}
namespace Asteria::User::Identity
{
namespace
{
using System::CapabilityHandle;
class LoginService final
{
public:
    [[nodiscard]] bool Start(const System::SystemCallClient&, const UInt64*, UInt64) noexcept;
    [[nodiscard]] bool Step() noexcept;
    void Clear() noexcept
    {
        Wipe();
    }

private:
    [[nodiscard]] bool Allocate(UInt64, UInt64, CapabilityHandle&) noexcept;
    [[nodiscard]] bool Send(CapabilityHandle, SeatFace, SeatMethod, CapabilityHandle = {}) noexcept;
    [[nodiscard]] bool Answers() noexcept;
    [[nodiscard]] bool Control() noexcept;
    [[nodiscard]] bool Key(const Input::InputEvent&) noexcept;
    [[nodiscard]] bool Probe() noexcept;
    void Words() noexcept
    {
        for (auto& w : m_words)
        {
            w = 0U;
        }
    }
    void Wipe() noexcept;
    void Close(CapabilityHandle h) noexcept
    {
        if (h.Selector != 0U)
        {
            static_cast<void>(m_client->CloseCapability(h));
        }
    }
    const System::SystemCallClient* m_client{};
    CapabilityHandle m_domain{};
    CapabilityHandle m_space{};
    CapabilityHandle m_wait{};
    CapabilityHandle m_receive{};
    CapabilityHandle m_root{};
    CapabilityHandle m_self{};
    CapabilityHandle m_auth{};
    CapabilityHandle m_draw{};
    CapabilityHandle m_input{};
    CapabilityHandle m_pixels{};
    CapabilityHandle m_passwordFrame{};
    CapabilityHandle m_testConsole{};
    UInt64 m_tag{};
    UInt64 m_base{};
    UInt64 m_rootSender{};
    UInt64 m_authSender{};
    UInt64 m_drawSender{};
    UInt64 m_inputSender{};
    UInt64 m_inputEpoch{};
    UInt64 m_epoch{};
    UInt64 m_authTransaction{};
    UInt64 m_attempt{};
    UInt64 m_transaction{};
    UInt64 m_serial{};
    UInt64 m_presentedEpoch{};
    UInt64 m_retrySeconds{};
    UInt64 m_words[SeatMaximumWords]{};
    ScreenState m_state{};
    EditState m_edit{};
    Name m_name{};
    SecretEntry m_secret{};
    Input::KeyboardLayout m_layouts[Component::InputMaximumSources]{};
    Input::KeyboardLayout m_layout{Input::KeyboardLayout::Us};
    Size m_mask{};
    bool m_passwordFocus{false};
    bool m_dirty{true};
    bool m_credit{false};
    bool m_drawing{false};
    bool m_submitting{false};
    bool m_copied{false};
    bool m_presented{false};
    Size m_probePhase{0U};
    Size m_probeBytes{0U};
    alignas(8) UInt8 m_probeFrame[Component::EchoMaximumFrameBytes]{};
    static constexpr Size ReplyCapacity{4U};
    CapabilityHandle m_targets[ReplyCapacity]{};
    Size m_sizes[ReplyCapacity]{};
    Size m_first{};
    Size m_count{};
    bool m_close[ReplyCapacity]{};
    UInt8* m_password{};
    Graphics::MemorySurface m_surface{};
    SeatMessage m_message{};
    Component::InputMessage m_inputMessage{};
    alignas(8) UInt8 m_frame[SeatFrameBytes]{}, m_pending[ReplyCapacity][SeatFrameBytes]{},
        m_delivery[Component::EchoMaximumFrameBytes + Component::EchoDeliveryHeaderBytes]{};
};
alignas(LoginService) UInt8 g_loginStorage[sizeof(LoginService)]{};
LoginService* g_login{};
void LoginService::Wipe() noexcept
{
    m_secret.Clear();
    if (m_password != nullptr)
    {
        Crypto::Wipe({m_password, SeatPageBytes});
    }
    m_mask = 0U;
    m_submitting = false;
    m_copied = false;
}
bool LoginService::Allocate(UInt64 address, UInt64 pages, CapabilityHandle& frame) noexcept
{
    const auto memory = System::CreateMemory(*m_client, m_domain, Abi::ObjectTypeMemoryObject, pages);
    const auto created = System::CreateRamFrame(*m_client, m_domain, pages, 0U, 0U);
    if (memory.Error != Abi::SystemCallError::None || created.Error != Abi::SystemCallError::None)
    {
        return false;
    }
    frame = {created.Value0, created.Value1};
    alignas(8) UInt8 request[Abi::FaultResolveRequestSizeBytes]{};
    System::StoreHandle(request + Abi::FaultResolveAddressSpaceOffset, m_space);
    System::StoreHandle(request + Abi::FaultResolveMemoryObjectOffset, {memory.Value0, memory.Value1});
    System::StoreHandle(request + Abi::FaultResolveFrameOffset, frame);
    System::StoreWord(request + Abi::FaultResolveVirtualStartOffset, address);
    System::StoreWord(request + Abi::FaultResolvePageCountOffset, pages);
    request[Abi::FaultResolvePermissionsOffset] = System::ReadWriteUser;
    return m_client->MapMemory(System::AddressOf(request)).Error == Abi::SystemCallError::None;
}
bool LoginService::Send(CapabilityHandle target, SeatFace face, SeatMethod method, CapabilityHandle attachment) noexcept
{
    if (m_count == ReplyCapacity)
    {
        return false;
    }
    const auto size = WriteSeatFrame(m_frame, face, method, ++m_transaction, m_words, attachment);
    if (size == 0U || target.Selector == 0U)
    {
        return false;
    }
    const Size at = (m_first + m_count) % ReplyCapacity;
    for (Size i{0U}; i < size; ++i)
    {
        m_pending[at][i] = m_frame[i];
    }
    m_targets[at] = target;
    m_sizes[at] = size;
    m_close[at] = false;
    ++m_count;
    return true;
}
bool LoginService::Answers() noexcept
{
    if (m_count == 0U)
    {
        return true;
    }
    const Size at = m_first;
    const auto error = m_client->Send(m_targets[at], System::AddressOf(m_pending[at]), m_sizes[at]).Error;
    if (error == Abi::SystemCallError::WouldBlock)
    {
        return true;
    }
    if (m_close[at])
    {
        Close(m_targets[at]);
    }
    m_targets[at] = {};
    m_sizes[at] = 0U;
    m_close[at] = false;
    m_first = (at + 1U) % ReplyCapacity;
    --m_count;
    return error == Abi::SystemCallError::None;
}
[[gnu::noinline]] bool LoginService::Start(const System::SystemCallClient& client, const UInt64* b,
                                           UInt64 region) noexcept
{
    m_client = &client;
    m_base = region;
    m_tag = b[Abi::ComponentIncarnationWord];
    m_domain = {b[Abi::ComponentAccountWord], b[Abi::ComponentAccountWord + 1U]};
    m_space = {b[Abi::ComponentAccountSpaceWord], b[Abi::ComponentAccountSpaceWord + 1U]};
    m_wait = {b[Abi::ComponentWaitSetWord], b[Abi::ComponentWaitSetWord + 1U]};
    m_receive = {b[Abi::ComponentReceiveWord], b[Abi::ComponentReceiveWord + 1U]};
    m_root = {b[Abi::ComponentReportWord], b[Abi::ComponentReportWord + 1U]};
    m_self = {b[Abi::ComponentReplyWord], b[Abi::ComponentReplyWord + 1U]};
    if (!Allocate(region + LoginPixelsAddress, LoginPixelsPages, m_pixels) ||
        !Allocate(region + LoginPasswordAddress, 1U, m_passwordFrame))
    {
        return false;
    }
    m_password = reinterpret_cast<UInt8*>(static_cast<Foundation::UIntPointer>(region + LoginPasswordAddress));
    Wipe();
    m_surface = {.Pixels = reinterpret_cast<UInt32*>(static_cast<Foundation::UIntPointer>(region + LoginPixelsAddress)),
                 .Width = LoginWidth,
                 .Height = LoginHeight,
                 .Stride = LoginWidth,
                 .Format = static_cast<Graphics::PixelFormat>(b[Abi::ComponentMountFlagsWord])};
    if (!Graphics::IsUsable(m_surface))
    {
        return false;
    }
    Words();
    m_words[0U] = m_tag;
    m_words[1U] = LoginRuntimePages;
    return Send(m_root, SeatFace::LoginControl, SeatMethod::Ready);
}
[[gnu::noinline]] bool LoginService::Control() noexcept
{
    auto* w = m_message.Words;
    if (m_message.Face == SeatFace::AuthSubmit && m_message.Method == SeatMethod::Copied &&
        m_message.Sender == m_authSender && w[0U] == m_epoch && w[1U] == m_authTransaction && w[2U] == m_attempt &&
        m_submitting)
    {
        Crypto::Wipe({m_password, SeatPageBytes});
        m_copied = true;
        Words();
        m_words[0U] = m_epoch;
        m_words[1U] = m_authTransaction;
        m_words[2U] = m_attempt;
        return Send(m_root, SeatFace::LoginControl, SeatMethod::Copied);
    }
    if (m_message.Face == SeatFace::LoginControl && m_message.Method == SeatMethod::Reply &&
        m_message.Sender == m_drawSender && w[0U] == m_epoch && w[2U] == m_serial)
    {
        m_drawing = false;
        if (m_presentedEpoch != m_epoch)
        {
            m_presented = true;
            m_presentedEpoch = m_epoch;
            Words();
            m_words[0U] = m_tag;
            m_words[1U] = LoginRuntimePages;
            m_words[2U] = m_epoch;
            m_words[3U] = m_serial;
            return Send(m_root, SeatFace::LoginControl, SeatMethod::Ready);
        }
        return true;
    }
    if (m_message.Face != SeatFace::LoginControl || m_message.Selector != LoginControlSelector ||
        (m_message.Rights & Component::EchoManageRight) == 0U || m_message.Sender == 0U ||
        (m_rootSender != 0U && m_message.Sender != m_rootSender))
    {
        Close(m_message.Attachment);
        return true;
    }
    m_rootSender = m_message.Sender;
    if (m_message.Method == SeatMethod::Query)
    {
        if (m_count == ReplyCapacity)
        {
            Close(m_message.Attachment);
            return true;
        }
        Words();
        m_words[0U] = m_tag;
        m_words[1U] = static_cast<UInt64>(Status::Ok);
        // Query replies must retain the caller's transaction, including on WouldBlock.
        const auto size =
            WriteSeatFrame(m_frame, SeatFace::LoginControl, SeatMethod::Reply, m_message.Transaction, m_words);
        const Size at = (m_first + m_count) % ReplyCapacity;
        for (Size i{0U}; i < size; ++i)
        {
            m_pending[at][i] = m_frame[i];
        }
        m_targets[at] = m_message.Attachment;
        m_sizes[at] = size;
        m_close[at] = true;
        ++m_count;
        return true;
    }
    if (m_message.Method == SeatMethod::Configure)
    {
        if (w[2U] == 1U)
        {
            Close(m_auth);
            m_auth = m_message.Attachment;
            m_authSender = w[1U];
        }
        else if (w[2U] == 2U)
        {
            Close(m_draw);
            m_draw = m_message.Attachment;
            m_drawSender = w[1U];
        }
        else if (w[2U] == 3U)
        {
            Close(m_input);
            m_input = m_message.Attachment;
            m_inputSender = w[1U];
            m_inputEpoch = w[3U];
        }
        else if (w[2U] == 4U)
        {
            Close(m_testConsole);
            m_testConsole = m_message.Attachment;
        }
        else
        {
            Close(m_message.Attachment);
        }
        return true;
    }
    if (m_message.Method != SeatMethod::Screen || w[0U] < m_epoch)
    {
        Close(m_message.Attachment);
        return true;
    }
    if (w[0U] != m_epoch)
    {
        Wipe();
        m_drawing = false;
        m_epoch = w[0U];
        m_serial = 0U;
        m_passwordFocus = false;
    }
    m_state = static_cast<ScreenState>(w[1U]);
    m_edit = static_cast<EditState>(w[2U]);
    m_authTransaction = w[3U];
    m_retrySeconds = w[7U];
    if (m_state == ScreenState::Locked)
    {
        m_name.Length = static_cast<UInt8>(w[9U]);
        for (Size i{0U}; i < m_name.Length; ++i)
        {
            m_name.Bytes[i] = static_cast<UInt8>(w[10U + i / 8U] >> (8U * (i % 8U)));
        }
        m_passwordFocus = true;
    }
    if (m_state != ScreenState::Login && m_state != ScreenState::Locked)
    {
        Wipe();
    }
    if (m_state == ScreenState::AwaitingAttention)
    {
        m_name = {};
        m_passwordFocus = false;
    }
    m_dirty = true;
    return true;
}
[[gnu::noinline]] bool LoginService::Key(const Input::InputEvent& event) noexcept
{
    if (event.Kind != Input::EventKind::KeyDown || (m_state != ScreenState::Login && m_state != ScreenState::Locked))
    {
        return true;
    }
    if (m_testConsole.Selector != 0U && event.Usage >= 66U && event.Usage <= 68U)
    {
        Words();
        m_words[0U] = m_epoch;
        m_words[1U] = event.Usage - 63U;
        Wipe();
        return Send(m_root, SeatFace::LoginControl, SeatMethod::Action);
    }
    if (event.Usage == 41U || (event.Usage == 69U && m_state == ScreenState::Locked))
    {
        Words();
        m_words[0U] = m_epoch;
        m_words[1U] = event.Usage == 41U ? 1U : 2U;
        Wipe();
        m_dirty = true;
        return Send(m_root, SeatFace::LoginControl, SeatMethod::Action);
    }
    if (m_edit != EditState::Editing || m_authTransaction == 0U || m_submitting || m_presentedEpoch != m_epoch)
    {
        return true;
    }
    if (event.Source == 0U || event.Source > Component::InputMaximumSources)
    {
        return true;
    }
    auto& layout = m_layouts[event.Source - 1U];
    if (event.Usage == 59U)
    {
        // Each keyboard retains its layout. Changing legends clears an incomplete password.
        layout = layout == Input::KeyboardLayout::Us ? Input::KeyboardLayout::Jis : Input::KeyboardLayout::Us;
        m_layout = layout;
        Wipe();
        m_dirty = true;
        return true;
    }
    if (m_layout != layout)
    {
        m_layout = layout;
        m_dirty = true;
    }
    if (event.Usage == 43U && m_state == ScreenState::Login)
    {
        m_passwordFocus = !m_passwordFocus;
        m_dirty = true;
        return true;
    }
    if (event.Usage == 42U)
    {
        if (m_passwordFocus)
        {
            m_secret.Backspace();
            m_mask = m_secret.Characters();
        }
        else if (m_name.Length != 0U)
        {
            Size first = m_name.Length - 1U;
            while (first != 0U && (m_name.Bytes[first] & 0xC0U) == 0x80U)
            {
                --first;
            }
            for (Size i = first; i < m_name.Length; ++i)
            {
                m_name.Bytes[i] = 0U;
            }
            m_name.Length = static_cast<UInt8>(first);
        }
        m_dirty = true;
        return true;
    }
    if (event.Usage == 40U)
    {
        if (!m_passwordFocus && m_state == ScreenState::Login)
        {
            m_passwordFocus = true;
            m_dirty = true;
            return true;
        }
        if (m_secret.View().Length == 0U || !FormatCodec::ValidName({m_name.Bytes, m_name.Length}) ||
            m_auth.Selector == 0U || m_count > ReplyCapacity - 2U)
        {
            return true;
        }
        ++m_attempt;
        Words();
        m_words[0U] = m_epoch;
        m_words[1U] = 0U;
        m_words[2U] = m_authTransaction;
        m_words[3U] = m_attempt;
        m_words[4U] = m_name.Length;
        for (Size i{0U}; i < m_name.Length; ++i)
        {
            m_words[5U + i / 8U] |= static_cast<UInt64>(m_name.Bytes[i]) << (8U * (i % 8U));
        }
        if (!Send(m_root, SeatFace::LoginControl, SeatMethod::Action))
        {
            return false;
        }
        const auto secret = m_secret.View();
        for (Size i{0U}; i < secret.Length; ++i)
        {
            m_password[i] = secret.Bytes[i];
        }
        Words();
        m_words[0U] = m_epoch;
        m_words[1U] = m_authTransaction;
        m_words[2U] = m_attempt;
        m_words[3U] = m_name.Length;
        m_words[4U] = secret.Length;
        for (Size i{0U}; i < m_name.Length; ++i)
        {
            m_words[5U + i / 8U] |= static_cast<UInt64>(m_name.Bytes[i]) << (8U * (i % 8U));
        }
        m_secret.Clear();
        m_submitting = true;
        m_edit = EditState::Verifying;
        m_dirty = true;
        return Send(m_auth, SeatFace::AuthSubmit, SeatMethod::Submit, m_passwordFrame);
    }
    const auto c = Input::LayoutKeyCharacter(layout, event.Usage, event.Modifiers);
    if (!c.Present || !Text::IsPrintable(c.CodePoint) ||
        (event.Modifiers & (Input::KeymapAlt | Input::KeymapGui)) != 0U)
    {
        return true;
    }
    UInt8 encoded[4U]{};
    const Size bytes = Text::EncodeUtf8(c.CodePoint, encoded);
    if (m_passwordFocus)
    {
        static_cast<void>(m_secret.Append({encoded, bytes}));
        m_mask = m_secret.Characters();
    }
    else if (bytes <= MaximumNameBytes - m_name.Length)
    {
        for (Size i = 0U; i < bytes; ++i)
        {
            m_name.Bytes[m_name.Length++] = encoded[i];
        }
    }
    Crypto::Wipe({encoded, sizeof(encoded)});
    m_dirty = true;
    return true;
}
[[gnu::noinline]] bool LoginService::Probe() noexcept
{
    // Explicit synthetic receipt only: exercise the installed Invoke faces
    // with forbidden focus/visibility and an unissued draw epoch. No secret.
    if (m_testConsole.Selector == 0U || !m_presented || m_probePhase >= 4U)
    {
        return true;
    }
    if (m_probePhase == 0U || m_probePhase == 1U)
    {
        Words();
        m_words[0U] = m_probePhase == 0U ? m_epoch : m_epoch + 1U;
        if (m_probePhase == 1U)
        {
            m_words[1U] = 1U;
            m_words[2U] = static_cast<UInt64>(m_surface.Format);
        }
        if (!Send(m_draw, SeatFace::TrustedDraw, m_probePhase == 0U ? SeatMethod::Hide : SeatMethod::Draw,
                  m_probePhase == 0U ? CapabilityHandle{} : m_pixels))
        {
            return false;
        }
        ++m_probePhase;
        return true;
    }
    if (m_probeBytes == 0U)
    {
        if (m_probePhase == 2U)
        {
            constexpr UInt8 publicText[]{'s', 'p', 'o', 'o', 'f'};
            m_probeBytes =
                Component::WriteDisplayConsoleWrite(m_probeFrame, ++m_transaction, publicText, sizeof(publicText));
        }
        else
        {
            const UInt64 words[4U]{m_inputEpoch, 1U, 0U, 0U};
            m_probeBytes = Component::WriteInputFrame(m_probeFrame, Component::InputSetFocusMethod, ++m_transaction,
                                                      words, nullptr, 0U, &m_self);
        }
    }
    const auto error =
        m_client->Send(m_probePhase == 2U ? m_testConsole : m_input, System::AddressOf(m_probeFrame), m_probeBytes)
            .Error;
    if (error == Abi::SystemCallError::WouldBlock)
    {
        return true;
    }
    if (error != Abi::SystemCallError::None)
    {
        return false;
    }
    m_probeBytes = 0U;
    ++m_probePhase;
    return true;
}
[[gnu::noinline]] bool LoginService::Step() noexcept
{
    if (!Answers())
    {
        Wipe();
        return false;
    }
    const auto received = m_client->Receive(m_receive, System::AddressOf(m_delivery), sizeof(m_delivery));
    if (received.Error == Abi::SystemCallError::None)
    {
        const auto bytes = static_cast<Size>(received.Value1);
        if (ReadSeatFrame(m_delivery, bytes, m_message))
        {
            if (!Control())
            {
                Wipe();
                return false;
            }
        }
        else if (Component::ReadInputFrame(m_delivery, bytes, m_inputMessage) &&
                 System::LoadWord(m_delivery + 24U) == m_inputSender &&
                 m_inputMessage.Method == Component::InputReplyMethod && m_probePhase == 4U)
        {
            m_probePhase = 5U;
            Words();
            m_words[0U] = m_tag;
            m_words[1U] = static_cast<UInt64>(Status::Ok);
            m_words[2U] = m_inputMessage.Words[1U];
            m_words[3U] = 3U;
            if (!Send(m_root, SeatFace::LoginControl, SeatMethod::Reply))
            {
                Wipe();
                return false;
            }
        }
        else if (Component::ReadInputFrame(m_delivery, bytes, m_inputMessage) &&
                 System::LoadWord(m_delivery + 24U) == m_inputSender &&
                 m_inputMessage.Method == Component::InputEventsMethod && m_inputMessage.Words[0U] == m_inputEpoch)
        {
            m_credit = false;
            for (Size i{0U}; i < m_inputMessage.Count; ++i)
            {
                if (!Key(Component::ReadInputEvent(m_inputMessage.Events + i * 24U)))
                {
                    Wipe();
                    return false;
                }
            }
        }
        else
        {
            DisposeSeatAttachments(*m_client, m_delivery, bytes);
        }
    }
    else if (received.Error != Abi::SystemCallError::WouldBlock)
    {
        Wipe();
        return false;
    }
    if (m_count != 0U)
    {
        return true;
    }
    if (!Probe())
    {
        Wipe();
        return false;
    }
    if (m_input.Selector != 0U && !m_credit)
    {
        const UInt64 words[4U]{m_inputEpoch, Component::InputMaximumEvents, 0U, 0U};
        const auto size = Component::WriteInputFrame(m_frame, Component::InputCreditMethod, ++m_transaction, words,
                                                     nullptr, 0U, &m_self);
        const auto error = m_client->Send(m_input, System::AddressOf(m_frame), size).Error;
        if (error == Abi::SystemCallError::None)
        {
            m_credit = true;
        }
        else if (error != Abi::SystemCallError::WouldBlock)
        {
            Wipe();
            return false;
        }
    }
    if (m_dirty && !m_drawing && m_draw.Selector != 0U && m_epoch != 0U)
    {
        RenderLogin(m_surface, {.State = m_state,
                                .Edit = m_edit,
                                .Account = {m_name.Bytes, m_name.Length},
                                .MaskCharacters = m_mask,
                                .PasswordFocus = m_passwordFocus,
                                .Jis = m_layout == Input::KeyboardLayout::Jis,
                                .RetrySeconds = m_retrySeconds});
        Words();
        m_words[0U] = m_epoch;
        m_words[1U] = ++m_serial;
        m_words[2U] = static_cast<UInt64>(m_surface.Format);
        if (!Send(m_draw, SeatFace::TrustedDraw, SeatMethod::Draw, m_pixels))
        {
            Wipe();
            return false;
        }
        m_dirty = false;
        m_drawing = true;
    }
    return true;
}
} // Namespace
UInt64 ServeLogin(const System::SystemCallClient& client, const UInt64 (&b)[Abi::ComponentBlockWords],
                  UInt64 region) noexcept
{
    if (!InitializeLogin(client, b, region))
    {
        ClearLogin();
        return 0xC0DE'00B6U;
    }
    for (;;)
    {
        if (!AdvanceLogin())
        {
            ClearLogin();
            return 0xC0DE'00E6U;
        }
        const auto waited = client.Wait({b[Abi::ComponentWaitSetWord], b[Abi::ComponentWaitSetWord + 1U]}, 1000U);
        if (waited.Error != Abi::SystemCallError::None && waited.Error != Abi::SystemCallError::TimedOut)
        {
            ClearLogin();
            return 0xC0DE'00E7U;
        }
    }
}
bool InitializeLogin(const System::SystemCallClient& client, const UInt64 (&block)[Abi::ComponentBlockWords],
                     UInt64 region) noexcept
{
    g_login = new (LoginPlacement{g_loginStorage}) LoginService{};
    return g_login->Start(client, block, region);
}
bool AdvanceLogin() noexcept
{
    return g_login != nullptr && g_login->Step();
}
void ClearLogin() noexcept
{
    if (g_login != nullptr)
    {
        g_login->Clear();
    }
}
} // Namespace Asteria::User::Identity
