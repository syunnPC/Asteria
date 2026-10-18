// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PAGER_COMPONENT_SUPERVISOR_HPP_INCLUDED
#define ASTERIA_USER_PAGER_COMPONENT_SUPERVISOR_HPP_INCLUDED

#include "Abi/SystemCallAbi.hpp"
#include "Foundation/Types.hpp"
#include "User/Pager/ComponentLoader.hpp"
#include "User/Pager/ComponentRecord.hpp"
#include "User/Pager/DeviceManagerRole.hpp"
#include "User/Pager/ObjectCalls.hpp"
#include "User/Pager/PagerProgram.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "User/Component/InputFrame.hpp"
#include "User/Session/SessionFrame.hpp"
#include "User/Identity/SeatFrame.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Pager
{
struct ComponentWatchdogEvidence
{
    Foundation::UInt64 Behavior;
    Foundation::UInt64 Transaction;
    Foundation::UInt64 UnresponsiveMilliseconds;
    Foundation::UInt64 Reports;
    Foundation::UInt64 Restarts;
    Foundation::UInt64 RestartFailures;
};
static_assert(sizeof(ComponentWatchdogEvidence) == 48U);
static_assert(__builtin_offsetof(ComponentWatchdogEvidence, Restarts) == 32U);
inline constexpr Foundation::Size ComponentWatchdogSlots{Abi::WatchdogServiceCount};
// Watchdog.md readiness allowance: the first uncached scanout may take a
// minute. This deadline is independent of watchdog=off and live Query periods.
inline constexpr Foundation::UInt64 ComponentStartupMicroseconds{60'000'000U};
extern "C" ComponentWatchdogEvidence AsteriaComponentWatchdogEvidence[ComponentWatchdogSlots];
/// The root service's supervisor: owns the record of every component the root service built, starts
/// each through the ComponentLoader, and runs the one supervision loop, which serves faults with
/// the earliest watchdog deadline and reaps every component whose thread ended. It tears a
/// component down, draining its ResourceDomain, and has the DeviceManagerRole confirm a drained
/// driver's device quiet. It lives in static storage of the pager image; it is not thread-safe.
class ComponentSupervisor final
{
public:
    constexpr ComponentSupervisor() noexcept = default;

    ComponentSupervisor(const ComponentSupervisor&) = delete;
    ComponentSupervisor& operator=(const ComponentSupervisor&) = delete;

    void Configure(ObjectCalls& calls, ComponentLoader& loader, DeviceManagerRole& devices, PagerProgram& pager,
                   const PagerBootstrap& bootstrap, const ComponentPlatform& platform) noexcept;

    /// Optional root-owned report drain, called between timed supervision waits. uses it for
    /// diagnostic lines without duplicating Write.
    void SetPoll(void (*poll)(void*) noexcept, void* context) noexcept
    {
        m_poll = poll;
        m_pollContext = context;
    }
    /// A free record for a component, or null.
    [[nodiscard]] ComponentRecord* Allocate() noexcept;
    [[nodiscard]] ComponentRecord* FindByTag(Foundation::UInt64 tag) noexcept;
    /// One unreported input driver whose build or thread ended. Caller owns
    /// reporting and bounded teardown; running services are never selected.
    [[nodiscard]] ComponentRecord* FindStoppedInputDriver() noexcept;
    /// Builds and starts the component build describes in component: a task
    /// under the platform's watchdog, a service (Echo, the Directory, a
    /// driver service) without an exit deadline.
    [[nodiscard, gnu::noinline]] bool Start(ComponentRecord& component, const ComponentBuild& build,
                                            Abi::SystemCallError& systemCall) noexcept;
    /// Starts the thread of a component its Deferred build left without
    /// one.
    [[nodiscard]] bool Launch(ComponentRecord& component, Abi::SystemCallError& systemCall) noexcept;
    /// The pager's count of crashed components.
    [[nodiscard]] Foundation::UInt64 GetCrashes() const noexcept;
    /// Whether a component crashed since component started.
    [[nodiscard]] bool HasCrashedSince(const ComponentRecord& component) const noexcept;
    /// Arms component's watchdog with the platform's period, or disarms it.
    void SetWatchdog(ComponentRecord& component, bool armed) const noexcept;
    using LivenessRecovery = bool (*)(void*, ComponentRecord&, Abi::SystemCallError&) noexcept;
    /// Bounded liveness Query. A nonzero startup allowance defers Queries
    /// until MarkServiceReady; expiry is reported separately, once.
    [[nodiscard]] bool WatchService(ComponentRecord& component, Abi::SystemCallError& systemCall,
                                    LivenessRecovery recovery = nullptr, void* context = nullptr,
                                    Foundation::UInt64 startupMicroseconds = 0U) noexcept;
    /// Pause an already-ready display while its first session scanout starts.
    [[nodiscard]] bool BeginServiceStartup(const ComponentRecord& component, Abi::SystemCallError& systemCall) noexcept;
    /// A first Presented/FrameShown is readiness. Late readiness returns
    /// false; the owner must fall back, then resume its ready console broker.
    [[nodiscard]] bool MarkServiceReady(const ComponentRecord& component) noexcept;
    [[nodiscard]] bool ServiceStartupExpired(const ComponentRecord& component) const noexcept;
    void UnwatchService(const ComponentRecord& component) noexcept;
    void PollLiveness() noexcept;
    /// One timed wait of the pager, which may serve faults; a timeout is no
    /// failure.
    [[nodiscard]] PagerServeError Pause(Foundation::UInt64 microseconds, Abi::SystemCallError& systemCall) noexcept;
    /// Timed waits of the pager, serving faults, until one passes whole with
    /// nothing to serve, at most QuietWaits of them; reaps no exit.
    [[nodiscard]] PagerServeError Quiet(Foundation::UInt64 microseconds, Abi::SystemCallError& systemCall) noexcept;
    static constexpr Foundation::Size QuietWaits{16U};
    /// One step of the event loop: a timed wait that serves
    /// faults, then the reap of every component whose thread ended.
    [[nodiscard]] PagerServeError Step(Foundation::UInt64 microseconds, Abi::SystemCallError& systemCall) noexcept;
    /// The supervision loop: serves faults, each wait lasting at most the
    /// earliest watchdog period of a running component, and reaps every
    /// component whose thread ObjectDestroy reclaims, until target ended. A
    /// wait that passes with components still running kills them
    /// (ThreadTerminate), or, if one just faulted, leaves it to the fault's
    /// termination.
    [[nodiscard, gnu::noinline]] PagerServeError Supervise(ComponentRecord& target,
                                                           Abi::SystemCallError& systemCall) noexcept;
    /// Steps until a message arrives on endpoint, into buffer of capacity
    /// bytes, while component runs: its size, or zero when none came.
    [[nodiscard, gnu::noinline]] Foundation::UInt64 AwaitMessage(const ComponentRecord& component,
                                                                 System::CapabilityHandle endpoint,
                                                                 Foundation::UInt8* buffer, Foundation::Size capacity,
                                                                 Abi::SystemCallError& systemCall) noexcept;
    /// AwaitMessage polling at most attempts times, ten milliseconds apart.
    [[nodiscard, gnu::noinline]] Foundation::UInt64
    AwaitMessageFor(const ComponentRecord& component, System::CapabilityHandle endpoint, Foundation::UInt8* buffer,
                    Foundation::Size capacity, Foundation::UInt64 attempts, Abi::SystemCallError& systemCall) noexcept;
    /// Kills component and supervises it until it ended: the watchdog bounds
    /// the wait for that end, and kills it again should the first kill not
    /// have ended it. The caller judges its exit code.
    [[nodiscard, gnu::noinline]] PagerServeError Stop(ComponentRecord& component,
                                                      Abi::SystemCallError& systemCall) noexcept;
    /// Drains the component's own ResourceDomain where it has one, then has a driver's device
    /// confirmed quiet; otherwise unmaps and destroys its memory, then destroys its domain, its
    /// CapabilitySpace and its AddressSpace. Either way frees its record. A build that failed is
    /// drained the same way, where it has its own ResourceDomain; without one it stays.
    [[nodiscard, gnu::noinline]] bool Teardown(ComponentRecord& component, Abi::SystemCallError& systemCall) noexcept;
    /// PowerOff.md: close root admission, stop task components, flush every
    /// running filesystem before any storage driver is stopped. Fail closed.
    [[nodiscard, gnu::noinline]] bool PreparePower(Abi::SystemCallError& systemCall) noexcept;
    /// After a scenario failed: kills every running component, supervises
    /// them until they ended and tears every record down, failed builds
    /// included. False, with the refused call, when that fails too: the
    /// records left stand.
    [[nodiscard, gnu::noinline]] bool Abandon(Abi::SystemCallError& systemCall) noexcept;

private:
    struct LivenessSlot
    {
        ComponentRecord* Component{};
        System::CapabilityHandle Reply{};
        Foundation::UInt64 Next{};
        Foundation::UInt64 SentAt{};
        Foundation::UInt64 Transaction{Foundation::UInt64{1U} << 63U};
        Foundation::UInt64 ReportedPeriods{};
        Foundation::UInt64 StartupAt{};
        Foundation::UInt64 StartupMicroseconds{};
        LivenessRecovery Recovery{};
        void* Context{};
        bool Pending{};
        bool Submitted{};
        bool Starting{};
        bool StartupExpired{};
    };
    [[nodiscard]] bool ExpireServiceStartup(LivenessSlot& slot, Foundation::UInt64 now) noexcept;
    [[nodiscard]] bool SendLiveness(LivenessSlot& slot) noexcept;
    [[nodiscard]] bool ReceiveLiveness(LivenessSlot& slot) noexcept;
    LivenessSlot m_liveness[ComponentWatchdogSlots]{};
    alignas(8U) Foundation::UInt8 m_livenessFrame[Component::DisplayMaximumFrameBytes]{};
    alignas(8U) Foundation::UInt8 m_livenessDelivery[552U]{};
    Component::DisplayMessage m_livenessDisplayReply{};
    Component::InputMessage m_livenessInputReply{};
    Session::SessionMessage m_livenessSessionReply{};
    Identity::SeatMessage m_livenessSeatReply{};
    bool m_pollingLiveness{};
    /// Reaps every running component whose thread ObjectDestroy reclaims.
    [[nodiscard, gnu::noinline]] bool Reap(Abi::SystemCallError& systemCall) noexcept;
    /// The Device Manager's confirmation, after a driver's drain, that the quarantined device is
    /// quiet.
    [[nodiscard, gnu::noinline]] bool RecoverDevice(Foundation::Size device, Abi::SystemCallError& systemCall) noexcept;
    /// Destroys the component's ResourceDomain, and so everything charged to
    /// it, then closes every handle of the component's objects.
    [[nodiscard, gnu::noinline]] bool Drain(ComponentRecord& component, Abi::SystemCallError& systemCall) noexcept;

    bool m_accepting{true};
    void (*m_poll)(void*) noexcept {nullptr};
    void* m_pollContext{nullptr};
    ObjectCalls* m_calls{nullptr};
    ComponentLoader* m_loader{nullptr};
    DeviceManagerRole* m_devices{nullptr};
    PagerProgram* m_pager{nullptr};
    const PagerBootstrap* m_bootstrap{nullptr};
    const ComponentPlatform* m_platform{nullptr};
    ComponentRecord m_components[MaximumComponents]{};
};
} // Namespace Asteria::User::Pager

#endif // ASTERIA_USER_PAGER_COMPONENT_SUPERVISOR_HPP_INCLUDED
