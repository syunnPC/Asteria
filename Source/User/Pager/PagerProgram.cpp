// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/PagerProgram.hpp"

#include "Abi/Clock.hpp"
#include "Abi/PagerProtocol.hpp"
#include "Abi/IpcCanonicalFrame.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "User/Pager/PagerWords.hpp"

namespace Asteria::User::Pager
{
using Platform::Synchronization::AtomicUInt64Reference;
namespace
{
// The waiting self-test thread's timeout, and the pager's timed waits while it waits.
constexpr Foundation::UInt64 WaitingThreadTimeoutMicroseconds{20'000U};
constexpr Foundation::UInt64 WaitingPollMicroseconds{10'000U};
// The polls the pager waits for its second waiting thread to start, and how
// long it then lets the thread block before it kills it: longer than a user
// time slice, so the thread got the processor.
constexpr Foundation::UInt64 WaitingAttempts{200U};
constexpr Foundation::UInt64 KillPauseMicroseconds{100'000U};
// The pager-protocol fault message as an Endpoint delivers it: a 48-byte
// delivery header, the 88-byte frame header, one 32-byte attachment (the
// FaultToken handle), then the 32-byte fault record whose first word is the
// faulting address.
constexpr Foundation::Size DeliveryHeaderBytes{48U};
constexpr Foundation::Size FrameHeaderBytes{88U};
constexpr Foundation::Size AttachmentBytes{32U};
// Component launch: record, tag and kernel domain identity, v1.2.
constexpr Foundation::Size FaultPayloadBytes{Abi::PagerFaultPayloadBytes};
constexpr Foundation::Size FaultDeliveryBytes{DeliveryHeaderBytes + FrameHeaderBytes + AttachmentBytes +
                                              FaultPayloadBytes};
constexpr Foundation::Size AttachmentCountOffset{DeliveryHeaderBytes + 81U};
constexpr Foundation::Size PayloadSizeOffset{DeliveryHeaderBytes + 12U};
constexpr Foundation::Size TokenOffset{DeliveryHeaderBytes + FrameHeaderBytes};
constexpr Foundation::Size FaultAddressOffset{TokenOffset + AttachmentBytes};
// The faulting domain's supervision tag, after the 32-byte fault record.
constexpr Foundation::Size SupervisionTagOffset{FaultAddressOffset + Abi::PagerFaultTagOffset};
constexpr Foundation::Size ProtectionIdentityOffset{FaultAddressOffset + Abi::PagerFaultIdentityOffset};
// The record's access and cause bytes (PagerProtocol.md): the pager maps
// only a missing translation of a data access; a protection fault, an
// instruction fetch or an exception gets nothing a lazy data page provides.
constexpr Foundation::Size FaultAccessOffset{FaultAddressOffset + 24U};
constexpr Foundation::Size FaultCauseOffset{FaultAddressOffset + 25U};
constexpr Foundation::UInt8 ExecuteFaultAccess{3U};
constexpr Foundation::UInt8 MissingMappingFaultCause{1U};
constexpr Foundation::UInt64 PageBytes{4096U};
constexpr Foundation::UInt8 FrameMagic[4U]{'A', 'I', 'P', 'C'};
// Read, Write and User: the lazy region is ordinary user data.
constexpr Foundation::UInt8 LazyPermissions{0x0BU};
// The self-test loops the canonical attachment-free asynchronous AIPC frame
// (IpcWireFormat.md) through a created Endpoint: sequence 1, delivered with
// its 48-byte header.
constexpr const auto& LoopbackFrame = Abi::CanonicalAsynchronousFrame;
static_assert(sizeof(LoopbackFrame) <= Abi::FaultResolveRequestSizeBytes);
constexpr Foundation::UInt64 LoopbackSequence{1U};
// Endpoints the self-test creates one after the other.
constexpr Foundation::Size SelfTestEndpoints{2U};
// The Frame and MemoryObject the self-test creates: two pages, the
// MemoryObject readable, writable and user-accessible.
constexpr Foundation::UInt64 SelfTestMemoryPages{2U};
constexpr Foundation::UInt8 SelfTestMemoryPermissions{0x0BU};
// The supervision tag of the self-test's first created domain; each next
// cycle's is one more.
constexpr Foundation::UInt64 SelfTestDomainTag{0x5E1F'0001U};

[[nodiscard]] constexpr PagerServeOutcome MakeOutcome(const PagerServeError error,
                                                      const Abi::SystemCallError systemCall,
                                                      const Foundation::UInt64 resolved) noexcept
{
    return PagerServeOutcome{.Error = error, .SystemCall = systemCall, .Resolved = resolved};
}

[[nodiscard]] CapabilityHandle LoadHandle(const Foundation::UInt64 (&words)[Abi::PagerBootstrapWords],
                                          const Foundation::Size word) noexcept
{
    return CapabilityHandle{words[word], words[word + 1U]};
}
} // namespace

bool DecodePagerBootstrap(const Foundation::UInt64 (&words)[Abi::PagerBootstrapWords], PagerBootstrap& output) noexcept
{
    const Foundation::UInt64 start = words[Abi::PagerBootstrapLazyStartWord];
    const Foundation::UInt64 pages = words[Abi::PagerBootstrapLazyPagesWord];
    const Foundation::UInt64 moduleBase = words[Abi::PagerBootstrapModuleBaseWord];
    const Foundation::UInt64 moduleBytes = words[Abi::PagerBootstrapModuleBytesWord];
    const Foundation::UInt64 windowStart = words[Abi::PagerBootstrapWindowStartWord];
    const Foundation::UInt64 windowPages = words[Abi::PagerBootstrapWindowPagesWord];
    if (((start % PageBytes) != 0U) || (pages == 0U) || (pages > ((~Foundation::UInt64{0U} - start) / PageBytes)) ||
        ((windowStart % PageBytes) != 0U) || (windowPages > ((~Foundation::UInt64{0U} - windowStart) / PageBytes)))
    {
        return false;
    }
    if ((moduleBase == 0U) != (moduleBytes == 0U) || (moduleBase % PageBytes) != 0U ||
        (moduleBytes % PageBytes) != 0U || moduleBytes > Abi::PagerModuleMaximumBytes ||
        moduleBase > ~Foundation::UInt64{0U} - moduleBytes)
    {
        return false;
    }
    output = PagerBootstrap{
        .Endpoint = LoadHandle(words, Abi::PagerBootstrapEndpointWord),
        .WaitSet = LoadHandle(words, Abi::PagerBootstrapWaitSetWord),
        .Diagnostic = LoadHandle(words, Abi::PagerBootstrapDiagnosticWord),
        .ClientSpace = LoadHandle(words, Abi::PagerBootstrapClientSpaceWord),
        .LazyMemory = LoadHandle(words, Abi::PagerBootstrapLazyMemoryWord),
        .LazyFrame = LoadHandle(words, Abi::PagerBootstrapLazyFrameWord),
        .LazyStart = start,
        .LazyPages = pages,
        .Management = LoadHandle(words, Abi::PagerBootstrapManagementWord),
        .ManagementFeatures = words[Abi::PagerBootstrapManagementFeaturesWord],
        .OwnSpace = LoadHandle(words, Abi::PagerBootstrapOwnSpaceWord),
        .WindowStart = windowStart,
        .WindowPages = windowPages,
        .Devices = {},
        .Scanout =
            PagerScanout{
                .Region = LoadHandle(words, Abi::PagerBootstrapScanoutRegionWord),
                .Width = static_cast<Foundation::UInt32>(words[Abi::PagerBootstrapScanoutModeWord]),
                .Height = static_cast<Foundation::UInt32>(words[Abi::PagerBootstrapScanoutModeWord] >> 32U),
                .Stride = static_cast<Foundation::UInt32>(words[Abi::PagerBootstrapScanoutModeWord + 1U]),
                .Format = static_cast<Foundation::UInt32>(words[Abi::PagerBootstrapScanoutModeWord + 1U] >> 32U),
            },
    };
    for (Foundation::Size device{0U}; device < Abi::PagerBootstrapDevices; ++device)
    {
        output.Devices[device] = PagerDevice{
            .Function = LoadHandle(words, Abi::PagerBootstrapDeviceFunctionWord(device)),
            .Region = LoadHandle(words, Abi::PagerBootstrapDeviceRegionWord(device)),
        };
    }
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        output.I8042[index] = LoadHandle(words, Abi::PagerBootstrapI8042DataWord + index * 2U);
    }
    output.InventoryFrame = LoadHandle(words, Abi::PagerBootstrapInventoryFrameWord);
    output.InventoryPages = words[Abi::PagerBootstrapInventoryPagesWord];
    output.ModuleBase = moduleBase;
    output.ModuleBytes = moduleBytes;
    output.I8042Declared = words[Abi::PagerBootstrapI8042DeclaredWord];
    output.ClockTicksPerMicrosecond = words[Abi::PagerBootstrapClockTicksPerMicrosecondWord];
    output.WatchdogSeconds = words[Abi::PagerBootstrapWatchdogSecondsWord];
    output.SessionMode = words[Abi::PagerBootstrapSessionModeWord];
    output.LoginEnabled = words[Abi::PagerBootstrapLoginEnabledWord];
    if (output.SessionMode > Abi::SessionDesktop || output.LoginEnabled > 1U)
    {
        return false;
    }
    if (output.WatchdogSeconds > Abi::WatchdogMaximumSeconds)
    {
        return false;
    }
    return true;
}

PagerServeOutcome PagerProgram::ServeOnce(const Foundation::UInt64 timeoutMicroseconds) noexcept
{
    const Abi::SystemCallResponse waited = m_client.Wait(m_bootstrap.WaitSet, timeoutMicroseconds);
    if ((waited.Error == Abi::SystemCallError::TimedOut) && (timeoutMicroseconds != 0U))
    {
        return MakeOutcome(PagerServeError::TimedOut, waited.Error, 0U);
    }
    if (waited.Error != Abi::SystemCallError::None)
    {
        return MakeOutcome(PagerServeError::WaitFailed, waited.Error, 0U);
    }
    if (waited.Value0 != Abi::PagerReadinessTag)
    {
        return MakeOutcome(PagerServeError::UnexpectedReadiness, Abi::SystemCallError::None, 0U);
    }
    Foundation::UInt64 resolved{0U};
    for (;;)
    {
        const Abi::SystemCallResponse received =
            m_client.Receive(m_bootstrap.Endpoint, AddressOf(m_delivery), sizeof(m_delivery));
        if (received.Error == Abi::SystemCallError::WouldBlock)
        {
            return MakeOutcome(PagerServeError::None, Abi::SystemCallError::None, resolved);
        }
        if (received.Error != Abi::SystemCallError::None)
        {
            return MakeOutcome(PagerServeError::ReceiveFailed, received.Error, resolved);
        }
        Abi::SystemCallError systemCall{Abi::SystemCallError::None};
        const PagerServeError error = ResolveDelivery(static_cast<Foundation::Size>(received.Value1), systemCall);
        if (error != PagerServeError::None)
        {
            return MakeOutcome(error, systemCall, resolved);
        }
        ++resolved;
    }
}

PagerServeError PagerProgram::ResolveDelivery(const Foundation::Size sizeBytes,
                                              Abi::SystemCallError& systemCall) noexcept
{
    const Foundation::UInt8* const delivery = m_delivery;
    bool magic{true};
    for (Foundation::Size index{0U}; index < sizeof(FrameMagic); ++index)
    {
        magic = magic && (delivery[DeliveryHeaderBytes + index] == FrameMagic[index]);
    }
    if ((sizeBytes != FaultDeliveryBytes) || !magic ||
        (delivery[DeliveryHeaderBytes + 36U] != Abi::PagerProtocolMajor) ||
        (delivery[DeliveryHeaderBytes + 37U] != 0U) ||
        (delivery[DeliveryHeaderBytes + 38U] != Abi::PagerProtocolMinor) ||
        (delivery[DeliveryHeaderBytes + 39U] != 0U) || (LoadWord(delivery + ProtectionIdentityOffset) == 0U) ||
        (delivery[AttachmentCountOffset] != 1U) ||
        ((static_cast<Foundation::Size>(delivery[PayloadSizeOffset]) |
          (static_cast<Foundation::Size>(delivery[PayloadSizeOffset + 1U]) << 8U)) != FaultPayloadBytes))
    {
        return PagerServeError::MalformedMessage;
    }
    const CapabilityHandle token{LoadWord(delivery + TokenOffset), LoadWord(delivery + TokenOffset + 8U)};
    // A fault of a domain this pager created, as root service, is a crash: it is never mapped,
    // whatever its address.
    if (LoadWord(delivery + SupervisionTagOffset) != 0U)
    {
        ++m_componentCrashes;
        return Terminate(token, systemCall);
    }
    // Any other fault of a boot thread, inside the lazy region or not, is terminated too: an
    // exception's address is its instruction's, and a fault on a page already mapped would only
    // repeat.
    if ((delivery[FaultCauseOffset] != MissingMappingFaultCause) || (delivery[FaultAccessOffset] == ExecuteFaultAccess))
    {
        return Terminate(token, systemCall);
    }
    const Foundation::UInt64 page = LoadWord(delivery + FaultAddressOffset) & ~(PageBytes - 1U);
    if ((page < m_bootstrap.LazyStart) || (((page - m_bootstrap.LazyStart) / PageBytes) >= m_bootstrap.LazyPages))
    {
        return Terminate(token, systemCall);
    }
    const Foundation::UInt64 offsetPages = (page - m_bootstrap.LazyStart) / PageBytes;

    Foundation::UInt8* const request = m_request;
    for (Foundation::UInt8& byte : m_request)
    {
        byte = 0U;
    }
    StoreHandle(request + Abi::FaultResolveAddressSpaceOffset, m_bootstrap.ClientSpace);
    StoreHandle(request + Abi::FaultResolveMemoryObjectOffset, m_bootstrap.LazyMemory);
    StoreHandle(request + Abi::FaultResolveFrameOffset, m_bootstrap.LazyFrame);
    StoreWord(request + Abi::FaultResolveVirtualStartOffset, page);
    StoreWord(request + Abi::FaultResolvePageCountOffset, 1U);
    StoreWord(request + Abi::FaultResolveMemoryObjectPageOffset, offsetPages);
    StoreWord(request + Abi::FaultResolveFramePageOffset, offsetPages);
    request[Abi::FaultResolvePermissionsOffset] = LazyPermissions;

    const Abi::SystemCallResponse resolved =
        m_client.FaultResolve(token, Abi::FaultResolveKind::Map, AddressOf(request));
    if (resolved.Error != Abi::SystemCallError::None)
    {
        systemCall = resolved.Error;
        return PagerServeError::ResolveFailed;
    }
    return Close(token, systemCall);
}

// A fault this pager does not back: as the boot domains' supervisor it ends the faulting thread and
// Keeps serving.
PagerServeError PagerProgram::Terminate(const CapabilityHandle token, Abi::SystemCallError& systemCall) noexcept
{
    const Abi::SystemCallResponse terminated = m_client.FaultTerminate(token);
    if ((terminated.Error != Abi::SystemCallError::None) || (terminated.Value0 != Abi::FaultTerminatedExitCode))
    {
        systemCall = terminated.Error;
        return PagerServeError::TerminateFailed;
    }
    ++m_terminated;
    return Close(token, systemCall);
}

PagerServeOutcome PagerProgram::TestManagement() noexcept
{
    Foundation::UInt64 created{0U};
    if ((m_bootstrap.Management.Selector == 0U) && (m_bootstrap.Management.Authenticator == 0U))
    {
        return MakeOutcome(PagerServeError::None, Abi::SystemCallError::None, created);
    }
    BuildDefaultEndpointRequest(m_endpointRequest);
    for (Foundation::Size round{0U}; round < SelfTestEndpoints; ++round)
    {
        Abi::SystemCallError systemCall{Abi::SystemCallError::None};
        const PagerServeError error = TestEndpoint(round == 0U, systemCall);
        if (error != PagerServeError::None)
        {
            return MakeOutcome(error, systemCall, created);
        }
        ++created;
    }
    if ((m_bootstrap.ManagementFeatures & Abi::PagerManagementFrames) != 0U)
    {
        Abi::SystemCallError systemCall{Abi::SystemCallError::None};
        const PagerServeError error = TestMemory(systemCall);
        if (error != PagerServeError::None)
        {
            return MakeOutcome(error, systemCall, created);
        }
        created += 2U;
    }
    if ((m_bootstrap.ManagementFeatures & Abi::PagerManagementMapping) != 0U)
    {
        Abi::SystemCallError systemCall{Abi::SystemCallError::None};
        const PagerServeError error = TestMapping(systemCall);
        if (error != PagerServeError::None)
        {
            return MakeOutcome(error, systemCall, created);
        }
        created += 2U;
    }
    if (((m_bootstrap.ManagementFeatures & Abi::PagerManagementThreads) != 0U) && (m_threadLaunch != nullptr))
    {
        Abi::SystemCallError systemCall{Abi::SystemCallError::None};
        const PagerServeError error = TestThread(systemCall);
        if (error != PagerServeError::None)
        {
            return MakeOutcome(error, systemCall, created);
        }
        ++created;
        const PagerServeError faulting = TestFaultingThread(m_threadLaunch->FaultEntry, systemCall);
        if (faulting != PagerServeError::None)
        {
            return MakeOutcome(faulting, systemCall, created);
        }
        ++created;
        if (m_threadLaunch->ExceptionEntry != 0U)
        {
            const PagerServeError exception = TestFaultingThread(m_threadLaunch->ExceptionEntry, systemCall);
            if (exception != PagerServeError::None)
            {
                return MakeOutcome(exception, systemCall, created);
            }
            ++created;
        }
        if (m_threadLaunch->SpinningEntry != 0U)
        {
            const PagerServeError killed = TestKilledThread(systemCall);
            if (killed != PagerServeError::None)
            {
                return MakeOutcome(killed, systemCall, created);
            }
            ++created;
        }
        if (((m_bootstrap.ManagementFeatures & Abi::PagerManagementWaitSets) != 0U) &&
            (m_threadLaunch->WaitingEntry != 0U))
        {
            const PagerServeError waited = TestWaitingThread(systemCall);
            if (waited != PagerServeError::None)
            {
                return MakeOutcome(waited, systemCall, created);
            }
            created += 3U;
        }
    }
    // More cycles than pool slots reuse a slot, with fresh RIDs on IA-64.
    constexpr Foundation::UInt64 domainFeatures{Abi::PagerManagementDomains | Abi::PagerManagementMapping};
    if (((m_bootstrap.ManagementFeatures & domainFeatures) == domainFeatures) && (m_threadLaunch != nullptr))
    {
        for (Foundation::UInt64 cycle{0U}; cycle < m_threadLaunch->DomainCycles; ++cycle)
        {
            Abi::SystemCallError systemCall{Abi::SystemCallError::None};
            const PagerServeError error = TestDomain(SelfTestDomainTag + cycle, systemCall);
            if (error != PagerServeError::None)
            {
                return MakeOutcome(error, systemCall, created);
            }
            created += 5U;
        }
    }
    return MakeOutcome(PagerServeError::None, Abi::SystemCallError::None, created);
}

// A thread in this pager's own domain runs beside it: ObjectDestroy refuses it until it exited, so
// polling waits for its exit code, and every refused poll is one the thread can see.
PagerServeError PagerProgram::TestThread(Abi::SystemCallError& systemCall) noexcept
{
    AtomicUInt64Reference polls{m_threadControl.Polls};
    polls.StoreRelease(0U);
    m_threadControl.Diagnostic = m_bootstrap.Diagnostic;
    m_threadControl.SpinLimit = m_threadLaunch->SpinLimit;
    CapabilityHandle thread{};
    if (CreateThread(m_threadLaunch->Entry, reinterpret_cast<Foundation::UIntPointer>(&m_threadControl), thread,
                     systemCall) != PagerServeError::None)
    {
        return PagerServeError::ManagementFailed;
    }
    Abi::SystemCallResponse destroyed = m_client.DestroyObject(thread, Abi::ObjectTypeThread);
    while (destroyed.Error == Abi::SystemCallError::WouldBlock)
    {
        polls.IncrementRelease();
        destroyed = m_client.DestroyObject(thread, Abi::ObjectTypeThread);
    }
    return ReclaimThread(thread, destroyed, PagerThreadExitCode, systemCall);
}

// A thread of this pager's own domain faults on page zero, which the pager does not back, or
// executes an invalid instruction, so the fault or exception reaches this pager, its supervisor,
// whose address rule terminates it. The pager serves faults between its ObjectDestroy calls; the
// loop ends because the thread can only fault.
PagerServeError PagerProgram::TestFaultingThread(const Foundation::UInt64 entry,
                                                 Abi::SystemCallError& systemCall) noexcept
{
    CapabilityHandle thread{};
    if (CreateThread(entry, 0U, thread, systemCall) != PagerServeError::None)
    {
        return PagerServeError::ManagementFailed;
    }
    Abi::SystemCallResponse destroyed = m_client.DestroyObject(thread, Abi::ObjectTypeThread);
    while (destroyed.Error == Abi::SystemCallError::WouldBlock)
    {
        const PagerServeOutcome served = ServeOnce();
        if (served.Error != PagerServeError::None)
        {
            systemCall = served.SystemCall;
            return served.Error;
        }
        destroyed = m_client.DestroyObject(thread, Abi::ObjectTypeThread);
    }
    return ReclaimThread(thread, destroyed, Abi::FaultTerminatedExitCode, systemCall);
}

// A thread of this pager's own domain spins forever. Once it counted, which on the one processor
// created threads share means it ran and a user tick preempted it so this pager could run again,
// the pager kills it with ThreadTerminate and reclaims it with KilledExitCode; a second kill of the
// exited thread changes nothing.
PagerServeError PagerProgram::TestKilledThread(Abi::SystemCallError& systemCall) noexcept
{
    AtomicUInt64Reference spins{m_spins};
    spins.StoreRelease(0U);
    CapabilityHandle thread{};
    if (CreateThread(m_threadLaunch->SpinningEntry, AddressOf(&m_spins), thread, systemCall) != PagerServeError::None)
    {
        return PagerServeError::ManagementFailed;
    }
    while (spins.LoadAcquire() == 0U)
    {
    }
    const Abi::SystemCallResponse killed = m_client.TerminateThread(thread);
    const Abi::SystemCallResponse again = m_client.TerminateThread(thread);
    if ((killed.Error != Abi::SystemCallError::None) || (again.Error != Abi::SystemCallError::None))
    {
        systemCall = (killed.Error != Abi::SystemCallError::None) ? killed.Error : again.Error;
        return PagerServeError::ManagementFailed;
    }
    return ReclaimThread(thread, m_client.DestroyObject(thread, Abi::ObjectTypeThread), Abi::KilledExitCode,
                         systemCall);
}

// A thread of this pager's own domain waits, with a timeout, on a WaitSet it created and nothing is
// bound to: the thread blocks, the kernel expires its wait and it exits with
// PagerWaitingThreadExitCode, having seen TimedOut. The pager polls ObjectDestroy between timed
// waits of its own, so the processor idles while both wait. A second thread then waits on it
// without a timeout; once it started waiting, and a timed wait of the pager longer than a time
// slice let it block, the pager kills it, which aborts its wait, and reclaims it with
// KilledExitCode.
PagerServeError PagerProgram::TestWaitingThread(Abi::SystemCallError& systemCall) noexcept
{
    for (Foundation::UInt8& byte : m_request)
    {
        byte = 0U;
    }
    CapabilityHandle waitSet{};
    if (CreateFromRequest(Abi::ObjectTypeWaitSet, Abi::ObjectCreateWaitSetRequestSizeBytes, waitSet, systemCall) !=
        PagerServeError::None)
    {
        return PagerServeError::ManagementFailed;
    }
    m_waiting.WaitSet = waitSet;
    m_waiting.TimeoutMicroseconds = WaitingThreadTimeoutMicroseconds;
    AtomicUInt64Reference{m_waiting.Waiting}.StoreRelease(0U);
    AtomicUInt64Reference{m_waiting.Result}.StoreRelease(0U);
    CapabilityHandle thread{};
    if (CreateThread(m_threadLaunch->WaitingEntry, reinterpret_cast<Foundation::UIntPointer>(&m_waiting), thread,
                     systemCall) != PagerServeError::None)
    {
        return PagerServeError::ManagementFailed;
    }
    Abi::SystemCallResponse destroyed = m_client.DestroyObject(thread, Abi::ObjectTypeThread);
    while (destroyed.Error == Abi::SystemCallError::WouldBlock)
    {
        const PagerServeOutcome served = ServeOnce(WaitingPollMicroseconds);
        if ((served.Error != PagerServeError::None) && (served.Error != PagerServeError::TimedOut))
        {
            systemCall = served.SystemCall;
            return served.Error;
        }
        destroyed = m_client.DestroyObject(thread, Abi::ObjectTypeThread);
    }
    const PagerServeError reclaimed = ReclaimThread(thread, destroyed, PagerWaitingThreadExitCode, systemCall);
    if (reclaimed != PagerServeError::None)
    {
        return reclaimed;
    }
    if (AtomicUInt64Reference::LoadAcquire(m_waiting.Result) !=
        static_cast<Foundation::UInt64>(Abi::SystemCallError::TimedOut))
    {
        return PagerServeError::ManagementFailed;
    }
    const PagerServeError killed = TestKilledWaiter(waitSet, systemCall);
    if (killed != PagerServeError::None)
    {
        return killed;
    }
    return DestroyAndClose(Abi::ObjectTypeWaitSet, waitSet, systemCall);
}

PagerServeError PagerProgram::TestKilledWaiter(const CapabilityHandle waitSet,
                                               Abi::SystemCallError& systemCall) noexcept
{
    m_waiting.WaitSet = waitSet;
    m_waiting.TimeoutMicroseconds = 0U;
    AtomicUInt64Reference{m_waiting.Waiting}.StoreRelease(0U);
    AtomicUInt64Reference{m_waiting.Result}.StoreRelease(0U);
    CapabilityHandle thread{};
    if (CreateThread(m_threadLaunch->WaitingEntry, reinterpret_cast<Foundation::UIntPointer>(&m_waiting), thread,
                     systemCall) != PagerServeError::None)
    {
        return PagerServeError::ManagementFailed;
    }
    const auto pause = [this, &systemCall](const Foundation::UInt64 microseconds) noexcept
    {
        const PagerServeOutcome served = ServeOnce(microseconds);
        if ((served.Error != PagerServeError::None) && (served.Error != PagerServeError::TimedOut))
        {
            systemCall = served.SystemCall;
            return served.Error;
        }
        return PagerServeError::None;
    };
    for (Foundation::UInt64 attempt{0U}; AtomicUInt64Reference::LoadAcquire(m_waiting.Waiting) == 0U; ++attempt)
    {
        const PagerServeError paused = pause(WaitingPollMicroseconds);
        if ((paused != PagerServeError::None) || (attempt == WaitingAttempts))
        {
            return (paused != PagerServeError::None) ? paused : PagerServeError::ManagementFailed;
        }
    }
    const PagerServeError settled = pause(KillPauseMicroseconds);
    if (settled != PagerServeError::None)
    {
        return settled;
    }
    const Abi::SystemCallResponse killed = m_client.TerminateThread(thread);
    if (killed.Error != Abi::SystemCallError::None)
    {
        systemCall = killed.Error;
        return PagerServeError::ManagementFailed;
    }
    Abi::SystemCallResponse destroyed = m_client.DestroyObject(thread, Abi::ObjectTypeThread);
    while (destroyed.Error == Abi::SystemCallError::WouldBlock)
    {
        const PagerServeError paused = pause(WaitingPollMicroseconds);
        if (paused != PagerServeError::None)
        {
            return paused;
        }
        destroyed = m_client.DestroyObject(thread, Abi::ObjectTypeThread);
    }
    return ReclaimThread(thread, destroyed, Abi::KilledExitCode, systemCall);
}

PagerServeError PagerProgram::CreateThread(const Foundation::UInt64 entry, const Foundation::UInt64 argument,
                                           CapabilityHandle& thread, Abi::SystemCallError& systemCall) noexcept
{
    for (Foundation::UInt8& byte : m_threadRequest)
    {
        byte = 0U;
    }
    StoreWord(m_threadRequest + Abi::ObjectCreateThreadEntryOffset, entry);
    StoreWord(m_threadRequest + Abi::ObjectCreateThreadStackOffset, m_threadLaunch->StackTop);
    StoreWord(m_threadRequest + Abi::ObjectCreateThreadArgumentOffset, argument);
    const Abi::SystemCallResponse created = m_client.CreateThread(m_bootstrap.Management, AddressOf(m_threadRequest));
    if (created.Error != Abi::SystemCallError::None)
    {
        systemCall = created.Error;
        return PagerServeError::ManagementFailed;
    }
    thread = CapabilityHandle{created.Value0, created.Value1};
    return PagerServeError::None;
}

PagerServeError PagerProgram::ReclaimThread(const CapabilityHandle thread, const Abi::SystemCallResponse& destroyed,
                                            const Foundation::UInt64 exitCode,
                                            Abi::SystemCallError& systemCall) noexcept
{
    const Abi::SystemCallResponse stale = m_client.QueryCapability(thread);
    const Abi::SystemCallResponse closed = m_client.CloseCapability(thread);
    if (destroyed.Error != Abi::SystemCallError::None)
    {
        systemCall = destroyed.Error;
        return PagerServeError::ManagementFailed;
    }
    if ((destroyed.Value0 != exitCode) || (stale.Error != Abi::SystemCallError::ObjectNotFound) ||
        (closed.Error != Abi::SystemCallError::None))
    {
        systemCall = (closed.Error != Abi::SystemCallError::None)
                         ? closed.Error
                         : ((destroyed.Value0 == exitCode) ? stale.Error : Abi::SystemCallError::None);
        return PagerServeError::ManagementFailed;
    }
    return PagerServeError::None;
}

// A Frame and a MemoryObject live together, then go; the MemoryObject goes first, which the page
// charge it carries allows.
PagerServeError PagerProgram::TestMemory(Abi::SystemCallError& systemCall) noexcept
{
    CapabilityHandle frame{};
    CapabilityHandle memory{};
    if ((CreatePair(frame, memory, systemCall) != PagerServeError::None) ||
        (DestroyAndClose(Abi::ObjectTypeMemoryObject, memory, systemCall) != PagerServeError::None) ||
        (DestroyAndClose(Abi::ObjectTypeFrame, frame, systemCall) != PagerServeError::None))
    {
        return PagerServeError::ManagementFailed;
    }
    return PagerServeError::None;
}

// A created pair mapped into this pager's own window, used from here, then unmapped and destroyed.
// Nothing touches the window after the unmap: a fault of this pager has no one to resolve it.
PagerServeError PagerProgram::TestMapping(Abi::SystemCallError& systemCall) noexcept
{
    CapabilityHandle frame{};
    CapabilityHandle memory{};
    if ((m_bootstrap.WindowPages < SelfTestMemoryPages) ||
        (CreatePair(frame, memory, systemCall) != PagerServeError::None) ||
        (MapWindow(m_bootstrap.OwnSpace, frame, memory, systemCall) != PagerServeError::None))
    {
        return PagerServeError::ManagementFailed;
    }
    constexpr Foundation::Size PageWords{PageBytes / sizeof(Foundation::UInt64)};
    volatile Foundation::UInt64* const words =
        reinterpret_cast<volatile Foundation::UInt64*>(static_cast<Foundation::UIntPointer>(m_bootstrap.WindowStart));
    bool used{true};
    for (Foundation::Size page{0U}; page < SelfTestMemoryPages; ++page)
    {
        volatile Foundation::UInt64* const first = words + (page * PageWords);
        used = used && (first[0U] == 0U) && (first[PageWords - 1U] == 0U);
        first[0U] = Abi::MappedPagePattern;
        first[1U] = page;
    }
    for (Foundation::Size page{0U}; page < SelfTestMemoryPages; ++page)
    {
        used = used && (words[page * PageWords] == Abi::MappedPagePattern) && (words[(page * PageWords) + 1U] == page);
    }
    if (!used)
    {
        return PagerServeError::ManagementFailed;
    }
    const Abi::SystemCallResponse unmapped =
        m_client.UnmapMemory(m_bootstrap.OwnSpace, m_bootstrap.WindowStart, SelfTestMemoryPages);
    if (unmapped.Error != Abi::SystemCallError::None)
    {
        systemCall = unmapped.Error;
        return PagerServeError::ManagementFailed;
    }
#if defined(ASTERIA_TARGET_IA64)
    for (Foundation::UInt64 cycle{0U}; cycle < LedgerReuseSelfTestCycles; ++cycle)
    {
        if ((MapWindow(m_bootstrap.OwnSpace, frame, memory, systemCall) != PagerServeError::None) ||
            (words[0U] != Abi::MappedPagePattern))
        {
            return PagerServeError::ManagementFailed;
        }
        const auto removed = m_client.UnmapMemory(m_bootstrap.OwnSpace, m_bootstrap.WindowStart, SelfTestMemoryPages);
        if (removed.Error != Abi::SystemCallError::None)
        {
            systemCall = removed.Error;
            return PagerServeError::ManagementFailed;
        }
    }
#endif
    if ((DestroyAndClose(Abi::ObjectTypeMemoryObject, memory, systemCall) != PagerServeError::None) ||
        (DestroyAndClose(Abi::ObjectTypeFrame, frame, systemCall) != PagerServeError::None))
    {
        return PagerServeError::ManagementFailed;
    }
    return PagerServeError::None;
}

PagerServeError PagerProgram::MapWindow(const CapabilityHandle space, const CapabilityHandle frame,
                                        const CapabilityHandle memory, Abi::SystemCallError& systemCall) noexcept
{
    for (Foundation::UInt8& byte : m_request)
    {
        byte = 0U;
    }
    StoreHandle(m_request + Abi::FaultResolveAddressSpaceOffset, space);
    StoreHandle(m_request + Abi::FaultResolveMemoryObjectOffset, memory);
    StoreHandle(m_request + Abi::FaultResolveFrameOffset, frame);
    StoreWord(m_request + Abi::FaultResolveVirtualStartOffset, m_bootstrap.WindowStart);
    StoreWord(m_request + Abi::FaultResolvePageCountOffset, SelfTestMemoryPages);
    m_request[Abi::FaultResolvePermissionsOffset] = SelfTestMemoryPermissions;
    const Abi::SystemCallResponse mapped = m_client.MapMemory(AddressOf(m_request));
    if (mapped.Error != Abi::SystemCallError::None)
    {
        systemCall = mapped.Error;
        return PagerServeError::ManagementFailed;
    }
    return PagerServeError::None;
}

// A whole domain, never run: its space gets a mapping it loses again while no processor is in it,
// and the domain, then its spaces, are destroyed, so the destroying calls reclaim every slot.
PagerServeError PagerProgram::TestDomain(const Foundation::UInt64 tag, Abi::SystemCallError& systemCall) noexcept
{
    CapabilityHandle space{};
    CapabilityHandle capabilities{};
    CapabilityHandle domain{};
    for (Foundation::UInt8& byte : m_request)
    {
        byte = 0U;
    }
    if ((CreateFromRequest(Abi::ObjectTypeAddressSpace, Abi::ObjectCreateAddressSpaceRequestSizeBytes, space,
                           systemCall) != PagerServeError::None) ||
        (CreateFromRequest(Abi::ObjectTypeCapabilitySpace, Abi::ObjectCreateCapabilitySpaceRequestSizeBytes,
                           capabilities, systemCall) != PagerServeError::None))
    {
        return PagerServeError::ManagementFailed;
    }
    StoreHandle(m_request + Abi::ObjectCreateDomainAddressSpaceOffset, space);
    StoreHandle(m_request + Abi::ObjectCreateDomainCapabilitySpaceOffset, capabilities);
    StoreWord(m_request + Abi::ObjectCreateDomainSupervisionTagOffset, tag);
    CapabilityHandle frame{};
    CapabilityHandle memory{};
    if ((CreateFromRequest(Abi::ObjectTypeProtectionDomain, Abi::ObjectCreateProtectionDomainRequestSizeBytes, domain,
                           systemCall) != PagerServeError::None) ||
        (CreatePair(frame, memory, systemCall) != PagerServeError::None) ||
        (MapWindow(space, frame, memory, systemCall) != PagerServeError::None))
    {
        return PagerServeError::ManagementFailed;
    }
    const Abi::SystemCallResponse unmapped = m_client.UnmapMemory(space, m_bootstrap.WindowStart, SelfTestMemoryPages);
    if (unmapped.Error != Abi::SystemCallError::None)
    {
        systemCall = unmapped.Error;
        return PagerServeError::ManagementFailed;
    }
    return ((DestroyAndClose(Abi::ObjectTypeMemoryObject, memory, systemCall) == PagerServeError::None) &&
            (DestroyAndClose(Abi::ObjectTypeFrame, frame, systemCall) == PagerServeError::None) &&
            (DestroyAndClose(Abi::ObjectTypeProtectionDomain, domain, systemCall) == PagerServeError::None) &&
            (DestroyAndClose(Abi::ObjectTypeCapabilitySpace, capabilities, systemCall) == PagerServeError::None) &&
            (DestroyAndClose(Abi::ObjectTypeAddressSpace, space, systemCall) == PagerServeError::None))
               ? PagerServeError::None
               : PagerServeError::ManagementFailed;
}

PagerServeError PagerProgram::CreateFromRequest(const Foundation::UInt64 type, const Foundation::Size bytes,
                                                CapabilityHandle& handle, Abi::SystemCallError& systemCall) noexcept
{
    const Abi::SystemCallResponse created =
        m_client.CreateObject(m_bootstrap.Management, type, AddressOf(m_request), bytes);
    if (created.Error != Abi::SystemCallError::None)
    {
        systemCall = created.Error;
        return PagerServeError::ManagementFailed;
    }
    handle = CapabilityHandle{created.Value0, created.Value1};
    return PagerServeError::None;
}

PagerServeError PagerProgram::CreatePair(CapabilityHandle& frame, CapabilityHandle& memory,
                                         Abi::SystemCallError& systemCall) noexcept
{
    for (Foundation::UInt8& byte : m_memoryRequest)
    {
        byte = 0U;
    }
    StoreWord(m_memoryRequest + Abi::ObjectCreatePagesOffset, SelfTestMemoryPages);
    if (CreateMemory(Abi::ObjectTypeFrame, frame, systemCall) != PagerServeError::None)
    {
        return PagerServeError::ManagementFailed;
    }
    m_memoryRequest[Abi::ObjectCreatePermissionsOffset] = SelfTestMemoryPermissions;
    return CreateMemory(Abi::ObjectTypeMemoryObject, memory, systemCall);
}

PagerServeError PagerProgram::CreateMemory(const Foundation::UInt64 type, CapabilityHandle& handle,
                                           Abi::SystemCallError& systemCall) noexcept
{
    const Abi::SystemCallResponse created =
        m_client.CreateObject(m_bootstrap.Management, type, AddressOf(m_memoryRequest), sizeof(m_memoryRequest));
    if (created.Error != Abi::SystemCallError::None)
    {
        systemCall = created.Error;
        return PagerServeError::ManagementFailed;
    }
    handle = CapabilityHandle{created.Value0, created.Value1};
    return PagerServeError::None;
}

PagerServeError PagerProgram::DestroyAndClose(const Foundation::UInt64 type, const CapabilityHandle handle,
                                              Abi::SystemCallError& systemCall) noexcept
{
    const Abi::SystemCallResponse destroyed = m_client.DestroyObject(handle, type);
    const Abi::SystemCallResponse stale = m_client.QueryCapability(handle);
    const Abi::SystemCallResponse closed = m_client.CloseCapability(handle);
    if (destroyed.Error != Abi::SystemCallError::None)
    {
        systemCall = destroyed.Error;
        return PagerServeError::ManagementFailed;
    }
    if ((stale.Error != Abi::SystemCallError::ObjectNotFound) || (closed.Error != Abi::SystemCallError::None))
    {
        systemCall = (closed.Error != Abi::SystemCallError::None) ? closed.Error : stale.Error;
        return PagerServeError::ManagementFailed;
    }
    return PagerServeError::None;
}

// A destroyed Endpoint no longer resolves (ObjectNotFound) and its handle
// stays closeable.
PagerServeError PagerProgram::TestEndpoint(const bool loopback, Abi::SystemCallError& systemCall) noexcept
{
    const Abi::SystemCallResponse created =
        m_client.CreateEndpoint(m_bootstrap.Management, AddressOf(m_endpointRequest));
    if (created.Error != Abi::SystemCallError::None)
    {
        systemCall = created.Error;
        return PagerServeError::ManagementFailed;
    }
    const CapabilityHandle endpoint{created.Value0, created.Value1};
    if (loopback && (LoopBack(endpoint, systemCall) != PagerServeError::None))
    {
        return PagerServeError::ManagementFailed;
    }
    const Abi::SystemCallResponse destroyed = m_client.DestroyObject(endpoint, Abi::ObjectTypeEndpoint);
    const Abi::SystemCallResponse stale = m_client.Send(endpoint, AddressOf(m_request), sizeof(LoopbackFrame));
    const Abi::SystemCallResponse closed = m_client.CloseCapability(endpoint);
    if (destroyed.Error != Abi::SystemCallError::None)
    {
        systemCall = destroyed.Error;
        return PagerServeError::ManagementFailed;
    }
    if ((stale.Error != Abi::SystemCallError::ObjectNotFound) || (closed.Error != Abi::SystemCallError::None))
    {
        systemCall = (closed.Error != Abi::SystemCallError::None) ? closed.Error : stale.Error;
        return PagerServeError::ManagementFailed;
    }
    return PagerServeError::None;
}

// Sends the canonical frame to the created Endpoint, whose receiver is this
// pager's space, and receives it back unchanged.
PagerServeError PagerProgram::LoopBack(const CapabilityHandle endpoint, Abi::SystemCallError& systemCall) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(LoopbackFrame); ++index)
    {
        m_request[index] = LoopbackFrame[index];
    }
    const Abi::SystemCallResponse sent = m_client.Send(endpoint, AddressOf(m_request), sizeof(LoopbackFrame));
    if ((sent.Error != Abi::SystemCallError::None) || (sent.Value0 != LoopbackSequence))
    {
        systemCall = sent.Error;
        return PagerServeError::ManagementFailed;
    }
    const Abi::SystemCallResponse received = m_client.Receive(endpoint, AddressOf(m_delivery), sizeof(m_delivery));
    bool equal{(received.Error == Abi::SystemCallError::None) && (received.Value0 == LoopbackSequence) &&
               (received.Value1 == (DeliveryHeaderBytes + sizeof(LoopbackFrame)))};
    for (Foundation::Size index{0U}; equal && (index < sizeof(LoopbackFrame)); ++index)
    {
        equal = m_delivery[DeliveryHeaderBytes + index] == LoopbackFrame[index];
    }
    if (!equal)
    {
        systemCall = received.Error;
        return PagerServeError::ManagementFailed;
    }
    return PagerServeError::None;
}

// The token is retired once resolved; its handle remains for Close.
PagerServeError PagerProgram::Close(const CapabilityHandle token, Abi::SystemCallError& systemCall) noexcept
{
    const Abi::SystemCallResponse closed = m_client.CloseCapability(token);
    if (closed.Error != Abi::SystemCallError::None)
    {
        systemCall = closed.Error;
        return PagerServeError::CloseFailed;
    }
    return PagerServeError::None;
}

namespace
{
// The bail-out line names the PagerServeError and the system-call error as
// two hex bytes each.
constexpr char StopLine[] = "Bail out! pager stopped serving faults: error 0x?? call 0x??\n";
constexpr Foundation::Size StopErrorOffset{sizeof("Bail out! pager stopped serving faults: error 0x") - 1U};
constexpr Foundation::Size StopCallOffset{StopErrorOffset + sizeof("?? call 0x") - 1U};
// A root service's failure ends its scenarios while the pager keeps serving
// faults; the diagnostic reports the same two error bytes.
constexpr char RootStopLine[] = "Bail out! root service stopped serving: error 0x?? call 0x??\n";
constexpr Foundation::Size RootStopErrorOffset{sizeof("Bail out! root service stopped serving: error 0x") - 1U};
constexpr Foundation::Size RootStopCallOffset{RootStopErrorOffset + sizeof("?? call 0x") - 1U};

void StoreHexByte(char* const digits, const Foundation::UInt64 value) noexcept
{
    constexpr char Alphabet[] = "0123456789abcdef";
    digits[0U] = Alphabet[(value >> 4U) & 0xFU];
    digits[1U] = Alphabet[value & 0xFU];
}

void ReportStop(const System::SystemCallClient& client, const CapabilityHandle diagnostic,
                const PagerServeOutcome& outcome) noexcept
{
    char line[sizeof(StopLine)]{};
    for (Foundation::Size index{0U}; index < sizeof(StopLine); ++index)
    {
        line[index] = StopLine[index];
    }
    StoreHexByte(line + StopErrorOffset, static_cast<Foundation::UInt64>(outcome.Error));
    StoreHexByte(line + StopCallOffset, static_cast<Foundation::UInt64>(outcome.SystemCall));
    static_cast<void>(client.DebugWrite(diagnostic, AddressOf(reinterpret_cast<const Foundation::UInt8*>(line)),
                                        sizeof(StopLine) - 1U));
}

void ReportRootStop(const System::SystemCallClient& client, const CapabilityHandle diagnostic,
                    const PagerServeOutcome& outcome) noexcept
{
    char line[sizeof(RootStopLine)]{};
    for (Foundation::Size index{0U}; index < sizeof(RootStopLine); ++index)
    {
        line[index] = RootStopLine[index];
    }
    StoreHexByte(line + RootStopErrorOffset, static_cast<Foundation::UInt64>(outcome.Error));
    StoreHexByte(line + RootStopCallOffset, static_cast<Foundation::UInt64>(outcome.SystemCall));
    static_cast<void>(client.DebugWrite(diagnostic, AddressOf(reinterpret_cast<const Foundation::UInt8*>(line)),
                                        sizeof(RootStopLine) - 1U));
}
} // Namespace

void RunPager(const System::SystemCallTransport transport, const Foundation::UInt64 (&words)[Abi::PagerBootstrapWords],
              const PagerThreadLaunch* const threadLaunch, const PagerRootService rootService) noexcept
{
    static constinit System::SystemCallClient client{System::SystemCallTransport{nullptr, nullptr}};
    static constinit PagerBootstrap bootstrap{};
    client = System::SystemCallClient{transport};
    if (!DecodePagerBootstrap(words, bootstrap))
    {
        return;
    }
    static constinit PagerProgram program{client, bootstrap};
    static constinit PagerThreadLaunch launch{};
    const PagerThreadLaunch* activeLaunch{nullptr};
    if (threadLaunch != nullptr)
    {
        launch = *threadLaunch;
        launch.ComponentImages = ModuleTableBytes(bootstrap.ModuleBase);
        launch.ComponentImagesBytes = bootstrap.ModuleBytes;
        activeLaunch = &launch;
    }
    program.SetThreadLaunch(activeLaunch);
    const PagerServeOutcome tested = program.TestManagement();
    if (tested.Error != PagerServeError::None)
    {
        ReportStop(client, bootstrap.Diagnostic, tested);
        return;
    }
    // As root service, it builds, supervises and restarts the component its module table names
    // before it serves only the client. A root service that fails is reported and ends, but the
    // pager goes on serving faults: a component it left running still needs them.
    if ((rootService != nullptr) && (activeLaunch != nullptr))
    {
        const PagerServeOutcome rooted = rootService(client, bootstrap, program, *activeLaunch);
        if (rooted.Error != PagerServeError::None)
        {
            ReportRootStop(client, bootstrap.Diagnostic, rooted);
        }
    }
    for (;;)
    {
        const PagerServeOutcome outcome = program.ServeOnce();
        if (outcome.Error != PagerServeError::None)
        {
            ReportStop(client, bootstrap.Diagnostic, outcome);
            return;
        }
    }
}
Foundation::UInt64 RunWaitingThread(const System::SystemCallTransport transport,
                                    PagerWaitingThreadControl& control) noexcept
{
    const System::SystemCallClient client{transport};
    AtomicUInt64Reference{control.Waiting}.StoreRelease(1U);
    const Abi::SystemCallResponse waited = client.Wait(control.WaitSet, control.TimeoutMicroseconds);
    AtomicUInt64Reference{control.Result}.StoreRelease(static_cast<Foundation::UInt64>(waited.Error));
    return (waited.Error == Abi::SystemCallError::TimedOut) ? PagerWaitingThreadExitCode
                                                            : PagerWaitingThreadWokenExitCode;
}

Foundation::UInt64 RunPagerThread(const System::SystemCallTransport transport, PagerThreadControl& control,
                                  const PagerThreadStateCheck stateIntact, const PagerThreadClock clock) noexcept
{
    const Foundation::UInt64 seen = AtomicUInt64Reference::LoadAcquire(control.Polls);
    const Foundation::UInt64 start = clock();
    bool preempted{false};
    while (!preempted && ((Abi::ElapsedClockTicks(clock(), start)) < control.SpinLimit))
    {
        preempted = AtomicUInt64Reference::LoadAcquire(control.Polls) != seen;
    }
    const bool intact = stateIntact();
    const System::SystemCallClient client{transport};
    const char* const line =
        !preempted ? PagerThreadTimeoutLine : (intact ? PagerThreadDoneLine : PagerThreadStateLostLine);
    Foundation::Size length{0U};
    while (line[length] != '\0')
    {
        ++length;
    }
    static_cast<void>(
        client.DebugWrite(control.Diagnostic, AddressOf(reinterpret_cast<const Foundation::UInt8*>(line)), length));
    return !preempted ? PagerThreadTimeoutExitCode : (intact ? PagerThreadExitCode : PagerThreadStateLostExitCode);
}
} // Namespace Asteria::User::Pager
