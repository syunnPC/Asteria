// SPDX-License-Identifier: Apache-2.0
#include "Guest/Pager/RootScenarios.hpp"

#include "Abi/ComponentBootstrap.hpp"
#include "User/Component/DriverFrame.hpp"
#include "User/Pager/PagerWords.hpp"

namespace Asteria::User::Pager
{
namespace
{
// How long the root service lets the restarted instance block again before
// it kills it: longer than a user time slice.
constexpr Foundation::UInt64 KillPauseMicroseconds{100'000U};
// The patterns the root service has copied through the device, one per
// generation it addresses.
constexpr Foundation::UInt64 FirstPattern{0x4B49'4C4C'4544'0000ULL};
constexpr Foundation::UInt64 SecondPattern{0x5345'5256'4544'0000ULL};
// edu's interrupt status bit for a finished transfer, which the Device
// Manager must find as it confirms the quarantined device quiet: the
// transfer the crashed instance started ended with no one to acknowledge it.
constexpr Foundation::UInt64 TransferEndedStatus{0x100U};
// The factorials the root service asks for instead where the device may not
// master the bus, and edu's interrupt status bit for a finished
// factorial, which the Device Manager must find likewise.
constexpr Foundation::UInt64 FirstFactorial{10U};
constexpr Foundation::UInt64 SecondFactorial{12U};
constexpr Foundation::UInt64 FactorialEndedStatus{0x1U};

/// What one recovery exercises: the method of its requests, their arguments
/// for the dead generation and the live one, the interrupt status the
/// Device Manager must find, and the line the root service writes.
struct Recovery
{
    Foundation::UInt32 Method;
    Foundation::UInt64 First;
    Foundation::UInt64 Second;
    Foundation::UInt64 EndedStatus;
    const char* Line;
    Foundation::Size LineBytes;
};

constexpr Recovery CopyRecovery{.Method = Component::DriverCopyMethod,
                                .First = FirstPattern,
                                .Second = SecondPattern,
                                .EndedStatus = TransferEndedStatus,
                                .Line = DriverRecoveredLine,
                                .LineBytes = sizeof(DriverRecoveredLine) - 1U};
constexpr Recovery FactorialRecovery{.Method = Component::DriverFactorialMethod,
                                     .First = FirstFactorial,
                                     .Second = SecondFactorial,
                                     .EndedStatus = FactorialEndedStatus,
                                     .Line = DriverFactorialRecoveredLine,
                                     .LineBytes = sizeof(DriverFactorialRecoveredLine) - 1U};

/// A reply to the request the root service awaits: its kind, zero for
/// none or another request's, and the status the instance found.
struct Reply
{
    Foundation::UInt64 Kind;
    Foundation::UInt64 Found;
};

/// Whether reply is of kind, from an instance that found the device quiet:
/// its Device Manager acknowledged what the dead one's transfer raised.
[[nodiscard]] constexpr bool Is(const Reply& reply, const Foundation::UInt64 kind) noexcept
{
    return (reply.Kind == kind) && (reply.Found == 0U);
}

// A request of the recovery's method on the instance's Endpoint, through the
// root service's first capability (sealed with DriverSealedSelector), with a
// copy of a send capability on the reply Endpoint.
[[nodiscard, gnu::noinline]] bool SendRequest(ScenarioContext& context, const ComponentRecord& driver,
                                              const Recovery& recovery, const Foundation::UInt64 transaction,
                                              const Foundation::UInt64 generation, const Foundation::UInt64 argument,
                                              const Foundation::UInt64 faults,
                                              Abi::SystemCallError& systemCall) noexcept
{
    const Component::EchoAttachment reply{.Handle = context.Reply,
                                          .Rights = Component::EchoInvokeRight,
                                          .Mode = Component::EchoCopy,
                                          .Type = Component::EchoEndpointType};
    const Foundation::Size bytes = Component::WriteDriverFrame(
        context.Frame, transaction, {generation, argument, faults, 0U}, &reply, recovery.Method);
    return Check(context.Client->Send(driver.Service, AddressOf(context.Frame), bytes), systemCall);
}

// The next reply, if it answers the request of transaction, generation and
// pattern.
[[nodiscard, gnu::noinline]] Reply AwaitReply(ScenarioContext& context, const ComponentRecord& driver,
                                              const Foundation::UInt64 transaction, const Foundation::UInt64 generation,
                                              const Foundation::UInt64 pattern,
                                              Abi::SystemCallError& systemCall) noexcept
{
    const Foundation::UInt64 received = AwaitMessage(context, driver, context.Reply, systemCall);
    Component::DriverMessage message{};
    if (!Component::ReadDriverFrame(context.Delivery, static_cast<Foundation::Size>(received), message) ||
        (message.Method != Component::DriverReplyMethod) || (message.Transaction != transaction) ||
        (message.Words[0U] != generation) || (message.Words[1U] != pattern))
    {
        return Reply{.Kind = 0U, .Found = 0U};
    }
    return Reply{.Kind = message.Words[2U], .Found = message.Words[3U]};
}

// A service ends only by a kill: its wait is aborted and it ends as it resumes; the watchdog bounds
// the wait for that end, and its ResourceDomain is drained, the device pre-pass first.
[[nodiscard, gnu::noinline]] PagerServeError Stop(ScenarioContext& context, ComponentRecord& driver,
                                                  Abi::SystemCallError& systemCall) noexcept
{
    const PagerServeError served = context.Supervisor->Stop(driver, systemCall);
    if (served != PagerServeError::None)
    {
        return served;
    }
    if (driver.ExitCode != Abi::KilledExitCode)
    {
        ReportUnexpectedEnd(context, driver.Tag, driver.ExitCode, false);
        return PagerServeError::ManagementFailed;
    }
    return context.Supervisor->Teardown(driver, systemCall) ? PagerServeError::None : PagerServeError::ManagementFailed;
}

// Says which await went wrong while the instance still runs: the await (2
// the DriverRestarted, 3 and 4 the served Started and Done) in bits 56-63,
// the reply's kind in bits 32-39 and the status it found below, in place of
// an exit code.
[[nodiscard]] PagerServeError Fail(const ScenarioContext& context, const ComponentRecord& driver,
                                   const Foundation::UInt64 await, const Reply& reply) noexcept
{
    ReportUnexpectedEnd(context, driver.Tag,
                        (driver.ExitCode != 0U)
                            ? driver.ExitCode
                            : ((await << 56U) | ((reply.Kind & 0xFFU) << 32U) | (reply.Found & 0xFFFF'FFFFU)),
                        false);
    return PagerServeError::ManagementFailed;
}
// The driver's recovery. The root service is the client here, an ordinary one: it sends requests to
// The driver service and reads the replies on its reply Endpoint. The first instance is asked for a
// copy, or a factorial, with a fault injected: it crashes as the copy's write-back transfer or the
// factorial starts, so it dies with its device busy, and the supervisor terminates it and drains
// its ResourceDomain, the device pre-pass first. Under the emulator that drain may come before the
// work's end or after it (the evidence says which); the crashed instance never sees the end either
// way. A factorial's end raises the device's interrupt, a level-triggered line on a platform
// without messages: the drain left it disabled at the function and masked at the controller, and
// the Device Manager acknowledges it. A restarted instance quiesces the device and answers the
// request still outstanding at the dead generation with DriverRestarted, executing nothing; another
// request addressed to it is served. The Device Manager, not the instance, makes the restart safe:
// the drain quarantined the device, and only its confirmation that the work ended lifts that. Every
// reply must be the one expected.
[[nodiscard, gnu::noinline]] PagerServeError RunRecovery(ScenarioContext& context, const Recovery& recovery,
                                                         Abi::SystemCallError& systemCall) noexcept
{
    ComponentSupervisor& supervisor = *context.Supervisor;
    const Foundation::UInt64 crashes = supervisor.GetCrashes();
    ComponentRecord* const first =
        StartComponent(context, context.Component, context.NextTag++, Abi::ComponentBehaviorDriverService, systemCall);
    if ((first == nullptr) || !SendRequest(context, *first, recovery, 1U, first->Tag, recovery.First,
                                           Component::DriverCrashAfterStart, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    const Foundation::UInt64 dead = first->Tag;
    supervisor.SetWatchdog(*first, true);
    const PagerServeError ended = supervisor.Supervise(*first, systemCall);
    if (ended != PagerServeError::None)
    {
        return ended;
    }
    if ((first->ExitCode != Abi::FaultTerminatedExitCode) || (supervisor.GetCrashes() != (crashes + 1U)))
    {
        ReportUnexpectedEnd(context, dead, first->ExitCode, supervisor.GetCrashes() != crashes);
        return PagerServeError::ManagementFailed;
    }
    // The teardown drains the dead instance's domain, which quarantines the
    // device, and the Device Manager confirms it quiet once the work has
    // ended: it must find the status that end raised.
    if (!supervisor.Teardown(*first, systemCall) || (context.Devices->GetLastQuiesce().Status != recovery.EndedStatus))
    {
        return PagerServeError::ManagementFailed;
    }
    ComponentRecord* const second =
        StartComponent(context, context.Component, context.NextTag++, Abi::ComponentBehaviorDriverService, systemCall);
    if ((second == nullptr) || !SendRequest(context, *second, recovery, 1U, dead, recovery.First, 0U, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    const Reply restarted = AwaitReply(context, *second, 1U, dead, recovery.First, systemCall);
    if (!Is(restarted, Component::DriverRestarted) ||
        !SendRequest(context, *second, recovery, 2U, second->Tag, recovery.Second, 0U, systemCall))
    {
        return Fail(context, *second, 2U, restarted);
    }
    const Foundation::UInt64 live = second->Tag;
    const Reply working = AwaitReply(context, *second, 2U, live, recovery.Second, systemCall);
    if (!Is(working, Component::DriverStarted))
    {
        return Fail(context, *second, 3U, working);
    }
    const Reply worked = AwaitReply(context, *second, 2U, live, recovery.Second, systemCall);
    if (!Is(worked, Component::DriverDone))
    {
        return Fail(context, *second, 4U, worked);
    }
    // Killed while it waits for the next request, as the other services are.
    const PagerServeError paused = supervisor.Pause(KillPauseMicroseconds, systemCall);
    const PagerServeError stopped = (paused != PagerServeError::None) ? paused : Stop(context, *second, systemCall);
    if (stopped != PagerServeError::None)
    {
        return stopped;
    }
    // Nothing else may wait on the reply Endpoint for the services after.
    const Abi::SystemCallResponse left =
        context.Client->Receive(context.Reply, AddressOf(context.Delivery), sizeof(context.Delivery));
    if ((left.Error != Abi::SystemCallError::WouldBlock) || (supervisor.GetCrashes() != (crashes + 1U)))
    {
        systemCall = left.Error;
        return PagerServeError::ManagementFailed;
    }
    WriteLine(context, recovery.Line, recovery.LineBytes);
    return PagerServeError::None;
}
} // Namespace

PagerServeError RunDriverDemo(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept
{
    return RunRecovery(context, CopyRecovery, systemCall);
}

PagerServeError RunFactorialDriverDemo(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept
{
    return RunRecovery(context, FactorialRecovery, systemCall);
}
} // Namespace Asteria::User::Pager
