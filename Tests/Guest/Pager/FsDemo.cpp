// SPDX-License-Identifier: Apache-2.0
#include "Guest/Pager/RootScenarios.hpp"

#include "Abi/ComponentBootstrap.hpp"
#include "User/Component/BlockFrame.hpp"
#include "User/Component/FsFrame.hpp"
#include "User/Pager/PagerWords.hpp"

namespace Asteria::User::Pager
{
namespace
{
// The test disk's token sector (Tools/Vm/AsteriaVm/Machine.py): the probe
// that tells a disk from none reads it.
constexpr Foundation::UInt64 TokenSector{8U};
// Page k of hello holds this word with k in its low byte throughout
// (Tools/Vm/AsteriaVm/Observers/DiskImages.py checks the same pages).
constexpr Foundation::UInt64 HelloPattern{0x4845'4C4C'4F00'0000ULL};
constexpr Foundation::Size PageWords{512U};
constexpr Foundation::UInt8 DataPermissions{0x0BU};
// How long the root service lets the service block again before it kills
// it, and how many times it waits for one reply: a FAT commit is dozens of
// sector transfers.
constexpr Foundation::UInt64 KillPauseMicroseconds{100'000U};
constexpr Foundation::Size ReplyWaits{16U};
constexpr char HelloName[] = "hello";
constexpr char ReadmeName[] = "readme.txt";
constexpr char CrashName[] = "crash.txt";
// README.TXT's first line names the case's format and scenario (the runner
// writes it): "asteria fat16 crash-before" crashes a commit before its
// directory entry.
constexpr char ScenarioPrefix[] = "asteria ";
constexpr char CrashBeforeScenario[] = " crash-before\n";
// The longest format word the line may carry ("fat12", "exfat").
constexpr Foundation::Size MaximumFormatBytes{8U};

/// What the root service holds as a client of the file protocol: its reply Endpoint, of per-sender
/// capacity FsReplyCapacity, a one-page data Frame, and the next transaction.
struct FsClient
{
    System::CapabilityHandle Reply;
    System::CapabilityHandle Data;
    Foundation::UInt64 Transaction;
    Foundation::UInt64 View{0U};
    Foundation::UInt64 RootGeneration{0U};
    Foundation::UInt64 Incarnation{0U};
    bool V2{false};
};

/// One reply: the transaction it answers, its kind, detail and extra; zero
/// kind for none.
struct FsReply
{
    Foundation::UInt64 Answers;
    Foundation::UInt64 Kind;
    Foundation::UInt64 Detail;
    Foundation::UInt64 Extra;
    Foundation::UInt64 NodeKind{0U};
    Foundation::UInt64 Size{0U};
    Foundation::UInt64 Object{0U};
    Foundation::UInt64 Generation{0U};
};

constexpr FsReply NoReply{.Answers = 0U, .Kind = 0U, .Detail = 0U, .Extra = 0U};

struct FsObject
{
    Foundation::UInt64 Identifier;
    Foundation::UInt64 Birth;
};

// The reply Endpoint: the default limits, but FsReplyCapacity messages per
// sender and no attachments.
[[nodiscard, gnu::noinline]] bool CreateReplyEndpoint(ScenarioContext& context, FsClient& client,
                                                      Abi::SystemCallError& systemCall) noexcept
{
    ObjectCalls& calls = *context.Calls;
    calls.ClearRequest();
    Foundation::UInt8(&request)[Abi::ObjectCreateEndpointRequestSizeBytes] =
        *reinterpret_cast<Foundation::UInt8(*)[Abi::ObjectCreateEndpointRequestSizeBytes]>(calls.GetRequest());
    System::BuildDefaultEndpointRequest(request);
    constexpr Foundation::UInt16 bytesPerSender{
        static_cast<Foundation::UInt16>(Component::FsReplyCapacity * Component::EchoMaximumFrameBytes)};
    request[Abi::ObjectCreateEndpointBytesPerSenderOffset] = static_cast<Foundation::UInt8>(bytesPerSender);
    request[Abi::ObjectCreateEndpointBytesPerSenderOffset + 1U] = static_cast<Foundation::UInt8>(bytesPerSender >> 8U);
    request[Abi::ObjectCreateEndpointMessagesPerSenderOffset] =
        static_cast<Foundation::UInt8>(Component::FsReplyCapacity);
    // Replies carry no attachment, and so few messages may not carry the
    // default's.
    request[Abi::ObjectCreateEndpointAttachmentsPerSenderOffset] = 0U;
    return calls.Create(context.Management, Abi::ObjectTypeEndpoint, Abi::ObjectCreateEndpointRequestSizeBytes,
                        client.Reply, systemCall);
}

// Maps the data Frame at the start of the root service's window; the caller
// unmaps it with UnmapData.
[[nodiscard, gnu::noinline]] volatile Foundation::UInt64* MapData(ScenarioContext& context, const FsClient& client,
                                                                  System::CapabilityHandle& memory,
                                                                  Abi::SystemCallError& systemCall) noexcept
{
    if (!context.Calls->CreateMemory(context.Management, Abi::ObjectTypeMemoryObject, 1U, DataPermissions, memory,
                                     systemCall))
    {
        return nullptr;
    }
    const ComponentMapping mapping{
        .Frame = client.Data, .Memory = memory, .VirtualStart = context.WindowStart, .Pages = 1U, .Shared = false};
    if (!context.Calls->Map(context.OwnSpace, mapping, context.WindowStart, DataPermissions, systemCall))
    {
        static_cast<void>(context.Calls->Destroy(memory, Abi::ObjectTypeMemoryObject, systemCall));
        return nullptr;
    }
    return reinterpret_cast<volatile Foundation::UInt64*>(static_cast<Foundation::UIntPointer>(context.WindowStart));
}

[[nodiscard, gnu::noinline]] bool UnmapData(ScenarioContext& context, const System::CapabilityHandle memory,
                                            Abi::SystemCallError& systemCall) noexcept
{
    return Check(context.Client->UnmapMemory(context.OwnSpace, context.WindowStart, 1U), systemCall) &&
           context.Calls->Destroy(memory, Abi::ObjectTypeMemoryObject, systemCall);
}

// Fills the data Frame with page k of hello, or, with check, whether it holds
// it.
[[nodiscard, gnu::noinline]] bool HelloPage(ScenarioContext& context, const FsClient& client,
                                            const Foundation::UInt64 page, const bool check,
                                            Abi::SystemCallError& systemCall) noexcept
{
    System::CapabilityHandle memory{};
    volatile Foundation::UInt64* const words = MapData(context, client, memory, systemCall);
    if (words == nullptr)
    {
        return false;
    }
    bool held{true};
    for (Foundation::Size index{0U}; index < PageWords; ++index)
    {
        if (check)
        {
            held = held && (words[index] == (HelloPattern | page));
        }
        else
        {
            words[index] = HelloPattern | page;
        }
    }
    return UnmapData(context, memory, systemCall) && held;
}

// Whether the data Frame starts with "asteria <format>", and whether its
// first line then asks for the crash before the directory entry.
[[nodiscard, gnu::noinline]] bool ReadScenario(ScenarioContext& context, FsClient& client, bool& crash,
                                               Abi::SystemCallError& systemCall) noexcept
{
    System::CapabilityHandle memory{};
    const volatile Foundation::UInt64* const words = MapData(context, client, memory, systemCall);
    if (words == nullptr)
    {
        return false;
    }
    const volatile char* const text = reinterpret_cast<const volatile char*>(words);
    bool named{true};
    Foundation::Size index{0U};
    for (; ScenarioPrefix[index] != '\0'; ++index)
    {
        named = named && (text[index] == ScenarioPrefix[index]);
    }
    // The format's word, then the scenario and the line's end.
    const Foundation::Size format = index;
    while ((index < (format + MaximumFormatBytes)) && (text[index] != ' ') && (text[index] != '\n'))
    {
        ++index;
    }
    named = named && (index != format);
    client.V2 = text[index + 1U] == 'v' && text[index + 2U] == '2';
    crash = true;
    for (Foundation::Size scenario{0U}; CrashBeforeScenario[scenario] != '\0'; ++scenario)
    {
        crash = crash && (text[index + scenario] == CrashBeforeScenario[scenario]);
    }
    return UnmapData(context, memory, systemCall) && named;
}

// A request to the service through the root service's first capability on
// its Endpoint, its words led by the service's tag; a Read or Write carries
// the data Frame. Its transaction into sent.
[[nodiscard, gnu::noinline]] bool SendFs(ScenarioContext& context, const ComponentRecord& service, FsClient& client,
                                         const Foundation::UInt32 method,
                                         const Foundation::UInt64 (&words)[Component::FsPayloadWords],
                                         const char* const name, Foundation::UInt64& sent,
                                         Abi::SystemCallError& systemCall) noexcept
{
    Foundation::Size length{0U};
    while ((name != nullptr) && (length <= Component::FsMaximumNameBytes) && (name[length] != '\0'))
    {
        ++length;
    }
    Foundation::UInt64 payload[Component::FsPayloadWords]{};
    for (Foundation::Size at{0U}; at < Component::FsPayloadWords; ++at)
    {
        payload[at] = words[at];
    }
    const bool manager = method == Component::FsInstallViewMethod || method == Component::FsRemoveViewMethod ||
                         method == Component::FsResetViewsMethod;
    if (!manager)
    {
        payload[4U] = client.View;
        if ((Component::Detail::HasFsName(method) || method == Component::FsCommitMethod ||
             method == Component::FsQueryCommitMethod) &&
            payload[6U] == 0U)
        {
            payload[6U] = client.RootGeneration;
        }
        if (method == Component::FsReadMethod)
        {
            payload[7U] = 1U;
        }
    }
    sent = ++client.Transaction;
    const bool data = (method == Component::FsReadMethod) || (method == Component::FsWriteMethod) ||
                      (method == Component::FsReadDirectoryMethod);
    const Foundation::Size bytes =
        Component::WriteFsFrame(context.Frame, method, sent, payload, reinterpret_cast<const Foundation::UInt8*>(name),
                                length, &client.Reply, data ? &client.Data : nullptr);
    return (bytes != 0U) &&
           Check(context.Client->Send(manager ? service.FsManager : service.Service, AddressOf(context.Frame), bytes),
                 systemCall);
}

// The reply to sent while the service runs; zero kind if none came.
[[nodiscard, gnu::noinline]] FsReply AwaitFsReply(ScenarioContext& context, const ComponentRecord& service,
                                                  const FsClient& client, const Foundation::UInt64 sent,
                                                  Abi::SystemCallError& systemCall) noexcept
{
    Foundation::UInt64 received{0U};
    for (Foundation::Size wait{0U};
         (wait < ReplyWaits) && (received == 0U) && (service.State == ComponentState::Running); ++wait)
    {
        received = AwaitMessage(context, service, client.Reply, systemCall);
    }
    Component::FsMessage message{};
    if ((received == 0U) ||
        !Component::ReadFsFrame(context.Delivery, static_cast<Foundation::Size>(received), message) ||
        (message.Method != Component::FsReplyMethod) || (message.Words[0U] != sent))
    {
        return NoReply;
    }
    return FsReply{.Answers = message.Words[0U],
                   .Kind = message.Words[1U],
                   .Detail = message.Words[2U],
                   .Extra = message.Words[3U],
                   .NodeKind = message.Words[4U],
                   .Size = message.Words[5U],
                   .Object = message.Words[6U],
                   .Generation = message.Words[7U]};
}

// One request and its reply.
[[nodiscard]] FsReply Request(ScenarioContext& context, const ComponentRecord& service, FsClient& client,
                              const Foundation::UInt32 method,
                              const Foundation::UInt64 (&words)[Component::FsPayloadWords], const char* const name,
                              Abi::SystemCallError& systemCall) noexcept
{
    if (method != Component::FsInstallViewMethod && client.Incarnation != service.Tag)
    {
        Foundation::UInt64 install[Component::FsPayloadWords]{
            service.Tag, 0U, 0U, 0U, 0U, Component::FsReadRight | Component::FsWriteRight | Component::FsExecRight};
        Foundation::UInt64 installed{0U};
        if (!SendFs(context, service, client, Component::FsInstallViewMethod, install, nullptr, installed, systemCall))
        {
            return NoReply;
        }
        auto granted = AwaitFsReply(context, service, client, installed, systemCall);
        if (granted.Kind == Component::FsRightsDenied)
        {
            install[5U] = Component::FsReadRight | Component::FsExecRight;
            if (!SendFs(context, service, client, Component::FsInstallViewMethod, install, nullptr, installed,
                        systemCall))
            {
                return NoReply;
            }
            granted = AwaitFsReply(context, service, client, installed, systemCall);
        }
        if (granted.Kind != Component::FsDone)
        {
            return granted;
        }
        client.View = granted.Detail;
        client.RootGeneration = granted.Extra;
        client.Incarnation = service.Tag;
    }
    Foundation::UInt64 sent{0U};
    return SendFs(context, service, client, method, words, name, sent, systemCall)
               ? AwaitFsReply(context, service, client, sent, systemCall)
               : NoReply;
}

// Reports which reply was not the one expected: the step in bits 56-63, the
// reply's kind in bits 48-55 and its detail's low 48 bits, in place of an
// exit code, as the block demonstration does.
[[nodiscard]] PagerServeError Fail(const ScenarioContext& context, const ComponentRecord& component,
                                   const Foundation::UInt64 step, const FsReply& reply) noexcept
{
    ReportUnexpectedEnd(context, component.Tag,
                        (component.ExitCode != 0U)
                            ? component.ExitCode
                            : ((step << 56U) | ((reply.Kind & 0xFFU) << 48U) | (reply.Detail & 0xFFFF'FFFF'FFFFULL)),
                        false);
    return PagerServeError::ManagementFailed;
}

// Open or Create name: the object into found.
[[nodiscard, gnu::noinline]] bool Name(ScenarioContext& context, const ComponentRecord& service, FsClient& client,
                                       const Foundation::UInt32 method, const char* const name, FsObject& found,
                                       FsReply& reply, Abi::SystemCallError& systemCall) noexcept
{
    const Foundation::UInt64 words[Component::FsPayloadWords]{service.Tag, 0U, 0U, 0U};
    reply = Request(context, service, client, method, words, name, systemCall);
    found = FsObject{.Identifier = reply.Detail, .Birth = reply.Extra};
    return reply.Kind == Component::FsDone;
}

// Writes pages [0, count) of hello from their patterns, the first one page
// first; Read checks them back.
[[nodiscard, gnu::noinline]] bool Transfer(ScenarioContext& context, const ComponentRecord& service, FsClient& client,
                                           const Foundation::UInt32 method, const FsObject& object,
                                           const Foundation::UInt64 index, const Foundation::UInt64 page,
                                           FsReply& reply, Abi::SystemCallError& systemCall) noexcept
{
    if ((method == Component::FsWriteMethod) && !HelloPage(context, client, page, false, systemCall))
    {
        return false;
    }
    const Foundation::UInt64 words[Component::FsPayloadWords]{service.Tag, object.Identifier, object.Birth, index};
    reply = Request(context, service, client, method, words, nullptr, systemCall);
    return (reply.Kind == Component::FsDone) && (reply.Detail == index) &&
           ((method == Component::FsWriteMethod) || HelloPage(context, client, page, true, systemCall));
}

// A Commit of the next transaction with flags; its transaction into sent.
[[nodiscard]] FsReply Commit(ScenarioContext& context, const ComponentRecord& service, FsClient& client,
                             const Foundation::UInt64 flags, Foundation::UInt64& sent,
                             Abi::SystemCallError& systemCall) noexcept
{
    sent = client.Transaction + 1U;
    const Foundation::UInt64 words[Component::FsPayloadWords]{service.Tag, sent, flags, 0U};
    return Request(context, service, client, Component::FsCommitMethod, words, nullptr, systemCall);
}

[[nodiscard]] FsReply Query(ScenarioContext& context, const ComponentRecord& service, FsClient& client,
                            const Foundation::UInt64 transaction, Abi::SystemCallError& systemCall) noexcept
{
    const Foundation::UInt64 words[Component::FsPayloadWords]{service.Tag, transaction, 0U, 0U};
    return Request(context, service, client, Component::FsQueryCommitMethod, words, nullptr, systemCall);
}

// The service, started on the driver with a blank volume formatted.
[[nodiscard, gnu::noinline]] ComponentRecord* StartService(ScenarioContext& context, const ComponentRecord& driver,
                                                           Abi::SystemCallError& systemCall) noexcept
{
    context.DriverService = driver.Service;
    context.DriverGeneration = driver.Tag;
    context.ServiceFlags = (context.Bootstrap->ManagementFeatures & Abi::PagerManagementDiskWritable) != 0U
                               ? Abi::ComponentMountFormatBlank
                               : 0U;
    ComponentRecord* const service =
        StartComponent(context, context.Component, context.NextTag++, Abi::ComponentBehaviorFilesystem, systemCall);
    context.DriverService = {};
    context.DriverGeneration = 0U;
    context.ServiceFlags = 0U;
    return service;
}

// A service or driver ends only by a kill; its ResourceDomain is then drained, and the driver's
// controller reset by the Device Manager.
[[nodiscard, gnu::noinline]] PagerServeError Stop(ScenarioContext& context, ComponentRecord& component,
                                                  Abi::SystemCallError& systemCall) noexcept
{
    ComponentSupervisor& supervisor = *context.Supervisor;
    const PagerServeError paused = supervisor.Pause(KillPauseMicroseconds, systemCall);
    const PagerServeError served = (paused != PagerServeError::None) ? paused : supervisor.Stop(component, systemCall);
    if (served != PagerServeError::None)
    {
        return served;
    }
    if (component.ExitCode != Abi::KilledExitCode)
    {
        ReportUnexpectedEnd(context, component.Tag, component.ExitCode, false);
        return PagerServeError::ManagementFailed;
    }
    return supervisor.Teardown(component, systemCall) ? PagerServeError::None : PagerServeError::ManagementFailed;
}

// A Commit with a crash flag: the service crashes, unanswered, and is torn down, then started again
// on the same driver; service names the new one.
[[nodiscard, gnu::noinline]] PagerServeError CrashCommit(ScenarioContext& context, const ComponentRecord& driver,
                                                         ComponentRecord*& service, FsClient& client,
                                                         const Foundation::UInt64 flags, Foundation::UInt64& sent,
                                                         Abi::SystemCallError& systemCall) noexcept
{
    ComponentSupervisor& supervisor = *context.Supervisor;
    const Foundation::UInt64 crashes = supervisor.GetCrashes();
    sent = client.Transaction + 1U;
    const Foundation::UInt64 words[Component::FsPayloadWords]{service->Tag, sent, flags, 0U};
    Foundation::UInt64 request{0U};
    if (!SendFs(context, *service, client, Component::FsCommitMethod, words, nullptr, request, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    supervisor.SetWatchdog(*service, true);
    const PagerServeError ended = supervisor.Supervise(*service, systemCall);
    if (ended != PagerServeError::None)
    {
        return ended;
    }
    if ((service->ExitCode != Abi::FaultTerminatedExitCode) || (supervisor.GetCrashes() != (crashes + 1U)))
    {
        ReportUnexpectedEnd(context, service->Tag, service->ExitCode, supervisor.GetCrashes() != crashes);
        return PagerServeError::ManagementFailed;
    }
    // The crashed instance never answered: nothing is left for the client.
    const Abi::SystemCallResponse left =
        context.Client->Receive(client.Reply, AddressOf(context.Delivery), sizeof(context.Delivery));
    if ((left.Error != Abi::SystemCallError::WouldBlock) || !supervisor.Teardown(*service, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    service = StartService(context, driver, systemCall);
    return (service != nullptr) ? PagerServeError::None : PagerServeError::ManagementFailed;
}

// Hello: created, two pages written, committed, read back and stated.
[[nodiscard, gnu::noinline]] PagerServeError CommitHello(ScenarioContext& context, const ComponentRecord& service,
                                                         FsClient& client, FsObject& hello,
                                                         Foundation::UInt64& committed,
                                                         Abi::SystemCallError& systemCall) noexcept
{
    FsReply reply = NoReply;
    if (!Name(context, service, client, Component::FsCreateMethod, HelloName, hello, reply, systemCall))
    {
        return Fail(context, service, 3U, reply);
    }
    for (Foundation::UInt64 page{0U}; page < 2U; ++page)
    {
        if (!Transfer(context, service, client, Component::FsWriteMethod, hello, page, page, reply, systemCall))
        {
            return Fail(context, service, 4U, reply);
        }
    }
    reply = Commit(context, service, client, 0U, committed, systemCall);
    if ((reply.Kind != Component::FsDone) && (reply.Kind != Component::FsCommitted))
    {
        return Fail(context, service, 5U, reply);
    }
    for (Foundation::UInt64 page{0U}; page < 2U; ++page)
    {
        if (!Transfer(context, service, client, Component::FsReadMethod, hello, page, page, reply, systemCall))
        {
            return Fail(context, service, 6U, reply);
        }
    }
    const Foundation::UInt64 words[Component::FsPayloadWords]{service.Tag, hello.Identifier, hello.Birth, 0U};
    reply = Request(context, service, client, Component::FsStatMethod, words, nullptr, systemCall);
    return ((reply.Kind == Component::FsDone) && (reply.Detail == 2U) && (reply.Extra == (2U * PageWords * 8U)))
               ? PagerServeError::None
               : Fail(context, service, 7U, reply);
}

[[nodiscard, gnu::noinline]] bool NameAt(ScenarioContext& context, const ComponentRecord& service, FsClient& client,
                                         const Foundation::UInt32 method, const FsObject& parent, const char* name,
                                         FsObject& found, Abi::SystemCallError& systemCall) noexcept
{
    const Foundation::UInt64 words[Component::FsPayloadWords]{service.Tag,       0U,          0U, 0U, 0U,
                                                              parent.Identifier, parent.Birth};
    const auto reply = Request(context, service, client, method, words, name, systemCall);
    found = {reply.Detail, reply.Extra};
    return reply.Kind == Component::FsDone;
}
[[nodiscard, gnu::noinline]] PagerServeError ViewsAndTrees(ScenarioContext& context, const ComponentRecord& service,
                                                           FsClient& client, Abi::SystemCallError& systemCall) noexcept
{
    static FsObject apps{};
    static FsObject nested{};
    static FsObject file{};
    static FsObject parent{};
    static FsReply reply{};
    apps = {};
    nested = {};
    file = {};
    parent = {};
    reply = NoReply;
    Foundation::UInt64 committed{0U};
    bool passed = NameAt(context, service, client, Component::FsMakeDirectoryMethod, {0U, client.RootGeneration},
                         "apps", apps, systemCall);
    reply = Commit(context, service, client, 0U, committed, systemCall);
    passed = passed && reply.Kind == Component::FsDone;
    passed = passed &&
             NameAt(context, service, client, Component::FsMakeDirectoryMethod, apps, "日本-😀", nested, systemCall);
    reply = Commit(context, service, client, 0U, committed, systemCall);
    passed = passed && reply.Kind == Component::FsDone;
    passed = passed &&
             NameAt(context, service, client, Component::FsCreateMethod, nested, "résumé-😀.txt", file, systemCall);
    for (Foundation::UInt64 page{0U}; passed && page < 8U; ++page)
    {
        passed =
            Transfer(context, service, client, Component::FsWriteMethod, file, page, page + 10U, reply, systemCall);
    }
    reply = Commit(context, service, client, 0U, committed, systemCall);
    passed = passed && reply.Kind == Component::FsDone;
    for (Foundation::UInt64 page{0U}; passed && page < 8U; ++page)
    {
        passed = Transfer(context, service, client, Component::FsReadMethod, file, page, page + 10U, reply, systemCall);
    }
    static Foundation::UInt64 stat[Component::FsPayloadWords]{};
    stat[0U] = service.Tag;
    stat[1U] = file.Identifier;
    stat[2U] = file.Birth;
    reply = Request(context, service, client, Component::FsStatMethod, stat, nullptr, systemCall);
    passed = passed && reply.Kind == Component::FsDone && reply.NodeKind == 1U && reply.Size == 8U * 4096U &&
             reply.Object != 0U && reply.Generation == service.Tag;
    static Foundation::UInt64 install[Component::FsPayloadWords]{};
    install[0U] = service.Tag;
    install[1U] = 0U;
    install[2U] = apps.Identifier;
    install[3U] = apps.Birth;
    install[4U] = client.View;
    install[5U] = Component::FsReadRight;
    reply = Request(context, service, client, Component::FsInstallViewMethod, install, nullptr, systemCall);
    passed = passed && reply.Kind == Component::FsDone;
    const Foundation::UInt64 original = client.View;
    const Foundation::UInt64 originalRoot = client.RootGeneration;
    client.View = reply.Detail;
    client.RootGeneration = reply.Extra;
    passed = passed &&
             NameAt(context, service, client, Component::FsLookupMethod, {0U, client.RootGeneration}, "..", parent,
                    systemCall) &&
             parent.Identifier == 0U && parent.Birth == client.RootGeneration;
    static Foundation::UInt64 outside[Component::FsPayloadWords]{};
    outside[0U] = service.Tag;
    reply = Request(context, service, client, Component::FsLookupMethod, outside, "readme.txt", systemCall);
    passed = passed && reply.Kind == Component::FsNotFound;
    passed = passed &&
             NameAt(context, service, client, Component::FsLookupMethod, {0U, client.RootGeneration}, "日本-😀", nested,
                    systemCall) &&
             NameAt(context, service, client, Component::FsLookupMethod, nested, "résumé-😀.txt", file, systemCall);
    static Foundation::UInt64 write[Component::FsPayloadWords]{};
    write[0U] = service.Tag;
    write[1U] = file.Identifier;
    write[2U] = file.Birth;
    reply = Request(context, service, client, Component::FsWriteMethod, write, nullptr, systemCall);
    passed = passed && reply.Kind == Component::FsRightsDenied;
    static Foundation::UInt64 directory[Component::FsPayloadWords]{};
    directory[0U] = service.Tag;
    directory[1U] = nested.Identifier;
    directory[2U] = nested.Birth;
    reply = Request(context, service, client, Component::FsReadDirectoryMethod, directory, nullptr, systemCall);
    passed = passed && reply.Kind == Component::FsDone && reply.Extra == sizeof("résumé-😀.txt") - 1U;
    System::CapabilityHandle memory{};
    const auto* bytes =
        reinterpret_cast<const volatile Foundation::UInt8*>(MapData(context, client, memory, systemCall));
    if (bytes != nullptr)
    {
        constexpr char expected[]{"résumé-😀.txt"};
        for (Foundation::Size at{0U}; at < sizeof(expected) - 1U; ++at)
        {
            passed = passed && bytes[at] == static_cast<Foundation::UInt8>(expected[at]);
        }
        passed = UnmapData(context, memory, systemCall) && passed;
    }
    else
    {
        passed = false;
    }
    reply = Request(context, service, client, Component::FsCloseMethod, write, nullptr, systemCall);
    passed = passed && reply.Kind == Component::FsDone;
    reply = Request(context, service, client, Component::FsStatMethod, write, nullptr, systemCall);
    passed = passed && reply.Kind == Component::FsStaleObject;
    static Foundation::UInt64 forged[Component::FsPayloadWords]{};
    forged[0U] = service.Tag;
    forged[1U] = Component::FsOpenSlots;
    forged[2U] = client.RootGeneration;
    reply = Request(context, service, client, Component::FsStatMethod, forged, nullptr, systemCall);
    passed = passed && reply.Kind == Component::FsStaleObject;
    static Foundation::UInt64 remove[Component::FsPayloadWords]{};
    remove[0U] = service.Tag;
    remove[1U] = 0U;
    remove[2U] = 0U;
    remove[3U] = 0U;
    remove[4U] = client.View;
    reply = Request(context, service, client, Component::FsRemoveViewMethod, remove, nullptr, systemCall);
    passed = passed && reply.Kind == Component::FsDone;
    reply = Request(context, service, client, Component::FsStatMethod, forged, nullptr, systemCall);
    passed = passed && reply.Kind == Component::FsRightsDenied;
    client.View = original;
    client.RootGeneration = originalRoot;
    if (!passed)
    {
        return Fail(context, service, 20U, reply);
    }
    constexpr char line[]{"# fs v2: views, confinement, slots, UTF-8 tree and eight pages passed\n"};
    WriteLine(context, line, sizeof(line) - 1U);
    return PagerServeError::None;
}

// FAT: readme.txt read, hello committed and read back; where the scenario asks, a commit of
// crash.txt crashes before its directory entry, and the restarted service, whose volume is dirty
// and so mounted read-only, does not find it.
[[nodiscard, gnu::noinline]] PagerServeError ServeFat(ScenarioContext& context, const ComponentRecord& driver,
                                                      ComponentRecord*& service, FsClient& client, bool& crashed,
                                                      Abi::SystemCallError& systemCall) noexcept
{
    FsObject readme{};
    FsReply reply = NoReply;
    if (!Name(context, *service, client, Component::FsOpenMethod, ReadmeName, readme, reply, systemCall))
    {
        return Fail(context, *service, 2U, reply);
    }
    const Foundation::UInt64 words[Component::FsPayloadWords]{service->Tag, readme.Identifier, readme.Birth, 0U};
    reply = Request(context, *service, client, Component::FsReadMethod, words, nullptr, systemCall);
    if ((reply.Kind != Component::FsDone) || !ReadScenario(context, client, crashed, systemCall))
    {
        return Fail(context, *service, 8U, reply);
    }
    FsObject hello{};
    Foundation::UInt64 committed{0U};
    const PagerServeError served = CommitHello(context, *service, client, hello, committed, systemCall);
    if ((served != PagerServeError::None) || !crashed)
    {
        return served == PagerServeError::None && client.V2 ? ViewsAndTrees(context, *service, client, systemCall)
                                                            : served;
    }
    FsObject crash{};
    if (!Name(context, *service, client, Component::FsCreateMethod, CrashName, crash, reply, systemCall) ||
        !Transfer(context, *service, client, Component::FsWriteMethod, crash, 0U, 0U, reply, systemCall))
    {
        return Fail(context, *service, 9U, reply);
    }
    Foundation::UInt64 sent{0U};
    const PagerServeError restarted =
        CrashCommit(context, driver, service, client, Component::FsCrashBeforeSuperblock, sent, systemCall);
    if (restarted != PagerServeError::None)
    {
        return restarted;
    }
    static_cast<void>(Name(context, *service, client, Component::FsOpenMethod, CrashName, crash, reply, systemCall));
    return (reply.Kind == Component::FsNotFound) ? PagerServeError::None : Fail(context, *service, 10U, reply);
}

// AsteriaFS: the blank volume formatted at mount, hello committed and read back; then page 0
// rewritten and committed with the crash before the superblock (NotCommitted, the old page reads
// back) and with the crash after it (Committed, the new page reads back).
[[nodiscard, gnu::noinline]] PagerServeError ServeAsteriaFs(ScenarioContext& context, const ComponentRecord& driver,
                                                            ComponentRecord*& service, FsClient& client,
                                                            Abi::SystemCallError& systemCall) noexcept
{
    FsObject hello{};
    Foundation::UInt64 committed{0U};
    PagerServeError served = CommitHello(context, *service, client, hello, committed, systemCall);
    FsReply reply =
        (served == PagerServeError::None) ? Query(context, *service, client, committed, systemCall) : NoReply;
    if ((served != PagerServeError::None) || (reply.Kind != Component::FsCommitted))
    {
        return (served != PagerServeError::None) ? served : Fail(context, *service, 11U, reply);
    }
    // The rewritten page 0 is pattern 2, then 3; page 1 stays pattern 1.
    constexpr Foundation::UInt64 Flags[2U]{Component::FsCrashBeforeSuperblock, Component::FsCrashAfterSuperblock};
    for (Foundation::Size round{0U}; round < 2U; ++round)
    {
        Foundation::UInt64 sent{0U};
        if (!Transfer(context, *service, client, Component::FsWriteMethod, hello, 0U, 2U + round, reply, systemCall))
        {
            return Fail(context, *service, 12U + round, reply);
        }
        served = CrashCommit(context, driver, service, client, Flags[round], sent, systemCall);
        if (served != PagerServeError::None)
        {
            return served;
        }
        reply = Query(context, *service, client, sent, systemCall);
        const Foundation::UInt64 expected = (round == 0U) ? Component::FsNotCommitted : Component::FsCommitted;
        const Foundation::UInt64 page = (round == 0U) ? 0U : 3U;
        if ((reply.Kind != expected)
            // a restarted incarnation has fresh open slots.
            || !Name(context, *service, client, Component::FsOpenMethod, HelloName, hello, reply, systemCall) ||
            !Transfer(context, *service, client, Component::FsReadMethod, hello, 0U, page, reply, systemCall))
        {
            return Fail(context, *service, 14U + round, reply);
        }
    }
    return PagerServeError::None;
}

// Report the capacity obtained over the protocol, in hexadecimal sectors.
[[gnu::noinline]] void ReportDescribe(const ScenarioContext& context, const Foundation::UInt64 sectors) noexcept
{
    constexpr char Prefix[]{"# fs: Describe 512-byte blocks, sectors 0x"};
    constexpr char Digits[]{"0123456789ABCDEF"};
    char line[64U]{};
    Foundation::Size length{0U};
    for (const char character : Prefix)
    {
        if (character != '\0')
        {
            line[length++] = character;
        }
    }
    for (Foundation::UInt64 shift{64U}; shift != 0U; shift -= 4U)
    {
        line[length++] = Digits[(sectors >> (shift - 4U)) & 0xFU];
    }
    line[length++] = '\n';
    WriteLine(context, line, length);
}

[[nodiscard, gnu::noinline]] bool DescribeDisk(ScenarioContext& context, const ComponentRecord& driver,
                                               FsClient& client, Foundation::UInt64& mediumGeneration,
                                               Abi::SystemCallError& systemCall) noexcept
{
    const Foundation::UInt64 words[Component::BlockPayloadWords]{driver.Tag, 0U, 0U, 0U};
    const Foundation::UInt64 sent = ++client.Transaction;
    const Foundation::Size bytes =
        Component::WriteBlockFrame(context.Frame, Component::BlockDescribeMethod, sent, words, &client.Reply, nullptr);
    if (!Check(context.Client->Send(driver.Service, AddressOf(context.Frame), bytes), systemCall))
    {
        return false;
    }
    const Foundation::UInt64 received = AwaitMessage(context, driver, client.Reply, systemCall);
    Component::BlockMessage message{};
    if (!Component::ReadBlockFrame(context.Delivery, static_cast<Foundation::Size>(received), message) ||
        (message.Method != Component::BlockDescribeReplyMethod) || (message.Transaction != sent) ||
        (message.Words[0U] != Component::BlockSectorBytes) ||
        ((message.Words[2U] & (Component::BlockReadOnly | Component::BlockRemovable)) != 0U))
    {
        return false;
    }
    mediumGeneration = message.Words[3U];
    ReportDescribe(context, message.Words[1U]);
    return true;
}

// Inventory persistence uses the mounted file service. A real disk is
// authorized only by a pre-existing consent file on a FAT/exFAT root.
// The protocol transfers whole pages; unused tail bytes are ASCII spaces.
[[nodiscard, gnu::noinline]] bool InventoryPage(ScenarioContext& context, const FsClient& client,
                                                const Foundation::UInt64 page,
                                                Abi::SystemCallError& systemCall) noexcept
{
    System::CapabilityHandle memory{};
    volatile Foundation::UInt64* const words = MapData(context, client, memory, systemCall);
    if (words == nullptr)
    {
        return false;
    }
    volatile char* const data = reinterpret_cast<volatile char*>(words);
    for (Foundation::Size index{0U}; index < 4096U; ++index)
    {
        const Foundation::UInt64 offset = page * 4096U + index;
        data[index] = offset < context.DeviceTextBytes ? context.DeviceText[offset] : ' ';
    }
    data[4095U] = page * 4096U + 4095U < context.DeviceTextBytes ? data[4095U] : '\n';
    return UnmapData(context, memory, systemCall);
}

void InventorySaveLine(const ScenarioContext& context, const char* const reason) noexcept
{
    Foundation::Size bytes{0U};
    while (reason[bytes] != '\0')
    {
        ++bytes;
    }
    WriteLine(context, reason, bytes);
}

[[gnu::noinline]] void SaveInventory(ScenarioContext& context, const ComponentRecord& service, FsClient& client,
                                     const Foundation::UInt64 format, Abi::SystemCallError& systemCall) noexcept
{
    if (context.DeviceText == nullptr || context.DeviceTextBytes == 0U)
    {
        InventorySaveLine(context, "# devices: not saved, inventory unavailable\n");
        return;
    }
    const bool emulator = (context.Bootstrap->ManagementFeatures & Abi::PagerManagementDiskWritable) != 0U;
    FsReply reply = NoReply;
    FsObject object{};
    if (!emulator)
    {
        const bool supported = format == Component::FsFormatFat || format == Component::FsFormatExfat;
        if (!supported ||
            !Name(context, service, client, Component::FsOpenMethod, "asteria.ok", object, reply, systemCall))
        {
            InventorySaveLine(context, "# devices: not saved, ASTERIA.OK consent file absent on FAT/exFAT\n");
            return;
        }
    }
    Foundation::UInt64 pages = (context.DeviceTextBytes + 4095U) / 4096U;
    if (Name(context, service, client, Component::FsOpenMethod, "devices.txt", object, reply, systemCall))
    {
        const Foundation::UInt64 stat[Component::FsPayloadWords]{service.Tag, object.Identifier, object.Birth, 0U};
        reply = Request(context, service, client, Component::FsStatMethod, stat, nullptr, systemCall);
        if (reply.Kind != Component::FsDone || reply.Detail > Component::FsMaximumFileBlocks)
        {
            InventorySaveLine(context, "# devices: not saved, existing file unavailable\n");
            return;
        }
        // Clear every old page as well, so a shorter inventory leaves no
        // stale device lines. The file protocol currently has no truncate.
        pages = pages > reply.Detail ? pages : reply.Detail;
    }
    else if (reply.Kind != Component::FsNotFound ||
             !Name(context, service, client, Component::FsCreateMethod, "devices.txt", object, reply, systemCall))
    {
        InventorySaveLine(context, "# devices: not saved, volume read-only or file creation failed\n");
        return;
    }
    bool written{pages <= Component::FsMaximumFileBlocks};
    for (Foundation::UInt64 page{0U}; written && page < pages; ++page)
    {
        written = InventoryPage(context, client, page, systemCall);
        const Foundation::UInt64 words[Component::FsPayloadWords]{service.Tag, object.Identifier, object.Birth, page};
        if (written)
        {
            reply = Request(context, service, client, Component::FsWriteMethod, words, nullptr, systemCall);
            written = reply.Kind == Component::FsDone;
        }
    }
    Foundation::UInt64 sent{0U};
    if (written)
    {
        reply = Commit(context, service, client, 0U, sent, systemCall);
        written = reply.Kind == Component::FsDone || reply.Kind == Component::FsCommitted;
    }
    InventorySaveLine(context, written ? "# devices: saved DEVICES.TXT\n"
                                       : "# devices: not saved, filesystem write/commit failed\n");
}

// Whether the driver serves a disk: a read of the token sector into the data
// Frame is Done, or Failed with BlockNoDevice.
[[nodiscard, gnu::noinline]] bool ProbeDisk(ScenarioContext& context, const ComponentRecord& driver, FsClient& client,
                                            bool& served, Abi::SystemCallError& systemCall) noexcept
{
    Foundation::UInt64 mediumGeneration{0U};
    if (!DescribeDisk(context, driver, client, mediumGeneration, systemCall))
    {
        return false;
    }
    const Foundation::UInt64 words[Component::BlockIoPayloadWords]{driver.Tag, TokenSector, 1U, 0U, mediumGeneration};
    const Foundation::UInt64 sent = ++client.Transaction;
    const Foundation::Size bytes =
        Component::WriteBlockFrame(context.Frame, Component::BlockReadMethod, sent, words, &client.Reply, &client.Data);
    if ((bytes == 0U) || !Check(context.Client->Send(driver.Service, AddressOf(context.Frame), bytes), systemCall))
    {
        return false;
    }
    const Foundation::UInt64 received = AwaitMessage(context, driver, client.Reply, systemCall);
    Component::BlockMessage message{};
    if ((received == 0U) ||
        !Component::ReadBlockFrame(context.Delivery, static_cast<Foundation::Size>(received), message) ||
        (message.Words[0U] != sent))
    {
        return false;
    }
    served = message.Words[1U] == Component::BlockDone;
    return served || ((message.Words[1U] == Component::BlockFailed) && (message.Words[2U] == Component::BlockNoDevice));
}

// The demonstration's line, by the volume's format (the service names it)
// and what happened.
void WriteFsLine(const ScenarioContext& context, const Foundation::UInt64 format, const bool crashed) noexcept
{
    if (format == Component::FsFormatExfat)
    {
        WriteLine(context, FsExfatLine, sizeof(FsExfatLine) - 1U);
        return;
    }
    if (format != Component::FsFormatFat)
    {
        WriteLine(context, FsAsteriaLine, sizeof(FsAsteriaLine) - 1U);
        return;
    }
    if (crashed)
    {
        WriteLine(context, FsFatCrashedLine, sizeof(FsFatCrashedLine) - 1U);
        return;
    }
    WriteLine(context, FsFatLine, sizeof(FsFatLine) - 1U);
}

[[nodiscard, gnu::noinline]] PagerServeError RunClient(ScenarioContext& context, FsClient& client,
                                                       Abi::SystemCallError& systemCall) noexcept
{
    if (!CreateReplyEndpoint(context, client, systemCall) ||
        !context.Calls->CreateMemory(context.Management, Abi::ObjectTypeFrame, 1U, 0U, client.Data, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    ComponentRecord* const driver =
        StartComponent(context, context.Component, context.NextTag++, Abi::ComponentBehaviorBlockDriver, systemCall);
    bool served{false};
    if ((driver == nullptr) || !ProbeDisk(context, *driver, client, served, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    if (!served)
    {
        const PagerServeError stopped = Stop(context, *driver, systemCall);
        if (stopped == PagerServeError::None)
        {
            WriteLine(context, FsNoDiskLine, sizeof(FsNoDiskLine) - 1U);
            InventorySaveLine(context, "# devices: not saved, no disk available\n");
        }
        return stopped;
    }
    ComponentRecord* service = StartService(context, *driver, systemCall);
    if (service == nullptr)
    {
        return PagerServeError::ManagementFailed;
    }
    // A FAT or exFAT volume keeps no transaction record and the answer names its format; AsteriaFS
    // has none for this transaction yet.
    const FsReply format = Query(context, *service, client, 1U, systemCall);
    const bool fat = format.Kind == Component::FsUnsupported;
    bool crashed{false};
    PagerServeError result = fat ? ServeFat(context, *driver, service, client, crashed, systemCall)
                             : (format.Kind == Component::FsNotCommitted)
                                 ? ServeAsteriaFs(context, *driver, service, client, systemCall)
                                 : Fail(context, *service, 1U, format);
    if (result != PagerServeError::None)
    {
        return result;
    }
    SaveInventory(context, *service, client, fat ? format.Detail : 0U, systemCall);
    result = Stop(context, *service, systemCall);
    result = (result == PagerServeError::None) ? Stop(context, *driver, systemCall) : result;
    if (result == PagerServeError::None)
    {
        WriteFsLine(context, fat ? format.Detail : 0U, crashed);
    }
    return result;
}
} // Namespace

PagerServeError RunDeviceInventorySave(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept
{
    FsClient client{};
    PagerServeError result{PagerServeError::None};
    const bool ready =
        CreateReplyEndpoint(context, client, systemCall) &&
        context.Calls->CreateMemory(context.Management, Abi::ObjectTypeFrame, 1U, 0U, client.Data, systemCall);
    if (ready)
    {
        ComponentRecord* const driver = StartComponent(context, context.Component, context.NextTag++,
                                                       Abi::ComponentBehaviorBlockDriver, systemCall);
        ComponentRecord* service = driver != nullptr ? StartService(context, *driver, systemCall) : nullptr;
        if (service != nullptr)
        {
            const FsReply format = Query(context, *service, client, 1U, systemCall);
            SaveInventory(context, *service, client, format.Kind == Component::FsUnsupported ? format.Detail : 0U,
                          systemCall);
            result = Stop(context, *service, systemCall);
        }
        else
        {
            InventorySaveLine(context, "# devices: not saved, writable volume unavailable\n");
        }
        if (driver != nullptr && result == PagerServeError::None)
        {
            result = Stop(context, *driver, systemCall);
        }
    }
    else
    {
        InventorySaveLine(context, "# devices: not saved, filesystem client unavailable\n");
    }
    Abi::SystemCallError released{Abi::SystemCallError::None};
    bool clean = client.Data.Selector == 0U || context.Calls->Destroy(client.Data, Abi::ObjectTypeFrame, released);
    clean = (client.Reply.Selector == 0U || context.Calls->Destroy(client.Reply, Abi::ObjectTypeEndpoint, released)) &&
            clean;
    return result != PagerServeError::None ? result
                                           : (clean ? PagerServeError::None : PagerServeError::ManagementFailed);
}

// The filesystem service's demonstration: the root service starts an AHCI driver and, when it
// serves a disk, the filesystem service on it, and is its client. On a FAT volume it reads
// readme.txt, whose first line names the case's scenario, and commits hello; on the blank volume,
// formatted as AsteriaFS, it commits hello and crashes the service on either side of a commit's
// superblock write. Without a disk the driver is killed while it waits.
PagerServeError RunFsDemo(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept
{
    FsClient client{.Reply = {}, .Data = {}, .Transaction = 0U};
    const PagerServeError result = RunClient(context, client, systemCall);
    Abi::SystemCallError released{Abi::SystemCallError::None};
    bool clean = (client.Data.Selector == 0U) || context.Calls->Destroy(client.Data, Abi::ObjectTypeFrame, released);
    clean =
        ((client.Reply.Selector == 0U) || context.Calls->Destroy(client.Reply, Abi::ObjectTypeEndpoint, released)) &&
        clean;
    systemCall = (systemCall == Abi::SystemCallError::None) ? released : systemCall;
    return (result != PagerServeError::None) ? result
                                             : (clean ? PagerServeError::None : PagerServeError::ManagementFailed);
}
} // Namespace Asteria::User::Pager
