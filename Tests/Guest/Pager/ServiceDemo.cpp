// SPDX-License-Identifier: Apache-2.0
#include "Guest/Pager/RootScenarios.hpp"

#include "Executive/ObjectDirectory/ObjectDirectoryProtocol.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"
#include "User/Client/ClientAbi.hpp"
#include "User/Directory/DirectoryAbi.hpp"
#include "User/Pager/PagerWords.hpp"

namespace Asteria::User::Pager
{
namespace
{
constexpr char EchoName[] = "echo";
// How long the root service lets its services block again before it stops
// them: longer than a user time slice.
constexpr Foundation::UInt64 StopPauseMicroseconds{100'000U};
constexpr Foundation::Size DeliveryHeaderBytes{48U};

[[nodiscard]] Kernel::Object::CapabilityHandle ToKernel(const System::CapabilityHandle handle) noexcept
{
    return Kernel::Object::CapabilityHandle{handle.Selector, handle.Authenticator};
}

[[nodiscard]] Executive::ObjectDirectory::DirectoryName MakeName(const char* const text) noexcept
{
    Foundation::UInt8 bytes[Executive::ObjectDirectory::DirectoryName::MaximumSizeBytes]{};
    Foundation::Size size{0U};
    while (text[size] != '\0')
    {
        bytes[size] = static_cast<Foundation::UInt8>(text[size]);
        ++size;
    }
    const auto created = Executive::ObjectDirectory::DirectoryName::Create(
        Executive::ObjectDirectory::DirectoryNameView{.Data = bytes, .SizeBytes = size});
    return created.HasValue() ? created.GetValue() : Executive::ObjectDirectory::DirectoryName{};
}

[[nodiscard]] constexpr Executive::ObjectDirectory::DirectoryMessageContext
MakeContext(const Foundation::UInt64 transaction) noexcept
{
    return Executive::ObjectDirectory::DirectoryMessageContext{
        .Trace = {}, .Deadline = Kernel::Object::MonotonicTime::Infinite(), .TransactionIdentifier = transaction};
}

// Service IPC: the root service binds `echo` in the Directory, as its only binder, with the Manage
// its capability on the Directory's Endpoint holds: the binding holds a copy of a send capability
// On Echo's Endpoint (Invoke, and Duplicate for the copies resolvers get) under Echo's tag as its
// service generation. Every deadline is infinite.
[[nodiscard, gnu::noinline]] bool BuildBind(ScenarioContext& context, const ComponentRecord& echo) noexcept
{
    const Executive::ObjectDirectory::BindIfAbsentRequest request{
        .Name = MakeName(EchoName),
        .ServiceInterface = Client::EchoInterface,
        .ServiceGeneration = echo.Tag,
        .Version = {.Major = 1U, .MinimumMinor = 0U, .MaximumMinor = 0U, .Reserved = 0U},
        .PublishedRights = Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Invoke},
        .TargetEndpoint = ToKernel(echo.Service),
        .TargetDeadline = Kernel::Object::MonotonicTime::Infinite(),
        .ReplyEndpoint = ToKernel(context.Reply),
        .Context = MakeContext(echo.Tag),
    };
    context.Message = Kernel::Ipc::MessageEnvelope{};
    return Executive::ObjectDirectory::ObjectDirectoryProtocol::BuildBindIfAbsentRequest(request, context.Message) ==
           Executive::ObjectDirectory::ObjectDirectoryProtocolError::None;
}

// A restarted Echo replaces the binding, if it is still the version the root service bound, with
// its own Endpoint under its own tag.
[[nodiscard, gnu::noinline]] bool BuildReplace(ScenarioContext& context, const ComponentRecord& echo,
                                               const Executive::ObjectDirectory::BindingVersion& version) noexcept
{
    const Executive::ObjectDirectory::ReplaceIfVersionRequest request{
        .Name = MakeName(EchoName),
        .ExpectedVersion = version,
        .ServiceInterface = Client::EchoInterface,
        .ServiceGeneration = echo.Tag,
        .Version = {.Major = 1U, .MinimumMinor = 0U, .MaximumMinor = 0U, .Reserved = 0U},
        .PublishedRights = Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Invoke},
        .TargetEndpoint = ToKernel(echo.Service),
        .TargetDeadline = Kernel::Object::MonotonicTime::Infinite(),
        .ReplyEndpoint = ToKernel(context.Reply),
        .Context = MakeContext(echo.Tag),
    };
    context.Message = Kernel::Ipc::MessageEnvelope{};
    return Executive::ObjectDirectory::ObjectDirectoryProtocol::BuildReplaceIfVersionRequest(
               request, context.Message) == Executive::ObjectDirectory::ObjectDirectoryProtocolError::None;
}

// Sends the bind or replace in the context's message to the Directory and
// requires its success on the root service's reply Endpoint: the binding's
// new version.
[[nodiscard, gnu::noinline]] PagerServeError Publish(ScenarioContext& context, const ComponentRecord& directory,
                                                     const bool replace,
                                                     Executive::ObjectDirectory::BindingVersion& version,
                                                     Abi::SystemCallError& systemCall) noexcept
{
    const auto encoded = Kernel::Ipc::MessageCodec::Encode(
        context.Message, Kernel::Ipc::WritableByteView{context.Frame, sizeof(context.Frame)});
    if (!encoded.HasValue() ||
        !Check(context.Client->Send(directory.Service, AddressOf(context.Frame), encoded.GetValue()), systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    const Foundation::UInt64 received = AwaitMessage(context, directory, context.Reply, systemCall);
    Executive::ObjectDirectory::MutationReply reply{};
    if ((received <= DeliveryHeaderBytes) ||
        (Kernel::Ipc::MessageCodec::Decode(
             Kernel::Ipc::ReadOnlyByteView{context.Delivery + DeliveryHeaderBytes,
                                           static_cast<Foundation::Size>(received - DeliveryHeaderBytes)},
             context.Message) != Kernel::Ipc::MessageCodecError::None) ||
        ((replace
              ? Executive::ObjectDirectory::ObjectDirectoryProtocol::ParseReplaceIfVersionReply(context.Message, reply)
              : Executive::ObjectDirectory::ObjectDirectoryProtocol::ParseBindIfAbsentReply(context.Message, reply)) !=
         Executive::ObjectDirectory::ObjectDirectoryProtocolError::None) ||
        (reply.Status != Executive::ObjectDirectory::DirectoryStatus::Success) ||
        (reply.Version.DirectoryGeneration != Directory::DirectoryGeneration))
    {
        return PagerServeError::ManagementFailed;
    }
    version = reply.Version;
    return PagerServeError::None;
}

// A service ends only by a fault or a kill: killed while it
// waits, its wait is aborted and it ends as it resumes; the watchdog bounds
// the wait for that end, and the service is then torn down.
[[nodiscard, gnu::noinline]] PagerServeError StopService(ScenarioContext& context, ComponentRecord& service,
                                                         Abi::SystemCallError& systemCall) noexcept
{
    const PagerServeError served = context.Supervisor->Stop(service, systemCall);
    if (served != PagerServeError::None)
    {
        return served;
    }
    if (service.ExitCode != Abi::KilledExitCode)
    {
        ReportUnexpectedEnd(context, service.Tag, service.ExitCode, false);
        return PagerServeError::ManagementFailed;
    }
    return context.Supervisor->Teardown(service, systemCall) ? PagerServeError::None
                                                             : PagerServeError::ManagementFailed;
}

// Echo is killed while it waits and torn down, which destroys its
// Endpoint and so every capability on it, the Directory's and the client's
// included; a fresh incarnation in a fresh domain, under the next tag,
// replaces the binding.
[[nodiscard, gnu::noinline]] PagerServeError RestartEcho(ScenarioContext& context, const ComponentRecord& directory,
                                                         ComponentRecord*& echo,
                                                         Executive::ObjectDirectory::BindingVersion& version,
                                                         Abi::SystemCallError& systemCall) noexcept
{
    const PagerServeError stopped = StopService(context, *echo, systemCall);
    if (stopped != PagerServeError::None)
    {
        return stopped;
    }
    echo = StartComponent(context, context.Component, context.NextTag++, Abi::ComponentBehaviorEcho, systemCall);
    if ((echo == nullptr) || !BuildReplace(context, *echo, version))
    {
        return PagerServeError::ManagementFailed;
    }
    return Publish(context, directory, true, version, systemCall);
}

// The client's report in the context's delivery buffer, of received bytes:
// it resolved `echo` at generation, every echo came back, and its stale send
// failed with stale (None in the first round).
[[nodiscard, gnu::noinline]] bool ReadClientReport(ScenarioContext& context, const Foundation::UInt64 received,
                                                   const Foundation::UInt64 generation,
                                                   const Abi::SystemCallError stale) noexcept
{
    const Foundation::UInt8* const payload = context.Message.Payload;
    return (received > DeliveryHeaderBytes) &&
           (Kernel::Ipc::MessageCodec::Decode(
                Kernel::Ipc::ReadOnlyByteView{context.Delivery + DeliveryHeaderBytes,
                                              static_cast<Foundation::Size>(received - DeliveryHeaderBytes)},
                context.Message) == Kernel::Ipc::MessageCodecError::None) &&
           (context.Message.Interface.GetLow() == Client::ClientReportInterface.GetLow()) &&
           (context.Message.Interface.GetHigh() == Client::ClientReportInterface.GetHigh()) &&
           (context.Message.PayloadSizeBytes == Client::ClientReportPayloadBytes) &&
           (context.Message.AttachmentCount == 0U) && (LoadWord(payload) == 1U) &&
           (LoadWord(payload + 8U) == Client::ClientEchoes) && (LoadWord(payload + 16U) == generation) &&
           (LoadWord(payload + 24U) == static_cast<Foundation::UInt64>(stale));
}

// The go that starts the client's second round, on its own Endpoint.
[[nodiscard, gnu::noinline]] bool SendGo(ScenarioContext& context, const ComponentRecord& client,
                                         Abi::SystemCallError& systemCall) noexcept
{
    context.Message = Kernel::Ipc::MessageEnvelope{};
    context.Message.Interface = Client::ClientControlInterface;
    context.Message.TransactionIdentifier = client.Tag;
    context.Message.Deadline = Kernel::Object::MonotonicTime::Infinite();
    context.Message.MethodIdentifier = Client::ClientGoMethod;
    context.Message.InterfaceVersionMajor = 1U;
    context.Message.Kind = Kernel::Ipc::MessageKind::Asynchronous;
    context.Message.Cancellation = Kernel::Ipc::CancellationState::None;
    const auto encoded = Kernel::Ipc::MessageCodec::Encode(
        context.Message, Kernel::Ipc::WritableByteView{context.Frame, sizeof(context.Frame)});
    return encoded.HasValue() &&
           Check(context.Client->Send(client.Service, AddressOf(context.Frame), encoded.GetValue()), systemCall);
}

// The client runs as a task: its first round resolves `echo` and gets its
// echoes under its watchdog, and its report arrives while it waits for the
// go. It then waits without a deadline (per-record deadlines are deferred)
// while Echo is restarted and rebound; after the go, its second round must
// find its stale send refused and resolve the restarted Echo. Its exit and
// second report are required before it is torn down.
[[nodiscard, gnu::noinline]] PagerServeError RunClient(ScenarioContext& context, const ComponentRecord& directory,
                                                       ComponentRecord*& echo,
                                                       Executive::ObjectDirectory::BindingVersion& version,
                                                       Abi::SystemCallError& systemCall) noexcept
{
    ComponentSupervisor& supervisor = *context.Supervisor;
    ComponentRecord* const client =
        StartComponent(context, context.ClientImage, context.NextTag++, Abi::ComponentBehaviorClient, systemCall);
    if (client == nullptr)
    {
        return PagerServeError::ManagementFailed;
    }
    const Foundation::UInt64 first = AwaitMessage(context, *client, context.Report, systemCall);
    if (!ReadClientReport(context, first, echo->Tag, Abi::SystemCallError::None))
    {
        ReportUnexpectedEnd(context, client->Tag, client->ExitCode, false);
        return PagerServeError::ManagementFailed;
    }
    WriteLine(context, ClientResolvedLine, sizeof(ClientResolvedLine) - 1U);
    supervisor.SetWatchdog(*client, false);
    const PagerServeError restarted = RestartEcho(context, directory, echo, version, systemCall);
    if (restarted != PagerServeError::None)
    {
        return restarted;
    }
    WriteLine(context, EchoReboundLine, sizeof(EchoReboundLine) - 1U);
    if (!SendGo(context, *client, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    supervisor.SetWatchdog(*client, true);
    const PagerServeError served = supervisor.Supervise(*client, systemCall);
    if (served != PagerServeError::None)
    {
        return served;
    }
    const Abi::SystemCallResponse second =
        context.Client->Receive(context.Report, AddressOf(context.Delivery), sizeof(context.Delivery));
    if ((client->ExitCode != Client::ClientExitCode) || !Check(second, systemCall) ||
        !ReadClientReport(context, second.Value1, echo->Tag, Abi::SystemCallError::ObjectNotFound))
    {
        ReportUnexpectedEnd(context, client->Tag, client->ExitCode, false);
        return PagerServeError::ManagementFailed;
    }
    if (!supervisor.Teardown(*client, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    WriteLine(context, ClientRecoveredLine, sizeof(ClientRecoveredLine) - 1U);
    return PagerServeError::None;
}
} // Namespace

// The services the root service runs side by side and their client. The Directory and Echo run as
// services, each in a domain of its own; the root service binds `echo`, runs the client where the
// image has one, restarting Echo meanwhile, lets both services block waiting, then stops Echo and
// the Directory and tears both down. With several components live, a crash cannot be attributed to
// one of them, so any crash fails the demonstration.
PagerServeError RunServiceDemo(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept
{
    ComponentSupervisor& supervisor = *context.Supervisor;
    const Foundation::UInt64 crashes = supervisor.GetCrashes();
    ComponentRecord* const directory =
        StartComponent(context, context.Directory, context.NextTag++, Abi::ComponentBehaviorDirectory, systemCall);
    if (directory == nullptr)
    {
        return PagerServeError::ManagementFailed;
    }
    context.DirectoryEndpoint = directory->Service;
    ComponentRecord* echo =
        StartComponent(context, context.Component, context.NextTag++, Abi::ComponentBehaviorEcho, systemCall);
    if ((echo == nullptr) || !BuildBind(context, *echo))
    {
        return PagerServeError::ManagementFailed;
    }
    Executive::ObjectDirectory::BindingVersion version{};
    const PagerServeError bound = Publish(context, *directory, false, version, systemCall);
    if (bound != PagerServeError::None)
    {
        return bound;
    }
    WriteLine(context, DirectoryBoundLine, sizeof(DirectoryBoundLine) - 1U);
    if (context.ClientImage.Bytes != nullptr)
    {
        const PagerServeError ran = RunClient(context, *directory, echo, version, systemCall);
        if (ran != PagerServeError::None)
        {
            return ran;
        }
    }
    const PagerServeError paused = supervisor.Pause(StopPauseMicroseconds, systemCall);
    if (paused != PagerServeError::None)
    {
        return paused;
    }
    const PagerServeError echoStopped = StopService(context, *echo, systemCall);
    if (echoStopped != PagerServeError::None)
    {
        return echoStopped;
    }
    const PagerServeError directoryStopped = StopService(context, *directory, systemCall);
    if (directoryStopped != PagerServeError::None)
    {
        return directoryStopped;
    }
    context.DirectoryEndpoint = {};
    if (supervisor.GetCrashes() != crashes)
    {
        return PagerServeError::ManagementFailed;
    }
    WriteLine(context, ServicesStoppedLine, sizeof(ServicesStoppedLine) - 1U);
    return PagerServeError::None;
}
} // Namespace Asteria::User::Pager
