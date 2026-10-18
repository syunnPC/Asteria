// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/ComponentSupervisor.hpp"
#include "Abi/Watchdog.hpp"
#include "Inventory/ReportText.hpp"
#include "User/Pager/PagerWords.hpp"

namespace Asteria::User::Pager
{
extern "C"
{
ComponentWatchdogEvidence AsteriaComponentWatchdogEvidence[ComponentWatchdogSlots]{};
}

bool ComponentSupervisor::WatchService(ComponentRecord& component, Abi::SystemCallError& systemCall,
                                       const LivenessRecovery recovery, void* const context,
                                       const Foundation::UInt64 startupMicroseconds) noexcept
{
    if (startupMicroseconds > ComponentStartupMicroseconds)
    {
        systemCall = Abi::SystemCallError::InvalidArguments;
        return false;
    }
    if ((m_bootstrap->WatchdogSeconds == 0U && startupMicroseconds == 0U) || m_platform->Clock == nullptr ||
        m_bootstrap->ClockTicksPerMicrosecond == 0U)
    {
        return true;
    }
    for (auto& slot : m_liveness)
    {
        if (slot.Component != nullptr && slot.Component != &component)
        {
            continue;
        }
        if (slot.Reply.Selector == 0U && !m_calls->CreateEndpoint(m_bootstrap->Management, slot.Reply, systemCall))
        {
            return false;
        }
        if (slot.Component == &component && slot.Starting && startupMicroseconds == 0U && !MarkServiceReady(component))
        {
            return false;
        }
        const bool retainStartup = slot.Component == &component && slot.Starting && startupMicroseconds != 0U;
        slot.Component = &component;
        slot.Recovery = recovery;
        slot.Context = context;
        slot.Pending = false;
        slot.Submitted = false;
        slot.ReportedPeriods = 0U;
        slot.Next = m_platform->Clock();
        if (!retainStartup)
        {
            slot.Starting = startupMicroseconds != 0U;
            slot.StartupExpired = false;
            slot.StartupAt = slot.Next;
            slot.StartupMicroseconds = startupMicroseconds;
        }
        // Transaction is never reset on reuse: late answers cannot renew a
        // new incarnation. Reply endpoints stay root-owned until unregister.
        return true;
    }
    systemCall = Abi::SystemCallError::WouldBlock;
    return false;
}
bool ComponentSupervisor::BeginServiceStartup(const ComponentRecord& component,
                                              Abi::SystemCallError& systemCall) noexcept
{
    for (auto& slot : m_liveness)
    {
        if (slot.Component != &component)
        {
            continue;
        }
        slot.Starting = true;
        slot.StartupExpired = false;
        slot.StartupAt = m_platform->Clock();
        slot.StartupMicroseconds = ComponentStartupMicroseconds;
        slot.Pending = false;
        slot.Submitted = false;
        slot.ReportedPeriods = 0U;
        return true;
    }
    // With watchdog=off the ready console broker has no live slot yet;
    // its native session startup still needs the independent deadline.
    auto* const owned = FindByTag(component.Tag);
    if (owned != &component)
    {
        systemCall = Abi::SystemCallError::InvalidArguments;
        return false;
    }
    return WatchService(*owned, systemCall, nullptr, nullptr, ComponentStartupMicroseconds);
}
bool ComponentSupervisor::ExpireServiceStartup(LivenessSlot& slot, const Foundation::UInt64 now) noexcept
{
    if (!slot.Starting)
    {
        return false;
    }
    if (now < slot.StartupAt)
    {
        slot.StartupAt = now;
    }
    const auto age = (now - slot.StartupAt) / m_bootstrap->ClockTicksPerMicrosecond;
    if (!slot.StartupExpired && age >= slot.StartupMicroseconds)
    {
        slot.StartupExpired = true;
        char line[128U]{};
        Inventory::ReportText text{line, sizeof(line)};
        text.Text("# startup: ");
        text.Text(Abi::WatchdogComponentName(slot.Component->Behavior));
        text.Text(" not ready within ");
        text.Decimal(slot.StartupMicroseconds / 1000U);
        text.Text(" ms\n");
        static_cast<void>(m_calls->GetClient().DebugWrite(m_bootstrap->Diagnostic, AddressOf(line), text.Bytes()));
    }
    return slot.StartupExpired;
}
bool ComponentSupervisor::MarkServiceReady(const ComponentRecord& component) noexcept
{
    for (auto& slot : m_liveness)
    {
        if (slot.Component != &component)
        {
            continue;
        }
        if (!slot.Starting)
        {
            return true;
        }
        const auto now = m_platform->Clock();
        const bool timely = !ExpireServiceStartup(slot, now);
        // Also resumes the known-ready console broker after failed GUI startup.
        slot.Starting = false;
        slot.StartupExpired = false;
        slot.Pending = false;
        slot.Submitted = false;
        slot.ReportedPeriods = 0U;
        slot.Next = now;
        return timely;
    }
    return true; // No clock / liveness disabled and no startup registration.
}
bool ComponentSupervisor::ServiceStartupExpired(const ComponentRecord& component) const noexcept
{
    for (const auto& slot : m_liveness)
    {
        if (slot.Component == &component)
        {
            return slot.StartupExpired;
        }
    }
    return false;
}
void ComponentSupervisor::UnwatchService(const ComponentRecord& component) noexcept
{
    for (auto& slot : m_liveness)
    {
        if (slot.Component == &component)
        {
            slot.Component = nullptr;
            slot.Pending = false;
            slot.Starting = false;
            slot.StartupExpired = false;
            Abi::SystemCallError error{Abi::SystemCallError::None};
            if (slot.Reply.Selector != 0U)
            {
                static_cast<void>(m_calls->Destroy(slot.Reply, Abi::ObjectTypeEndpoint, error));
            }
            slot.Reply = {};
        }
    }
}
bool ComponentSupervisor::SendLiveness(LivenessSlot& slot) noexcept
{
    Foundation::Size bytes{0U};
    const auto behavior = slot.Component->Behavior;
    if (behavior == Abi::ComponentBehaviorDisplay)
    {
        bytes = Component::WriteDisplayQuery(m_livenessFrame, slot.Transaction, slot.Reply);
    }
    else if (behavior == Abi::ComponentBehaviorInput)
    {
        const Foundation::UInt64 words[4U]{slot.Component->Tag, 0U, 0U, 0U};
        bytes = Component::WriteInputFrame(m_livenessFrame, Component::InputQueryMethod, slot.Transaction, words,
                                           nullptr, 0U, &slot.Reply);
    }
    else if (behavior == Abi::ComponentBehaviorCompositor)
    {
        const Foundation::UInt64 words[4U]{slot.Component->Tag, 0U, 0U, 0U};
        bytes = Session::WriteSessionFrame(m_livenessFrame, Session::SessionQueryMethod, slot.Transaction, words,
                                           &slot.Reply, 1U);
    }
    else if (behavior == Abi::ComponentBehaviorAuth || behavior == Abi::ComponentBehaviorLogin)
    {
        // Auth services Query at public hash segment boundaries; the root
        // never runs a hash or pauses secure-attention processing for it.
        const Foundation::UInt64 words[Identity::SeatMaximumWords]{slot.Component->Tag};
        bytes = Identity::WriteSeatFrame(m_livenessFrame,
                                         behavior == Abi::ComponentBehaviorAuth ? Identity::SeatFace::AuthControl
                                                                                : Identity::SeatFace::LoginControl,
                                         Identity::SeatMethod::Query, slot.Transaction, words, slot.Reply);
    }
    return bytes != 0U && m_calls->GetClient().Send(slot.Component->Service, AddressOf(m_livenessFrame), bytes).Error ==
                              Abi::SystemCallError::None;
}
bool ComponentSupervisor::ReceiveLiveness(LivenessSlot& slot) noexcept
{
    const auto received =
        m_calls->GetClient().Receive(slot.Reply, AddressOf(m_livenessDelivery), sizeof(m_livenessDelivery));
    if (received.Error != Abi::SystemCallError::None)
    {
        return false;
    }
    const auto bytes = static_cast<Foundation::Size>(received.Value1);
    // Receive has already installed every attachment. Own and close them before protocol
    // validation/early exits.
    if (bytes < Component::EchoDeliveryHeaderBytes + Component::EchoHeaderBytes)
    {
        return false;
    }
    const auto* frame = m_livenessDelivery + Component::EchoDeliveryHeaderBytes;
    const Foundation::Size descriptors = static_cast<Foundation::Size>(frame[80U]) * 8U;
    const Foundation::Size attachments = frame[81U];
    const auto start = Component::EchoDeliveryHeaderBytes + Component::EchoHeaderBytes + descriptors;
    if (attachments > 4U || start > bytes || attachments * Component::EchoAttachmentBytes > bytes - start)
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < attachments; ++index)
    {
        const auto* record = m_livenessDelivery + start + index * Component::EchoAttachmentBytes;
        static_cast<void>(
            m_calls->GetClient().CloseCapability({System::LoadWord(record), System::LoadWord(record + 8U)}));
    }
    if (attachments != 0U)
    {
        return false;
    } // A liveness Query returns no authority.
    const auto behavior = slot.Component->Behavior;
    if (behavior == Abi::ComponentBehaviorDisplay)
    {
        return Component::ReadDisplayFrame(m_livenessDelivery, bytes, m_livenessDisplayReply) &&
               m_livenessDisplayReply.Method == Component::DisplayReplyMethod &&
               m_livenessDisplayReply.Transaction == slot.Transaction &&
               m_livenessDisplayReply.Words[1U] == Component::DisplayDone;
    }
    if (behavior == Abi::ComponentBehaviorInput)
    {
        return Component::ReadInputFrame(m_livenessDelivery, bytes, m_livenessInputReply) &&
               m_livenessInputReply.Method == Component::InputReplyMethod &&
               m_livenessInputReply.Transaction == slot.Transaction &&
               m_livenessInputReply.Words[0U] == slot.Component->Tag &&
               m_livenessInputReply.Words[1U] == static_cast<Foundation::UInt64>(Component::InputAnswer::Done);
    }
    if (behavior == Abi::ComponentBehaviorAuth || behavior == Abi::ComponentBehaviorLogin)
    {
        return Identity::ReadSeatFrame(m_livenessDelivery, bytes, m_livenessSeatReply) &&
               m_livenessSeatReply.Method == Identity::SeatMethod::Reply &&
               m_livenessSeatReply.Transaction == slot.Transaction &&
               m_livenessSeatReply.Words[0U] == slot.Component->Tag &&
               m_livenessSeatReply.Words[1U] == static_cast<Foundation::UInt64>(Identity::Status::Ok);
    }
    return behavior == Abi::ComponentBehaviorCompositor &&
           Session::ReadSessionFrame(m_livenessDelivery, bytes, m_livenessSessionReply) &&
           m_livenessSessionReply.Method == Session::SessionReplyMethod &&
           m_livenessSessionReply.Transaction == slot.Transaction &&
           m_livenessSessionReply.Words[1U] == static_cast<Foundation::UInt64>(Session::SessionAnswer::Done);
}
void ComponentSupervisor::PollLiveness() noexcept
{
    if (m_pollingLiveness || m_platform->Clock == nullptr || m_bootstrap->ClockTicksPerMicrosecond == 0U)
    {
        return;
    }
    m_pollingLiveness = true;
    const auto scale = m_bootstrap->ClockTicksPerMicrosecond;
    const auto period = m_bootstrap->WatchdogSeconds * 1'000'000U * scale;
    const auto now = m_platform->Clock();
    for (Foundation::Size index{0U}; index < ComponentWatchdogSlots; ++index)
    {
        auto& slot = m_liveness[index];
        if (slot.Component == nullptr || slot.Component->State != ComponentState::Running)
        {
            continue;
        }
        if (slot.Starting)
        {
            static_cast<void>(ExpireServiceStartup(slot, now));
            continue;
        }
        if (m_bootstrap->WatchdogSeconds == 0U)
        {
            continue;
        }
        if (slot.Pending && ReceiveLiveness(slot))
        {
            slot.Pending = false;
            slot.ReportedPeriods = 0U;
        }
        if (!slot.Pending && now >= slot.Next)
        {
            ++slot.Transaction;
            slot.SentAt = now;
            slot.Next = now + period;
            slot.Pending = true;
            slot.Submitted = false;
        }
        // Transient queue pressure does not lose the Query forever. Retry
        // one bounded send per poll until accepted, retaining the miss age.
        if (slot.Pending && !slot.Submitted)
        {
            slot.Submitted = SendLiveness(slot);
        }
        if (!slot.Pending || now < slot.SentAt || now - slot.SentAt < period)
        {
            continue;
        }
        const auto periods = (now - slot.SentAt) / period;
        if (periods <= slot.ReportedPeriods)
        {
            continue;
        }
        slot.ReportedPeriods = periods;
        auto& evidence = AsteriaComponentWatchdogEvidence[index];
        evidence.Behavior = slot.Component->Behavior;
        evidence.Transaction = slot.Transaction;
        evidence.UnresponsiveMilliseconds = (now - slot.SentAt) / scale / 1000U;
        ++evidence.Reports;
        char line[128U]{};
        Inventory::ReportText text{line, sizeof(line)};
        text.Text("# watchdog: ");
        text.Text(Abi::WatchdogComponentName(evidence.Behavior));
        text.Text(" unresponsive for ");
        text.Decimal(evidence.UnresponsiveMilliseconds);
        text.Text(" ms\n");
        static_cast<void>(m_calls->GetClient().DebugWrite(m_bootstrap->Diagnostic, AddressOf(line), text.Bytes()));
        bool faceConsumers{false};
        if (evidence.Behavior == Abi::ComponentBehaviorDisplay)
        {
            for (const auto& component : m_components)
            {
                faceConsumers = faceConsumers || (component.State == ComponentState::Running &&
                                                  (component.Behavior == Abi::ComponentBehaviorCompositor ||
                                                   component.Behavior == Abi::ComponentBehaviorSessionProbe));
            }
        }
        // The session owner must republish face/epoch links. The console-only recovery hook cannot
        // reconnect an active face.
        if (!faceConsumers && periods >= Abi::WatchdogRestartPeriods && slot.Recovery != nullptr)
        {
            Abi::SystemCallError error{Abi::SystemCallError::None};
            if (slot.Recovery(slot.Context, *slot.Component, error))
            {
                ++evidence.Restarts;
            }
            else
            {
                ++evidence.RestartFailures;
            }
        }
    }
    m_pollingLiveness = false;
}
} // Namespace Asteria::User::Pager
