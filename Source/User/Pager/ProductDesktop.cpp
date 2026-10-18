// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/ProductDesktop.hpp"
#include "User/Pager/ProductLogin.hpp"
#include "User/Pager/ToolkitLaunch.hpp"
#include "User/Pager/PowerOff.hpp"
#include "User/Pager/InputTerminal.hpp"
#include "User/Pager/ComponentImage.hpp"
#include "User/Loader/ExecutableFormats.hpp"
#include "User/Console/TextConsole.hpp"
#include "User/Session/SessionFrame.hpp"
#include "User/Session/Decorations.hpp"
#include "User/Session/AddressPlan.hpp"
#include "User/Session/SessionFaceFrame.hpp"
#include "User/System/DeviceAccess.hpp"
#include "Inventory/ReportText.hpp"
namespace Asteria::User::Pager
{
namespace
{
namespace S = Session;
using Foundation::UInt64;
using Foundation::UInt32;
using Foundation::Size;
using System::CapabilityHandle;
// All owners and wire scratch are static: the target stack budget is 512 bytes.
class ProductDesktop final
{
public:
    void Configure(ScenarioContext& context) noexcept;
    void Start(const ComponentRecord& input) noexcept;
    void Fail(const char* reason) noexcept
    {
        Fallback(reason);
    }
    [[nodiscard]] bool Step(Abi::SystemCallError& error) noexcept;
    [[nodiscard]] bool SeatFocus() noexcept
    {
        return m_active && Focus(m_inputId);
    }
    [[nodiscard]] bool SeatStop() noexcept;
    [[nodiscard]] bool Active() const noexcept
    {
        return m_active;
    }

private:
    static void Observe(void*, const char*, Size) noexcept;
    static void Poll(void*) noexcept;
    static bool Recover(void*, ComponentRecord&, Abi::SystemCallError&) noexcept;
    void Drain() noexcept;
    void Words(UInt64 first = 0U) noexcept;
    [[nodiscard]] bool Send(CapabilityHandle target, UInt32 method, bool face = false,
                            const CapabilityHandle* handles = nullptr, Size count = 0U) noexcept;
    [[nodiscard]] bool Focus(UInt64 id, bool lock = false) noexcept;
    [[nodiscard]] bool StartSession() noexcept;
    [[nodiscard]] bool InitializeSession() noexcept;
    [[nodiscard]] bool StartupFailed() noexcept;
    [[nodiscard]] bool Pools() noexcept;
    [[nodiscard]] bool Present() noexcept;
    [[nodiscard]] bool StopSession() noexcept;
    [[nodiscard]] bool RebindInput(bool retire = false) noexcept;
    [[nodiscard]] bool ReleasePools() noexcept;
    void Fallback(const char* reason) noexcept;
    void ReportEvent(const S::SessionEvent& event) noexcept;
    Abi::SystemCallError m_error{Abi::SystemCallError::None};
    ScenarioContext* m_context{nullptr};
    const ComponentRecord* m_input{nullptr};
    ComponentRecord* m_compositor{nullptr};
    LoadedImage m_image{};
    CapabilityHandle m_face{};
    CapabilityHandle m_control{};
    CapabilityHandle m_inputClient{};
    CapabilityHandle m_client{};
    CapabilityHandle m_events{};
    CapabilityHandle m_reports{};
    CapabilityHandle m_reply{};
    CapabilityHandle m_attention{};
    CapabilityHandle m_memories[2U]{};
    CapabilityHandle m_frames[2U][S::SessionMaximumPoolFrames]{};
    UInt64 m_chunks[S::SessionMaximumPoolFrames]{};
    UInt64 m_pages{0U};
    UInt64 m_frameCount{0U};
    UInt64 m_inputId{0U};
    UInt64 m_faceEpoch{0U};
    UInt64 m_mappedPages[2U]{};
    UInt64 m_transaction{0U};
    UInt64 m_serial{0U};
    UInt64 m_arrange{0U};
    UInt32 m_width{0U};
    UInt32 m_height{0U};
    UInt32 m_poolReady{0U};
    UInt32 m_lent{0U};
    bool m_active{false};
    bool m_trusted{false};
    bool m_credit{false};
    bool m_attentionCredit{false};
    bool m_dirty{true};
    bool m_shown{false};
    bool m_failed{false};
    bool m_fallback{false};
    bool m_recovering{false};
    bool m_closed{false};
    bool m_starting{false};
    bool m_startupTimedOut{false};
    Foundation::UInt8 m_deaths{0U};
    Console::TextConsole m_grid{};
    char m_log[16U * 1024U]{};
    Size m_logBytes{0U};
    UInt64 m_words[S::SessionMaximumWords]{};
    S::SessionMessage m_message{};
    S::SessionFaceMessage m_faceMessage{};
    Component::DisplayMessage m_report{};
    Component::InputMessage m_inputMessage{};
    alignas(8) Foundation::UInt8 m_frame[504U]{}, m_delivery[PagerProgram::DeliveryCapacityBytes]{};
};
constinit ProductDesktop g_desktop{};
void ProductDesktop::Words(const UInt64 first) noexcept
{
    for (auto& word : m_words)
    {
        word = 0U;
    }
    m_words[0U] = first;
}
void ProductDesktop::Observe(void* const pointer, const char* const text, const Size bytes) noexcept
{
    auto& self = *static_cast<ProductDesktop*>(pointer);
    // A bounded boot-log tail; resize replays it into the new cell grid.
    for (Size i{0U}; i < bytes; ++i)
    {
        if (self.m_logBytes == sizeof(self.m_log))
        {
            constexpr Size kept{sizeof(self.m_log) / 2U};
            for (Size j{0U}; j < kept; ++j)
            {
                self.m_log[j] = self.m_log[j + kept];
            }
            self.m_logBytes = kept;
        }
        self.m_log[self.m_logBytes++] = text[i];
    }
    self.m_dirty = true;
}
void ProductDesktop::Configure(ScenarioContext& context) noexcept
{
    m_context = &context;
    if (context.Bootstrap->SessionMode == Abi::SessionDesktop)
    {
        context.ConsoleObserver = Observe;
        context.ConsoleObserverContext = this;
    }
}
bool ProductDesktop::Send(const CapabilityHandle target, const UInt32 method, const bool face,
                          const CapabilityHandle* const handles, const Size count) noexcept
{
    const auto bytes = face ? S::WriteSessionFaceFrame(m_frame, method, ++m_transaction, m_words, handles, count)
                            : S::WriteSessionFrame(m_frame, method, ++m_transaction, m_words, handles, count);
    if (bytes == 0U)
    {
        return false;
    }
    for (Size i{0U}; i < 6000U; ++i)
    {
        if (m_starting && StartupFailed())
        {
            return false;
        }
        const auto error = m_context->Client->Send(target, System::AddressOf(m_frame), bytes).Error;
        if (error != Abi::SystemCallError::WouldBlock)
        {
            return error == Abi::SystemCallError::None;
        }
        Abi::SystemCallError waited{Abi::SystemCallError::None};
        // Poll only receives: it cannot overwrite the frozen outgoing frame.
        if (m_context->Supervisor->Pause(10'000U, waited) != PagerServeError::None)
        {
            return false;
        }
    }
    return false;
}
bool ProductDesktop::Focus(const UInt64 id, const bool lock) noexcept
{
    const UInt64 words[4U]{m_input->Tag, id, lock ? 1U : 0U, 0U};
    const auto bytes = Component::WriteInputFrame(m_frame, Component::InputSetFocusMethod, ++m_transaction, words,
                                                  nullptr, 0U, &m_reply);
    if (m_context->Client->Send(m_input->Service, System::AddressOf(m_frame), bytes).Error !=
        Abi::SystemCallError::None)
    {
        return false;
    }
    Abi::SystemCallError error{Abi::SystemCallError::None};
    const auto received = m_context->Supervisor->AwaitMessage(*m_input, m_reply, m_delivery, sizeof(m_delivery), error);
    return Component::ReadInputFrame(m_delivery, static_cast<Size>(received), m_inputMessage) &&
           m_inputMessage.Transaction == m_transaction && m_inputMessage.Words[1U] == 1U;
}
void ProductDesktop::Poll(void* const pointer) noexcept
{
    auto& self = *static_cast<ProductDesktop*>(pointer);
    self.Drain();
    if (self.m_context->Bootstrap->LoginEnabled != 0U)
    {
        PollProductLogin(*self.m_context);
    }
}
void ProductDesktop::Drain() noexcept
{
    for (Size i{0U}; i < 16U; ++i)
    {
        const auto received = m_context->Client->Receive(m_reports, System::AddressOf(m_delivery), sizeof(m_delivery));
        if (received.Error != Abi::SystemCallError::None)
        {
            break;
        }
        if (Component::ReadDisplayFrame(m_delivery, static_cast<Size>(received.Value1), m_report) &&
            m_report.Method == Component::DisplayConsoleWriteMethod)
        {
            static_cast<void>(
                m_context->Client->DebugWrite(m_context->Diagnostic, System::AddressOf(m_report.Text), m_report.Count));
        }
    }
    for (Size i{0U}; i < 16U; ++i)
    {
        const auto received = m_context->Client->Receive(m_events, System::AddressOf(m_delivery), sizeof(m_delivery));
        if (received.Error != Abi::SystemCallError::None)
        {
            break;
        }
        if (!S::ReadSessionFrame(m_delivery, static_cast<Size>(received.Value1), m_message) ||
            m_message.Method != S::SessionEventsMethod)
        {
            m_failed = true;
            continue;
        }
        m_credit = false;
        for (Size j{0U}; j < S::SessionEventCount(m_message); ++j)
        {
            S::SessionEvent event{};
            if (!S::GetSessionEvent(m_message, j, event))
            {
                m_failed = true;
                break;
            }
            ReportEvent(event);
            const auto* w = event.Words;
            switch (event.Kind)
            {
            case S::SessionEventKind::PoolReady:
                m_poolReady |= 1U << (w[0U] - 1U);
                break;
            case S::SessionEventKind::ImageReturned:
                m_lent &= ~static_cast<UInt32>(w[0U]);
                break;
            case S::SessionEventKind::FrameShown:
                m_shown = true;
                break;
            case S::SessionEventKind::Arrange:
                m_arrange = w[1U];
                m_width = static_cast<UInt32>(w[2U]);
                m_height = static_cast<UInt32>(w[2U] >> 32U);
                m_dirty = true;
                break;
            case S::SessionEventKind::CloseAsked:
                // Root-owned boot-log viewer closes locally; shell windows come with CM.
                m_closed = true;
                break;
            case S::SessionEventKind::Key:
                if (w[1U] == S::SessionKeyDown)
                {
                    // InputTerminal's existing simple echo, with the shell engine deferred.
                    const Input::InputEvent key{.Kind = Input::EventKind::KeyDown,
                                                .Modifiers = static_cast<Foundation::UInt8>(w[3U]),
                                                .Usage = static_cast<Foundation::UInt16>(w[2U]),
                                                .Source = 0U,
                                                .Dx = 0,
                                                .Dy = 0,
                                                .Buttons = 0U,
                                                .Reserved = {},
                                                .Wheel = 0};
                    const char character = InputTerminalCharacter(key);
                    if (character != '\0')
                    {
                        Observe(this, &character, 1U);
                    }
                }
                break;
            case S::SessionEventKind::PoolRefused:
            case S::SessionEventKind::ImageRefused:
            case S::SessionEventKind::RequestRefused:
                m_failed = true;
                break;
            default:
                break;
            }
        }
    }
}
void ProductDesktop::ReportEvent(const S::SessionEvent& event) noexcept
{
    char line[160U]{};
    Inventory::ReportText text{line, sizeof(line)};
    text.Text("# session-client: event ");
    text.Decimal(static_cast<UInt64>(event.Kind));
    for (const auto word : event.Words)
    {
        text.Character(' ');
        text.Decimal(word);
    }
    text.Character('\n');
    static_cast<void>(m_context->Client->DebugWrite(m_context->Diagnostic, System::AddressOf(line), text.Bytes()));
}
bool ProductDesktop::Pools() noexcept
{
#if defined(ASTERIA_TARGET_IA64)
    constexpr UInt64 base{Abi::Ia64ComponentCompositorAddress};
#else
    constexpr UInt64 base{Abi::ComponentCompositorAddress};
#endif
    const auto& mode = m_context->Bootstrap->Scanout;
    m_pages = (static_cast<UInt64>(mode.Width) * mode.Height * 4U + 4095U) / 4096U;
    if (m_pages == 0U || m_pages > S::SessionMaximumPoolPages)
    {
        return false;
    }
    m_frameCount = (m_pages + S::SessionMaximumFramePages - 1U) / S::SessionMaximumFramePages;
    for (Size pool{0U}; pool < 2U; ++pool)
    {
        const auto memory =
            System::CreateMemory(*m_context->Client, m_context->Management, Abi::ObjectTypeMemoryObject, m_pages);
        if (memory.Error != Abi::SystemCallError::None)
        {
            return false;
        }
        m_memories[pool] = {memory.Value0, memory.Value1};
        UInt64 offset{0U};
        for (Size i{0U}; i < m_frameCount; ++i)
        {
            const UInt64 left = m_pages - offset;
            const UInt64 pages = left < S::SessionMaximumFramePages ? left : S::SessionMaximumFramePages;
            const auto frame = System::CreateRamFrame(*m_context->Client, m_context->Management, pages);
            if (frame.Error != Abi::SystemCallError::None)
            {
                return false;
            }
            m_frames[pool][i] = {frame.Value0, frame.Value1};
            m_chunks[i] = pages;
            auto* const request = m_context->Calls->GetRequest();
            m_context->Calls->ClearRequest();
            System::StoreHandle(request + Abi::FaultResolveAddressSpaceOffset, m_context->OwnSpace);
            System::StoreHandle(request + Abi::FaultResolveMemoryObjectOffset, m_memories[pool]);
            System::StoreHandle(request + Abi::FaultResolveFrameOffset, m_frames[pool][i]);
            System::StoreWord(request + Abi::FaultResolveVirtualStartOffset,
                              base + pool * S::AddressPoolSpan + offset * 4096U);
            System::StoreWord(request + Abi::FaultResolveMemoryObjectPageOffset, offset);
            System::StoreWord(request + Abi::FaultResolvePageCountOffset, pages);
            request[Abi::FaultResolvePermissionsOffset] = System::ReadWriteUser;
            if (m_context->Client->MapMemory(System::AddressOf(request)).Error != Abi::SystemCallError::None)
            {
                return false;
            }
            offset += pages;
            m_mappedPages[pool] = offset;
        }
    }
    return true;
}
bool ProductDesktop::StartSession() noexcept
{
    m_starting = true;
    m_startupTimedOut = false;
    const bool ready = InitializeSession();
    m_starting = false;
    return ready;
}
bool ProductDesktop::StartupFailed() noexcept
{
    m_startupTimedOut = m_startupTimedOut ||
                        (m_compositor != nullptr && m_context->Supervisor->ServiceStartupExpired(*m_compositor)) ||
                        (m_context->Display->Broker != nullptr &&
                         m_context->Supervisor->ServiceStartupExpired(*m_context->Display->Broker));
    return m_startupTimedOut;
}
bool ProductDesktop::InitializeSession() noexcept
{
    // Watchdog.md: native first scanout has a separate startup deadline.
    // First FrameShown and the Show fence prove both services ready.
    if (!m_context->Supervisor->BeginServiceStartup(*m_context->Display->Broker, m_error))
    {
        return false;
    }
    if (m_events.Selector == 0U && !m_context->Calls->CreateEndpoint(m_context->Management, m_events, m_error))
    {
        return false;
    }
    m_poolReady = 0U;
    m_credit = false;
    m_lent = 0U;
    m_shown = false;
    m_failed = false;
    m_dirty = true;
    m_serial = 0U;
    m_arrange = 0U;
    // The first client prefers 80x25 cells; chrome comes from the Classic theme.
    constexpr UInt32 PreferredColumns{80U};
    constexpr UInt32 PreferredRows{25U};
    const auto& font = Graphics::Bold8x16;
    const auto& mode = m_context->Bootstrap->Scanout;
    const UInt32 roomWidth = mode.Width - S::FrameExtraWidth;
    const UInt32 roomHeight = mode.Height - S::FrameExtraHeight;
    const UInt32 preferredWidth = PreferredColumns * font.GlyphWidth;
    const UInt32 preferredHeight = PreferredRows * font.GlyphHeight;
    m_width = (roomWidth < preferredWidth ? roomWidth : preferredWidth) / font.GlyphWidth * font.GlyphWidth;
    m_height = (roomHeight < preferredHeight ? roomHeight : preferredHeight) / font.GlyphHeight * font.GlyphHeight;
    const auto report = m_context->Report;
    m_context->Report = m_reports;
    m_context->DriverService = m_face;
    m_context->DriverGeneration = m_context->Display->Broker->Tag;
    m_context->ServiceFlags = m_context->Bootstrap->Scanout.Format;
    m_context->SessionInput = m_inputClient;
    m_context->SessionInputEpoch = m_input->Tag;
    const UInt64 tag = m_context->NextTag++;
    m_compositor = StartComponent(*m_context, m_image, tag, Abi::ComponentBehaviorCompositor, m_error);
    m_context->Report = report;
    m_context->DriverService = {};
    m_context->DriverGeneration = 0U;
    m_context->ServiceFlags = 0U;
    m_context->SessionInput = {};
    m_context->SessionInputEpoch = 0U;
    if (m_compositor == nullptr)
    {
        m_compositor = m_context->Supervisor->FindByTag(tag);
        return false;
    }
    if (!m_context->Supervisor->WatchService(*m_compositor, m_error, Recover, this, ComponentStartupMicroseconds))
    {
        return false;
    }
    Words();
    m_words[1U] = S::SessionClassSystemModule;
    m_words[2U] = 8U;
    constexpr char name[]{"boot-log"};
    for (Size i{0U}; i < 8U; ++i)
    {
        m_words[4U] |= static_cast<UInt64>(name[i]) << (i * 8U);
    }
    if (!Send(m_compositor->Service, S::SessionRegisterClientMethod, false, &m_reply, 1U))
    {
        return false;
    }
    const auto received =
        m_context->Supervisor->AwaitMessage(*m_compositor, m_reply, m_delivery, sizeof(m_delivery), m_error);
    if (!S::ReadSessionFrame(m_delivery, static_cast<Size>(received), m_message) || m_message.AttachmentCount != 1U)
    {
        return false;
    }
    m_client = m_message.Attachments[0U];
    if (m_memories[0U].Selector == 0U && !Pools())
    {
        return false;
    }
    for (Size pool{0U}; pool < 2U; ++pool)
    {
        Words(pool + 1U);
        m_words[1U] = m_pages;
        m_words[2U] = m_frameCount;
        for (Size i{0U}; i < m_frameCount; ++i)
        {
            m_words[4U + i / 4U] |= m_chunks[i] << ((i % 4U) * 16U);
        }
        const Size first = m_frameCount < 4U ? static_cast<Size>(m_frameCount) : 4U;
        if (!Send(m_client, S::SessionCreatePoolMethod, false, m_frames[pool], first))
        {
            return false;
        }
        for (Size i{first}; i < m_frameCount; i += 4U)
        {
            Words(pool + 1U);
            m_words[1U] = i;
            const Size count = m_frameCount - i < 4U ? static_cast<Size>(m_frameCount) - i : 4U;
            if (!Send(m_client, S::SessionAddPoolFramesMethod, false, m_frames[pool] + i, count))
            {
                return false;
            }
        }
    }
    Words(1U);
    m_words[1U] = S::SessionToplevelRole;
    if (!Send(m_client, S::SessionCreateSurfaceMethod))
    {
        return false;
    }
    Words(1U);
    m_words[1U] = S::PackSessionPair(font.GlyphWidth, font.GlyphHeight);
    m_words[2U] = S::PackSessionPair(m_context->Bootstrap->Scanout.Width, m_context->Bootstrap->Scanout.Height);
    m_words[3U] = S::PackSessionPair(font.GlyphWidth, font.GlyphHeight);
    m_words[4U] = S::PackSessionPair(0U, 0U);
    if (!Send(m_client, S::SessionSetSizeHintsMethod))
    {
        return false;
    }
    m_context->Supervisor->SetPoll(Poll, this);
    for (Size i{0U}; i < 6000U && !m_shown && !m_failed; ++i)
    {
        if (!m_credit)
        {
            Words(m_context->Display->Broker->Tag);
            m_words[1U] = S::SessionMaximumEvents;
            if (!Send(m_client, S::SessionCreditMethod, false, &m_events, 1U))
            {
                return false;
            }
            m_credit = true;
        }
        if (m_poolReady == 3U && m_dirty && !Present())
        {
            return false;
        }
        if (m_context->Supervisor->Step(10'000U, m_error) != PagerServeError::None)
        {
            return false;
        }
        if (StartupFailed())
        {
            return false;
        }
        if (m_compositor->State != ComponentState::Running)
        {
            return false;
        }
    }
    if (!m_shown || m_failed)
    {
        return false;
    }
    Words(1U);
    if (!Send(m_face, S::FaceShow, true) || (m_context->Bootstrap->LoginEnabled == 0U && !Focus(m_inputId)))
    {
        return false;
    }
    UInt64 epoch{0U};
    UInt64 sequence{0U};
    // FrameShown can arrive while the plane is hidden. Keep the startup
    // allowance until the Show's full uncached scanout is fenced too.
    if (!FencePointer(*m_context, epoch, sequence, m_error) || StartupFailed())
    {
        return false;
    }
    const bool compositorReady = m_context->Supervisor->MarkServiceReady(*m_compositor);
    const bool displayReady = m_context->Supervisor->MarkServiceReady(*m_context->Display->Broker);
    if (!compositorReady || !displayReady)
    {
        m_startupTimedOut = true;
        return false;
    }
    m_active = true;
    m_trusted = false;
    char line[160U]{};
    Inventory::ReportText text{line, sizeof(line)};
    text.Text("# session: desktop ");
    text.Decimal(m_context->Bootstrap->Scanout.Width);
    text.Character('x');
    text.Decimal(m_context->Bootstrap->Scanout.Height);
    text.Text(" output-pages=");
    text.Decimal(m_pages);
    text.Text(" client-pages=");
    text.Decimal(m_pages * 2U);
    text.Character('\n');
    WriteLine(*m_context, line, text.Bytes());
    StartToolkitPreview(*m_context, *m_compositor, m_reports);
    return true;
}
bool ProductDesktop::Present() noexcept
{
    const UInt32 available = (~m_lent) & 3U;
    if (available == 0U)
    {
        return true;
    }
    const UInt32 image = (available & 1U) != 0U ? 1U : 2U;
    const auto& mode = m_context->Bootstrap->Scanout;
    if (m_width == 0U || m_height == 0U || m_width > mode.Width || m_height > mode.Height)
    {
        return false;
    }
#if defined(ASTERIA_TARGET_IA64)
    constexpr UInt64 base{Abi::Ia64ComponentCompositorAddress};
#else
    constexpr UInt64 base{Abi::ComponentCompositorAddress};
#endif
    const Graphics::MemorySurface surface{.Pixels = reinterpret_cast<UInt32*>(static_cast<Foundation::UIntPointer>(
                                              base + (image - 1U) * S::AddressPoolSpan)),
                                          .Width = m_width,
                                          .Height = m_height,
                                          .Stride = m_width,
                                          .Format = static_cast<Graphics::PixelFormat>(mode.Format)};
    m_grid.Configure(m_width, m_height, Graphics::Bold8x16);
    m_grid.Write(reinterpret_cast<const Foundation::UInt8*>(m_log), m_logBytes);
    m_grid.RenderRegion(surface, {.X = 0, .Y = 0, .Width = m_width, .Height = m_height}, {.X = 0, .Y = 0});
    Words(image);
    m_words[1U] = image;
    m_words[3U] = S::PackSessionPair(m_width, m_height);
    m_words[4U] = m_width;
    m_words[5U] = mode.Format;
    if (!Send(m_client, S::SessionDefineImageMethod))
    {
        return false;
    }
    System::OrderDmaStorage();
    Words(1U);
    m_words[1U] = image;
    m_words[2U] = ++m_serial;
    m_words[3U] = S::SessionWantFrameShown;
    m_words[4U] = m_arrange;
    m_words[5U] = 1U;
    m_words[6U] = S::PackSessionDamage(0U, 0U, m_width, m_height);
    if (!Send(m_client, S::SessionPresentMethod))
    {
        return false;
    }
    m_lent |= 1U << (image - 1U);
    m_dirty = false;
    return true;
}
bool ProductDesktop::RebindInput(const bool retire) noexcept
{
    // The old thread is stopped. Fence its same client route, including
    // late queued credits, before clearing the retained reply/transaction.
    const UInt64 fenceWords[4U]{m_input->Tag, 0U, 0U, 0U};
    const auto fenceBytes = Component::WriteInputFrame(m_frame, Component::InputQueryMethod, ++m_transaction,
                                                       fenceWords, nullptr, 0U, &m_reply);
    if (m_context->Client->Send(m_inputClient, System::AddressOf(m_frame), fenceBytes).Error !=
        Abi::SystemCallError::None)
    {
        return false;
    }
    const auto fenced = m_context->Supervisor->AwaitMessage(*m_input, m_reply, m_delivery, sizeof(m_delivery), m_error);
    if (!Component::ReadInputFrame(m_delivery, static_cast<Size>(fenced), m_inputMessage) ||
        m_inputMessage.Transaction != m_transaction ||
        m_inputMessage.Words[1U] != static_cast<UInt64>(Component::InputAnswer::Done))
    {
        return false;
    }
    const UInt64 words[4U]{m_input->Tag, m_inputId, retire ? 1U : 0U, 0U};
    const auto bytes = Component::WriteInputFrame(m_frame, Component::InputRebindClientMethod, ++m_transaction, words,
                                                  nullptr, 0U, &m_reply);
    if (m_context->Client->Send(m_input->Service, System::AddressOf(m_frame), bytes).Error !=
        Abi::SystemCallError::None)
    {
        return false;
    }
    const auto received =
        m_context->Supervisor->AwaitMessage(*m_input, m_reply, m_delivery, sizeof(m_delivery), m_error);
    if (!Component::ReadInputFrame(m_delivery, static_cast<Size>(received), m_inputMessage) ||
        m_inputMessage.Transaction != m_transaction ||
        m_inputMessage.Words[1U] != static_cast<UInt64>(Component::InputAnswer::Done) ||
        (!retire && m_inputMessage.Reply.Selector == 0U))
    {
        return false;
    }
    if (!m_context->Calls->Close(m_inputClient, m_error))
    {
        return false;
    }
    m_inputClient = retire ? CapabilityHandle{} : m_inputMessage.Reply;
    return true;
}
bool ProductDesktop::StopSession() noexcept
{
    m_active = false;
    StopToolkitPreview(*m_context);
    if (m_compositor != nullptr)
    {
        m_context->Supervisor->UnwatchService(*m_compositor);
        if (m_compositor->State == ComponentState::Running &&
            m_context->Supervisor->Stop(*m_compositor, m_error) != PagerServeError::None)
        {
            return false;
        }
        if (m_context->Display->Broker != nullptr && m_faceEpoch == m_context->Display->Broker->Tag)
        {
            Words(m_faceEpoch);
            if (!Send(m_face, S::FaceFence, true, &m_reply, 1U))
            {
                return false;
            }
            const auto fenced = m_context->Supervisor->AwaitMessage(*m_context->Display->Broker, m_reply, m_delivery,
                                                                    sizeof(m_delivery), m_error);
            if (!S::ReadSessionFaceFrame(m_delivery, static_cast<Size>(fenced), m_faceMessage) ||
                m_faceMessage.Transaction != m_transaction ||
                m_faceMessage.Words[1U] != static_cast<UInt64>(S::SessionAnswer::Done))
            {
                return false;
            }
            Words(m_faceEpoch);
            if (!Send(m_control, S::FaceDetach, true))
            {
                return false;
            }
            UInt64 epoch{0U};
            UInt64 sequence{0U};
            if (!FencePointer(*m_context, epoch, sequence, m_error))
            {
                return false;
            }
        }
        if (!m_context->Supervisor->Teardown(*m_compositor, m_error))
        {
            return false;
        }
        m_compositor = nullptr;
    }
    CollectToolkitPreview(*m_context);
    if (m_client.Selector != 0U)
    {
        static_cast<void>(m_context->Calls->Close(m_client, m_error));
        m_client = {};
    }
    // Discard the old event channel and its retained late answers on restart.
    if (m_events.Selector != 0U)
    {
        static_cast<void>(m_context->Calls->Destroy(m_events, Abi::ObjectTypeEndpoint, m_error));
        m_events = {};
    }
    return true;
}
bool ProductDesktop::ReleasePools() noexcept
{
#if defined(ASTERIA_TARGET_IA64)
    constexpr UInt64 base{Abi::Ia64ComponentCompositorAddress};
#else
    constexpr UInt64 base{Abi::ComponentCompositorAddress};
#endif
    for (Size pool{0U}; pool < 2U; ++pool)
    {
        if (m_mappedPages[pool] != 0U &&
            m_context->Client->UnmapMemory(m_context->OwnSpace, base + pool * S::AddressPoolSpan, m_mappedPages[pool])
                    .Error != Abi::SystemCallError::None)
        {
            return false;
        }
        m_mappedPages[pool] = 0U;
        for (auto& frame : m_frames[pool])
        {
            if (frame.Selector != 0U)
            {
                if (!m_context->Calls->Destroy(frame, Abi::ObjectTypeFrame, m_error))
                {
                    return false;
                }
                frame = {};
            }
        }
        if (m_memories[pool].Selector != 0U)
        {
            if (!m_context->Calls->Destroy(m_memories[pool], Abi::ObjectTypeMemoryObject, m_error))
            {
                return false;
            }
            m_memories[pool] = {};
        }
    }
    return true;
}
bool ProductDesktop::SeatStop() noexcept
{
    // Settle the broker's borrowed pixels before
    // releasing pools and the old session faces.
    if (!StopSession())
    {
        return false;
    }
    // Logout uses the same stopped-receiver fence and fresh cumulative baseline as compositor
    // recovery before seat reuse.
    if (m_inputClient.Selector != 0U && !RebindInput())
    {
        return false;
    }
    Words();
    if (m_face.Selector != 0U && !Send(m_face, S::FaceDrop, true))
    {
        return false;
    }
    if (!ReleasePools())
    {
        return false;
    }
    if (m_face.Selector != 0U && !m_context->Calls->Close(m_face, m_error))
    {
        return false;
    }
    if (m_control.Selector != 0U && !m_context->Calls->Close(m_control, m_error))
    {
        return false;
    }
    m_face = {};
    m_control = {};
    m_active = false;
    // A fully drained partial session permits a new authenticated startup. No fallback state
    // survives this seat teardown.
    m_fallback = false;
    m_failed = false;
    return true;
}
void ProductDesktop::Fallback(const char* const reason) noexcept
{
    if (m_fallback)
    {
        return;
    }
    m_fallback = true;
    m_active = false;
    if (m_face.Selector != 0U)
    {
        const bool stopped = StopSession();
        if (m_context->Display->Broker != nullptr && m_faceEpoch == m_context->Display->Broker->Tag)
        {
            Words();
            static_cast<void>(Send(m_face, S::FaceDrop, true));
        }
        if (stopped)
        {
            static_cast<void>(ReleasePools());
        }
    }
    m_context->Supervisor->SetPoll(nullptr, nullptr);
    if (m_context->Display->Broker != nullptr)
    {
        static_cast<void>(m_context->Supervisor->MarkServiceReady(*m_context->Display->Broker));
    }
    if (m_input != nullptr && m_reply.Selector != 0U)
    {
        if (m_inputClient.Selector != 0U && m_compositor == nullptr)
        {
            static_cast<void>(RebindInput(true));
        }
        static_cast<void>(Focus(m_context->Bootstrap->LoginEnabled != 0U ? 0U : 1U, true));
    }
    char line[96U]{};
    Inventory::ReportText text{line, sizeof(line)};
    text.Text("# session: fallback ");
    text.Text(reason);
    text.Character('\n');
    WriteLine(*m_context, line, text.Bytes());
}
bool ProductDesktop::Recover(void* const pointer, ComponentRecord& component, Abi::SystemCallError& error) noexcept
{
    auto& self = *static_cast<ProductDesktop*>(pointer);
    // The event loop performs detach/drain/restart outside PollLiveness.
    const bool stopped = self.m_context->Supervisor->Stop(component, error) == PagerServeError::None;
    self.m_failed = true;
    return stopped;
}
void ProductDesktop::Start(const ComponentRecord& input) noexcept
{
    if (m_context->Bootstrap->SessionMode != Abi::SessionDesktop)
    {
        return;
    }
    m_input = &input;
    if ((m_reply.Selector == 0U && !m_context->Calls->CreateEndpoint(m_context->Management, m_reply, m_error)) ||
        (m_attention.Selector == 0U && !m_context->Calls->CreateEndpoint(m_context->Management, m_attention, m_error)))
    {
        Fallback("admission");
        return;
    }
    if (m_fallback)
    {
        return;
    }
    const auto& mode = m_context->Bootstrap->Scanout;
    if (m_context->Display->Broker == nullptr || mode.Width < S::FrameExtraWidth + Graphics::Bold8x16.GlyphWidth ||
        mode.Height < S::FrameExtraHeight + Graphics::Bold8x16.GlyphHeight)
    {
        Fallback("no-framebuffer");
        return;
    }
    const bool found = FindComponentImage(m_context->Launch->ComponentImages, m_context->Launch->ComponentImagesBytes,
                                          "compositor", m_image.Bytes, m_image.Size);
    if (!found || Loader::DescribeExecutable(m_image.Bytes, m_image.Size, m_context->Launch->Component.Image,
                                             m_image.Layout) != Loader::LoaderError::None)
    {
        Fallback("compositor-image");
        return;
    }
    if ((m_events.Selector == 0U && !m_context->Calls->CreateEndpoint(m_context->Management, m_events, m_error)) ||
        (m_reports.Selector == 0U && !m_context->Calls->CreateEndpoint(m_context->Management, m_reports, m_error)))
    {
        Fallback("admission");
        return;
    }
    Words(m_context->Display->Broker->Tag);
    if (!Send(m_context->Display->Console, S::FaceOpen, true, &m_reply, 1U))
    {
        Fallback("session-face");
        return;
    }
    const auto received = m_context->Supervisor->AwaitMessage(*m_context->Display->Broker, m_reply, m_delivery,
                                                              sizeof(m_delivery), m_error);
    if (!S::ReadSessionFaceFrame(m_delivery, static_cast<Size>(received), m_faceMessage) ||
        m_faceMessage.AttachmentCount != 2U)
    {
        Fallback("session-face");
        return;
    }
    m_faceEpoch = m_context->Display->Broker->Tag;
    m_face = m_faceMessage.Attachments[0U];
    m_control = m_faceMessage.Attachments[1U];
    if (m_inputClient.Selector == 0U)
    {
        const UInt64 words[4U]{input.Tag, Component::InputBrokerPointer, 0U, 0U};
        const auto bytes = Component::WriteInputFrame(m_frame, Component::InputRegisterClientMethod, ++m_transaction,
                                                      words, nullptr, 0U, &m_reply);
        if (m_context->Client->Send(input.Service, System::AddressOf(m_frame), bytes).Error !=
            Abi::SystemCallError::None)
        {
            Fallback("input-client");
            return;
        }
        const auto registered =
            m_context->Supervisor->AwaitMessage(input, m_reply, m_delivery, sizeof(m_delivery), m_error);
        const bool decoded = Component::ReadInputFrame(m_delivery, static_cast<Size>(registered), m_inputMessage);
        if (!decoded || m_inputMessage.Reply.Selector == 0U)
        {
            char line[128U]{};
            Inventory::ReportText text{line, sizeof(line)};
            text.Text("# session: input registration bytes=");
            text.Decimal(registered);
            text.Text(" status=");
            text.Decimal(m_inputMessage.Words[1U]);
            text.Text(" error=");
            text.Decimal(static_cast<UInt64>(m_error));
            text.Character('\n');
            WriteLine(*m_context, line, text.Bytes());
            Fallback("input-client");
            return;
        }
        m_inputClient = m_inputMessage.Reply;
        m_inputId = m_inputMessage.Words[2U];
    }
    if (!StartSession())
    {
        Fallback(StartupFailed() ? "startup-timeout" : "admission");
    }
}
bool ProductDesktop::Step(Abi::SystemCallError& error) noexcept
{
    if (m_input == nullptr || m_attention.Selector == 0U)
    {
        return true;
    }
    if (m_context->Bootstrap->LoginEnabled == 0U && !m_attentionCredit)
    {
        const UInt64 words[4U]{m_input->Tag, 0U, 0U, 0U};
        const auto bytes = Component::WriteInputFrame(m_frame, Component::InputAttentionCreditMethod, ++m_transaction,
                                                      words, nullptr, 0U, &m_attention);
        const auto sent = m_context->Client->Send(m_input->Service, System::AddressOf(m_frame), bytes).Error;
        if (sent == Abi::SystemCallError::None)
        {
            m_attentionCredit = true;
        }
        else if (sent != Abi::SystemCallError::WouldBlock)
        {
            error = sent;
            return false;
        }
    }
    const auto attention =
        m_context->Bootstrap->LoginEnabled == 0U
            ? m_context->Client->Receive(m_attention, System::AddressOf(m_delivery), sizeof(m_delivery))
            : Abi::SystemCallResponse{.Error = Abi::SystemCallError::WouldBlock, .Value0 = 0U, .Value1 = 0U};
    if (attention.Error == Abi::SystemCallError::None &&
        Component::ReadInputFrame(m_delivery, static_cast<Size>(attention.Value1), m_inputMessage))
    {
        m_attentionCredit = false;
        if (m_inputMessage.Method == Component::InputAttentionMethod)
        {
            const auto usage = m_inputMessage.Words[2U];
            if (usage != 0U)
            {
                if (usage == 66U)
                {
                    static_cast<void>(PreparePowerTestVolume(*m_context, error));
                }
                else
                {
                    return RequestPower(*m_context, usage == 68U, error);
                }
            }
            else
            {
                WriteLine(*m_context, "# session: secure attention\n", sizeof("# session: secure attention\n") - 1U);
                if (m_active)
                {
                    m_trusted = !m_trusted;
                    if (!Focus(m_trusted ? 1U : m_inputId, m_trusted))
                    {
                        return false;
                    }
                    Words(m_trusted ? 0U : 1U);
                    m_words[1U] = m_trusted ? 1U : 0U;
                    if (!Send(m_face, S::FaceShow, true))
                    {
                        return false;
                    }
                    UInt64 epoch{0U};
                    UInt64 sequence{0U};
                    if (!FencePointer(*m_context, epoch, sequence, error))
                    {
                        return false;
                    }
                    if (m_trusted)
                    {
                        WriteLine(*m_context, "# session: trusted console\n",
                                  sizeof("# session: trusted console\n") - 1U);
                    }
                }
            }
        }
    }
    if (m_active && (m_context->Display->Broker == nullptr || m_faceEpoch != m_context->Display->Broker->Tag))
    {
        Fallback("display");
        static_cast<void>(SetPointerRoute(*m_context, *m_input, error));
    }
    StepToolkitPreview(*m_context);
    if (!m_active)
    {
        return true;
    }
    if (m_closed)
    {
        Fallback("client-closed");
        return true;
    }
    if (m_failed || m_compositor->State != ComponentState::Running)
    {
        if (m_recovering)
        {
            return true;
        }
        m_recovering = true;
        if (++m_deaths >= Abi::DesktopCompositorDeathLimit || !StopSession() || !RebindInput() || !StartSession())
        {
            Fallback(StartupFailed() ? "startup-timeout" : "compositor");
        }
        m_recovering = false;
        return true;
    }
    if (!m_credit)
    {
        Words(m_context->Display->Broker->Tag);
        m_words[1U] = S::SessionMaximumEvents;
        if (!Send(m_client, S::SessionCreditMethod, false, &m_events, 1U))
        {
            Fallback("client");
            return true;
        }
        m_credit = true;
    }
    if (m_dirty && !Present())
    {
        Fallback("client");
    }
    return true;
}
} // Namespace
bool FocusProductDesktop() noexcept
{
    return g_desktop.SeatFocus();
}
bool StopProductDesktop() noexcept
{
    return g_desktop.SeatStop();
}
bool ProductDesktopActive() noexcept
{
    return g_desktop.Active();
}
void ConfigureProductDesktop(ScenarioContext& context) noexcept
{
    g_desktop.Configure(context);
}
void FallbackProductDesktop(ScenarioContext&, const char* const reason) noexcept
{
    g_desktop.Fail(reason);
}
void StartProductDesktop(ScenarioContext&, const ComponentRecord& input) noexcept
{
    g_desktop.Start(input);
}
bool StepProductDesktop(ScenarioContext&, Abi::SystemCallError& error) noexcept
{
    return g_desktop.Step(error);
}
} // Namespace Asteria::User::Pager
