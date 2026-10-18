// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/ProductLogin.hpp"
#include "User/Pager/ProductDesktop.hpp"
#include "User/Pager/PowerOff.hpp"
#include "User/Loader/ExecutableFormats.hpp"
#include "User/Session/SessionFaceFrame.hpp"
#include "User/Identity/SeatFrame.hpp"
#include "User/Identity/Budget.hpp"
#include "User/System/DeviceAccess.hpp"
#include "User/Identity/LoginRenderer.hpp"
#include "Inventory/ReportText.hpp"
namespace
{
struct SeatPlacement
{
    void* Address;
};
} // Namespace
[[nodiscard]] inline void* operator new(Asteria::Foundation::Size, SeatPlacement place) noexcept
{
    return place.Address;
}
namespace Asteria::User::Pager
{
namespace
{
namespace I = Identity;
namespace C = Component;
namespace S = Session;
using Foundation::UInt64;
using Foundation::UInt8;
using Foundation::Size;
using System::CapabilityHandle;
class ProductLogin final
{
public:
    void Start(ScenarioContext&, const ComponentRecord&) noexcept;
    [[nodiscard]] bool Step(Abi::SystemCallError&) noexcept;
    void Attention() noexcept;

private:
    [[nodiscard]] bool Send(CapabilityHandle, I::SeatFace, I::SeatMethod, CapabilityHandle = {}) noexcept;
    [[nodiscard]] bool Focus(UInt64, bool = true) noexcept;
    [[nodiscard]] bool Image(const char*, LoadedImage&) noexcept;
    [[nodiscard]] bool Notice() noexcept;
    [[nodiscard]] bool Screen() noexcept;
    [[nodiscard]] UInt64 Now() const noexcept;
    [[nodiscard]] bool Begin() noexcept;
    [[nodiscard]] bool OpenFace() noexcept;
    [[nodiscard]] bool Configure(CapabilityHandle, UInt64, UInt64, CapabilityHandle, UInt64 = 0U) noexcept;
    [[nodiscard]] bool Read(CapabilityHandle, I::SeatMessage&) noexcept;
    void Result(const I::SeatMessage&) noexcept;
    void Failure() noexcept;
    void RetireEnded() noexcept;
    [[nodiscard]] bool CoverDeadDisplay() noexcept;
    static bool Recover(void*, ComponentRecord&, Abi::SystemCallError&) noexcept;
    void Log(const char*) noexcept;
    void Words() noexcept
    {
        for (auto& w : m_words)
        {
            w = 0U;
        }
    }
    ScenarioContext* m_context{};
    const ComponentRecord* m_input{};
    ComponentRecord* m_auth{};
    ComponentRecord* m_login{};
    LoadedImage m_authImage{};
    LoadedImage m_loginImage{};
    I::TrustedScreen m_seat{};
    I::Name m_name{};
    CapabilityHandle m_authReport{};
    CapabilityHandle m_loginReport{};
    CapabilityHandle m_reply{};
    CapabilityHandle m_attention{};
    CapabilityHandle m_inputClient{};
    CapabilityHandle m_draw{};
    CapabilityHandle m_detach{};
    CapabilityHandle m_submit{};
    UInt64 m_authSender{};
    UInt64 m_loginSender{};
    UInt64 m_displaySender{};
    UInt64 m_inputSender{};
    UInt64 m_inputId{};
    UInt64 m_displayEpoch{};
    UInt64 m_fencedEpoch{};
    UInt64 m_noticed{};
    UInt64 m_beginEpoch{};
    UInt64 m_beginAt{};
    UInt64 m_transaction{};
    UInt64 m_session{1U};
    bool m_ready{false};
    bool m_attentionCredit{false};
    bool m_polling{false};
    bool m_failed{false};
    bool m_starting{false};
    bool m_sending{false};
    bool m_fixtureTest{false};
    bool m_resultPending{false};
    UInt64 m_copiedEpoch{};
    UInt64 m_copiedTransaction{};
    UInt64 m_copiedAttempt{};
    UInt64 m_revisionTransaction{};
    UInt64 m_revisionEpoch{};
    UInt64 m_revisionAttempt{};
    UInt64 m_retrySeconds{};
    UInt64 m_startStep{};
    bool m_revisionValidated{false};
    bool m_emergencyCovered{false};
    bool m_emergencyFailed{false};
    CapabilityHandle m_emergencyFrame{};
    CapabilityHandle m_emergencyMemory{};
    Abi::SystemCallError m_error{};
    UInt64 m_words[I::SeatMaximumWords]{};
    I::SeatMessage m_message{};
    I::SeatMessage m_result{};
    C::InputMessage m_inputMessage{};
    S::SessionFaceMessage m_faceMessage{};
    alignas(8) UInt8 m_frame[C::EchoMaximumFrameBytes]{}, m_frozen[C::EchoMaximumFrameBytes]{},
        m_delivery[PagerProgram::DeliveryCapacityBytes]{};
};
alignas(ProductLogin) UInt8 g_productLoginStorage[sizeof(ProductLogin)]{};
ProductLogin* g_productLogin{};
void ProductLogin::Log(const char* message) noexcept
{
    char line[192U]{};
    Inventory::ReportText text{line, sizeof(line)};
    text.Text("# login: ");
    text.Text(message);
    text.Character('\n');
    static_cast<void>(m_context->Client->DebugWrite(m_context->Diagnostic, System::AddressOf(line), text.Bytes()));
}
bool ProductLogin::Image(const char* name, LoadedImage& image) noexcept
{
    return FindComponentImage(m_context->Launch->ComponentImages, m_context->Launch->ComponentImagesBytes, name,
                              image.Bytes, image.Size) &&
           Loader::DescribeExecutable(image.Bytes, image.Size, m_context->Launch->Component.Image, image.Layout) ==
               Loader::LoaderError::None;
}
bool ProductLogin::Send(CapabilityHandle target, I::SeatFace face, I::SeatMethod method,
                        CapabilityHandle attachment) noexcept
{
    const auto size = I::WriteSeatFrame(m_frame, face, method, ++m_transaction, m_words, attachment);
    if (size == 0U)
    {
        return false;
    }
    // Root sends only public words. Poll callbacks cannot change a frozen
    // frame or reenter seat IPC while its bounded retries are pending.
    for (Size i{0U}; i < size; ++i)
    {
        m_frozen[i] = m_frame[i];
    }
    m_sending = true;
    for (Size i{0U}; i < 200U; ++i)
    {
        const auto error = m_context->Client->Send(target, System::AddressOf(m_frozen), size).Error;
        if (error != Abi::SystemCallError::WouldBlock)
        {
            m_sending = false;
            return error == Abi::SystemCallError::None;
        }
        if (m_context->Supervisor->Pause(10'000U, m_error) != PagerServeError::None)
        {
            m_sending = false;
            return false;
        }
    }
    m_sending = false;
    return false;
}
bool ProductLogin::Focus(UInt64 id, bool lock) noexcept
{
    const UInt64 words[4U]{m_input->Tag, id, lock ? 1U : 0U, 0U};
    const UInt64 transaction = ++m_transaction;
    const auto size = C::WriteInputFrame(m_frame, C::InputSetFocusMethod, transaction, words, nullptr, 0U, &m_reply);
    m_sending = true;
    if (m_context->Client->Send(m_input->Service, System::AddressOf(m_frame), size).Error != Abi::SystemCallError::None)
    {
        m_sending = false;
        return false;
    }
    const auto bytes = m_context->Supervisor->AwaitMessage(*m_input, m_reply, m_delivery, sizeof(m_delivery), m_error);
    m_sending = false;
    return C::ReadInputFrame(m_delivery, static_cast<Size>(bytes), m_inputMessage) &&
           m_inputMessage.Transaction == transaction &&
           m_inputMessage.Words[1U] == static_cast<UInt64>(C::InputAnswer::Done);
}
bool ProductLogin::Configure(CapabilityHandle target, UInt64 tag, UInt64 kind, CapabilityHandle endpoint,
                             UInt64 sender) noexcept
{
    Words();
    m_words[0U] = tag;
    m_words[1U] = sender;
    m_words[2U] = kind;
    if (kind == 3U)
    {
        m_words[3U] = m_input->Tag;
    }
    return Send(target, I::SeatFace::LoginControl, I::SeatMethod::Configure, endpoint);
}
bool ProductLogin::OpenFace() noexcept
{
    if (m_context->Display->Broker == nullptr)
    {
        return false;
    }
    m_displayEpoch = m_context->Display->Broker->Tag;
    Words();
    m_words[0U] = m_displayEpoch;
    if (!Send(m_context->Display->Console, I::SeatFace::TrustedDraw, I::SeatMethod::OpenDraw, m_reply))
    {
        return false;
    }
    const auto bytes = m_context->Supervisor->AwaitMessage(*m_context->Display->Broker, m_reply, m_delivery,
                                                           sizeof(m_delivery), m_error);
    if (!I::ReadSeatFrame(m_delivery, static_cast<Size>(bytes), m_message) ||
        m_message.Method != I::SeatMethod::DrawOpened || m_message.Transaction != m_transaction ||
        m_message.Words[0U] != m_displayEpoch)
    {
        return false;
    }
    m_displaySender = m_message.Sender;
    m_draw = m_message.Attachment;
    return true;
}
void ProductLogin::Start(ScenarioContext& context, const ComponentRecord& input) noexcept
{
    m_context = &context;
    m_input = &input;
    const UInt8* marker{};
    Size markerBytes{0U};
    m_fixtureTest = FindComponentImage(context.Launch->ComponentImages, context.Launch->ComponentImagesBytes,
                                       "seat-test", marker, markerBytes) &&
                    markerBytes != 0U;
    m_startStep = 1U;
    if (!context.Calls->CreateEndpoint(context.Management, m_authReport, m_error) ||
        !context.Calls->CreateEndpoint(context.Management, m_loginReport, m_error) ||
        !context.Calls->CreateEndpoint(context.Management, m_reply, m_error) ||
        !context.Calls->CreateEndpoint(context.Management, m_attention, m_error) || !Image("auth", m_authImage) ||
        !Image("login", m_loginImage))
    {
        Failure();
        return;
    }
    m_startStep = 2U;
    if (!OpenFace())
    {
        Failure();
        return;
    }
    // Root retains only the nonsecret limiter checkpoint. The build window is
    // unmapped before component admission and never contains a credential.
    CapabilityHandle memory{};
    m_startStep = 3U;
    if (!context.Calls->CreateMemory(context.Management, Abi::ObjectTypeFrame, 1U, 0U, context.IdentityLimiter,
                                     m_error) ||
        !context.Calls->CreateMemory(context.Management, Abi::ObjectTypeMemoryObject, 1U, System::ReadWriteUser, memory,
                                     m_error))
    {
        Failure();
        return;
    }
    m_startStep = 4U;
    const ComponentMapping mapping{.Frame = context.IdentityLimiter,
                                   .Memory = memory,
                                   .VirtualStart = context.WindowStart,
                                   .Pages = 1U,
                                   .Shared = false};
    if (!context.Calls->Map(context.OwnSpace, mapping, context.WindowStart, System::ReadWriteUser, m_error))
    {
        Failure();
        return;
    }
    static_cast<void>(new (SeatPlacement{
        reinterpret_cast<void*>(static_cast<Foundation::UIntPointer>(context.WindowStart))}) I::LimiterState{});
    m_startStep = 5U;
    if (context.Client->UnmapMemory(context.OwnSpace, context.WindowStart, 1U).Error != Abi::SystemCallError::None)
    {
        Failure();
        return;
    }
    static_cast<void>(context.Calls->Destroy(memory, Abi::ObjectTypeMemoryObject, m_error));
    m_startStep = 6U;
    const auto saved = context.Report;
    context.Report = m_authReport;
    m_auth = StartComponent(context, m_authImage, context.NextTag++, Abi::ComponentBehaviorAuth, m_error);
    if (m_auth == nullptr)
    {
        context.Report = saved;
        Failure();
        return;
    }
    m_startStep = 7U;
    context.Report = m_loginReport;
    context.ServiceFlags = context.Bootstrap->Scanout.Format;
    m_login = StartComponent(context, m_loginImage, context.NextTag++, Abi::ComponentBehaviorLogin, m_error);
    context.Report = saved;
    context.ServiceFlags = 0U;
    if (m_login == nullptr)
    {
        Failure();
        return;
    }
    m_startStep = 8U;
    if (!context.Supervisor->WatchService(*m_auth, m_error, Recover, this, ComponentStartupMicroseconds) ||
        !context.Supervisor->WatchService(*m_login, m_error, Recover, this, ComponentStartupMicroseconds))
    {
        Failure();
        return;
    }
    m_startStep = 9U;
    if (!Focus(0U))
    {
        Failure();
        return;
    }
    const UInt64 words[4U]{input.Tag, 0U, 0U, 0U};
    const auto size =
        C::WriteInputFrame(m_frame, C::InputRegisterClientMethod, ++m_transaction, words, nullptr, 0U, &m_reply);
    m_startStep = 10U;
    if (context.Client->Send(input.Service, System::AddressOf(m_frame), size).Error != Abi::SystemCallError::None)
    {
        Failure();
        return;
    }
    m_startStep = 11U;
    const auto bytes = context.Supervisor->AwaitMessage(input, m_reply, m_delivery, sizeof(m_delivery), m_error);
    if (!C::ReadInputFrame(m_delivery, static_cast<Size>(bytes), m_inputMessage) || m_inputMessage.Reply.Selector == 0U)
    {
        Failure();
        return;
    }
    m_inputClient = m_inputMessage.Reply;
    m_inputId = m_inputMessage.Words[2U];
    m_inputSender = System::LoadWord(m_delivery + 24U);
    m_startStep = 12U;
    Log("starting synthetic fixture seat; session storage is not private");
}
bool ProductLogin::Recover(void* pointer, ComponentRecord& component, Abi::SystemCallError& error) noexcept
{
    auto& self = *static_cast<ProductLogin*>(pointer);
    self.m_seat.Fail();
    self.m_noticed = 0U;
    // The root event loop paints Recovery and retains the focus lock. No
    // watchdog callback reenters screen IPC or grants a session input.
    return self.m_context->Supervisor->Stop(component, error) == PagerServeError::None;
}
void ProductLogin::Failure() noexcept
{
    if (m_failed)
    {
        return;
    }
    m_failed = true;
    const auto old = m_seat.Snapshot();
    if (!m_ready)
    {
        char line[160U]{};
        Inventory::ReportText text{line, sizeof(line)};
        text.Text("# login: startup failed stage=");
        text.Decimal(m_startStep);
        text.Text(" syscall-error=");
        text.Decimal(static_cast<UInt64>(m_error));
        text.Character('\n');
        static_cast<void>(m_context->Client->DebugWrite(m_context->Diagnostic, System::AddressOf(line), text.Bytes()));
    }
    if (m_auth != nullptr && m_auth->State == ComponentState::Running && old.Transaction != 0U)
    {
        Words();
        m_words[0U] = old.Epoch;
        m_words[1U] = old.Transaction;
        static_cast<void>(Send(m_auth->Service, I::SeatFace::AuthControl, I::SeatMethod::Cancel));
    }
    m_seat.Fail();
    Log("Recovery; seat locked");
    if (m_input != nullptr && m_reply.Selector != 0U)
    {
        static_cast<void>(Focus(0U));
    }
    if (m_loginSender != 0U && m_draw.Selector != 0U)
    {
        static_cast<void>(Notice());
    }
}
void ProductLogin::RetireEnded() noexcept
{
    // a faulted owner cannot wipe its own storage. Domain
    // drain retires its mappings and returns confidential Frames through
    // the kernel's zero-before-reuse path. A live owner handles Cancel.
    if (m_auth != nullptr && m_auth->State == ComponentState::Ended)
    {
        static_cast<void>(m_context->Supervisor->Teardown(*m_auth, m_error));
    }
    if (m_login != nullptr && m_login->State == ComponentState::Ended)
    {
        static_cast<void>(m_context->Supervisor->Teardown(*m_login, m_error));
    }
    if (m_context->Display->Broker != nullptr && m_context->Display->Broker->State == ComponentState::Ended)
    {
        auto* broker = m_context->Supervisor->FindByTag(m_context->Display->Broker->Tag);
        if (broker != nullptr)
        {
            static_cast<void>(m_context->Supervisor->Teardown(*broker, m_error));
        }
    }
}
[[gnu::noinline]] bool ProductLogin::CoverDeadDisplay() noexcept
{
    if (m_emergencyCovered)
    {
        return true;
    }
    const auto& mode = m_context->Bootstrap->Scanout;
    if (mode.Region.Selector == 0U || mode.Width == 0U || mode.Height == 0U)
    {
        return false;
    }
    const UInt64 bytes = static_cast<UInt64>(mode.Stride) * mode.Height * 4U;
    const UInt64 pages = (bytes + I::SeatPageBytes - 1U) / I::SeatPageBytes;
    if (pages == 0U || pages > Abi::ComponentSessionMaximumPages)
    {
        return false;
    }
    // After the dead broker is drained, root's retained scanout Region permits a fixed blank cover.
    // No application pixels, cursor, file parser or credential rendering enters this failure path.
    const auto frame = System::CreateDeviceFrame(*m_context->Client, m_context->Management, mode.Region, pages);
    if (frame.Error != Abi::SystemCallError::None)
    {
        return false;
    }
    m_emergencyFrame = {frame.Value0, frame.Value1};
    const auto memory = System::CreateMemory(*m_context->Client, m_context->Management, Abi::ObjectTypeMemoryObject,
                                             pages, Abi::ObjectCreateCacheUncached);
    if (memory.Error != Abi::SystemCallError::None)
    {
        return false;
    }
    m_emergencyMemory = {memory.Value0, memory.Value1};
    const UInt64 address = m_context->Launch->Component.ScanoutAddress;
    for (UInt64 offset = 0U; offset < pages; offset += I::AuthRunPages)
    {
        const UInt64 left = pages - offset;
        if (System::MapWindow(*m_context->Client, m_context->OwnSpace, m_emergencyMemory, m_emergencyFrame,
                              address + offset * I::SeatPageBytes, left < I::AuthRunPages ? left : I::AuthRunPages,
                              offset) != Abi::SystemCallError::None)
        {
            return false;
        }
    }
    auto* pixels = reinterpret_cast<volatile Foundation::UInt32*>(static_cast<Foundation::UIntPointer>(address));
    const auto color = Graphics::PackColor(static_cast<Graphics::PixelFormat>(mode.Format), I::LoginBackground);
    for (UInt64 row = 0U; row < mode.Height; ++row)
    {
        for (UInt64 column = 0U; column < mode.Width; ++column)
        {
            pixels[row * mode.Stride + column] = color;
        }
    }
    System::OrderScanoutStorage();
    m_emergencyCovered = true;
    Log("dead display covered; seat locked");
    return true;
}
bool ProductLogin::Read(CapabilityHandle endpoint, I::SeatMessage& message) noexcept
{
    const auto received = m_context->Client->Receive(endpoint, System::AddressOf(m_delivery), sizeof(m_delivery));
    return received.Error == Abi::SystemCallError::None &&
           I::ReadSeatFrame(m_delivery, static_cast<Size>(received.Value1), message);
}
bool ProductLogin::Screen() noexcept
{
    const auto& s = m_seat.Snapshot();
    Words();
    m_words[0U] = s.Epoch;
    m_words[1U] = static_cast<UInt64>(s.State);
    m_words[2U] = static_cast<UInt64>(s.Edit);
    m_words[3U] = s.Transaction;
    m_words[4U] = s.RetryDeadline;
    m_words[5U] = s.Account;
    m_words[6U] = s.Revision;
    m_retrySeconds = m_seat.RetrySeconds(Now());
    m_words[7U] = m_retrySeconds;
    m_words[9U] = m_name.Length;
    for (Size i{0U}; i < m_name.Length; ++i)
    {
        m_words[10U + i / 8U] |= static_cast<UInt64>(m_name.Bytes[i]) << (8U * (i % 8U));
    }
    return Send(m_login->Service, I::SeatFace::LoginControl, I::SeatMethod::Screen);
}
UInt64 ProductLogin::Now() const noexcept
{
    const auto clock = m_context->Launch->Component.Clock;
    const auto scale = m_context->Bootstrap->ClockTicksPerMicrosecond;
    if (clock == nullptr || scale == 0U)
    {
        return 0U;
    }
    const UInt64 microseconds = clock() / scale;
    return microseconds > I::MaximumCounter / 1000U ? I::MaximumCounter : microseconds * 1000U;
}
void ProductLogin::Result(const I::SeatMessage& message) noexcept
{
    const auto* w = message.Words;
    const auto& seat = m_seat.Snapshot();
    // A current result cannot leave the trusted interval until login acknowledges wiping the
    // submitted password page.
    if (w[0U] == seat.Epoch && w[1U] == seat.Transaction && w[2U] == seat.Attempt &&
        (m_copiedEpoch != w[0U] || m_copiedTransaction != w[1U] || m_copiedAttempt != w[2U]))
    {
        m_result = message;
        m_resultPending = true;
        return;
    }
    if (w[0U] == seat.Epoch && w[1U] == seat.Transaction && w[2U] == seat.Attempt &&
        w[4U] == static_cast<UInt64>(I::Status::Ok))
    {
        // Requires an authenticated revision check before the root consumes success, even for this
        // immutable test store.
        if (m_revisionTransaction == 0U)
        {
            Words();
            m_words[0U] = w[5U];
            m_words[1U] = w[6U];
            if (!Send(m_auth->Service, I::SeatFace::AuthControl, I::SeatMethod::CheckRevision))
            {
                Failure();
                return;
            }
            m_revisionTransaction = m_transaction;
            m_revisionEpoch = w[0U];
            m_revisionAttempt = w[2U];
            m_revisionValidated = false;
        }
        if (!m_revisionValidated || m_revisionEpoch != w[0U] || m_revisionAttempt != w[2U])
        {
            m_result = message;
            m_resultPending = true;
            return;
        }
    }
    m_resultPending = false;
    m_revisionTransaction = 0U;
    m_revisionValidated = false;
    char line[192U]{};
    Inventory::ReportText text{line, sizeof(line)};
    text.Text("# auth: pages=");
    text.Decimal(I::AuthArenaPages);
    text.Text(" passes=");
    text.Decimal(Crypto::LoginArgon2Defaults.Passes);
    text.Text(" lanes=");
    text.Decimal(Crypto::LoginArgon2Defaults.Lanes);
    text.Text(" status=");
    text.Decimal(w[4U]);
    text.Text(" elapsed-ns=");
    text.Decimal(w[8U]);
    text.Text(" wipe=");
    text.Decimal(w[9U]);
    text.Character('\n');
    static_cast<void>(m_context->Client->DebugWrite(m_context->Diagnostic, System::AddressOf(line), text.Bytes()));
    if (w[9U] != 1U)
    {
        Failure();
    }
    else if (m_seat.Result(w[0U], w[1U], w[2U], static_cast<I::Purpose>(w[3U]), static_cast<I::Status>(w[4U]), w[5U],
                           w[6U], w[7U]) &&
             !Notice())
    {
        Failure();
    }
}
bool ProductLogin::Notice() noexcept
{
    const auto& s = m_seat.Snapshot();
    if (s.Epoch == m_noticed)
    {
        return true;
    }
    // Retain the focus lock through both Show and Hide. The current epoch's draw fence proves that
    // trusted pixels were replaced before a session receives input again.
    if (!Focus(0U))
    {
        return false;
    }
    Words();
    m_words[0U] = s.Epoch;
    m_words[1U] = m_loginSender;
    if (!Send(m_draw, I::SeatFace::TrustedDraw, I::SeatMethod::Enroll, m_login->Service))
    {
        return false;
    }
    Words();
    m_words[0U] = s.Epoch;
    if (!Send(m_draw, I::SeatFace::TrustedDraw, m_seat.Trusted() ? I::SeatMethod::Show : I::SeatMethod::Hide) ||
        !Screen())
    {
        return false;
    }
    m_fencedEpoch = 0U;
    m_noticed = s.Epoch;
    char line[160U]{};
    Inventory::ReportText text{line, sizeof(line)};
    text.Text("# login: state ");
    text.Decimal(static_cast<UInt64>(s.State));
    text.Text(" edit ");
    text.Decimal(static_cast<UInt64>(s.Edit));
    text.Text(" epoch ");
    text.Decimal(s.Epoch);
    text.Character('\n');
    static_cast<void>(m_context->Client->DebugWrite(m_context->Diagnostic, System::AddressOf(line), text.Bytes()));
    return true;
}
bool ProductLogin::Begin() noexcept
{
    const auto& s = m_seat.Snapshot();
    if ((s.State != I::ScreenState::Login && s.State != I::ScreenState::Locked) || s.Edit != I::EditState::Editing ||
        s.Transaction != 0U || m_fencedEpoch != s.Epoch)
    {
        return true;
    }
    const auto clock = m_context->Launch->Component.Clock;
    const auto scale = m_context->Bootstrap->ClockTicksPerMicrosecond;
    const UInt64 now = clock == nullptr || scale == 0U ? 0U : clock() / scale;
    if (m_beginEpoch == s.Epoch && now >= m_beginAt && now - m_beginAt < 100'000U)
    {
        return true;
    }
    Words();
    m_words[0U] = s.Epoch;
    m_words[1U] = m_loginSender;
    m_words[2U] = static_cast<UInt64>(s.State == I::ScreenState::Locked ? I::Purpose::Unlock : I::Purpose::Login);
    m_words[3U] = s.Account;
    m_words[4U] = s.State == I::ScreenState::Locked ? s.Session : m_session;
    if (!Send(m_auth->Service, I::SeatFace::AuthControl, I::SeatMethod::Begin))
    {
        return false;
    }
    m_beginEpoch = s.Epoch;
    m_beginAt = now;
    return true;
}
void ProductLogin::Attention() noexcept
{
    if (m_polling || m_sending || m_attention.Selector == 0U)
    {
        return;
    }
    m_polling = true;
    if (!m_attentionCredit)
    {
        const UInt64 words[4U]{m_input->Tag, 0U, 0U, 0U};
        const auto size = C::WriteInputFrame(m_frame, C::InputAttentionCreditMethod, ++m_transaction, words, nullptr,
                                             0U, &m_attention);
        const auto sent = m_context->Client->Send(m_input->Service, System::AddressOf(m_frame), size).Error;
        if (sent == Abi::SystemCallError::None)
        {
            m_attentionCredit = true;
        }
    }
    const auto received = m_context->Client->Receive(m_attention, System::AddressOf(m_delivery), sizeof(m_delivery));
    if (received.Error == Abi::SystemCallError::None &&
        C::ReadInputFrame(m_delivery, static_cast<Size>(received.Value1), m_inputMessage))
    {
        m_attentionCredit = false;
        if (m_inputMessage.Method == C::InputAttentionMethod)
        {
            const auto usage = m_inputMessage.Words[2U];
            if (usage != 0U)
            {
                if (usage == 66U)
                {
                    static_cast<void>(PreparePowerTestVolume(*m_context, m_error));
                }
                else
                {
                    static_cast<void>(RequestPower(*m_context, usage == 68U, m_error));
                }
            }
            else
            {
                const auto previous = m_seat.Snapshot();
                if (previous.Transaction != 0U)
                {
                    Words();
                    m_words[0U] = previous.Epoch;
                    m_words[1U] = previous.Transaction;
                    static_cast<void>(Send(m_auth->Service, I::SeatFace::AuthControl, I::SeatMethod::Cancel));
                }
                static_cast<void>(m_seat.Attention());
                Log("secure attention reaches root");
                if (m_ready && !Notice())
                {
                    Failure();
                }
            }
        }
    }
    m_polling = false;
}
bool ProductLogin::Step(Abi::SystemCallError& error) noexcept
{
    // Seat failures are handled by Recovery, not propagated as a stale
    // transport error to the root's still-running input/pager loop.
    error = Abi::SystemCallError::None;
    Attention();
    if (m_auth == nullptr || m_login == nullptr)
    {
        return true;
    }
    const bool unavailable = m_auth->State != ComponentState::Running || m_login->State != ComponentState::Running ||
                             m_context->Supervisor->ServiceStartupExpired(*m_auth) ||
                             m_context->Supervisor->ServiceStartupExpired(*m_login) ||
                             m_context->Display->Broker == nullptr ||
                             m_context->Display->Broker->State != ComponentState::Running ||
                             m_context->Display->Broker->Tag != m_displayEpoch;
    if (unavailable)
    {
        Failure();
    }
    if (Read(m_loginReport, m_message))
    {
        const auto* w = m_message.Words;
        if (m_loginSender == 0U && m_message.Face == I::SeatFace::LoginControl &&
            m_message.Method == I::SeatMethod::Ready && w[0U] == m_login->Tag)
        {
            m_loginSender = m_message.Sender;
        }
        else if (m_message.Sender == m_loginSender && m_message.Method == I::SeatMethod::Ready && w[2U] != 0U)
        {
            if (!m_context->Supervisor->MarkServiceReady(*m_login))
            {
                Failure();
            }
            else if (w[2U] == m_seat.Snapshot().Epoch)
            {
                m_fencedEpoch = w[2U];
                Log("login ready after trusted draw fence");
                if (m_seat.Trusted())
                {
                    if (!Focus(m_failed ? 0U : m_inputId))
                    {
                        Failure();
                    }
                }
                else if (!m_failed && !FocusProductDesktop())
                {
                    Failure();
                }
            }
        }
        else if (m_message.Sender == m_loginSender && m_message.Method == I::SeatMethod::Copied &&
                 w[0U] == m_seat.Snapshot().Epoch && w[1U] == m_seat.Snapshot().Transaction &&
                 w[2U] == m_seat.Snapshot().Attempt)
        {
            m_copiedEpoch = w[0U];
            m_copiedTransaction = w[1U];
            m_copiedAttempt = w[2U];
            Log("password snapshot held by auth only");
        }
        else if (m_message.Sender == m_loginSender && m_message.Method == I::SeatMethod::Reply && m_fixtureTest)
        {
            if (w[2U] != static_cast<UInt64>(C::InputAnswer::Refused))
            {
                Failure();
            }
            else
            {
                Log("negative probe focus refused; trusted writes sent");
            }
        }
        else if (m_message.Sender == m_loginSender && m_message.Face == I::SeatFace::LoginControl &&
                 m_message.Method == I::SeatMethod::Action && w[0U] == m_seat.Snapshot().Epoch)
        {
            if (w[1U] == 0U)
            {
                if (m_seat.Submitted(w[0U], w[2U], w[3U]))
                {
                    m_name.Length = static_cast<UInt8>(w[4U]);
                    for (Size i{0U}; i < m_name.Length; ++i)
                    {
                        m_name.Bytes[i] = static_cast<UInt8>(w[5U + i / 8U] >> (8U * (i % 8U)));
                    }
                }
            }
            else if (w[1U] == 1U)
            {
                const auto old = m_seat.Snapshot();
                if (old.Transaction != 0U)
                {
                    Words();
                    m_words[0U] = old.Epoch;
                    m_words[1U] = old.Transaction;
                    static_cast<void>(Send(m_auth->Service, I::SeatFace::AuthControl, I::SeatMethod::Cancel));
                }
                static_cast<void>(m_seat.Cancel());
                if (!Notice())
                {
                    Failure();
                }
            }
            else if (m_fixtureTest && w[1U] >= 3U && w[1U] <= 5U)
            {
                ComponentRecord* victim = w[1U] == 3U   ? m_auth
                                          : w[1U] == 4U ? m_login
                                          : m_context->Display->Broker == nullptr
                                              ? nullptr
                                              : m_context->Supervisor->FindByTag(m_context->Display->Broker->Tag);
                Log("synthetic service failure injection");
                if (victim != nullptr)
                {
                    static_cast<void>(m_context->Supervisor->Stop(*victim, m_error));
                }
                Failure();
            }
            else if (w[1U] == 2U)
            {
                const auto old = m_seat.Snapshot();
                if (old.Transaction != 0U)
                {
                    Words();
                    m_words[0U] = old.Epoch;
                    m_words[1U] = old.Transaction;
                    static_cast<void>(Send(m_auth->Service, I::SeatFace::AuthControl, I::SeatMethod::Cancel));
                }
                static_cast<void>(m_seat.Logout());
                if (!Notice())
                {
                    Failure();
                }
            }
        }
    }
    if (m_resultPending)
    {
        Result(m_result);
    }
    if (Read(m_authReport, m_message))
    {
        const auto* w = m_message.Words;
        if (m_authSender == 0U && m_message.Face == I::SeatFace::AuthControl &&
            m_message.Method == I::SeatMethod::Ready && w[0U] == m_auth->Tag && w[1U] == I::AuthArenaPages &&
            w[2U] == I::AuthRuns)
        {
            m_authSender = m_message.Sender;
            if (!m_context->Supervisor->MarkServiceReady(*m_auth))
            {
                Failure();
            }
            Log("auth admitted 16384 pages in 32 runs");
        }
        else if (m_authSender == 0U && m_message.Face == I::SeatFace::AuthControl &&
                 m_message.Method == I::SeatMethod::Reply && w[0U] == m_auth->Tag &&
                 w[1U] == static_cast<UInt64>(I::Status::Unavailable))
        {
            char line[192U]{};
            Inventory::ReportText text{line, sizeof(line)};
            text.Text("# auth: admission failed stage=");
            text.Decimal(w[2U]);
            text.Text(" syscall-error=");
            text.Decimal(w[3U]);
            text.Text(" required-pages=");
            text.Decimal(I::AuthArenaPages);
            text.Character('\n');
            static_cast<void>(
                m_context->Client->DebugWrite(m_context->Diagnostic, System::AddressOf(line), text.Bytes()));
            Failure();
        }
        else if (m_message.Sender == m_authSender && m_message.Method == I::SeatMethod::OpenSubmit)
        {
            m_submit = m_message.Attachment;
            if (!Configure(m_login->Service, m_login->Tag, 1U, m_submit, m_authSender))
            {
                Failure();
            }
        }
        else if (m_message.Sender == m_authSender && m_message.Method == I::SeatMethod::Reply &&
                 m_revisionTransaction != 0U && m_message.Transaction == m_revisionTransaction && w[0U] == m_auth->Tag)
        {
            if (w[1U] != static_cast<UInt64>(I::Status::Ok))
            {
                Failure();
            }
            else
            {
                m_revisionValidated = true;
            }
        }
        else if (m_message.Sender == m_authSender && m_message.Method == I::SeatMethod::Reply &&
                 w[0U] == m_seat.Snapshot().Epoch && w[1U] == 0U)
        {
            if (m_seat.Begin(w[0U], w[2U]))
            {
                if (!Screen())
                {
                    Failure();
                }
                else
                {
                    Log("editing ready");
                }
            }
        }
        else if (m_message.Sender == m_authSender && m_message.Method == I::SeatMethod::Result)
        {
            Result(m_message);
        }
    }
    // Keep the public admission-failure receipt readable after an exited
    // startup. It names the exact run/map stage and syscall refusal only.
    if (unavailable)
    {
        const bool displayGone = m_context->Display->Broker == nullptr ||
                                 m_context->Display->Broker->State != ComponentState::Running ||
                                 m_context->Display->Broker->Tag != m_displayEpoch;
        if (displayGone && m_context->Display->Broker != nullptr &&
            m_context->Display->Broker->State == ComponentState::Running)
        {
            auto* broker = m_context->Supervisor->FindByTag(m_context->Display->Broker->Tag);
            if (broker != nullptr)
            {
                static_cast<void>(m_context->Supervisor->Stop(*broker, m_error));
            }
        }
        RetireEnded();
        if (displayGone && !m_emergencyCovered && !m_emergencyFailed &&
            (m_context->Display->Broker == nullptr || m_context->Display->Broker->State == ComponentState::Free) &&
            !CoverDeadDisplay())
        {
            m_emergencyFailed = true;
            Log("dead display cover unavailable; input remains locked");
        }
        return true;
    }
    if (!m_ready && m_authSender != 0U && m_loginSender != 0U)
    {
        if (!Configure(m_login->Service, m_login->Tag, 2U, m_draw, m_displaySender) ||
            !Configure(m_login->Service, m_login->Tag, 3U, m_inputClient, m_inputSender))
        {
            Failure();
            return true;
        }
        if (m_fixtureTest &&
            !Configure(m_login->Service, m_login->Tag, 4U, m_context->Display->Console, m_displaySender))
        {
            Failure();
            return true;
        }
        Words();
        m_words[0U] = m_auth->Tag;
        m_words[1U] = m_loginSender;
        if (!Send(m_auth->Service, I::SeatFace::AuthControl, I::SeatMethod::Configure, m_login->Service))
        {
            Failure();
            return true;
        }
        m_ready = true;
        if (!Notice())
        {
            Failure();
        }
    }
    if (!m_ready || m_failed)
    {
        return true;
    }
    const auto state = m_seat.Snapshot().State;
    if (state == I::ScreenState::StartingSession && !m_starting)
    {
        m_starting = true;
        StartProductDesktop(*m_context, *m_input);
        m_starting = false;
        // An ordinary session admission/readiness failure returns to Login only after its borrowed
        // pixels and domain are drained. Trusted-service or teardown failure remains fail-closed
        // Recovery.
        if (!ProductDesktopActive())
        {
            if (!StopProductDesktop() || !m_seat.SessionFailed(m_seat.Snapshot().Epoch) || !Notice())
            {
                Failure();
            }
            else
            {
                Log("partial session drained; returned to Login");
            }
        }
        else if (!m_seat.SessionReady(m_seat.Snapshot().Epoch, m_session++))
        {
            Failure();
        }
        else if (!Notice())
        {
            Failure();
        }
    }
    else if (state == I::ScreenState::LoggingOut)
    {
        if (!StopProductDesktop() || !m_seat.SessionDrained(m_seat.Snapshot().Epoch))
        {
            Failure();
        }
        else if (!Notice())
        {
            Failure();
        }
    }
    const UInt64 now = Now();
    if (m_seat.Retry(now) && !Notice())
    {
        Failure();
    }
    else if (m_seat.RetrySeconds(now) != m_retrySeconds && !Screen())
    {
        Failure();
    }
    if (m_submit.Selector != 0U && !Begin())
    {
        Failure();
    }
    if (state == I::ScreenState::Active && !StepProductDesktop(*m_context, error))
    {
        Failure();
        error = Abi::SystemCallError::None;
    }
    return true;
}
} // Namespace
void StartProductLogin(ScenarioContext& context, const ComponentRecord& input) noexcept
{
    g_productLogin = new (SeatPlacement{g_productLoginStorage}) ProductLogin{};
    g_productLogin->Start(context, input);
}
bool StepProductLogin(ScenarioContext&, Abi::SystemCallError& error) noexcept
{
    return g_productLogin == nullptr || g_productLogin->Step(error);
}
void PollProductLogin(ScenarioContext&) noexcept
{
    if (g_productLogin != nullptr)
    {
        g_productLogin->Attention();
    }
}
} // Namespace Asteria::User::Pager
