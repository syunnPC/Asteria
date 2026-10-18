// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_SESSION_COMPOSITOR_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_SESSION_COMPOSITOR_HPP_INCLUDED
#include "Abi/ComponentBootstrap.hpp"
#include "User/Session/UiText.hpp"
#include "User/Session/AddressPlan.hpp"
#include "User/Session/WindowManager.hpp"
#include "User/Session/SessionFaceFrame.hpp"
#include "User/Session/InputOrder.hpp"
#include "User/Component/InputFrame.hpp"
namespace Asteria::User::Component
{
static_assert(Abi::ComponentUiFontPages * Session::SessionPageBytes <= Session::AddressGranule);
// The owner lives in static storage in COMPOSIT.ELF; all borrowed pool views are read-only and
// fixed-slot mapped.
inline constexpr Foundation::UInt32 SessionOutputWidth{Abi::ComponentSessionTestWidth};
inline constexpr Foundation::UInt32 SessionOutputHeight{Abi::ComponentSessionTestHeight};
inline constexpr Foundation::UInt64 SessionOutputPages{(SessionOutputWidth * SessionOutputHeight * 4U + 4095U) / 4096U};
inline constexpr Foundation::UInt64 SessionComposeIntervalUs{16000U};
// Root-started test media alone enables the Frame write-authority probe.
inline constexpr Foundation::UInt64 SessionTestPoolWriteFlag{1ULL << 32U};
inline constexpr Foundation::UInt64 SessionTestSlowPresentFlag{1ULL << 34U};
inline constexpr Foundation::UInt64 SessionTestPresentDelayUs{200000U};
static_assert(SessionOutputPages == Abi::ComponentCompositorPages);
using SessionClock = Foundation::UInt64 (*)() noexcept;
class SessionCompositor final
{
public:
    [[nodiscard]] bool Start(const System::SystemCallClient& client,
                             const Foundation::UInt64 (&block)[Abi::ComponentBlockWords], SessionClock clock,
                             Foundation::UInt64 regionBase = 0U) noexcept;
    [[nodiscard]] bool Step(const System::SystemCallClient& client) noexcept;

private:
    System::CapabilityHandle m_watchdogReply{};
    Foundation::UInt64 m_watchdogTransaction{};
    struct Pool
    {
        System::CapabilityHandle Memory{};
        System::CapabilityHandle Frames[Session::SessionMaximumPoolFrames]{};
        Foundation::UInt32 Pages[Session::SessionMaximumPoolFrames]{};
        Foundation::UInt64 Offsets[Session::SessionMaximumPoolFrames]{};
        Foundation::UInt64 Total{0U};
        Foundation::Size Count{0U};
        Foundation::Size Mapped{0U};
        bool Destroy{false};
    };
    struct Image
    {
        Session::ImageView View{};
        Foundation::UInt64 Pool{0U};
        bool Forget{false};
    };
    struct Surface
    {
        Foundation::UInt64 Image{0U};
        Foundation::UInt64 Serial{0U};
        Foundation::UInt64 Pending{0U};
        Foundation::UInt64 Composed{0U};
        Session::SizeHints Hints{};
        bool Created{false};
        bool HasHints{false};
    };
    struct Client
    {
        System::CapabilityHandle Endpoint{};
        System::CapabilityHandle Credit{};
        System::CapabilityHandle Reply{};
        Foundation::UInt64 CreditTransaction{0U};
        Foundation::UInt64 ReplyTransaction{0U};
        Foundation::UInt64 ReplyValue{0U};
        Foundation::UInt64 Generation{0U};
        Foundation::UInt64 Launch{0U};
        Foundation::UInt64 Class{0U};
        Pool Pools[Session::SessionMaximumPools]{};
        Image Images[Session::SessionMaximumImages]{};
        Surface Surfaces[Session::SessionMaximumSurfaces]{};
        Foundation::UInt8 Name[Session::SessionMaximumNameBytes]{};
        Foundation::UInt8 Subtitle[Session::SessionMaximumSurfaces][Session::SessionMaximumSubtitleBytes]{};
        Foundation::Size NameLength{0U};
        Foundation::Size SubtitleLength[Session::SessionMaximumSurfaces]{};
    };
    struct Snapshot
    {
        Foundation::Size Slot{0U};
        Graphics::Rect Frame{};
        Session::WindowLook Look{};
        Foundation::UInt64 Class{0U};
        Foundation::UInt64 Serial{0U};
        Foundation::UInt32 Width{0U};
        Foundation::UInt32 Height{0U};
        Foundation::UInt8 Name[Session::SessionMaximumNameBytes]{};
        Foundation::UInt8 Subtitle[Session::SessionMaximumSubtitleBytes]{};
        Foundation::Size NameLength{0U};
        Foundation::Size SubtitleLength{0U};
    };
    [[nodiscard, gnu::noinline]] bool LoadUiFonts(const System::SystemCallClient& client,
                                                  const Foundation::UInt64 (&block)[Abi::ComponentBlockWords]) noexcept;
    [[nodiscard]] Foundation::UInt64 Now() const noexcept;
    [[nodiscard]] bool Receive(const System::SystemCallClient& client, Foundation::Size route) noexcept;
    [[nodiscard]] bool Manager(const System::SystemCallClient& client) noexcept;
    [[nodiscard]] bool Request(const System::SystemCallClient& client, Foundation::Size slot) noexcept;
    [[nodiscard]] bool Add(const System::SystemCallClient& client, Foundation::Size slot, Foundation::Size pool,
                           Foundation::Size first) noexcept;
    [[nodiscard]] bool ReleasePool(const System::SystemCallClient& client, Foundation::Size slot,
                                   Foundation::Size pool) noexcept;
    [[nodiscard]] bool Drop(const System::SystemCallClient& client, Foundation::Size slot) noexcept;
    void ReleaseImage(Foundation::Size slot, Foundation::UInt64 image) noexcept;
    [[nodiscard]] bool Lent(Foundation::Size slot, Foundation::UInt64 image) const noexcept;
    [[nodiscard]] bool Settle(const System::SystemCallClient& client, Foundation::Size slot) noexcept;
    void Event(Foundation::Size slot, Session::SessionEventKind kind, Foundation::UInt64 a = 0U,
               Foundation::UInt64 b = 0U, Foundation::UInt64 c = 0U, Foundation::UInt64 d = 0U) noexcept;
    void Refuse(Foundation::Size slot, Session::SessionRefusal reason) noexcept;
    void Answers(const System::SystemCallClient& client) noexcept;
    [[nodiscard]] bool Compose(const System::SystemCallClient& client) noexcept;
    [[nodiscard]] bool InputCredits(const System::SystemCallClient& client) noexcept;
    void Keys(const InputMessage& message) noexcept;
    void Pointers() noexcept;
    void ApplyInput() noexcept;
    void Capture() noexcept;
    void Record(bool queued) noexcept;
    void Log(const System::SystemCallClient& client) noexcept;
    void CloseAttachments(const System::SystemCallClient& client, Foundation::Size bytes) noexcept;
    Client m_clients[Session::SessionMaximumClients]{};
    Session::ClientEvents m_events[Session::SessionMaximumClients]{};
    Session::UiText m_ui{};
    Session::Scene m_scene{};
    Session::WindowManager m_manager{};
    bool m_inputResumed{false};
    Foundation::UInt8 m_pointerSuppressed{0U};
    Session::InputOrder m_order{};
    static constexpr Foundation::Size InputCapacity{Session::InputOrder::Capacity};
    Input::InputEvent m_keys[InputCapacity]{};
    Foundation::UInt64 m_pointers[InputCapacity][4U]{};
    Foundation::Size m_keyFirst{0U};
    Foundation::Size m_keyCount{0U};
    Foundation::Size m_pointerFirst{0U};
    Foundation::Size m_pointerCount{0U};
    System::CapabilityHandle m_input{};
    Foundation::UInt64 m_inputEpoch{0U};
    Foundation::UInt64 m_keyTransaction{0U};
    Foundation::UInt64 m_pointerTransaction{0U};
    Foundation::UInt64 m_inputDropped{0U};
    bool m_keyCredit{false};
    bool m_pointerCredit{false};
    bool m_inputLive{false};
    bool m_faceLive{false};
    System::CapabilityHandle m_domain{};
    System::CapabilityHandle m_space{};
    System::CapabilityHandle m_wait{};
    System::CapabilityHandle m_main{};
    System::CapabilityHandle m_reply{};
    System::CapabilityHandle m_face{};
    System::CapabilityHandle m_report{};
    System::CapabilityHandle m_outputFrames[Session::SessionMaximumPoolFrames]{};
    System::CapabilityHandle m_outputMemory{};
    System::CapabilityHandle m_refusalReply{};
    Foundation::UInt64 m_refusalTransaction{0U};
    Graphics::MemorySurface m_output{};
    SessionClock m_clock{nullptr};
    Foundation::UInt64 m_scale{0U};
    Foundation::UInt64 m_epoch{0U};
    Foundation::UInt64 m_base{0U};
    Foundation::UInt64 m_serial{0U};
    Foundation::UInt64 m_lastCompose{0U};
    Foundation::UInt64 m_passUs{0U};
    Foundation::UInt64 m_passPixels{0U};
    Foundation::UInt64 m_passStarted{0U};
    Foundation::UInt64 m_presentedAt{0U};
    bool m_fullPass{false};
    bool m_failed{false};
    bool m_fontWriteRefused{false};
    bool m_fontMapped{false};
    bool m_testPoolWrite{false};
    bool m_poolWriteRefused{false};
    bool m_testSlowPresent{false};
    bool m_pending{false};
    bool m_sent{false};
    bool m_logging{false};
    bool m_logQueued{false};
    Foundation::Size m_logIndex{0U};
    Foundation::Size m_snapshotCount{0U};
    Foundation::Size m_captureCount{0U};
    Foundation::Size m_queuedCount{0U};
    Foundation::UInt64 m_logSerial{0U};
    Foundation::UInt64 m_logPixels{0U};
    Foundation::UInt64 m_logUs{0U};
    Foundation::UInt64 m_logDropped{0U};
    Foundation::UInt64 m_queuedSerial{0U};
    Foundation::UInt64 m_queuedPixels{0U};
    Foundation::UInt64 m_queuedUs{0U};
    Foundation::UInt64 m_queuedDropped{0U};
    Snapshot m_snapshots[Session::Scene::MaximumWindows]{};
    Snapshot m_captured[Session::Scene::MaximumWindows]{};
    Snapshot m_queued[Session::Scene::MaximumWindows]{};
    Session::SessionMessage m_message{};
    Session::SessionFaceMessage m_faceMessage{};
    Session::SessionEvent m_answer[Session::SessionMaximumEvents]{};
    Graphics::Rect m_rectangles[Session::SessionMaximumDamage]{};
    Foundation::UInt64 m_words[Session::SessionMaximumWords]{};
    alignas(8) Foundation::UInt8 m_delivery[Session::SessionDeliveryCapacity]{},
        m_frame[Session::SessionKernelFrameLimit]{};
    Foundation::Size m_presentBytes{0U};
    alignas(8) Foundation::UInt8 m_present[Session::SessionKernelFrameLimit]{};
    char m_line[Abi::DebugWriteMaximumBytes]{};
};
[[nodiscard]] Foundation::UInt64 ServeSession(const System::SystemCallClient& client,
                                              const Foundation::UInt64 (&block)[Abi::ComponentBlockWords],
                                              SessionClock clock) noexcept;
} // Namespace Asteria::User::Component
#endif
