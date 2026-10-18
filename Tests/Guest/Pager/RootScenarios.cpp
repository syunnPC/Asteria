// SPDX-License-Identifier: Apache-2.0
#include "Guest/Pager/RootScenarios.hpp"

#include "Abi/ComponentBootstrap.hpp"
#include "Abi/IpcCanonicalFrame.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/Pager/PagerWords.hpp"

namespace Asteria::User::Pager
{
namespace
{
constexpr Foundation::Size DeliveryHeaderBytes{48U};
// How long the root service lets the echo block again before it kills it:
// longer than a user time slice, so the echo got the processor.
constexpr Foundation::UInt64 KillPauseMicroseconds{100'000U};
static_assert(Component::EchoRequestBytes <= Kernel::Ipc::MessageCodec::MaximumWireSizeBytes);

// Whether exitCode is that of an incarnation of behavior that reported: the
// account's and the driver's are their own, and a driver on a kernel without
// an IOMMU has one of its own too (the runner requires each ISA's line).
[[nodiscard]] constexpr bool IsReportedExitCode(const Foundation::UInt64 behavior,
                                                const Foundation::UInt64 exitCode) noexcept
{
    return (behavior == Abi::ComponentBehaviorAccount) ? (exitCode == Abi::ComponentAccountExitCode)
           : (behavior == Abi::ComponentBehaviorDriver)
               ? ((exitCode == Abi::ComponentDriverExitCode) || (exitCode == Abi::ComponentDriverRegistersExitCode))
               : (exitCode == Abi::ComponentExitCode);
}

// Requires the report: the canonical frame on the report Endpoint.
[[nodiscard, gnu::noinline]] bool ReceiveReport(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept
{
    const Abi::SystemCallResponse received =
        context.Client->Receive(context.Report, AddressOf(context.Delivery), sizeof(context.Delivery));
    if (!Check(received, systemCall) ||
        (received.Value1 != (DeliveryHeaderBytes + sizeof(Abi::CanonicalAsynchronousFrame))))
    {
        return false;
    }
    bool equal{true};
    for (Foundation::Size index{0U}; index < sizeof(Abi::CanonicalAsynchronousFrame); ++index)
    {
        equal = equal && (context.Delivery[DeliveryHeaderBytes + index] == Abi::CanonicalAsynchronousFrame[index]);
    }
    return equal;
}

// The request carries a copy of a send capability on the reply Endpoint, on
// which the reply arrives, and a Frame moved to the service, which moves it
// back: the Frame returns under a new handle, and the one it left by is gone.
[[nodiscard, gnu::noinline]] PagerServeError Echo(ScenarioContext& context, const ComponentRecord& component,
                                                  Abi::SystemCallError& systemCall) noexcept
{
    System::CapabilityHandle frame{};
    if (!context.Calls->CreateMemory(context.Management, Abi::ObjectTypeFrame, 1U, 0U, frame, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    const Component::EchoAttachment attachments[2U]{
        {.Handle = context.Reply,
         .Rights = Component::EchoInvokeRight,
         .Mode = Component::EchoCopy,
         .Type = Component::EchoEndpointType},
        {.Handle = frame,
         .Rights = Component::EchoFrameRights,
         .Mode = Component::EchoMove,
         .Type = Component::EchoFrameType},
    };
    Component::WriteEchoFrame(context.Frame, attachments, 2U);
    if (!Check(context.Client->Send(component.Service, AddressOf(context.Frame), Component::EchoRequestBytes),
               systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    const Foundation::UInt64 received = AwaitMessage(context, component, context.Reply, systemCall);
    if ((received != (Component::EchoDeliveryHeaderBytes + Component::EchoReplyBytes)) ||
        (Component::GetEchoAttachmentCount(context.Delivery) != 1U) ||
        (Component::ReadEchoType(context.Delivery, 0U) != Component::EchoFrameType))
    {
        return PagerServeError::ManagementFailed;
    }
    const System::CapabilityHandle returned = Component::ReadEchoHandle(context.Delivery, 0U);
    const Abi::SystemCallResponse queried = context.Client->QueryCapability(returned);
    const Abi::SystemCallResponse stale = context.Client->QueryCapability(frame);
    if (!Check(queried, systemCall) || ((queried.Value0 & 0xFFU) != Abi::ObjectTypeFrame) ||
        (stale.Error == Abi::SystemCallError::None) ||
        !context.Calls->Destroy(returned, Abi::ObjectTypeFrame, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    return PagerServeError::None;
}

} // Namespace

// A crashing incarnation's fault reaches this pager, which terminates it as
// the supervisor of the domain it tagged; a hanging one is killed by the
// watchdog; a reporting one sends its report and exits. Either way its
// domain is torn down before the next is built.
PagerServeError RunIncarnation(ScenarioContext& context, const Foundation::UInt64 incarnation,
                               const Foundation::UInt64 behavior, Abi::SystemCallError& systemCall) noexcept
{
    ComponentSupervisor& supervisor = *context.Supervisor;
    ComponentRecord* const component = StartComponent(context, context.Component, incarnation, behavior, systemCall);
    if (component == nullptr)
    {
        return PagerServeError::ManagementFailed;
    }
    const PagerServeError served = supervisor.Supervise(*component, systemCall);
    if (served != PagerServeError::None)
    {
        return served;
    }
    const Foundation::UInt64 exitCode = component->ExitCode;
    const bool crashed = supervisor.HasCrashedSince(*component);
    // A hang may fault as it is killed: either kernel exit code ends it.
    const bool expected =
        (behavior == Abi::ComponentBehaviorCrash)
            ? ((exitCode == Abi::FaultTerminatedExitCode) && crashed)
            : ((behavior == Abi::ComponentBehaviorHang)
                   ? (((exitCode == Abi::KilledExitCode) && !crashed) ||
                      ((exitCode == Abi::FaultTerminatedExitCode) && crashed))
                   : (IsReportedExitCode(behavior, exitCode) && !crashed && ReceiveReport(context, systemCall)));
    if (!expected)
    {
        ReportUnexpectedEnd(context, incarnation, exitCode, crashed);
        return PagerServeError::ManagementFailed;
    }
    if (!supervisor.Teardown(*component, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    if (behavior == Abi::ComponentBehaviorAccount)
    {
        WriteLine(context, ComponentAccountLine, sizeof(ComponentAccountLine) - 1U);
        return PagerServeError::None;
    }
    if (behavior == Abi::ComponentBehaviorDriver)
    {
        if (exitCode == Abi::ComponentDriverRegistersExitCode)
        {
            WriteLine(context, ComponentDriverRegistersLine, sizeof(ComponentDriverRegistersLine) - 1U);
            return PagerServeError::None;
        }
        if (context.Launch->Component.Image.Machine == Loader::ExecutableMachine::Ia64)
        {
            WriteLine(context, ComponentDriverIntxLine, sizeof(ComponentDriverIntxLine) - 1U);
        }
        else
        {
            WriteLine(context, ComponentDriverLine, sizeof(ComponentDriverLine) - 1U);
        }
        return PagerServeError::None;
    }
    const char* const line =
        (behavior == Abi::ComponentBehaviorCrash)
            ? ComponentCrashedLine
            : ((behavior == Abi::ComponentBehaviorHang) ? ComponentHungLine : ComponentReportedLine);
    const Foundation::Size bytes =
        (behavior == Abi::ComponentBehaviorCrash)
            ? (sizeof(ComponentCrashedLine) - 1U)
            : ((behavior == Abi::ComponentBehaviorHang) ? (sizeof(ComponentHungLine) - 1U)
                                                        : (sizeof(ComponentReportedLine) - 1U));
    WriteLine(context, line, bytes);
    return PagerServeError::None;
}

// A service runs until it is killed: this one exchanges one
// request and is then killed while it waits for the next, so its wait is
// aborted and it ends as it resumes, with KilledExitCode.
PagerServeError RunEcho(ScenarioContext& context, const Foundation::UInt64 tag,
                        Abi::SystemCallError& systemCall) noexcept
{
    ComponentSupervisor& supervisor = *context.Supervisor;
    ComponentRecord* const component =
        StartComponent(context, context.Component, tag, Abi::ComponentBehaviorEcho, systemCall);
    if (component == nullptr)
    {
        return PagerServeError::ManagementFailed;
    }
    const PagerServeError echoed = Echo(context, *component, systemCall);
    if (echoed != PagerServeError::None)
    {
        return echoed;
    }
    const PagerServeError paused = supervisor.Pause(KillPauseMicroseconds, systemCall);
    if (paused != PagerServeError::None)
    {
        return paused;
    }
    const PagerServeError stopped = supervisor.Stop(*component, systemCall);
    if (stopped != PagerServeError::None)
    {
        return stopped;
    }
    const bool crashed = supervisor.HasCrashedSince(*component);
    if ((component->ExitCode != Abi::KilledExitCode) || crashed)
    {
        ReportUnexpectedEnd(context, tag, component->ExitCode, crashed);
        return PagerServeError::ManagementFailed;
    }
    if (!supervisor.Teardown(*component, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    WriteLine(context, ComponentEchoedLine, sizeof(ComponentEchoedLine) - 1U);
    return PagerServeError::None;
}
} // Namespace Asteria::User::Pager
