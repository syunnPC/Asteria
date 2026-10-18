// SPDX-License-Identifier: Apache-2.0
#include "User/Pager/ComponentSupervisor.hpp"
#include "User/Pager/PagerWords.hpp"
#include "Support/TestContext.hpp"
#include <cstring>
#include <initializer_list>

namespace
{
namespace Abi = Asteria::Abi;
namespace Foundation = Asteria::Foundation;
namespace TestSupport = Asteria::TestSupport;
namespace User = Asteria::User;
Foundation::UInt64 g_now{};
Foundation::UInt64 Clock() noexcept
{
    return g_now;
}
struct Model
{
    Foundation::UInt64 Transaction{};
    Foundation::UInt64 Creates{};
    Foundation::UInt64 CreationAuthority{};
    Foundation::UInt64 CreatedType{};
    Foundation::UInt64 ReceivedEndpoint{};
    Foundation::UInt64 Sends{};
    Foundation::UInt64 Recoveries{};
    Foundation::UInt64 SendRefusals{};
    Foundation::UInt64 Kills{};
    Foundation::UInt64 KilledAt{};
    bool TaskEnded{};
    bool KillBlocksOnFault{};
    bool Reply{};
    bool Attach{};
    bool Malformed{};
    Foundation::UInt64 Closed{};
    bool Stale{};
    bool HoldStartup{};
    bool SessionReply{};
    bool InputReply{};
    bool HoldPrivateReply{};
    Foundation::UInt64 Diagnostics{};
    char Diagnostic[128U]{};
    static void Transport(void* const context, const Abi::SystemCallRequest* const request,
                          Abi::SystemCallResponse* const response) noexcept
    {
        auto& model = *static_cast<Model*>(context);
        *response = {};
        switch (static_cast<Abi::SystemCallNumber>(request->Number))
        {
        case Abi::SystemCallNumber::ObjectCreate:
            model.CreationAuthority = request->Arguments[0U];
            model.CreatedType = request->Arguments[2U];
            response->Value0 = ++model.Creates;
            response->Value1 = 42U;
            break;
        case Abi::SystemCallNumber::IpcSend:
            model.Transaction =
                User::System::LoadWord(reinterpret_cast<const Foundation::UInt8*>(request->Arguments[2U]) + 40U);
            ++model.Sends;
            if (model.SendRefusals != 0U)
            {
                --model.SendRefusals;
                response->Error = Abi::SystemCallError::WouldBlock;
            }
            break;
        case Abi::SystemCallNumber::CapabilityClose:
            if (request->Arguments[0U] == 777U)
            {
                ++model.Closed;
            }
            break;
        case Abi::SystemCallNumber::IpcReceive:
            model.ReceivedEndpoint = request->Arguments[0U];
            if (!model.Reply || (model.HoldPrivateReply && request->Arguments[0U] != 98U))
            {
                response->Error = Abi::SystemCallError::WouldBlock;
                break;
            }
            {
                auto* const delivery = reinterpret_cast<Foundation::UInt8*>(request->Arguments[2U]);
                std::memset(delivery, 0, 552U);
                User::Component::DisplayInfo info{};
                info.Version = 2U;
                const Foundation::UInt64 words[4U]{
                    model.Transaction, static_cast<Foundation::UInt64>(User::Session::SessionAnswer::Done), 0U, 0U};
                const Foundation::UInt64 inputWords[4U]{
                    1U, static_cast<Foundation::UInt64>(User::Component::InputAnswer::Done), model.Attach ? 1U : 0U,
                    model.Attach ? User::Component::InputProtocolVersion : 0U};
                const auto bytes =
                    model.InputReply
                        ? User::Component::WriteInputFrame(delivery + User::Component::EchoDeliveryHeaderBytes,
                                                           User::Component::InputReplyMethod, model.Transaction,
                                                           inputWords, nullptr, 0U)
                    : model.SessionReply
                        ? User::Session::WriteSessionFrame(delivery + User::Component::EchoDeliveryHeaderBytes,
                                                           User::Session::SessionReplyMethod, model.Transaction, words,
                                                           nullptr, 0U)
                        : User::Component::WriteDisplayReply(delivery + User::Component::EchoDeliveryHeaderBytes,
                                                             model.Transaction - (model.Stale ? 1U : 0U),
                                                             User::Component::DisplayDone, info);
                auto deliveredBytes = bytes;
                if (model.Attach)
                {
                    auto* frame = delivery + User::Component::EchoDeliveryHeaderBytes;
                    const auto descriptors = model.Malformed ? 1U : 0U;
                    const auto at = User::Component::EchoHeaderBytes + descriptors * 8U;
                    std::memmove(frame + at + 32U, frame + User::Component::EchoHeaderBytes,
                                 bytes - User::Component::EchoHeaderBytes);
                    std::memset(frame + User::Component::EchoHeaderBytes, 0, descriptors * 8U + 32U);
                    frame[80U] = static_cast<Foundation::UInt8>(descriptors);
                    frame[81U] = 1U;
                    User::System::StoreWord(frame + at, 777U);
                    User::System::StoreWord(frame + at + 8U, 42U);
                    User::System::StoreWord(frame + at + 16U, ~Foundation::UInt64{0U});
                    frame[at + 24U] = User::Component::EchoInvokeRight;
                    frame[at + 25U] = 1U;
                    frame[at + 26U] = User::Component::EchoCopy;
                    frame[at + 27U] = User::Component::EchoEndpointType;
                    // A kernel-valid zero-length descriptor after the fixed
                    // prefix is foreign to these fixed-schema reply codecs.
                    if (descriptors != 0U)
                    {
                        frame[User::Component::EchoHeaderBytes] = frame[14U];
                        frame[User::Component::EchoHeaderBytes + 1U] = frame[15U];
                    }
                    deliveredBytes += descriptors * 8U + 32U;
                    frame[8U] = static_cast<Foundation::UInt8>(deliveredBytes);
                    frame[9U] = static_cast<Foundation::UInt8>(deliveredBytes >> 8U);
                }
                response->Value1 = deliveredBytes + User::Component::EchoDeliveryHeaderBytes;
                model.Reply = false;
            }
            break;
        case Abi::SystemCallNumber::WaitSetWait:
            g_now += request->Arguments[2U];
            // Model the channel owner terminating its delivered fault.
            if (model.KillBlocksOnFault)
            {
                model.TaskEnded = true;
            }
            model.Reply = !model.HoldStartup;
            response->Error = Abi::SystemCallError::TimedOut;
            break;
        case Abi::SystemCallNumber::ObjectDestroy:
            if (request->Arguments[2U] == Abi::ObjectTypeThread)
            {
                response->Error = model.TaskEnded ? Abi::SystemCallError::None : Abi::SystemCallError::WouldBlock;
                response->Value0 = Abi::KilledExitCode;
            }
            break;
        case Abi::SystemCallNumber::ThreadTerminate:
            ++model.Kills;
            model.KilledAt = g_now;
            model.TaskEnded = !model.KillBlocksOnFault;
            if (model.KillBlocksOnFault)
            {
                response->Error = Abi::SystemCallError::WouldBlock;
            }
            break;
        case Abi::SystemCallNumber::DebugWrite:
            ++model.Diagnostics;
            std::memset(model.Diagnostic, 0, sizeof(model.Diagnostic));
            std::memcpy(model.Diagnostic, reinterpret_cast<const void*>(request->Arguments[2U]),
                        static_cast<Foundation::Size>(request->Arguments[3U]));
            break;
        default:
            break;
        }
    }
    static bool Recover(void* const context, User::Pager::ComponentRecord&, Abi::SystemCallError&) noexcept
    {
        ++static_cast<Model*>(context)->Recoveries;
        return true;
    }
};
} // namespace
int main()
{
    TestSupport::TestContext context{"ComponentWatchdogTests"};
    Model model{};
    const User::System::SystemCallClient client{{Model::Transport, &model}};
    User::Pager::ObjectCalls calls;
    calls.Configure(client);
    static User::Pager::ComponentSupervisor supervisor;
    static User::Pager::ComponentLoader loader;
    User::Pager::DeviceManagerRole devices;
    User::Pager::PagerBootstrap bootstrap{};
    bootstrap.ClockTicksPerMicrosecond = 1U;
    bootstrap.Management = {101U, 42U};
    bootstrap.Endpoint = {102U, 42U};
    bootstrap.WatchdogSeconds = 5U;
    User::Pager::PagerProgram pager{client, bootstrap};
    User::Pager::ComponentPlatform platform{};
    platform.Clock = Clock;
    supervisor.Configure(calls, loader, devices, pager, bootstrap, platform);
    User::Pager::ComponentRecord component{};
    component.Behavior = Abi::ComponentBehaviorDisplay;
    component.Service = {99U, 42U};
    component.Account = {103U, 42U};
    component.State = User::Pager::ComponentState::Running;
    Abi::SystemCallError error{};
    ASTERIA_EXPECT(context, "private reply registration",
                   supervisor.WatchService(component, error, Model::Recover, &model), model.Creates == 1U);
    ASTERIA_EXPECT(context, "reply object uses the root grant, not the component's restricted creation limits",
                   model.CreatedType == Abi::ObjectTypeEndpoint,
                   model.CreationAuthority == bootstrap.Management.Selector);
    supervisor.PollLiveness();
    ASTERIA_EXPECT(context, "one asynchronous ping", model.Sends == 1U, model.Transaction >> 63U == 1U);
    g_now = 4'999'999U;
    supervisor.PollLiveness();
    ASTERIA_EXPECT(context, "bounded reply wait has no premature miss", model.Diagnostic[0U] == '\0',
                   model.Recoveries == 0U);
    model.Reply = true;
    model.Stale = true;
    g_now = 5'000'000U;
    supervisor.PollLiveness();
    ASTERIA_EXPECT(context, "stale reply cannot renew liveness",
                   std::strcmp(model.Diagnostic, "# watchdog: display-broker unresponsive for 5000 ms\n") == 0,
                   model.Recoveries == 0U);
    ASTERIA_EXPECT(context, "Query answers use the private endpoint and leave fault deliveries to the pager",
                   model.ReceivedEndpoint == 1U, model.ReceivedEndpoint != bootstrap.Endpoint.Selector);
    g_now = 15'000'000U;
    supervisor.PollLiveness();
    ASTERIA_EXPECT(context, "three missed periods invoke owner recovery", model.Recoveries == 1U,
                   User::Pager::AsteriaComponentWatchdogEvidence[0U].Restarts == 1U);
    model.Stale = false;
    model.Reply = true;
    g_now = 15'000'001U;
    supervisor.PollLiveness();
    ASTERIA_EXPECT(context, "matching reply starts a new check", model.Sends == 2U);
    const auto reports = User::Pager::AsteriaComponentWatchdogEvidence[0U].Reports;
    bootstrap.WatchdogSeconds = 0U;
    g_now = 999'999'999U;
    supervisor.PollLiveness();
    ASTERIA_EXPECT(context, "off suppresses queries and reports", model.Sends == 2U,
                   User::Pager::AsteriaComponentWatchdogEvidence[0U].Reports == reports);
    supervisor.UnwatchService(component);
    // All imported attachments belong to root even if the protocol, method
    // or transaction is rejected. A Query reply never carries authority.
    bootstrap.WatchdogSeconds = 5U;
    for (const auto behavior :
         {Abi::ComponentBehaviorDisplay, Abi::ComponentBehaviorInput, Abi::ComponentBehaviorCompositor})
    {
        for (const bool malformed : {false, true})
        {
            component.Behavior = behavior;
            component.Tag = 1U;
            model.InputReply = behavior == Abi::ComponentBehaviorInput;
            model.SessionReply = behavior == Abi::ComponentBehaviorCompositor;
            model.Attach = true;
            model.Malformed = malformed;
            model.Stale = false;
            model.Sends = 0U;
            model.Reply = false;
            g_now = 0U;
            ASTERIA_EXPECT(context, "register attachment rejection probe", supervisor.WatchService(component, error));
            supervisor.PollLiveness();
            const auto closedBefore = model.Closed;
            model.Reply = true;
            g_now = 5'000'000U;
            supervisor.PollLiveness();
            ASTERIA_EXPECT(context, "unexpected and undecodable Query attachments are closed without renewing liveness",
                           model.Closed == closedBefore + 1U, model.Sends == 1U);
            supervisor.UnwatchService(component);
        }
    }
    model.Attach = false;
    model.Malformed = false;
    model.InputReply = false;
    // A slow but healthy first scanout must never acquire a live miss age.
    bootstrap.WatchdogSeconds = Abi::WatchdogDefaultSeconds;
    component.Behavior = Abi::ComponentBehaviorCompositor;
    component.Tag = 1U;
    g_now = 0U;
    model.Sends = 0U;
    model.Reply = false;
    model.Diagnostics = 0U;
    model.Diagnostic[0U] = '\0';
    model.Recoveries = 0U;
    model.SessionReply = true;
    ASTERIA_EXPECT(
        context, "session startup registers its owner without a live Query",
        supervisor.WatchService(component, error, Model::Recover, &model, User::Pager::ComponentStartupMicroseconds));
    supervisor.PollLiveness();
    g_now = User::Pager::ComponentStartupMicroseconds - 1U;
    supervisor.PollLiveness();
    ASTERIA_EXPECT(context, "healthy startup outlasts three live periods without misses or restarts", model.Sends == 0U,
                   model.Diagnostics == 0U, model.Recoveries == 0U);
    ASTERIA_EXPECT(context, "the first Presented marks a healthy compositor ready",
                   supervisor.MarkServiceReady(component), !supervisor.ServiceStartupExpired(component));
    supervisor.PollLiveness();
    ASTERIA_EXPECT(context, "liveness starts only after readiness", model.Sends == 1U);
    model.Reply = true;
    g_now += 5'000'000U;
    supervisor.PollLiveness();
    ASTERIA_EXPECT(context, "a ready session answers its live Query with no startup miss", model.Sends == 2U,
                   model.Diagnostics == 0U, model.Recoveries == 0U);
    // The broker's first session scanout receives the same allowance;
    // in-flight old Queries must not retain their old miss age across it.
    ASTERIA_EXPECT(context, "register the first visible native scanout allowance",
                   supervisor.BeginServiceStartup(component, error));
    g_now += 20'000'000U;
    supervisor.PollLiveness();
    ASTERIA_EXPECT(context, "first native session scanout defers an already-pending live Query", model.Sends == 2U,
                   model.Diagnostics == 0U, model.Recoveries == 0U);
    ASTERIA_EXPECT(context, "first native session scanout can complete its readiness allowance",
                   supervisor.MarkServiceReady(component));
    supervisor.UnwatchService(component);
    // A component that never becomes ready is bounded even with live
    // watchdogs off. AwaitMessage must return the owner to its fallback path.
    bootstrap.WatchdogSeconds = 0U;
    g_now = 0U;
    model.Sends = 0U;
    model.Diagnostics = 0U;
    model.Reply = false;
    model.HoldStartup = true;
    ASTERIA_EXPECT(
        context, "off retains the bounded startup deadline",
        supervisor.WatchService(component, error, Model::Recover, &model, User::Pager::ComponentStartupMicroseconds));
    g_now = 40'000'000U;
    ASTERIA_EXPECT(
        context, "installing the owner hook cannot extend an existing startup deadline",
        supervisor.WatchService(component, error, Model::Recover, &model, User::Pager::ComponentStartupMicroseconds));
    alignas(8U) Foundation::UInt8 startupDelivery[552U]{};
    ASTERIA_EXPECT(context, "never-ready startup returns control for fallback at the original deadline",
                   supervisor.AwaitMessage(component, {98U, 42U}, startupDelivery, sizeof(startupDelivery), error) ==
                       0U,
                   g_now == User::Pager::ComponentStartupMicroseconds, supervisor.ServiceStartupExpired(component));
    ASTERIA_EXPECT(context, "startup expiry reports once without a live miss or restart",
                   std::strcmp(model.Diagnostic, "# startup: session-compositor not ready within 60000 ms\n") == 0,
                   model.Diagnostics == 1U, model.Sends == 0U, model.Recoveries == 0U);
    g_now += 1'000'000U;
    supervisor.PollLiveness();
    ASTERIA_EXPECT(context, "an expired startup cannot produce repeated miss lines", model.Diagnostics == 1U);
    ASTERIA_EXPECT(context, "late readiness still requires owner fallback", !supervisor.MarkServiceReady(component));
    supervisor.PollLiveness();
    ASTERIA_EXPECT(context, "off keeps live Query supervision disabled after startup", model.Sends == 0U);
    supervisor.UnwatchService(component);
    component.Behavior = Abi::ComponentBehaviorDisplay;
    g_now = 0U;
    model.Diagnostics = 0U;
    model.Reply = false;
    auto* const consoleBroker = supervisor.Allocate();
    *consoleBroker = component;
    consoleBroker->Tag = 2U;
    ASTERIA_EXPECT(context, "off still registers a deadline for a previously ready console broker's native scanout",
                   supervisor.BeginServiceStartup(*consoleBroker, error));
    g_now = User::Pager::ComponentStartupMicroseconds;
    supervisor.PollLiveness();
    ASTERIA_EXPECT(context, "a visible scanout that never completes expires without live Queries",
                   supervisor.ServiceStartupExpired(*consoleBroker), model.Diagnostics == 1U, model.Sends == 0U);
    ASTERIA_EXPECT(context, "fallback can resume the previously ready console broker after late scanout",
                   !supervisor.MarkServiceReady(*consoleBroker), !supervisor.ServiceStartupExpired(*consoleBroker));
    supervisor.UnwatchService(*consoleBroker);
    consoleBroker->State = User::Pager::ComponentState::Free;
    // Broker readiness is a successful first manager reply. Ordinary
    // replies after that must not acknowledge a private liveness Query.
    component.Behavior = Abi::ComponentBehaviorInput;
    bootstrap.WatchdogSeconds = Abi::WatchdogDefaultSeconds;
    g_now = 0U;
    model.Diagnostics = 0U;
    model.Sends = 0U;
    model.Reply = false;
    model.HoldStartup = false;
    model.InputReply = true;
    model.HoldPrivateReply = true;
    ASTERIA_EXPECT(
        context, "register broker startup before its first manager reply",
        supervisor.WatchService(component, error, nullptr, nullptr, User::Pager::ComponentStartupMicroseconds));
    ASTERIA_EXPECT(context, "the first successful broker reply ends startup without a premature Query",
                   supervisor.AwaitMessage(component, {98U, 42U}, startupDelivery, sizeof(startupDelivery), error) !=
                       0U,
                   model.Sends == 0U, model.Diagnostics == 0U);
    supervisor.PollLiveness();
    const auto firstBrokerQuery = g_now;
    ASTERIA_EXPECT(context, "a ready broker receives its own private liveness Query", model.Sends == 1U);
    ASTERIA_EXPECT(context, "later manager replies leave the pending private Query outstanding",
                   supervisor.AwaitMessage(component, {98U, 42U}, startupDelivery, sizeof(startupDelivery), error) !=
                       0U);
    g_now = firstBrokerQuery + 5'000'000U;
    supervisor.PollLiveness();
    ASTERIA_EXPECT(context, "ordinary input replies cannot hide a missing watchdog answer",
                   std::strcmp(model.Diagnostic, "# watchdog: input-broker unresponsive for 5000 ms\n") == 0,
                   model.Diagnostics == 1U, model.Sends == 1U);
    supervisor.UnwatchService(component);
    model.InputReply = false;
    model.HoldPrivateReply = false;
    component.Behavior = Abi::ComponentBehaviorDisplay;
    model.HoldStartup = false;
    model.SessionReply = false;
    bootstrap.WatchdogSeconds = Abi::WatchdogDefaultSeconds;
    g_now = 0U;
    model.Reply = false;
    model.SendRefusals = 1U;
    model.Sends = 0U;
    ASTERIA_EXPECT(context, "register after queue pressure", supervisor.WatchService(component, error));
    supervisor.PollLiveness();
    supervisor.PollLiveness();
    ASTERIA_EXPECT(context, "a refused Query is retried within the original deadline", model.Sends == 2U);
    supervisor.UnwatchService(component);
    bootstrap.WatchdogSeconds = Abi::WatchdogDefaultSeconds;
    g_now = 0U;
    model.Sends = 0U;
    ASTERIA_EXPECT(context, "register for task-wait progress", supervisor.WatchService(component, error));
    supervisor.PollLiveness();
    auto* const task = supervisor.Allocate();
    task->State = User::Pager::ComponentState::Running;
    task->Mode = User::Pager::ComponentMode::Task;
    task->Thread = {11U, 42U};
    task->WatchdogMicroseconds = 6'000'000U;
    ASTERIA_EXPECT(context, "service queries continue while the task exit deadline is preserved",
                   supervisor.Supervise(*task, error) == User::Pager::PagerServeError::None, model.Kills == 1U,
                   model.KilledAt == 6'000'000U, model.Sends == 2U);
    supervisor.UnwatchService(component);
    auto* const faulted = supervisor.Allocate();
    faulted->State = User::Pager::ComponentState::Running;
    faulted->Mode = User::Pager::ComponentMode::Service;
    faulted->Thread = {12U, 42U};
    model.TaskEnded = false;
    model.KillBlocksOnFault = true;
    ASTERIA_EXPECT(context, "recovery waits for a fault-blocked thread's supervisor before draining",
                   supervisor.Stop(*faulted, error) == User::Pager::PagerServeError::None,
                   faulted->State == User::Pager::ComponentState::Ended, model.Kills == 2U);
    return context.Finish();
}
