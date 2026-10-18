// SPDX-License-Identifier: Apache-2.0
#include "Guest/Pager/TestRootService.hpp"
#include "User/Session/SessionFrame.hpp"
#include "User/Loader/ExecutableFormats.hpp"
#include "User/Session/SessionFaceFrame.hpp"
#include "User/Component/SessionCompositor.hpp"
#include "User/Component/Ps2Driver.hpp"
#include "Inventory/ReportText.hpp"
namespace Asteria::User::Pager
{
namespace
{
constinit LoadedImage g_compositorImage{};
constinit LoadedImage g_patternImage{};
constinit LoadedImage g_seatImage{};
constinit ComponentRecord* g_startingCompositor{nullptr};
constinit Component::InputMessage g_inputAnswer{};
constinit Foundation::UInt64 g_signal{0U};
constinit Foundation::UInt64 g_shown[2U]{};
bool g_sceneOpen{false};
System::CapabilityHandle g_secondClient{};
Foundation::UInt64 g_secondId{0U};
char g_phaseLine[128U]{};
constinit Session::SessionMessage g_clientAnswer{};
constinit Session::SessionFaceMessage g_faceAnswer{};
constinit Foundation::UInt64 g_words[32U]{};
constinit Component::DisplayMessage g_report{};
alignas(8) constinit Foundation::UInt8 g_reportBytes[PagerProgram::DeliveryCapacityBytes]{};
void Reports(void* pointer) noexcept
{
    auto& context = *static_cast<ScenarioContext*>(pointer);
    for (Foundation::Size i{0U}; i < 16U; ++i)
    {
        const auto received =
            context.Client->Receive(context.Report, System::AddressOf(g_reportBytes), sizeof(g_reportBytes));
        if (received.Error != Abi::SystemCallError::None)
        {
            return;
        }
        if (Component::ReadDisplayFrame(g_reportBytes, static_cast<Foundation::Size>(received.Value1), g_report) &&
            g_report.Method == Component::DisplayConsoleWriteMethod)
        {
            static_cast<void>(
                context.Client->DebugWrite(context.Diagnostic, System::AddressOf(g_report.Text), g_report.Count));
            constexpr char begin[]{"# session: begin "};
            constexpr char presented[]{"# session: presented "};
            const auto starts = [&](const char* text, const Foundation::Size count) noexcept
            {
                if (g_report.Count < count)
                {
                    return false;
                }
                for (Foundation::Size j{0U}; j < count; ++j)
                {
                    if (g_report.Text[j] != text[j])
                    {
                        return false;
                    }
                }
                return true;
            };
            if (starts(begin, sizeof(begin) - 1U))
            {
                g_sceneOpen = true;
            }
            if (starts(presented, sizeof(presented) - 1U))
            {
                g_sceneOpen = false;
                if (g_startingCompositor != nullptr)
                {
                    static_cast<void>(context.Supervisor->MarkServiceReady(*g_startingCompositor));
                    g_startingCompositor = nullptr;
                }
            }
            // Test fence key F12, acknowledged in the receiving window's
            // diagnostic transcript. Parse bounded decimal fields only.
            constexpr char prefix[]{"# window-event: "};
            bool ours = g_report.Count > sizeof(prefix);
            for (Foundation::Size j{0U}; ours && j < sizeof(prefix) - 1U; ++j)
            {
                ours = g_report.Text[j] == prefix[j];
            }
            Foundation::UInt64 values[7U]{};
            Foundation::Size field{0U};
            for (Foundation::Size j{sizeof(prefix) - 1U}; ours && j < g_report.Count && field < 7U; ++j)
            {
                const auto c = g_report.Text[j];
                if (c >= '0' && c <= '9')
                {
                    values[field] = values[field] * 10U + c - '0';
                }
                else if (c == ' ' || c == '\n')
                {
                    ++field;
                }
                else
                {
                    ours = false;
                }
            }
            if (ours && values[1U] == static_cast<Foundation::UInt64>(Session::SessionEventKind::Key) &&
                values[3U] == Session::SessionKeyDown && values[4U] == 0x45U)
            {
                ++g_signal;
            }
            if (ours && values[1U] == static_cast<Foundation::UInt64>(Session::SessionEventKind::FrameShown) &&
                values[2U] >= 1U && values[2U] <= 2U)
            {
                g_shown[values[0U] == 2U ? 1U : values[2U] - 1U] = values[3U];
            }
        }
    }
}
// Pause serves one wake and may return early for a fault. Fence delays must
// measure the ISA clock while continuing the root's fault/report drain.
[[nodiscard]] bool Delay(ScenarioContext& context, const Foundation::UInt64 us, Abi::SystemCallError& error) noexcept
{
    const auto clock = context.Launch->Component.Clock;
    if (clock == nullptr || context.Bootstrap->ClockTicksPerMicrosecond == 0U)
    {
        return false;
    }
    const auto start = clock();
    const auto ticks = us * context.Bootstrap->ClockTicksPerMicrosecond;
    for (Foundation::Size poll{0U}; poll < 6000U; ++poll)
    {
        if (clock() - start >= ticks)
        {
            return true;
        }
        if (context.Supervisor->Pause(10000U, error) != PagerServeError::None)
        {
            return false;
        }
    }
    return false;
}
void Words(const Foundation::UInt64 first = 0U) noexcept
{
    for (auto& w : g_words)
    {
        w = 0U;
    }
    g_words[0U] = first;
}
[[nodiscard]] bool Send(ScenarioContext& context, const System::CapabilityHandle target,
                        const Foundation::UInt32 method, const Foundation::UInt64 transaction, const bool face,
                        const System::CapabilityHandle* handles, const Foundation::Size count,
                        Abi::SystemCallError& error) noexcept
{
    const auto bytes = face
                           ? Session::WriteSessionFaceFrame(context.Frame, method, transaction, g_words, handles, count)
                           : Session::WriteSessionFrame(context.Frame, method, transaction, g_words, handles, count);
    if (bytes == 0U)
    {
        return false;
    }
    for (Foundation::Size i{0U}; i < 200U; ++i)
    {
        const auto sent = context.Client->Send(target, System::AddressOf(context.Frame), bytes);
        if (sent.Error != Abi::SystemCallError::WouldBlock)
        {
            return Check(sent, error);
        }
        if (context.Supervisor->Pause(10'000U, error) != PagerServeError::None)
        {
            return false;
        }
    }
    return false;
}
[[nodiscard]] bool Reply(ScenarioContext& context, const ComponentRecord& component,
                         const Foundation::UInt64 transaction, Abi::SystemCallError& error) noexcept
{
    const auto received = AwaitMessage(context, component, context.Reply, error);
    return Session::ReadSessionFrame(context.Delivery, static_cast<Foundation::Size>(received), g_clientAnswer) &&
           g_clientAnswer.Method == Session::SessionReplyMethod && g_clientAnswer.Transaction == transaction &&
           g_clientAnswer.Words[1U] == static_cast<Foundation::UInt64>(Session::SessionAnswer::Done);
}
[[nodiscard]] bool Describe(ScenarioContext& context, const char* name, LoadedImage& image) noexcept
{
    return FindComponentImage(context.Launch->ComponentImages, context.Launch->ComponentImagesBytes, name, image.Bytes,
                              image.Size) &&
           Loader::DescribeExecutable(image.Bytes, image.Size, context.Launch->Component.Image, image.Layout) ==
               Loader::LoaderError::None;
}
// Desktop session: all eight maximum pools plus maximum output, mapped twice at their fixed slots.
// The runner observes the tree/ledger.
[[nodiscard, gnu::noinline]] bool ArenaMaxima(ScenarioContext& context, ObjectCalls& calls,
                                              Abi::SystemCallError& error) noexcept
{
    static System::CapabilityHandle frames[9U][Session::SessionMaximumPoolFrames]{};
    static System::CapabilityHandle memories[9U]{};
    static_assert(Session::SessionMaximumPoolFrames * Session::SessionMaximumFramePages ==
                  Session::SessionMaximumPoolPages);
    System::CapabilityHandle space{};
    calls.ClearRequest();
    calls.GetRequest()[0U] = Abi::ObjectCreateAddressSpaceFlagLargeArena;
    if (!calls.Create(context.Management, Abi::ObjectTypeAddressSpace, Abi::ObjectCreateAddressSpaceRequestSizeBytes,
                      space, error))
    {
        return false;
    }
    for (Foundation::Size i{0U}; i < 9U; ++i)
    {
        if (!calls.CreateMemory(context.Management, Abi::ObjectTypeMemoryObject, Session::SessionMaximumPoolPages,
                                System::ReadWriteUser, memories[i], error))
        {
            return false;
        }
        for (Foundation::Size frame{0U}; frame < Session::SessionMaximumPoolFrames; ++frame)
        {
            calls.ClearRequest();
            System::StoreWord(calls.GetRequest(), Session::SessionMaximumFramePages);
            calls.GetRequest()[Abi::ObjectCreateRamFrameLimitOffset] = 32U;
            if (!calls.Create(context.Management, Abi::ObjectTypeFrame, Abi::ObjectCreateRamFrameRequestSizeBytes,
                              frames[i][frame], error))
            {
                return false;
            }
        }
    }
#if defined(ASTERIA_TARGET_IA64)
    constexpr auto base = Abi::Ia64ComponentCompositorAddress;
#else
    constexpr auto base = Abi::ComponentCompositorAddress;
#endif
    for (Foundation::Size round{0U}; round < 2U; ++round)
    {
        for (Foundation::Size i{0U}; i < 9U; ++i)
        {
            const auto offset =
                i == 8U ? Session::AddressOutputOffset
                        : Session::AddressPlan::Offset(i / 2U, static_cast<Foundation::UInt32>(i % 2U + 1U));
            for (Foundation::Size frame{0U}; frame < Session::SessionMaximumPoolFrames; ++frame)
            {
                calls.ClearRequest();
                auto* const request = calls.GetRequest();
                const auto page = frame * Session::SessionMaximumFramePages;
                System::StoreHandle(request + Abi::FaultResolveAddressSpaceOffset, space);
                System::StoreHandle(request + Abi::FaultResolveMemoryObjectOffset, memories[i]);
                System::StoreHandle(request + Abi::FaultResolveFrameOffset, frames[i][frame]);
                System::StoreWord(request + Abi::FaultResolveVirtualStartOffset,
                                  base + offset + page * Session::SessionPageBytes);
                System::StoreWord(request + Abi::FaultResolvePageCountOffset, Session::SessionMaximumFramePages);
                System::StoreWord(request + Abi::FaultResolveMemoryObjectPageOffset, page);
                request[Abi::FaultResolvePermissionsOffset] = System::ReadWriteUser;
                if (!Check(context.Client->MapMemory(System::AddressOf(request)), error))
                {
                    return false;
                }
            }
        }
        constexpr char first[]{"# session-input: maxima first mapped\n"};
        constexpr char second[]{"# session-input: maxima second mapped\n"};
        WriteLine(context, round == 0U ? first : second, round == 0U ? sizeof(first) - 1U : sizeof(second) - 1U);
        if (!Delay(context, 5000000U, error))
        {
            return false;
        }
        for (Foundation::Size i{0U}; i < 9U; ++i)
        {
            const auto offset =
                i == 8U ? Session::AddressOutputOffset
                        : Session::AddressPlan::Offset(i / 2U, static_cast<Foundation::UInt32>(i % 2U + 1U));
            for (Foundation::Size frame{0U}; frame < Session::SessionMaximumPoolFrames; ++frame)
            {
                if (!Check(context.Client->UnmapMemory(
                               space,
                               base + offset + frame * Session::SessionMaximumFramePages * Session::SessionPageBytes,
                               Session::SessionMaximumFramePages),
                           error))
                {
                    return false;
                }
            }
        }
    }
    for (Foundation::Size i{0U}; i < 9U; ++i)
    {
        if (!calls.Destroy(memories[i], Abi::ObjectTypeMemoryObject, error))
        {
            return false;
        }
        for (const auto frame : frames[i])
        {
            if (!calls.Destroy(frame, Abi::ObjectTypeFrame, error))
            {
                return false;
            }
        }
    }
    if (!calls.Destroy(space, Abi::ObjectTypeAddressSpace, error))
    {
        return false;
    }
    return true;
}

[[nodiscard, gnu::noinline]] bool Dropped(ScenarioContext& context, ComponentRecord& compositor,
                                          const System::CapabilityHandle control, const Foundation::UInt64 epoch,
                                          const Foundation::UInt64 transaction, Abi::SystemCallError& error) noexcept
{
    // Use the ISA clock, not a count of wakes which can return early for faults. A hung compositor
    // is killed and its output detached before the root's failure cleanup can drain a dead client's
    // domain. Supervisor::Stop also serves the fault channel of a fault-blocked
    // thread; direct ThreadTerminate is not sufficient here.
    const auto clock = context.Launch->Component.Clock;
    const auto start = clock();
    const auto ticks = Session::ClientDropTimeoutUs * context.Bootstrap->ClockTicksPerMicrosecond;
    for (Foundation::Size poll{0U}; poll < 2000U; ++poll)
    {
        if (clock() - start >= ticks)
        {
            break;
        }
        const auto received =
            context.Client->Receive(context.Reply, System::AddressOf(context.Delivery), sizeof(context.Delivery));
        if (received.Error == Abi::SystemCallError::None)
        {
            return Session::ReadSessionFrame(context.Delivery, static_cast<Foundation::Size>(received.Value1),
                                             g_clientAnswer) &&
                   g_clientAnswer.Method == Session::SessionReplyMethod && g_clientAnswer.Transaction == transaction &&
                   g_clientAnswer.Words[1U] == static_cast<Foundation::UInt64>(Session::SessionAnswer::Done);
        }
        if (received.Error != Abi::SystemCallError::WouldBlock ||
            context.Supervisor->Step(Session::ClientRetryUs, error) != PagerServeError::None)
        {
            return false;
        }
    }
    if (compositor.State == ComponentState::Running &&
        context.Supervisor->Stop(compositor, error) != PagerServeError::None)
    {
        return false;
    }
    Words(epoch);
    static_cast<void>(Send(context, control, Session::FaceDetach, transaction + 1000U, true, nullptr, 0U, error));
    error = Abi::SystemCallError::TimedOut;
    return false;
}
struct ClientWatch
{
    ComponentRecord* Pattern{nullptr};
    ComponentRecord* Compositor{nullptr};
    ComponentRecord* Input{nullptr};
    System::CapabilityHandle Seat{};
    Foundation::UInt64 SeatId{0U};
    System::CapabilityHandle Control{};
    Foundation::UInt64 Epoch{0U};
    Foundation::UInt64 Id{0U};
    Foundation::UInt64 Other{0U};
    bool Dropped{false};
    bool Detached{false};
};
constinit ClientWatch g_watch{};
[[nodiscard, gnu::noinline]] bool RetireWatchedInput(ScenarioContext& context, Abi::SystemCallError& error) noexcept
{
    // Desktop session: this demo discards the stopped consumer. Fence its client route before
    // retiring retained recovery keys and focus.
    Foundation::UInt64 words[4U]{g_watch.Input->Tag, 0U, 0U, 0U};
    for (Foundation::UInt32 step{0U}; step < 2U; ++step)
    {
        const auto method = step == 0U ? Component::InputQueryMethod : Component::InputRebindClientMethod;
        const auto target = step == 0U ? g_watch.Seat : g_watch.Input->Service;
        const Foundation::UInt64 transaction = 1201U + step;
        const auto bytes =
            Component::WriteInputFrame(context.Frame, method, transaction, words, nullptr, 0U, &context.Reply);
        if (!Check(context.Client->Send(target, System::AddressOf(context.Frame), bytes), error))
        {
            return false;
        }
        const auto received = context.Supervisor->AwaitMessage(*g_watch.Input, context.Reply, context.Delivery,
                                                               sizeof(context.Delivery), error);
        if (!Component::ReadInputFrame(context.Delivery, static_cast<Foundation::Size>(received), g_inputAnswer) ||
            g_inputAnswer.Method != Component::InputReplyMethod || g_inputAnswer.Transaction != transaction ||
            g_inputAnswer.Words[1U] != static_cast<Foundation::UInt64>(Component::InputAnswer::Done))
        {
            return false;
        }
        words[1U] = g_watch.SeatId;
        words[2U] = 1U;
    }
    return true;
}
[[nodiscard, gnu::noinline]] bool DetachWatched(ScenarioContext& context, Abi::SystemCallError& error) noexcept
{
    if (g_watch.Detached)
    {
        return true;
    }
    if (g_watch.Compositor->State == ComponentState::Running &&
        context.Supervisor->Stop(*g_watch.Compositor, error) != PagerServeError::None)
    {
        return false;
    }
    Words(g_watch.Epoch);
    if (!Send(context, g_watch.Control, Session::FaceDetach, 1200U, true, nullptr, 0U, error) ||
        !context.Supervisor->Teardown(*g_watch.Compositor, error) || !RetireWatchedInput(context, error))
    {
        return false;
    }
    // Losing the compositor must not abandon display or seat. Product restart/re-registration
    // remains; the manual demo continues serving its independent physical cursor and trusted input
    // services.
    g_watch.Detached = true;
    constexpr char line[]{"# session-input: compositor fault detached\n"};
    WriteLine(context, line, sizeof(line) - 1U);
    return true;
}
[[nodiscard, gnu::noinline]] bool DropWatched(ScenarioContext& context, const Foundation::UInt64 id,
                                              const Foundation::UInt64 transaction,
                                              Abi::SystemCallError& error) noexcept
{
    if (g_watch.Detached)
    {
        return true;
    }
    Words(id);
    if (Send(context, g_watch.Compositor->Service, Session::SessionDropClientMethod, transaction, false, &context.Reply,
             1U, error) &&
        Dropped(context, *g_watch.Compositor, g_watch.Control, g_watch.Epoch, transaction, error))
    {
        return true;
    }
    // An unread manager queue or failed DropClient is a compositor failure,
    // never a reason to stop the broker or either SEAT instance.
    return DetachWatched(context, error);
}
[[nodiscard, gnu::noinline]] bool ClientStep(ScenarioContext& context, Abi::SystemCallError& error) noexcept
{
    if (context.Supervisor->Step(Session::ClientRetryUs, error) != PagerServeError::None)
    {
        return false;
    }
    if (!g_watch.Detached && g_watch.Compositor->State == ComponentState::Ended && !DetachWatched(context, error))
    {
        return false;
    }
    if (!g_watch.Dropped && g_watch.Pattern->State == ComponentState::Ended)
    {
        ReportUnexpectedEnd(context, g_watch.Pattern->Tag, g_watch.Pattern->ExitCode,
                            context.Supervisor->HasCrashedSince(*g_watch.Pattern));
        // a fault/exit removes only the dead client's surfaces.
        // Keep display, compositor and seat alive during an interactive wait.
        if (!DropWatched(context, g_watch.Id, 200U, error) ||
            (g_watch.Other != 0U && !DropWatched(context, g_watch.Other, 201U, error)))
        {
            return false;
        }
        if (!context.Supervisor->Teardown(*g_watch.Pattern, error))
        {
            return false;
        }
        g_watch.Dropped = true;
        constexpr char line[]{"# session-input: client fault dropped\n"};
        WriteLine(context, line, sizeof(line) - 1U);
    }
    return true;
}
} // Namespace
bool TestRootService::RunCompositor(Abi::SystemCallError& error) noexcept
{
    if (m_broker == nullptr || !Describe(m_context, "session-test", g_compositorImage) ||
        !Describe(m_context, "pattern", g_patternImage) ||
        !m_calls.CreateEndpoint(m_context.Management, m_context.Reply, error))
    {
        return false;
    }
    m_supervisor.SetPoll(Reports, &m_context);
    static constexpr char described[]{"# session-test: images described\n"};
    WriteLine(m_context, described, sizeof(described) - 1U);
    Words(m_broker->Tag);
    if (!Send(m_context, m_broker->Service, Session::FaceOpen, 1U, true, &m_context.Reply, 1U, error))
    {
        return false;
    }
    const auto opened = AwaitMessage(m_context, *m_broker, m_context.Reply, error);
    if (!Session::ReadSessionFaceFrame(m_context.Delivery, static_cast<Foundation::Size>(opened), g_faceAnswer) ||
        g_faceAnswer.AttachmentCount != 2U || g_faceAnswer.Words[1U] != Component::DisplayDone)
    {
        return false;
    }
    const auto face = g_faceAnswer.Attachments[0U];
    const auto control = g_faceAnswer.Attachments[1U];
    static constexpr char openedLine[]{"# session-test: face opened\n"};
    WriteLine(m_context, openedLine, sizeof(openedLine) - 1U);
    m_context.DriverService = face;
    m_context.DriverGeneration = m_broker->Tag;
    m_context.ServiceFlags = m_context.Bootstrap->Scanout.Format | Component::SessionTestPoolWriteFlag |
                             Abi::ComponentSessionTestSmallOutputFlag;
    auto* const compositor = StartComponent(m_context, g_compositorImage, 1U, Abi::ComponentBehaviorCompositor, error);
    if (compositor == nullptr)
    {
        return false;
    }
    g_startingCompositor = compositor;
    static constexpr char started[]{"# session-test: compositor started\n"};
    WriteLine(m_context, started, sizeof(started) - 1U);
    const auto epoch = m_broker->Tag;
    // A later launch wins over a delayed client's first map.
    Words(8U);
    if (!Send(m_context, compositor->Service, Session::SessionUserLaunchMethod, 9U, false, nullptr, 0U, error))
    {
        return false;
    }
    System::CapabilityHandle endpoints[2U]{};
    Foundation::UInt64 ids[2U]{};
    ComponentRecord* patterns[2U]{};
    for (Foundation::Size i{0U}; i < 2U; ++i)
    {
        Words(i == 0U ? 0U : 7U);
        g_words[1U] = i == 0U ? Session::SessionClassSystemModule : Session::SessionClassVolumeBundle;
        constexpr char names[2U][8U]{"pattern", "second"};
        g_words[2U] = i == 0U ? 7U : 6U;
        for (Foundation::Size j{0U}; j < g_words[2U]; ++j)
        {
            g_words[4U] |= static_cast<Foundation::UInt64>(names[i][j]) << (j * 8U);
        }
        if (!Send(m_context, compositor->Service, Session::SessionRegisterClientMethod, 10U + i, false,
                  &m_context.Reply, 1U, error) ||
            !Reply(m_context, *compositor, 10U + i, error) || g_clientAnswer.AttachmentCount != 1U)
        {
            return false;
        }
        endpoints[i] = g_clientAnswer.Attachments[0U];
        ids[i] = g_clientAnswer.Words[2U];
        m_context.DriverService = endpoints[i];
        m_context.DriverGeneration = epoch;
        m_context.ServiceFlags = m_context.Bootstrap->Scanout.Format | (static_cast<Foundation::UInt64>(i + 1U) << 32U);
        patterns[i] = StartComponent(m_context, g_patternImage, 2U + i, Abi::ComponentBehaviorPatternClient, error);
        if (patterns[i] == nullptr)
        {
            return false;
        }
        // Each pattern initializes its maximum attachment batch before the
        // next one starts, keeping the global capability budget bounded.
        if (m_supervisor.Pause(500'000U, error) != PagerServeError::None)
        {
            return false;
        }
    }
    m_context.DriverService = {};
    m_context.DriverGeneration = 0U;
    m_context.ServiceFlags = 0U;
    Words(1U);
    if (!Send(m_context, face, Session::FaceShow, 20U, true, nullptr, 0U, error))
    {
        return false;
    }
    const auto command = [&](ComponentRecord& pattern, const Foundation::UInt64 serial) noexcept -> bool
    {
        Words(epoch);
        g_words[1U] = serial;
        g_words[2U] = 1U;
        g_words[4U] = Session::PackSessionDamage(0U, 0U, 1U, 1U);
        const bool okay =
            Send(m_context, pattern.Service, Session::FacePresent, 100U + serial, true, &m_context.Reply, 1U, error) &&
            Reply(m_context, pattern, 100U + serial, error);
        if (!okay)
        {
            ReportUnexpectedEnd(m_context, pattern.Tag, pattern.ExitCode, m_supervisor.HasCrashedSince(pattern));
        }
        return okay;
    };
    const auto observe = [&](const char* line, const Foundation::Size bytes) noexcept -> bool
    {
        WriteLine(m_context, line, bytes);
        Foundation::UInt64 e{0U};
        Foundation::UInt64 s{0U};
        // Desktop session: a fault wake must not advance past the capture fence.
        return FencePointer(m_context, e, s, error) && Delay(m_context, 5'000'000U, error);
    };
    static constexpr char stacked[]{"# session-test: stacked\n"};
    static constexpr char damage[]{"# session-test: damage\n"};
    static constexpr char clipped[]{"# session-test: clipped\n"};
    static constexpr char dropped[]{"# session-test: client dropped\n"};
    static constexpr char destroyed[]{"# session-test: surface destroyed\n"};
    static constexpr char detached[]{"# session-test: compositor detached\n"};
    if (!command(*patterns[0U], 1U) || !command(*patterns[1U], 1U) || !observe(stacked, sizeof(stacked) - 1U) ||
        !command(*patterns[0U], 2U) || !command(*patterns[1U], 2U) || !observe(damage, sizeof(damage) - 1U) ||
        !command(*patterns[1U], 3U) || !observe(clipped, sizeof(clipped) - 1U))
    {
        return false;
    }
    static constexpr char protocol[]{
        "# session-test: bad range refused, 16 Frame attachments in four messages per client\n"};
    WriteLine(m_context, protocol, sizeof(protocol) - 1U);
    // A killed producer leaves its last image pinned until DropClient. The
    // supervisor's drain then verifies the creator's charge can be returned.
    if (m_supervisor.Stop(*patterns[1U], error) != PagerServeError::None)
    {
        return false;
    }
    static constexpr char pinned[]{"# session-test: client killed, last image retained\n"};
    if (!observe(pinned, sizeof(pinned) - 1U))
    {
        return false;
    }
    Words(ids[1U]);
    if (!Send(m_context, compositor->Service, Session::SessionDropClientMethod, 200U, false, &m_context.Reply, 1U,
              error) ||
        !Dropped(m_context, *compositor, control, m_broker->Tag, 200U, error) ||
        !m_supervisor.Teardown(*patterns[1U], error))
    {
        return false;
    }
    // DropClient answers before composition. Let its bounded pacing pass
    // finish before the diagnostic/screenshot fence.
    if (m_supervisor.Pause(250'000U, error) != PagerServeError::None || !observe(dropped, sizeof(dropped) - 1U) ||
        !command(*patterns[0U], 4U) || m_supervisor.Pause(250'000U, error) != PagerServeError::None ||
        !observe(destroyed, sizeof(destroyed) - 1U))
    {
        return false;
    }
    Words(ids[0U]);
    if (!Send(m_context, compositor->Service, Session::SessionDropClientMethod, 201U, false, &m_context.Reply, 1U,
              error) ||
        !Dropped(m_context, *compositor, control, m_broker->Tag, 201U, error) ||
        m_supervisor.Stop(*patterns[0U], error) != PagerServeError::None ||
        !m_supervisor.Teardown(*patterns[0U], error) || m_supervisor.Stop(*compositor, error) != PagerServeError::None)
    {
        return false;
    }
    Words(epoch);
    if (!Send(m_context, control, Session::FaceDetach, 300U, true, nullptr, 0U, error) ||
        !m_supervisor.Teardown(*compositor, error) || !observe(detached, sizeof(detached) - 1U))
    {
        return false;
    }
    Words();
    if (!Send(m_context, face, Session::FaceDrop, 301U, true, nullptr, 0U, error))
    {
        return false;
    }
    for (const auto endpoint : endpoints)
    {
        if (!m_calls.Close(endpoint, error))
        {
            return false;
        }
    }
    if (!m_calls.Close(face, error) || !m_calls.Close(control, error) ||
        !m_calls.Destroy(m_context.Reply, Abi::ObjectTypeEndpoint, error))
    {
        return false;
    }
    // Processor scheduling: an AP may enqueue its final console report after the last poll. All
    // producers have ended; drain before Report becomes the display fence's reply Endpoint again.
    Reports(&m_context);
    m_supervisor.SetPoll(nullptr, nullptr);
    m_context.Reply = {};
    static constexpr char done[]{"# session-test: torn down\n"};
    WriteLine(m_context, done, sizeof(done) - 1U);
    return true;
}
bool TestRootService::RunSessionInput(Abi::SystemCallError& error, const bool largeMode) noexcept
{
    if (largeMode && !ArenaMaxima(m_context, m_calls, error))
    {
        return false;
    }
    const bool stress = Describe(m_context, "session-stress", g_compositorImage);
    if (m_broker == nullptr ||
        (!stress && !Describe(m_context, largeMode ? "session-large" : "session-input", g_compositorImage)) ||
        !Describe(m_context, "pattern", g_patternImage) || !Describe(m_context, "input-seat", g_seatImage) ||
        !m_calls.CreateEndpoint(m_context.Management, m_context.Reply, error))
    {
        return false;
    }
    auto* const input = StartComponent(m_context, g_seatImage, 1U, Abi::ComponentBehaviorInput, error);
    if (input == nullptr)
    {
        return false;
    }
    const auto exchange = [&](const Foundation::UInt32 method, const Foundation::UInt64 argument,
                              const Foundation::UInt64 transaction) noexcept -> bool
    {
        const Foundation::UInt64 words[4U]{input->Tag, argument, 0U, 0U};
        const auto bytes =
            Component::WriteInputFrame(m_context.Frame, method, transaction, words, nullptr, 0U, &m_context.Reply);
        if (!Check(m_context.Client->Send(input->Service, System::AddressOf(m_context.Frame), bytes), error))
        {
            return false;
        }
        const auto received = AwaitMessage(m_context, *input, m_context.Reply, error);
        return Component::ReadInputFrame(m_context.Delivery, static_cast<Foundation::Size>(received), g_inputAnswer) &&
               g_inputAnswer.Method == Component::InputReplyMethod &&
               g_inputAnswer.Words[1U] == static_cast<Foundation::UInt64>(Component::InputAnswer::Done);
    };
    if (!exchange(Component::InputRegisterClientMethod, Component::InputBrokerPointer, 1U))
    {
        return false;
    }
    const auto seat = g_inputAnswer.Reply;
    const auto seatId = g_inputAnswer.Words[2U];
    if (!exchange(Component::InputRegisterSourceMethod, 0U, 2U))
    {
        return false;
    }
    const auto source = g_inputAnswer.Reply;
    if (!SetPointerRoute(m_context, *input, error) || !exchange(Component::InputSetFocusMethod, seatId, 3U))
    {
        return false;
    }
    m_context.DriverService = source;
    m_context.DriverGeneration = input->Tag;
    auto* const ps2 = StartComponent(m_context, g_seatImage, 4U, Abi::ComponentBehaviorPs2, error);
    m_context.DriverService = {};
    m_context.DriverGeneration = 0U;
    if (ps2 == nullptr)
    {
        return false;
    }
    const auto ready = AwaitMessage(m_context, *ps2, m_context.Report, error);
    if (ready != Component::EchoDeliveryHeaderBytes + Component::EchoHeaderBytes ||
        (Component::Detail::InputLoad32(m_context.Delivery + Component::EchoDeliveryHeaderBytes + 32U) &
         Component::Ps2ReadyMethodMask) != Component::Ps2ReadyMethod)
    {
        return false;
    }
    m_supervisor.SetWatchdog(*ps2, false);
    Words(m_broker->Tag);
    if (!Send(m_context, m_broker->Service, Session::FaceOpen, 1U, true, &m_context.Reply, 1U, error))
    {
        return false;
    }
    const auto opened = AwaitMessage(m_context, *m_broker, m_context.Reply, error);
    if (!Session::ReadSessionFaceFrame(m_context.Delivery, static_cast<Foundation::Size>(opened), g_faceAnswer) ||
        g_faceAnswer.AttachmentCount != 2U)
    {
        return false;
    }
    const auto face = g_faceAnswer.Attachments[0U];
    const auto control = g_faceAnswer.Attachments[1U];
    m_context.DriverService = face;
    m_context.DriverGeneration = m_broker->Tag;
    m_context.SessionInput = seat;
    m_context.SessionInputEpoch = input->Tag;
    m_context.ServiceFlags = m_context.Bootstrap->Scanout.Format |
                             (largeMode ? 0U : Abi::ComponentSessionTestSmallOutputFlag) |
                             (stress ? Component::SessionTestSlowPresentFlag : 0U);
    auto* const compositor = StartComponent(m_context, g_compositorImage, 2U, Abi::ComponentBehaviorCompositor, error);
    m_context.SessionInput = {};
    m_context.SessionInputEpoch = 0U;
    if (compositor == nullptr)
    {
        return false;
    }
    g_startingCompositor = compositor;
    Words();
    g_words[1U] = Session::SessionClassSystemModule;
    g_words[2U] = 7U;
    g_words[4U] = 0x006e726574746170ULL;
    if (!Send(m_context, compositor->Service, Session::SessionRegisterClientMethod, 10U, false, &m_context.Reply, 1U,
              error) ||
        !Reply(m_context, *compositor, 10U, error))
    {
        return false;
    }
    const auto endpoint = g_clientAnswer.Attachments[0U];
    const auto id = g_clientAnswer.Words[2U];
    const auto nativePages = (static_cast<Foundation::UInt64>(m_context.Bootstrap->Scanout.Width) *
                                  m_context.Bootstrap->Scanout.Height * 4U +
                              Session::SessionPageBytes - 1U) /
                             Session::SessionPageBytes;
    const bool independent = stress || (largeMode && nativePages > Session::SessionMaximumPoolPages / 2U);
    if (independent)
    {
        Words();
        g_words[1U] = Session::SessionClassSystemModule;
        g_words[2U] = 7U;
        g_words[4U] = 0x006e726574746170ULL;
        if (!Send(m_context, compositor->Service, Session::SessionRegisterClientMethod, 11U, false, &m_context.Reply,
                  1U, error) ||
            !Reply(m_context, *compositor, 11U, error))
        {
            return false;
        }
        g_secondClient = g_clientAnswer.Attachments[0U];
        g_secondId = g_clientAnswer.Words[2U];
        m_context.SessionInput = g_secondClient;
    }
    m_context.DriverService = endpoint;
    m_context.DriverGeneration = m_broker->Tag;
    m_context.ServiceFlags = m_context.Bootstrap->Scanout.Format | ((stress ? 5ULL : largeMode ? 4ULL : 3ULL) << 32U);
    m_supervisor.SetPoll(Reports, &m_context);
    auto* const pattern = StartComponent(m_context, g_patternImage, 3U, Abi::ComponentBehaviorPatternClient, error);
    m_context.DriverService = {};
    m_context.DriverGeneration = 0U;
    m_context.ServiceFlags = 0U;
    m_context.SessionInput = {};
    if (pattern == nullptr)
    {
        return false;
    }
    g_watch.Pattern = pattern;
    g_watch.Compositor = compositor;
    g_watch.Control = control;
    g_watch.Input = input;
    g_watch.Seat = seat;
    g_watch.SeatId = seatId;
    g_watch.Epoch = m_broker->Tag;
    g_watch.Id = id;
    g_watch.Other = independent ? g_secondId : 0U;
    g_watch.Dropped = false;
    g_watch.Detached = false;
    if (stress)
    {
        // Force report WouldBlock while client replies are ready. A retry must yield rather than
        // exhaust a poll count on readiness.
        m_supervisor.SetPoll(nullptr, nullptr);
        const bool delayed = Delay(m_context, 500000U, error);
        m_supervisor.SetPoll(Reports, &m_context);
        if (!delayed)
        {
            return false;
        }
    }
    // The hardware demo's readiness fence has the same lifetime rule as
    // its manual F12 phases: a slow/dead client cannot abandon display/seat.
    for (Foundation::Size poll{0U}; (!stress || poll < 6000U) && (g_shown[0U] < 3U || g_shown[1U] < 3U); ++poll)
    {
        if (!ClientStep(m_context, error))
        {
            return false;
        }
    }
    if (g_shown[0U] < 3U || g_shown[1U] < 3U)
    {
        return false;
    }
    Words(1U);
    if (!Send(m_context, face, Session::FaceShow, 20U, true, nullptr, 0U, error))
    {
        return false;
    }
    const auto line = [&](const char* kind, const char* phase) noexcept
    {
        Inventory::ReportText text{g_phaseLine, sizeof(g_phaseLine)};
        text.Text("# session-input: ");
        text.Text(kind);
        text.Character(' ');
        text.Text(phase);
        text.Character('\n');
        WriteLine(m_context, g_phaseLine, text.Bytes());
    };
    const auto fence = [&](const char* phase) noexcept -> bool
    {
        // F12 proves the client received the preceding action. Wait for its
        // Arrange/Present and all decoration damage, then the broker Query.
        if (!Delay(m_context, 750000U, error))
        {
            return false;
        }
        for (Foundation::Size poll{0U}; poll < 6000U && g_sceneOpen; ++poll)
        {
            if (m_supervisor.Pause(10000U, error) != PagerServeError::None)
            {
                return false;
            }
        }
        if (g_sceneOpen)
        {
            return false;
        }
        line("fence", phase);
        Foundation::UInt64 e{0U};
        Foundation::UInt64 seq{0U};
        return FencePointer(m_context, e, seq, error) && Delay(m_context, 5000000U, error);
    };
    if (!fence("ready"))
    {
        return false;
    }
    static constexpr const char* phases[]{"move", "resize", "click-key", "key-click", "alt-tab"};
    static constexpr const char* largePhases[]{"maximize-first", "restore-first", "maximize-second", "restore-second"};
    const auto* selected = largeMode ? largePhases : phases;
    const Foundation::Size phaseCount = largeMode ? 4U : 5U;
    for (Foundation::Size phaseIndex{0U}; phaseIndex < phaseCount; ++phaseIndex)
    {
        if (stress)
        {
            break;
        }
        const auto phase = selected[phaseIndex];
        const auto signal = g_signal;
        line("phase", phase);
        bool acknowledged{false};
        // Session-input also serves as the owner's hardware demo. Missing the VM's F12 fence must
        // never time out and kill display/seat. Each service turn still bounds its report/fault
        // work.
        for (;;)
        {
            if (g_signal != signal)
            {
                acknowledged = true;
                break;
            }
            if (!ClientStep(m_context, error))
            {
                return false;
            }
        }
        if (!acknowledged || !fence(phase))
        {
            return false;
        }
    }
    const auto command = [&](const Foundation::UInt64 value) noexcept -> bool
    {
        Words(m_broker->Tag);
        g_words[1U] = value;
        g_words[2U] = 1U;
        g_words[4U] = Session::PackSessionDamage(0U, 0U, 1U, 1U);
        return Send(m_context, pattern->Service, Session::FacePresent, 100U + value, true, &m_context.Reply, 1U,
                    error) &&
               Reply(m_context, *pattern, 100U + value, error);
    };
    const auto driven = [&](const char* phase) noexcept -> bool
    {
        const auto signal = g_signal;
        line("phase", phase);
        for (Foundation::Size i{0U}; i < 18000U; ++i)
        {
            if (g_signal != signal)
            {
                return fence(phase);
            }
            if (!ClientStep(m_context, error))
            {
                return false;
            }
        }
        return false;
    };
    if (stress && !driven("cycles"))
    {
        return false;
    }
    if (!command(90U))
    {
        return false;
    }
    if (stress)
    {
        if (!driven("stalled") || !command(91U) || !driven("recovered"))
        {
            return false;
        }
    }
    else
    {
        line("phase", "hung");
        if (!Delay(m_context, 15000000U, error) || !fence("hung") || !command(91U))
        {
            return false;
        }
    }
    const auto signal = g_signal;
    line("phase", "close");
    bool acknowledged{false};
    for (Foundation::Size i{0U}; !stress || i < 6000U; ++i)
    {
        if (g_signal != signal)
        {
            acknowledged = true;
            break;
        }
        if (!ClientStep(m_context, error))
        {
            return false;
        }
    }
    if (!acknowledged || !fence("close"))
    {
        return false;
    }
    if (stress)
    {
        const auto crashes = m_supervisor.GetCrashes();
        line("phase", "fault");
        if (!command(92U))
        {
            return false;
        }
        for (Foundation::Size i{0U}; i < 2000U && !g_watch.Dropped; ++i)
        {
            if (!ClientStep(m_context, error))
            {
                return false;
            }
        }
        if (!g_watch.Dropped || m_supervisor.GetCrashes() != crashes + 1U || !fence("faulted"))
        {
            return false;
        }
        if (m_supervisor.Stop(*compositor, error) != PagerServeError::None || !ClientStep(m_context, error) ||
            !g_watch.Detached)
        {
            return false;
        }
        line("phase", "compositor-dead");
        // Leave room for >64 key edges and independent cursor comparisons
        // after the dead compositor's seat credit has been revoked.
        if (!Delay(m_context, 3U * Session::ClientTimeoutUs, error))
        {
            return false;
        }
    }
    if (!g_watch.Dropped)
    {
        if (m_supervisor.Stop(*pattern, error) != PagerServeError::None)
        {
            return false;
        }
        Words(id);
        if (!Send(m_context, compositor->Service, Session::SessionDropClientMethod, 200U, false, &m_context.Reply, 1U,
                  error) ||
            !Dropped(m_context, *compositor, control, m_broker->Tag, 200U, error))
        {
            return false;
        }
    }
    if (stress)
    {
        if (!m_calls.Close(g_secondClient, error))
        {
            return false;
        }
    }
    if ((!g_watch.Dropped && !m_supervisor.Teardown(*pattern, error)) ||
        (!g_watch.Detached && m_supervisor.Stop(*compositor, error) != PagerServeError::None) ||
        m_supervisor.Stop(*ps2, error) != PagerServeError::None ||
        m_supervisor.Stop(*input, error) != PagerServeError::None)
    {
        return false;
    }
    // Held seat and face credits borrow the compositor's reply Endpoint.
    // End their owners before draining its domain.
    Words();
    if (!Send(m_context, face, Session::FaceDrop, 301U, true, nullptr, 0U, error))
    {
        return false;
    }
    if (!m_supervisor.Teardown(*ps2, error) || !m_supervisor.Teardown(*input, error) ||
        (!g_watch.Detached && !m_supervisor.Teardown(*compositor, error)))
    {
        return false;
    }
    static System::CapabilityHandle handles[5U]{};
    handles[0U] = seat;
    handles[1U] = source;
    handles[2U] = endpoint;
    handles[3U] = face;
    handles[4U] = control;
    for (const auto handle : handles)
    {
        if (!m_calls.Close(handle, error))
        {
            return false;
        }
    }
    Reports(&m_context);
    m_supervisor.SetPoll(nullptr, nullptr);
    if (!m_calls.Destroy(m_context.Reply, Abi::ObjectTypeEndpoint, error))
    {
        return false;
    }
    m_context.Reply = {};
    line("torn", "down");
    return true;
}

} // Namespace Asteria::User::Pager
