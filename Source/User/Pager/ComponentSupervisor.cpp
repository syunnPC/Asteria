// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/ComponentSupervisor.hpp"
#include "User/Component/FsFrame.hpp"

#include "Abi/ComponentBootstrap.hpp"
#include "User/Pager/PagerWords.hpp"

namespace Asteria::User::Pager
{
namespace
{
// The timed waits while a message is due, and how many: a service's reply
// comes within a few slices.
constexpr Foundation::UInt64 MessagePollMicroseconds{10'000U};
constexpr Foundation::UInt64 MessageAttempts{200U};
static_assert(ComponentStartupMicroseconds % MessagePollMicroseconds == 0U);
// How long the supervisor waits before it asks again for a drain the kernel cannot finish yet, and
// how often.
constexpr Foundation::UInt64 DrainPauseMicroseconds{10'000U};
constexpr Foundation::UInt64 DrainAttempts{100U};
// How long the Device Manager waits between looks at a quarantined device
// and how often: far longer than edu's 100 ms transfers.
constexpr Foundation::UInt64 QuiescePauseMicroseconds{10'000U};
constexpr Foundation::UInt64 QuiesceAttempts{100U};
} // namespace

void ComponentSupervisor::Configure(ObjectCalls& calls, ComponentLoader& loader, DeviceManagerRole& devices,
                                    PagerProgram& pager, const PagerBootstrap& bootstrap,
                                    const ComponentPlatform& platform) noexcept
{
    m_accepting = true;
    m_calls = &calls;
    m_loader = &loader;
    m_devices = &devices;
    m_pager = &pager;
    m_bootstrap = &bootstrap;
    m_platform = &platform;
}

ComponentRecord* ComponentSupervisor::FindByTag(const Foundation::UInt64 tag) noexcept
{
    for (auto& component : m_components)
    {
        if (component.State != ComponentState::Free && component.Tag == tag)
        {
            return &component;
        }
    }
    return nullptr;
}

ComponentRecord* ComponentSupervisor::Allocate() noexcept
{
    if (!m_accepting)
    {
        return nullptr;
    }
    for (ComponentRecord& component : m_components)
    {
        if (component.State == ComponentState::Free)
        {
            return &component;
        }
    }
    return nullptr;
}

ComponentRecord* ComponentSupervisor::FindStoppedInputDriver() noexcept
{
    for (ComponentRecord& component : m_components)
    {
        if (((component.Behavior == Abi::ComponentBehaviorPs2) || (component.Behavior == Abi::ComponentBehaviorXhci)) &&
            ((component.State == ComponentState::Ended) || (component.State == ComponentState::Building)) &&
            !component.FailureReported)
        {
            return &component;
        }
    }
    return nullptr;
}

// A service has no exit deadline; a task keeps the platform's
// Watchdog. No thread of the component runs before the loader starts it, so a crash counted from
// here is one of a component started before or after.
bool ComponentSupervisor::Start(ComponentRecord& component, const ComponentBuild& build,
                                Abi::SystemCallError& systemCall) noexcept
{
    const Foundation::UInt64 behavior = build.Behavior;
    component.FailureReported = false;
    component.Mode =
        ((behavior == Abi::ComponentBehaviorFaultSupervisor) || (behavior == Abi::ComponentBehaviorEcho) ||
         (behavior == Abi::ComponentBehaviorDirectory) || (behavior == Abi::ComponentBehaviorDriverService) ||
         (behavior == Abi::ComponentBehaviorBlockDriver) || (behavior == Abi::ComponentBehaviorFilesystem) ||
         (behavior == Abi::ComponentBehaviorDisplay) || (behavior == Abi::ComponentBehaviorInput) ||
         (behavior == Abi::ComponentBehaviorInputClient) || (behavior == Abi::ComponentBehaviorSessionProbe) ||
         (behavior == Abi::ComponentBehaviorCompositor) || (behavior == Abi::ComponentBehaviorPatternClient) ||
         (behavior == Abi::ComponentBehaviorToolkit) || (behavior == Abi::ComponentBehaviorPs2) ||
         (behavior == Abi::ComponentBehaviorXhci) || (behavior == Abi::ComponentBehaviorNicDriver) ||
         (behavior == Abi::ComponentBehaviorNetwork)) ||
                behavior == Abi::ComponentBehaviorAuth || behavior == Abi::ComponentBehaviorLogin
            ? ComponentMode::Service
            : ComponentMode::Task;
    SetWatchdog(component, component.Mode == ComponentMode::Task);
    component.CrashesAtStart = m_pager->GetComponentCrashes();
    component.State = ComponentState::Building;
    if (!m_loader->Load(component, build, systemCall))
    {
        return false;
    }
    return (behavior != Abi::ComponentBehaviorCompositor && behavior != Abi::ComponentBehaviorInput &&
            behavior != Abi::ComponentBehaviorAuth && behavior != Abi::ComponentBehaviorLogin) ||
           build.Deferred || WatchService(component, systemCall, nullptr, nullptr, ComponentStartupMicroseconds);
}

bool ComponentSupervisor::Launch(ComponentRecord& component, Abi::SystemCallError& systemCall) noexcept
{
    if ((component.State != ComponentState::Building) || (component.Thread.Selector != 0U))
    {
        systemCall = Abi::SystemCallError::InvalidArguments;
        return false;
    }
    return m_loader->StartThread(component, systemCall) &&
           ((component.Behavior != Abi::ComponentBehaviorCompositor &&
             component.Behavior != Abi::ComponentBehaviorInput && component.Behavior != Abi::ComponentBehaviorAuth &&
             component.Behavior != Abi::ComponentBehaviorLogin) ||
            WatchService(component, systemCall, nullptr, nullptr, ComponentStartupMicroseconds));
}

// Kill running components, supervise termination under the watchdog, and tear down
// their records, including partially built ResourceDomains.
bool ComponentSupervisor::PreparePower(Abi::SystemCallError& systemCall) noexcept
{
    m_accepting = false;
    for (auto& component : m_components)
    {
        if (component.State == ComponentState::Running && component.Mode == ComponentMode::Task)
        {
            if (Stop(component, systemCall) != PagerServeError::None || !Teardown(component, systemCall))
            {
                return false;
            }
        }
    }
    System::CapabilityHandle reply{};
    if (!m_calls->CreateEndpoint(m_bootstrap->Management, reply, systemCall))
    {
        return false;
    }
    alignas(8) static Foundation::UInt8 frame[Component::FsCommitRequestBytes]{};
    alignas(8) static Foundation::UInt8 delivery[Component::EchoDeliveryHeaderBytes + Component::FsReplyBytes]{};
    bool clean{true};
    for (auto& component : m_components)
    {
        if (component.State != ComponentState::Free && component.Behavior == Abi::ComponentBehaviorFilesystem)
        {
            if (component.State != ComponentState::Running || component.FsManager.Selector == 0U)
            {
                clean = false;
                break;
            }
            const Foundation::UInt64 words[Component::FsPayloadWords]{component.Tag, 0U, 0U, 0U};
            const auto size = Component::WriteFsFrame(frame, Component::FsQuiesceMethod, component.Tag, words, nullptr,
                                                      0U, &reply, nullptr);
            if (!Check(m_calls->GetClient().Send(component.FsManager, System::AddressOf(frame), size), systemCall))
            {
                clean = false;
                break;
            }
            const auto received = AwaitMessageFor(component, reply, delivery, sizeof(delivery), 500U, systemCall);
            Component::FsMessage message{};
            if (received == 0U || !Component::ReadFsFrame(delivery, static_cast<Foundation::Size>(received), message) ||
                message.Method != Component::FsReplyMethod || message.Words[0U] != component.Tag ||
                message.Words[1U] != Component::FsDone)
            {
                clean = false;
                break;
            }
        }
    }
    const bool closed = m_calls->Close(reply, systemCall);
    return clean && closed;
}

bool ComponentSupervisor::Abandon(Abi::SystemCallError& systemCall) noexcept
{
    for (ComponentRecord& component : m_components)
    {
        if (component.State != ComponentState::Running)
        {
            continue;
        }
        const Abi::SystemCallResponse killed = m_calls->GetClient().TerminateThread(component.Thread);
        if ((killed.Error != Abi::SystemCallError::None) && (killed.Error != Abi::SystemCallError::WouldBlock))
        {
            systemCall = killed.Error;
            return false;
        }
        SetWatchdog(component, true);
    }
    for (ComponentRecord& component : m_components)
    {
        if ((component.State == ComponentState::Running) && (Supervise(component, systemCall) != PagerServeError::None))
        {
            return false;
        }
    }
    // The pager's staging map can pin private objects charged to a child.
    // Release it before draining that child, as well as before device reuse.
    if (!m_loader->CleanWindow(systemCall))
    {
        return false;
    }
    for (ComponentRecord& component : m_components)
    {
        if ((component.State != ComponentState::Free) && !Teardown(component, systemCall))
        {
            return false;
        }
    }
    return m_loader->CleanWindow(systemCall);
}

Foundation::UInt64 ComponentSupervisor::GetCrashes() const noexcept
{
    return m_pager->GetComponentCrashes();
}

bool ComponentSupervisor::HasCrashedSince(const ComponentRecord& component) const noexcept
{
    return m_pager->GetComponentCrashes() != component.CrashesAtStart;
}

void ComponentSupervisor::SetWatchdog(ComponentRecord& component, const bool armed) const noexcept
{
    component.WatchdogMicroseconds = armed ? m_platform->WatchdogMicroseconds : 0U;
}

PagerServeError ComponentSupervisor::Pause(const Foundation::UInt64 microseconds,
                                           Abi::SystemCallError& systemCall) noexcept
{
    const PagerServeOutcome served = m_pager->ServeOnce(microseconds);
    if (m_poll != nullptr)
    {
        m_poll(m_pollContext);
    }
    PollLiveness();
    if ((served.Error != PagerServeError::None) && (served.Error != PagerServeError::TimedOut))
    {
        systemCall = served.SystemCall;
        return served.Error;
    }
    return PagerServeError::None;
}

PagerServeError ComponentSupervisor::Quiet(const Foundation::UInt64 microseconds,
                                           Abi::SystemCallError& systemCall) noexcept
{
    for (Foundation::Size wait{0U}; wait < QuietWaits; ++wait)
    {
        const PagerServeOutcome served = m_pager->ServeOnce(microseconds);
        if (m_poll != nullptr)
        {
            m_poll(m_pollContext);
        }
        PollLiveness();
        if (served.Error == PagerServeError::TimedOut)
        {
            return PagerServeError::None;
        }
        if (served.Error != PagerServeError::None)
        {
            systemCall = served.SystemCall;
            return served.Error;
        }
    }
    return PagerServeError::None;
}

PagerServeError ComponentSupervisor::Step(const Foundation::UInt64 microseconds,
                                          Abi::SystemCallError& systemCall) noexcept
{
    const PagerServeError paused = Pause(microseconds, systemCall);
    if (paused != PagerServeError::None)
    {
        return paused;
    }
    return Reap(systemCall) ? PagerServeError::None : PagerServeError::ManagementFailed;
}

// ObjectDestroy refuses a thread until it exited; meanwhile this pager serves faults, and a created
// domain's exit wakes its wait too. A wait that times out with components running is the watchdog's
// expiry: ThreadTerminate kills a Ready thread, and a thread blocked on a fault (WouldBlock) is
// left to that fault, which the next wait delivers and terminates.
PagerServeError ComponentSupervisor::Supervise(ComponentRecord& target, Abi::SystemCallError& systemCall) noexcept
{
    if (!Reap(systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    const bool timedLiveness = m_platform->Clock != nullptr && m_bootstrap->ClockTicksPerMicrosecond != 0U &&
                               m_bootstrap->WatchdogSeconds != 0U;
    auto waitStart = timedLiveness ? m_platform->Clock() : 0U;
    while (target.State == ComponentState::Running)
    {
        Foundation::UInt64 timeout{0U};
        for (const ComponentRecord& component : m_components)
        {
            if ((component.State == ComponentState::Running) && (component.WatchdogMicroseconds != 0U) &&
                ((timeout == 0U) || (component.WatchdogMicroseconds < timeout)))
            {
                timeout = component.WatchdogMicroseconds;
            }
        }
        // Watchdog.md: wake to ping services even during task-exit waits.
        // A short liveness wake must not become a task's exit timeout.
        const auto slice = timedLiveness && (timeout == 0U || timeout > Abi::WatchdogCheckMicroseconds)
                               ? Abi::WatchdogCheckMicroseconds
                               : timeout;
        const PagerServeOutcome served = m_pager->ServeOnce(slice);
        if (m_poll != nullptr)
        {
            m_poll(m_pollContext);
        }
        PollLiveness();
        auto elapsed = timeout;
        if (timedLiveness)
        {
            const auto now = m_platform->Clock();
            if (now < waitStart)
            {
                waitStart = now;
            }
            elapsed = (now - waitStart) / m_bootstrap->ClockTicksPerMicrosecond;
            if (served.Error != PagerServeError::TimedOut)
            {
                waitStart = now;
            }
        }
        if (served.Error == PagerServeError::TimedOut && (!timedLiveness || (timeout != 0U && elapsed >= timeout)))
        {
            for (const ComponentRecord& component : m_components)
            {
                if ((component.State != ComponentState::Running) || (component.WatchdogMicroseconds == 0U))
                {
                    continue;
                }
                const Abi::SystemCallResponse killed = m_calls->GetClient().TerminateThread(component.Thread);
                if ((killed.Error != Abi::SystemCallError::None) && (killed.Error != Abi::SystemCallError::WouldBlock))
                {
                    systemCall = killed.Error;
                    return PagerServeError::ManagementFailed;
                }
            }
        }
        else if (served.Error != PagerServeError::None && served.Error != PagerServeError::TimedOut)
        {
            systemCall = served.SystemCall;
            return served.Error;
        }
        if (!Reap(systemCall))
        {
            return PagerServeError::ManagementFailed;
        }
    }
    return PagerServeError::None;
}

bool ComponentSupervisor::Reap(Abi::SystemCallError& systemCall) noexcept
{
    const System::SystemCallClient& client = m_calls->GetClient();
    for (ComponentRecord& component : m_components)
    {
        if (component.State != ComponentState::Running)
        {
            continue;
        }
        const Abi::SystemCallResponse destroyed = client.DestroyObject(component.Thread, Abi::ObjectTypeThread);
        if (destroyed.Error == Abi::SystemCallError::WouldBlock)
        {
            continue;
        }
        if (!Check(destroyed, systemCall) || !Check(client.CloseCapability(component.Thread), systemCall))
        {
            return false;
        }
        component.ExitCode = destroyed.Value0;
        component.State = ComponentState::Ended;
    }
    return true;
}

// The Endpoint is polled between steps of the loop, which also reaps a
// component that ended meanwhile (a crash ends the wait for its message).
Foundation::UInt64 ComponentSupervisor::AwaitMessage(const ComponentRecord& component,
                                                     const System::CapabilityHandle endpoint,
                                                     Foundation::UInt8* const buffer, const Foundation::Size capacity,
                                                     Abi::SystemCallError& systemCall) noexcept
{
    Foundation::UInt64 attempts{MessageAttempts};
    for (const auto& slot : m_liveness)
    {
        if (slot.Component == &component && slot.Starting)
        {
            attempts = (slot.StartupMicroseconds + MessagePollMicroseconds - 1U) / MessagePollMicroseconds;
            break;
        }
    }
    return AwaitMessageFor(component, endpoint, buffer, capacity, attempts, systemCall);
}

Foundation::UInt64
ComponentSupervisor::AwaitMessageFor(const ComponentRecord& component, const System::CapabilityHandle endpoint,
                                     Foundation::UInt8* const buffer, const Foundation::Size capacity,
                                     const Foundation::UInt64 attempts, Abi::SystemCallError& systemCall) noexcept
{
    for (Foundation::UInt64 attempt{0U}; (attempt < attempts) && (component.State == ComponentState::Running);
         ++attempt)
    {
        if (Step(MessagePollMicroseconds, systemCall) != PagerServeError::None)
        {
            return 0U;
        }
        if (ServiceStartupExpired(component))
        {
            return 0U;
        }
        const Abi::SystemCallResponse received = m_calls->GetClient().Receive(endpoint, AddressOf(buffer), capacity);
        if (received.Error == Abi::SystemCallError::WouldBlock)
        {
            continue;
        }
        if (!Check(received, systemCall))
        {
            return 0U;
        }
        // A successful manager reply proves the Input Broker's event loop
        // ready. Later input replies cannot renew a private watchdog Query.
        if (component.Behavior == Abi::ComponentBehaviorInput &&
            Component::ReadInputFrame(buffer, static_cast<Foundation::Size>(received.Value1), m_livenessInputReply) &&
            m_livenessInputReply.Method == Component::InputReplyMethod &&
            m_livenessInputReply.Words[0U] == component.Tag &&
            m_livenessInputReply.Words[1U] == static_cast<Foundation::UInt64>(Component::InputAnswer::Done) &&
            !MarkServiceReady(component))
        {
            return 0U;
        }
        return received.Value1;
    }
    return 0U;
}

// A service ends only by a fault or a kill: killed while it
// waits, its wait is aborted and it ends as it resumes.
PagerServeError ComponentSupervisor::Stop(ComponentRecord& component, Abi::SystemCallError& systemCall) noexcept
{
    // ThreadTerminate leaves a fault-blocked thread to its supervisor's channel. Keep
    // serving/reaping that path before recovery drains the account.
    const auto killed = m_calls->GetClient().TerminateThread(component.Thread);
    if (killed.Error != Abi::SystemCallError::None && killed.Error != Abi::SystemCallError::WouldBlock)
    {
        systemCall = killed.Error;
        return PagerServeError::ManagementFailed;
    }
    SetWatchdog(component, true);
    return Supervise(component, systemCall);
}

// The thread is gone, so the domain retires once its memory is unmapped and reclaimed, and its
// spaces once the domain no longer binds them; each destroying call's collection reclaims what it
// released.
bool ComponentSupervisor::Teardown(ComponentRecord& component, Abi::SystemCallError& systemCall) noexcept
{
    UnwatchService(component);
    if (component.Account.Selector != 0U)
    {
        const Foundation::UInt8 device = component.Device;
        component.Device = ComponentNoDevice;
        const bool ps2 = component.Behavior == Abi::ComponentBehaviorPs2;
        if (!Drain(component, systemCall))
        {
            return false;
        }
        bool quiet{true};
        if (device == ComponentUsbHostGroup)
        {
            // Every function the group held, each on its own: one that stays
            // quarantined does not keep the others from their release.
            for (Foundation::Size slot{0U}; slot < Abi::PagerUsbHostControllers; ++slot)
            {
                Abi::SystemCallError recovered{Abi::SystemCallError::None};
                if (m_devices->IsPending(Abi::PagerDeviceUsbHost + slot) &&
                    !RecoverDevice(Abi::PagerDeviceUsbHost + slot, recovered))
                {
                    systemCall = (quiet ? recovered : systemCall);
                    quiet = false;
                }
            }
        }
        else if ((device != ComponentNoDevice) && m_devices->IsPending(device))
        {
            quiet = RecoverDevice(device, systemCall);
        }
        return quiet && (!ps2 || m_devices->QuiescePs2(m_bootstrap->I8042, systemCall));
    }
    // Without a ResourceDomain of its own, a build that failed part way
    // cannot be told apart from one whose objects are all there: it stays.
    if (component.State == ComponentState::Building)
    {
        systemCall = Abi::SystemCallError::InvalidArguments;
        return false;
    }
    // A service's WaitSet first: destroying it drops the registration that holds its Endpoint.
    const bool service = (component.WaitSet.Selector != 0U) || (component.Service.Selector != 0U);
    if (service && (!m_calls->Destroy(component.WaitSet, Abi::ObjectTypeWaitSet, systemCall) ||
                    !m_calls->Destroy(component.Service, Abi::ObjectTypeEndpoint, systemCall)))
    {
        return false;
    }
    if (component.FsManager.Selector != 0U &&
        !m_calls->Destroy(component.FsManager, Abi::ObjectTypeEndpoint, systemCall))
    {
        return false;
    }
    component.WaitSet = {};
    component.FsManager = {};
    component.Service = {};
    for (Foundation::Size index{0U}; index < component.MappingCount; ++index)
    {
        const ComponentMapping& mapping = component.Mappings[index];
        if (!Check(m_calls->GetClient().UnmapMemory(component.Space, mapping.VirtualStart, mapping.Pages),
                   systemCall) ||
            (!mapping.Shared && (!m_calls->Destroy(mapping.Memory, Abi::ObjectTypeMemoryObject, systemCall) ||
                                 !m_calls->Destroy(mapping.Frame, Abi::ObjectTypeFrame, systemCall))))
        {
            return false;
        }
    }
    component.SharedImage = nullptr;
    component.MappingCount = 0U;
    component.Foreign = ForeignImage{};
    if (!m_calls->Destroy(component.Domain, Abi::ObjectTypeProtectionDomain, systemCall) ||
        !m_calls->Destroy(component.Capabilities, Abi::ObjectTypeCapabilitySpace, systemCall) ||
        !m_calls->Destroy(component.Space, Abi::ObjectTypeAddressSpace, systemCall))
    {
        return false;
    }
    component.State = ComponentState::Free;
    return true;
}

// the drain left the device quarantined,
// perhaps still running a transfer the dead driver started, so the Device Manager confirms it quiet
// before any driver gets it again, polling between pauses (the supervisor also serves faults
// meanwhile). A device that does not go quiet keeps its quarantine and fails the teardown.
bool ComponentSupervisor::RecoverDevice(const Foundation::Size device, Abi::SystemCallError& systemCall) noexcept
{
    if (!m_loader->CleanWindow(systemCall) ||
        !m_devices->BeginQuiesce(device, m_bootstrap->Management, m_bootstrap->OwnSpace, m_bootstrap->WindowStart,
                                 systemCall))
    {
        return false;
    }
    for (Foundation::UInt64 attempt{0U}; attempt < QuiesceAttempts; ++attempt)
    {
        if (m_devices->PollQuiesce() == DeviceQuiescePoll::Quiet)
        {
            return m_devices->FinishQuiesce(systemCall);
        }
        if (Pause(QuiescePauseMicroseconds, systemCall) != PagerServeError::None)
        {
            break;
        }
    }
    m_devices->AbandonQuiesce();
    systemCall = (systemCall == Abi::SystemCallError::None) ? Abi::SystemCallError::WouldBlock : systemCall;
    return false;
}

// The thread is reaped, so the kernel destroys everything charged to the component's domain at
// once, its mappings left in its AddressSpace. A ProtectionDomain the platform cannot retire yet
// makes the drain WouldBlock, and the supervisor waits and asks again. The handles it kept, stale
// now, are then closed.
bool ComponentSupervisor::Drain(ComponentRecord& component, Abi::SystemCallError& systemCall) noexcept
{
    for (Foundation::UInt64 attempt{0U};; ++attempt)
    {
        const Abi::SystemCallResponse drained =
            m_calls->GetClient().DestroyObject(component.Account, Abi::ObjectTypeResourceDomain);
        if (drained.Error != Abi::SystemCallError::WouldBlock)
        {
            if (!Check(drained, systemCall))
            {
                return false;
            }
            break;
        }
        if (attempt == DrainAttempts)
        {
            systemCall = drained.Error;
            return false;
        }
        if (Pause(DrainPauseMicroseconds, systemCall) != PagerServeError::None)
        {
            return false;
        }
    }
    bool closed = m_calls->Close(component.Account, systemCall) && m_calls->Close(component.Space, systemCall) &&
                  m_calls->Close(component.Capabilities, systemCall) && m_calls->Close(component.Domain, systemCall) &&
                  m_calls->Close(component.Service, systemCall) && m_calls->Close(component.WaitSet, systemCall) &&
                  m_calls->Close(component.FsManager, systemCall);
    for (Foundation::Size index{0U}; closed && (index < component.MappingCount); ++index)
    {
        ComponentMapping& mapping = component.Mappings[index];
        if (mapping.Shared)
        {
            continue;
        }
        closed = m_calls->Close(mapping.Frame, systemCall) && m_calls->Close(mapping.Memory, systemCall);
    }
    if (!closed)
    {
        return false;
    }
    component.Account = {};
    component.WaitSet = {};
    component.FsManager = {};
    component.Service = {};
    component.SharedImage = nullptr;
    component.MappingCount = 0U;
    component.Foreign = ForeignImage{};
    component.State = ComponentState::Free;
    return true;
}
} // Namespace Asteria::User::Pager
