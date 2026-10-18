// SPDX-License-Identifier: Apache-2.0
#include "Guest/Pager/TestRootService.hpp"
#include "Guest/SessionProbe.hpp"
#include "User/Session/SessionFaceFrame.hpp"

namespace Asteria::User::Pager
{
namespace
{
constinit Session::SessionFaceMessage g_sessionReply{};
constinit Component::DisplayInfo g_sessionInfo{};
constinit Foundation::UInt64 g_sessionWords[32U]{};
[[nodiscard]] bool SendFrame(ScenarioContext& context, const System::CapabilityHandle target,
                             const Foundation::Size bytes, Abi::SystemCallError& error) noexcept
{
    if (bytes == 0U)
    {
        error = Abi::SystemCallError::InvalidArguments;
        return false;
    }
    // Console mirroring and this probe share the bounded console queue.
    // Preserve the request while its sender quota is occupied.
    for (Foundation::Size attempt{0U}; attempt < 6'000U; ++attempt)
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
    error = Abi::SystemCallError::WouldBlock;
    return false;
}
[[nodiscard]] bool SendFace(ScenarioContext& context, const System::CapabilityHandle target,
                            const Foundation::UInt32 method, const Foundation::UInt64 transaction,
                            const System::CapabilityHandle* handles, const Foundation::Size count,
                            Abi::SystemCallError& error) noexcept
{
    const auto bytes =
        Session::WriteSessionFaceFrame(context.Frame, method, transaction, g_sessionWords, handles, count);
    return SendFrame(context, target, bytes, error);
}
void Words(const Foundation::UInt64 first = 0U) noexcept
{
    for (auto& word : g_sessionWords)
    {
        word = 0U;
    }
    g_sessionWords[0U] = first;
}
} // Namespace
bool TestRootService::RunSessionProbe(const LoadedImage& image, Abi::SystemCallError& error) noexcept
{
    if (m_broker == nullptr || !m_calls.CreateEndpoint(m_context.Management, m_context.Reply, error))
    {
        return false;
    }
    Words(m_broker->Tag);
    if (!SendFace(m_context, m_broker->Service, Session::FaceOpen, 1U, &m_context.Reply, 1U, error))
    {
        return false;
    }
    const auto received = AwaitMessage(m_context, *m_broker, m_context.Reply, error);
    if (!Session::ReadSessionFaceFrame(m_context.Delivery, static_cast<Foundation::Size>(received), g_sessionReply) ||
        g_sessionReply.AttachmentCount != 2U || g_sessionReply.Words[1U] != Component::DisplayDone)
    {
        return false;
    }
    const auto face = g_sessionReply.Attachments[0U];
    const auto control = g_sessionReply.Attachments[1U];
    static constexpr char opened[] = "# session-probe: face opened\n";
    WriteLine(m_context, opened, sizeof(opened) - 1U);
    // A retained credit is answered during the pointer drain, before Render; a nonzero key stamp
    // survives without the Input Broker rollout.
    Words(m_broker->Tag);
    g_sessionWords[1U] = 7U;
    if (!SendFace(m_context, face, Session::FaceCredit, 2U, &m_context.Reply, 1U, error))
    {
        return false;
    }
    Foundation::UInt64 pointer[]{
        m_broker->Tag,
        1U,
        Component::DisplayRelativeMotion,
        static_cast<Foundation::UInt32>(96 - static_cast<Foundation::Int32>(m_context.Bootstrap->Scanout.Width / 2U)) |
            (static_cast<Foundation::UInt64>(static_cast<Foundation::UInt32>(
                 72 - static_cast<Foundation::Int32>(m_context.Bootstrap->Scanout.Height / 2U)))
             << 32U),
        0U,
        1U,
        0U,
        42U};
    const auto pointerBytes = Component::WriteDisplayPointer(m_context.Frame, 3U, pointer);
    if (!SendFrame(m_context, m_broker->Service, pointerBytes, error))
    {
        return false;
    }
    const auto snapshot = AwaitMessage(m_context, *m_broker, m_context.Reply, error);
    if (!Session::ReadSessionFaceFrame(m_context.Delivery, static_cast<Foundation::Size>(snapshot), g_sessionReply) ||
        g_sessionReply.Method != Session::FaceSnapshots || g_sessionReply.Words[4U] != 1U ||
        g_sessionReply.Words[5U] != (96U | (72ULL << 32U)) || g_sessionReply.Words[7U] != 42U)
    {
        return false;
    }
    static constexpr char stamp[] = "# session-probe: pointer 96,72 sequence 1 stamp 42\n";
    WriteLine(m_context, stamp, sizeof(stamp) - 1U);
    m_context.DriverService = face;
    m_context.DriverGeneration = m_broker->Tag;
    m_context.ServiceFlags = m_context.Bootstrap->Scanout.Format;
    auto* const probe = StartComponent(m_context, image, 1U, Abi::ComponentBehaviorSessionProbe, error);
    m_context.DriverService = {};
    m_context.DriverGeneration = 0U;
    m_context.ServiceFlags = 0U;
    if (probe == nullptr)
    {
        return false;
    }
    // Ordinary text writes continue in the hidden grid. Each observation is
    // fenced, and a bounded pause lets the runner stop and capture that state.
    const auto observe = [&](const char* line, const Foundation::Size bytes) noexcept -> bool
    {
        WriteLine(m_context, line, bytes);
        Foundation::UInt64 epoch{0U};
        Foundation::UInt64 sequence{0U};
        return FencePointer(m_context, epoch, sequence, error) &&
               m_supervisor.Pause(5'000'000U, error) == PagerServeError::None;
    };
    const auto show = [&](const bool on, const bool clear = false) noexcept -> bool
    {
        Words(on ? 1U : 0U);
        g_sessionWords[1U] = clear ? 1U : 0U;
        return SendFace(m_context, face, Session::FaceShow, ++m_display.Transaction, nullptr, 0U, error);
    };
    const auto pattern = [&](const Foundation::UInt64 serial) noexcept -> bool
    {
        Words(m_broker->Tag);
        g_sessionWords[1U] = serial;
        g_sessionWords[2U] = 1U;
        g_sessionWords[4U] = Session::PackSessionDamage(0U, 0U, Test::ProbeWidth, Test::ProbeHeight);
        if (!SendFace(m_context, probe->Service, Session::FacePresent, 100U + serial, &m_context.Reply, 1U, error))
        {
            return false;
        }
        const auto answer = AwaitMessage(m_context, *probe, m_context.Reply, error);
        if (!Session::ReadSessionFaceFrame(m_context.Delivery, static_cast<Foundation::Size>(answer), g_sessionReply) ||
            g_sessionReply.Words[1U] != Session::SessionPresented || g_sessionReply.Transaction != 100U + serial)
        {
            return false;
        }
        if (serial == 2U)
        {
            static constexpr char holder[] = "# session-probe: holder snapshot 96,72 sequence 2 stamp 42\n";
            WriteLine(m_context, holder, sizeof(holder) - 1U);
            if (!QueryDisplay(g_sessionInfo, error) || g_sessionInfo.SessionPixels == 0U)
            {
                return false;
            }
            WriteDisplayMetrics(m_context, g_sessionInfo);
        }
        return true;
    };
    static constexpr char first[] = "# session-probe: pattern 1\n";
    static constexpr char second[] = "# session-probe: pattern 2\n";
    static constexpr char console[] = "# session-probe: console restored\n";
    static constexpr char again[] = "# session-probe: shown again\n";
    static constexpr char drop[] = "# session-probe: dropped\n";
    static constexpr char clear[] = "# session-probe: cleared trusted console\n";
    if (!show(true) || !pattern(1U) || !observe(first, sizeof(first) - 1U))
    {
        return false;
    }
    pointer[1U] = 2U;
    pointer[3U] = 0U;
    const auto secondPointerBytes = Component::WriteDisplayPointer(m_context.Frame, 4U, pointer);
    if (!SendFrame(m_context, m_broker->Service, secondPointerBytes, error) || !pattern(2U) ||
        !observe(second, sizeof(second) - 1U) || !show(false) || !observe(console, sizeof(console) - 1U) ||
        !show(true) || !observe(again, sizeof(again) - 1U))
    {
        return false;
    }
    Words();
    if (!SendFace(m_context, face, Session::FaceDrop, 300U, nullptr, 0U, error) || !observe(drop, sizeof(drop) - 1U))
    {
        return false;
    }
    Foundation::UInt64 clearedEpoch{0U};
    Foundation::UInt64 clearedSequence{0U};
    // The clear and the following trusted text use different Endpoints. Fence
    // the visibility change before writing the line.
    if (!show(false, true) || !FencePointer(m_context, clearedEpoch, clearedSequence, error) ||
        !observe(clear, sizeof(clear) - 1U))
    {
        return false;
    }
    if (m_supervisor.Stop(*probe, error) != PagerServeError::None || !m_supervisor.Teardown(*probe, error) ||
        !m_calls.Close(face, error) || !m_calls.Close(control, error) ||
        !m_calls.Destroy(m_context.Reply, Abi::ObjectTypeEndpoint, error))
    {
        return false;
    }
    m_context.Reply = {};
    static constexpr char done[] = "# session-probe: torn down\n";
    WriteLine(m_context, done, sizeof(done) - 1U);
    return true;
}
} // Namespace Asteria::User::Pager
