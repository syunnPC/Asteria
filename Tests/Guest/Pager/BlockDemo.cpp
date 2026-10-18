// SPDX-License-Identifier: Apache-2.0
#include "Guest/Pager/RootScenarios.hpp"

#include "Abi/ComponentBootstrap.hpp"
#include "User/Component/BlockFrame.hpp"
#include "User/Pager/PagerWords.hpp"

namespace Asteria::User::Pager
{
namespace
{
// The test disk's token sector, its token (Tools/Vm/AsteriaVm/Machine.py,
// sixteen times over the sector), and a sector the demonstration may write.
constexpr Foundation::UInt64 TokenSector{8U};
constexpr char Token[] = "ASTERIA-READ-DMA-EXT-SECTOR-0001";
constexpr Foundation::Size TokenBytes{sizeof(Token) - 1U};
constexpr Foundation::UInt64 ScratchSector{64U};
// The words written to the scratch sector, the sector's word index added.
constexpr Foundation::UInt64 ScratchPattern{0x424C'4F43'4B57'0000ULL};
constexpr Foundation::Size SectorWords{Component::BlockSectorBytes >> 3U};
// How long the root service lets the driver block again before it kills it:
// longer than a user time slice.
constexpr Foundation::UInt64 KillPauseMicroseconds{100'000U};
// How long a read the throttle holds stays unanswered at least, and how many
// probes the root service makes before it gives up waiting for the runner,
// which polls the debug channel, to throttle the disk.
constexpr Foundation::UInt64 ThrottleProbeMicroseconds{300'000U};
constexpr Foundation::UInt64 ThrottleProbes{16U};
// The demonstration's transactions: a read of the token, a read cancelled
// While queued and its Cancel, a Cancel too late for the first read, and the scratch sector written
// and read back.
constexpr Foundation::UInt64 ReadTokenTransaction{1U};
constexpr Foundation::UInt64 QueuedTransaction{2U};
constexpr Foundation::UInt64 CancelQueuedTransaction{3U};
constexpr Foundation::UInt64 CancelLateTransaction{4U};
constexpr Foundation::UInt64 WriteTransaction{5U};
constexpr Foundation::UInt64 ReadBackTransaction{6U};
constexpr Foundation::UInt64 CrashTransaction{7U};
// The throttle's probes take transactions from here, two a probe.
constexpr Foundation::UInt64 ProbeTransactions{16U};
constexpr Foundation::UInt8 DataPermissions{0x0BU};
constexpr Foundation::UInt64 PageBytes{4096U};

/// What the demonstration holds: its reply Endpoint, created with a per-sender capacity of 2K + 1,
/// two data Frames of a page, and the Frame the crash's read goes into, which it destroys.
constexpr Foundation::Size BlockClientFrames{3U};

struct BlockClient
{
    System::CapabilityHandle Reply;
    System::CapabilityHandle Data[BlockClientFrames];
};

/// One reply: its header's transaction, the transaction it answers, its
/// kind and detail; zero kind for none.
struct BlockReply
{
    Foundation::UInt64 Header;
    Foundation::UInt64 Answers;
    Foundation::UInt64 Kind;
    Foundation::UInt64 Detail;
};

// The reply Endpoint: the default limits, but five messages per sender.
[[nodiscard, gnu::noinline]] bool CreateReplyEndpoint(ScenarioContext& context, BlockClient& client,
                                                      Abi::SystemCallError& systemCall) noexcept
{
    ObjectCalls& calls = *context.Calls;
    calls.ClearRequest();
    Foundation::UInt8(&request)[Abi::ObjectCreateEndpointRequestSizeBytes] =
        *reinterpret_cast<Foundation::UInt8(*)[Abi::ObjectCreateEndpointRequestSizeBytes]>(calls.GetRequest());
    System::BuildDefaultEndpointRequest(request);
    constexpr Foundation::UInt16 bytesPerSender{
        static_cast<Foundation::UInt16>(Component::BlockReplyCapacity * Component::EchoMaximumFrameBytes)};
    request[Abi::ObjectCreateEndpointBytesPerSenderOffset] = static_cast<Foundation::UInt8>(bytesPerSender);
    request[Abi::ObjectCreateEndpointBytesPerSenderOffset + 1U] = static_cast<Foundation::UInt8>(bytesPerSender >> 8U);
    request[Abi::ObjectCreateEndpointMessagesPerSenderOffset] =
        static_cast<Foundation::UInt8>(Component::BlockReplyCapacity);
    return calls.Create(context.Management, Abi::ObjectTypeEndpoint, Abi::ObjectCreateEndpointRequestSizeBytes,
                        client.Reply, systemCall);
}

// A Read, Write or Cancel to the driver through the root service's first
// capability on its Endpoint, sealed with BlockSealedSelector: words are the
// payload, data the Frame a Read or Write names.
template <Foundation::Size Words>
[[nodiscard, gnu::noinline]] bool
SendBlock(ScenarioContext& context, const ComponentRecord& driver, const BlockClient& client,
          const Foundation::UInt32 method, const Foundation::UInt64 transaction,
          const Foundation::UInt64 (&words)[Words], const System::CapabilityHandle* const data,
          Abi::SystemCallError& systemCall) noexcept
{
    const Foundation::Size bytes =
        Component::WriteBlockFrame(context.Frame, method, transaction, words, &client.Reply, data);
    return (bytes != 0U) && Check(context.Client->Send(driver.Service, AddressOf(context.Frame), bytes), systemCall);
}

// A Read or Write of one sector at lba into or from data, for the live
// generation.
[[nodiscard]] bool SendTransfer(ScenarioContext& context, const ComponentRecord& driver, const BlockClient& client,
                                const Foundation::UInt32 method, const Foundation::UInt64 transaction,
                                const Foundation::UInt64 lba, const System::CapabilityHandle& data,
                                Abi::SystemCallError& systemCall) noexcept
{
    const Foundation::UInt64 words[Component::BlockIoPayloadWords]{driver.Tag, lba, 1U, 0U, driver.Tag};
    return SendBlock(context, driver, client, method, transaction, words, &data, systemCall);
}

[[nodiscard]] bool SendCancel(ScenarioContext& context, const ComponentRecord& driver, const BlockClient& client,
                              const Foundation::UInt64 transaction, const Foundation::UInt64 cancelled,
                              Abi::SystemCallError& systemCall) noexcept
{
    const Foundation::UInt64 words[Component::BlockPayloadWords]{driver.Tag, cancelled, 0U, 0U};
    return SendBlock(context, driver, client, Component::BlockCancelMethod, transaction, words, nullptr, systemCall);
}

// The next reply on the client's Endpoint while the driver runs.
[[nodiscard, gnu::noinline]] BlockReply AwaitBlockReply(ScenarioContext& context, const ComponentRecord& driver,
                                                        const BlockClient& client,
                                                        Abi::SystemCallError& systemCall) noexcept
{
    const Foundation::UInt64 received = AwaitMessage(context, driver, client.Reply, systemCall);
    Component::BlockMessage message{};
    if (!Component::ReadBlockFrame(context.Delivery, static_cast<Foundation::Size>(received), message) ||
        (message.Method != Component::BlockReplyMethod))
    {
        return BlockReply{.Header = 0U, .Answers = 0U, .Kind = 0U, .Detail = 0U};
    }
    return BlockReply{.Header = message.Transaction,
                      .Answers = message.Words[0U],
                      .Kind = message.Words[1U],
                      .Detail = message.Words[2U]};
}

[[nodiscard]] constexpr bool Is(const BlockReply& reply, const Foundation::UInt64 header,
                                const Foundation::UInt64 answers, const Foundation::UInt64 kind,
                                const Foundation::UInt64 detail) noexcept
{
    return (reply.Header == header) && (reply.Answers == answers) && (reply.Kind == kind) && (reply.Detail == detail);
}

// Maps data at the start of the root service's window, where the loader
// stages nothing meanwhile: the demonstration reads or writes it, then
// unmaps it and destroys the MemoryObject that mapped it.
[[nodiscard, gnu::noinline]] volatile Foundation::UInt64* MapData(ScenarioContext& context,
                                                                  const System::CapabilityHandle data,
                                                                  System::CapabilityHandle& memory,
                                                                  Abi::SystemCallError& systemCall) noexcept
{
    if (!context.Calls->CreateMemory(context.Management, Abi::ObjectTypeMemoryObject, 1U, DataPermissions, memory,
                                     systemCall))
    {
        return nullptr;
    }
    const ComponentMapping mapping{.Frame = data, .Memory = memory, .VirtualStart = context.WindowStart, .Pages = 1U};
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

// Whether data's first sector holds the token sixteen times; its first word
// into found.
[[nodiscard, gnu::noinline]] bool HoldsToken(ScenarioContext& context, const System::CapabilityHandle data,
                                             Foundation::UInt64& found, Abi::SystemCallError& systemCall) noexcept
{
    System::CapabilityHandle memory{};
    const volatile Foundation::UInt64* const page = MapData(context, data, memory, systemCall);
    if (page == nullptr)
    {
        return false;
    }
    found = page[0U];
    const volatile Foundation::UInt8* const bytes = reinterpret_cast<const volatile Foundation::UInt8*>(page);
    bool held{true};
    for (Foundation::Size index{0U}; index < Component::BlockSectorBytes; ++index)
    {
        held = held && (bytes[index] == static_cast<Foundation::UInt8>(Token[index % TokenBytes]));
    }
    return UnmapData(context, memory, systemCall) && held;
}

// Writes the scratch pattern into data's first sector, or, with check,
// whether it holds it; its first word into found.
[[nodiscard, gnu::noinline]] bool Pattern(ScenarioContext& context, const System::CapabilityHandle data,
                                          const bool check, Foundation::UInt64& found,
                                          Abi::SystemCallError& systemCall) noexcept
{
    System::CapabilityHandle memory{};
    volatile Foundation::UInt64* const page = MapData(context, data, memory, systemCall);
    if (page == nullptr)
    {
        return false;
    }
    found = page[0U];
    bool held{true};
    for (Foundation::Size index{0U}; index < SectorWords; ++index)
    {
        if (check)
        {
            held = held && (page[index] == (ScratchPattern + index));
        }
        else
        {
            page[index] = ScratchPattern + index;
        }
    }
    return UnmapData(context, memory, systemCall) && held;
}

// A service ends only by a kill; its ResourceDomain is then drained, the device pre-pass first, and
// the Device Manager resets the controller's ports before it confirms the device quiet.
[[nodiscard, gnu::noinline]] PagerServeError Stop(ScenarioContext& context, ComponentRecord& driver,
                                                  Abi::SystemCallError& systemCall) noexcept
{
    ComponentSupervisor& supervisor = *context.Supervisor;
    const PagerServeError paused = supervisor.Pause(KillPauseMicroseconds, systemCall);
    const PagerServeError served = (paused != PagerServeError::None) ? paused : supervisor.Stop(driver, systemCall);
    if (served != PagerServeError::None)
    {
        return served;
    }
    if (driver.ExitCode != Abi::KilledExitCode)
    {
        ReportUnexpectedEnd(context, driver.Tag, driver.ExitCode, false);
        return PagerServeError::ManagementFailed;
    }
    return supervisor.Teardown(driver, systemCall) ? PagerServeError::None : PagerServeError::ManagementFailed;
}

// Reports which reply was not the one expected while the driver still runs:
// the step in bits 56-63, the reply's kind in bits 48-55 and its detail's
// low 48 bits, in place of an exit code.
[[nodiscard]] PagerServeError Fail(const ScenarioContext& context, const ComponentRecord& driver,
                                   const Foundation::UInt64 step, const BlockReply& reply) noexcept
{
    ReportUnexpectedEnd(context, driver.Tag,
                        (driver.ExitCode != 0U)
                            ? driver.ExitCode
                            : ((step << 56U) | ((reply.Kind & 0xFFU) << 48U) | (reply.Detail & 0xFFFF'FFFF'FFFFULL)),
                        false);
    return PagerServeError::ManagementFailed;
}

// The first three requests are queued before the driver's thread starts:
// a read of the token, a read of it into the other Frame, and a Cancel of
// that second read. The driver drains them together, so the second read is
// still queued behind the first when the Cancel finds it: its
// final reply and the Cancel's answer are Cancelled. (Sent while the driver
// ran, a preemption between them could let it issue the second read first.) The first read is Done, or Failed with
// BlockNoDevice on a controller without a disk; the replies may come in
// either order. Returns whether the disk served, through served.
[[nodiscard, gnu::noinline]] PagerServeError ReadAndCancel(ScenarioContext& context, ComponentRecord& driver,
                                                           const BlockClient& client, bool& served,
                                                           Abi::SystemCallError& systemCall) noexcept
{
    if (!SendTransfer(context, driver, client, Component::BlockReadMethod, ReadTokenTransaction, TokenSector,
                      client.Data[0U], systemCall) ||
        !SendTransfer(context, driver, client, Component::BlockReadMethod, QueuedTransaction, TokenSector,
                      client.Data[1U], systemCall) ||
        !SendCancel(context, driver, client, CancelQueuedTransaction, QueuedTransaction, systemCall) ||
        !context.Supervisor->Launch(driver, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    bool cancelled{false};
    bool answered{false};
    bool read{false};
    for (Foundation::Size index{0U}; index < 3U; ++index)
    {
        const BlockReply reply = AwaitBlockReply(context, driver, client, systemCall);
        if (!cancelled && Is(reply, QueuedTransaction, QueuedTransaction, Component::BlockCancelled, 0U))
        {
            cancelled = true;
        }
        else if (!answered && Is(reply, CancelQueuedTransaction, CancelQueuedTransaction, Component::BlockCancelled,
                                 QueuedTransaction))
        {
            answered = true;
        }
        else if (!read && Is(reply, ReadTokenTransaction, ReadTokenTransaction, Component::BlockDone, 1U))
        {
            read = true;
            served = true;
        }
        else if (!read && Is(reply, ReadTokenTransaction, ReadTokenTransaction, Component::BlockFailed,
                             Component::BlockNoDevice))
        {
            read = true;
            served = false;
        }
        else
        {
            return Fail(context, driver, 1U + index, reply);
        }
    }
    return PagerServeError::None;
}

// With a disk: the token arrived; a Cancel of that finished read is
// TooLate; the scratch sector written from one Frame reads back into the
// other.
[[nodiscard, gnu::noinline]] PagerServeError Serve(ScenarioContext& context, const ComponentRecord& driver,
                                                   const BlockClient& client, Abi::SystemCallError& systemCall) noexcept
{
    Foundation::UInt64 found{0U};
    if (!HoldsToken(context, client.Data[0U], found, systemCall))
    {
        return Fail(context, driver, 7U, BlockReply{.Header = 0U, .Answers = 0U, .Kind = 0U, .Detail = found});
    }
    if (!SendCancel(context, driver, client, CancelLateTransaction, ReadTokenTransaction, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    const BlockReply late = AwaitBlockReply(context, driver, client, systemCall);
    if (!Is(late, CancelLateTransaction, CancelLateTransaction, Component::BlockTooLate, ReadTokenTransaction))
    {
        return Fail(context, driver, 4U, late);
    }
    if (!Pattern(context, client.Data[1U], false, found, systemCall) ||
        !SendTransfer(context, driver, client, Component::BlockWriteMethod, WriteTransaction, ScratchSector,
                      client.Data[1U], systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    const BlockReply written = AwaitBlockReply(context, driver, client, systemCall);
    if (!Is(written, WriteTransaction, WriteTransaction, Component::BlockDone, 1U) ||
        !SendTransfer(context, driver, client, Component::BlockReadMethod, ReadBackTransaction, ScratchSector,
                      client.Data[0U], systemCall))
    {
        return Fail(context, driver, 5U, written);
    }
    const BlockReply readBack = AwaitBlockReply(context, driver, client, systemCall);
    if (!Is(readBack, ReadBackTransaction, ReadBackTransaction, Component::BlockDone, 1U))
    {
        return Fail(context, driver, 6U, readBack);
    }
    return Pattern(context, client.Data[0U], true, found, systemCall)
               ? PagerServeError::None
               : Fail(context, driver, 8U, BlockReply{.Header = 0U, .Answers = 0U, .Kind = 0U, .Detail = found});
}

// Waits until the runner throttled the disk: QEMU lets the first
// request after an idle spell through at once, so each probe is a read that
// fills the throttle's bucket and a second one; the throttle is on once the
// second is still unanswered after ThrottleProbeMicroseconds in which the
// root service served nothing (a fault served ends a wait early). That read is
// left running, its answer to be taken after the crash: the driver issues
// the crash's read only behind it, and the throttle holds that one too. The
// probe's second read's transaction into pending.
[[nodiscard, gnu::noinline]] PagerServeError AwaitThrottle(ScenarioContext& context, const ComponentRecord& driver,
                                                           const BlockClient& client, Foundation::UInt64& pending,
                                                           Abi::SystemCallError& systemCall) noexcept
{
    for (Foundation::UInt64 probe{0U}; probe < ThrottleProbes; ++probe)
    {
        const Foundation::UInt64 fill = ProbeTransactions + (probe << 1U);
        if (!SendTransfer(context, driver, client, Component::BlockReadMethod, fill, TokenSector, client.Data[0U],
                          systemCall))
        {
            return PagerServeError::ManagementFailed;
        }
        const BlockReply filled = AwaitBlockReply(context, driver, client, systemCall);
        if (!Is(filled, fill, fill, Component::BlockDone, 1U) ||
            !SendTransfer(context, driver, client, Component::BlockReadMethod, fill + 1U, TokenSector, client.Data[0U],
                          systemCall))
        {
            return Fail(context, driver, 9U, filled);
        }
        const PagerServeError paused = context.Supervisor->Quiet(ThrottleProbeMicroseconds, systemCall);
        if (paused != PagerServeError::None)
        {
            return paused;
        }
        const Abi::SystemCallResponse answered =
            context.Client->Receive(client.Reply, AddressOf(context.Delivery), sizeof(context.Delivery));
        if (answered.Error == Abi::SystemCallError::WouldBlock)
        {
            pending = fill + 1U;
            return PagerServeError::None;
        }
        Component::BlockMessage message{};
        if ((answered.Error != Abi::SystemCallError::None) ||
            !Component::ReadBlockFrame(context.Delivery, static_cast<Foundation::Size>(answered.Value1), message) ||
            (message.Transaction != (fill + 1U)) || (message.Words[1U] != Component::BlockDone))
        {
            systemCall = answered.Error;
            return PagerServeError::ManagementFailed;
        }
    }
    return PagerServeError::ManagementFailed;
}

// The recovery's evidence: the root service asks the runner to
// throttle the disk and waits until it has, then sends a read of the token
// into the third Frame, whose command the driver issues and then crashes on.
// As soon as the crash is reported the client destroys that Frame; the drain
// parks its pin, still mapped for the device, and the Device Manager's reset
// of the controller lets the read land in it before the confirmation
// releases the pin. The read must still have been running as the Device
// Manager began (its quiesce found it issued); its request is never
// answered, the driver being dead; the probe's read it waited behind was,
// and nothing else is left on the reply Endpoint.
[[nodiscard, gnu::noinline]] PagerServeError Crash(ScenarioContext& context, ComponentRecord& driver,
                                                   BlockClient& client, Abi::SystemCallError& systemCall) noexcept
{
    ComponentSupervisor& supervisor = *context.Supervisor;
    const Foundation::UInt64 crashes = supervisor.GetCrashes();
    WriteLine(context, BlockThrottleLine, sizeof(BlockThrottleLine) - 1U);
    Foundation::UInt64 pending{0U};
    const PagerServeError throttled = AwaitThrottle(context, driver, client, pending, systemCall);
    if (throttled != PagerServeError::None)
    {
        return throttled;
    }
    const Foundation::UInt64 words[Component::BlockIoPayloadWords]{driver.Tag, TokenSector, 1U,
                                                                   Component::BlockCrashAfterIssue, driver.Tag};
    if (!SendBlock(context, driver, client, Component::BlockReadMethod, CrashTransaction, words, &client.Data[2U],
                   systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    supervisor.SetWatchdog(driver, true);
    const PagerServeError ended = supervisor.Supervise(driver, systemCall);
    if (ended != PagerServeError::None)
    {
        return ended;
    }
    if ((driver.ExitCode != Abi::FaultTerminatedExitCode) || (supervisor.GetCrashes() != (crashes + 1U)))
    {
        ReportUnexpectedEnd(context, driver.Tag, driver.ExitCode, supervisor.GetCrashes() != crashes);
        return PagerServeError::ManagementFailed;
    }
    const bool destroyed = context.Calls->Destroy(client.Data[2U], Abi::ObjectTypeFrame, systemCall);
    client.Data[2U] = {};
    if (!destroyed || !supervisor.Teardown(driver, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    // The probe's read was answered before the driver issued the crash's.
    const Abi::SystemCallResponse probed =
        context.Client->Receive(client.Reply, AddressOf(context.Delivery), sizeof(context.Delivery));
    Component::BlockMessage message{};
    if ((probed.Error != Abi::SystemCallError::None) ||
        !Component::ReadBlockFrame(context.Delivery, static_cast<Foundation::Size>(probed.Value1), message) ||
        (message.Transaction != pending) || (message.Words[1U] != Component::BlockDone) ||
        !context.Devices->GetLastQuiesce().Running)
    {
        // The driver ended as it should, with its crash: report the step,
        // the probe's reply kind and whether the quiesce found the read
        // issued, not that crash's exit code.
        systemCall = probed.Error;
        ReportUnexpectedEnd(context, driver.Tag,
                            (Foundation::UInt64{10U} << 56U) | ((message.Words[1U] & 0xFFU) << 48U) |
                                (context.Devices->GetLastQuiesce().Running ? 1U : 0U),
                            false);
        return PagerServeError::ManagementFailed;
    }
    return PagerServeError::None;
}

// Destroys what the demonstration created; a zero handle is skipped.
[[nodiscard, gnu::noinline]] bool Release(ScenarioContext& context, BlockClient& client,
                                          Abi::SystemCallError& systemCall) noexcept
{
    bool released{true};
    for (Foundation::Size index{0U}; index < BlockClientFrames; ++index)
    {
        const System::CapabilityHandle data = client.Data[index];
        client.Data[index] = {};
        released =
            ((data.Selector == 0U) || context.Calls->Destroy(data, Abi::ObjectTypeFrame, systemCall)) && released;
    }
    released =
        ((client.Reply.Selector == 0U) || context.Calls->Destroy(client.Reply, Abi::ObjectTypeEndpoint, systemCall)) &&
        released;
    client.Reply = {};
    return released;
}

[[nodiscard, gnu::noinline]] PagerServeError RunClient(ScenarioContext& context, BlockClient& client,
                                                       Abi::SystemCallError& systemCall) noexcept
{
    if (!CreateReplyEndpoint(context, client, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    for (System::CapabilityHandle& data : client.Data)
    {
        if (!context.Calls->CreateMemory(context.Management, Abi::ObjectTypeFrame, 1U, 0U, data, systemCall))
        {
            return PagerServeError::ManagementFailed;
        }
    }
    ComponentRecord* const driver =
        BuildComponent(context, context.Component, context.NextTag++, Abi::ComponentBehaviorBlockDriver, systemCall);
    if (driver == nullptr)
    {
        return PagerServeError::ManagementFailed;
    }
    bool served{false};
    PagerServeError result = ReadAndCancel(context, *driver, client, served, systemCall);
    result = ((result == PagerServeError::None) && served) ? Serve(context, *driver, client, systemCall) : result;
    if (result != PagerServeError::None)
    {
        return result;
    }
    // With a disk the driver ends by the crash; without one, killed while
    // it waits.
    result = served ? Crash(context, *driver, client, systemCall) : Stop(context, *driver, systemCall);
    if (result != PagerServeError::None)
    {
        return result;
    }
    // Every request was answered once: nothing is left on the Endpoint.
    const Abi::SystemCallResponse left =
        context.Client->Receive(client.Reply, AddressOf(context.Delivery), sizeof(context.Delivery));
    if (left.Error != Abi::SystemCallError::WouldBlock)
    {
        systemCall = left.Error;
        return PagerServeError::ManagementFailed;
    }
    WriteLine(context, served ? BlockServedLine : BlockNoDiskLine,
              (served ? sizeof(BlockServedLine) : sizeof(BlockNoDiskLine)) - 1U);
    return PagerServeError::None;
}
} // Namespace

// The AHCI driver's demonstration: the root service, an ordinary client of the block protocol,
// starts the driver service, reads the disk's token into a Frame of its own, has a queued read
// Cancelled and a finished one's Cancel answered TooLate, writes a scratch sector from one Frame
// and reads it back into the other; then the driver crashes with a read running into a Frame the
// client destroys at once, and the Device Manager resets the controller before the drain's
// quarantine lifts and the Frame's pin goes. A controller without a disk answers the read
// BlockNoDevice, and its driver is killed while it waits.
PagerServeError RunBlockDemo(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept
{
    BlockClient client{.Reply = {}, .Data = {}};
    const PagerServeError result = RunClient(context, client, systemCall);
    Abi::SystemCallError released{Abi::SystemCallError::None};
    const bool clean = Release(context, client, released);
    systemCall = (systemCall == Abi::SystemCallError::None) ? released : systemCall;
    return (result != PagerServeError::None) ? result
                                             : (clean ? PagerServeError::None : PagerServeError::ManagementFailed);
}
} // Namespace Asteria::User::Pager
