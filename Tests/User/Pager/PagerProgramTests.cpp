// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "Kernel/Ipc/Endpoint.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "User/Pager/PagerProgram.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Foundation::UInt8;
using Platform::Synchronization::AtomicUInt64Reference;
using User::Pager::PagerBootstrap;
using User::Pager::PagerProgram;
using User::Pager::PagerServeError;
using User::Pager::PagerServeOutcome;
using User::Pager::PagerThreadControl;
using User::Pager::PagerThreadExitCode;
using User::Pager::PagerThreadLaunch;
using User::System::CapabilityHandle;
using User::System::SystemCallClient;
using User::System::SystemCallTransport;

constexpr UInt64 LazyStart{0x7000'0000U};

void StoreWord(UInt8* const bytes, const UInt64 value) noexcept
{
    for (Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (8U * index));
    }
}

[[nodiscard]] UInt64 LoadWord(const UInt8* const bytes) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (8U * index);
    }
    return value;
}

/// Answers Wait with the readiness tag, then one Receive per scripted fault
/// (writing a fault delivery), then WouldBlock; records FaultResolve and Close.
class ScriptedKernel final
{
public:
    static void Transport(void* const context, const Abi::SystemCallRequest* const request,
                          Abi::SystemCallResponse* const response) noexcept
    {
        static_cast<ScriptedKernel*>(context)->Handle(*request, *response);
    }

    void Handle(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        response = Abi::SystemCallResponse{.Error = Abi::SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        switch (static_cast<Abi::SystemCallNumber>(request.Number))
        {
        case Abi::SystemCallNumber::WaitSetWait:
            ++Waits;
            LastWaitTimeout = request.Arguments[2U];
            response.Value0 = ReadinessTag;
            response.Value1 = 1U;
            return;
        case Abi::SystemCallNumber::ObjectCreate:
        {
            const UInt64 type = request.Arguments[2U] & 0xFU;
            if (type == Abi::ObjectTypeThread)
            {
                LastThreadCreate = request;
                std::memcpy(ThreadBytes[ThreadCreates % 8U], reinterpret_cast<const UInt8*>(request.Arguments[3U]),
                            sizeof(ThreadBytes[0U]));
                // The spinning thread counts once before the pager runs again.
                if ((SpinEntry != 0U) &&
                    (LoadWord(ThreadBytes[ThreadCreates % 8U] + Abi::ObjectCreateThreadEntryOffset) == SpinEntry))
                {
                    auto* const spins = reinterpret_cast<UInt64*>(static_cast<Foundation::UIntPointer>(
                        LoadWord(ThreadBytes[ThreadCreates % 8U] + Abi::ObjectCreateThreadArgumentOffset)));
                    AtomicUInt64Reference{*spins}.IncrementRelease();
                }
                if ((WaitEntry != 0U) &&
                    (LoadWord(ThreadBytes[ThreadCreates % 8U] + Abi::ObjectCreateThreadEntryOffset) == WaitEntry))
                {
                    auto* const control =
                        reinterpret_cast<User::Pager::PagerWaitingThreadControl*>(static_cast<Foundation::UIntPointer>(
                            LoadWord(ThreadBytes[ThreadCreates % 8U] + Abi::ObjectCreateThreadArgumentOffset)));
                    AtomicUInt64Reference{control->Waiting}.StoreRelease(1U);
                    AtomicUInt64Reference{control->Result}.StoreRelease(static_cast<UInt64>(WaitResult));
                }
                ++ThreadCreates;
                response.Error = ThreadCreateError;
                response.Value0 = 0x100U + type;
                response.Value1 = 0x200U + type;
                LiveByType[type] = ThreadCreateError == Abi::SystemCallError::None;
                return;
            }
            if (type != Abi::ObjectTypeEndpoint)
            {
                ++CreatesByType[type];
                if (type == Abi::ObjectTypeProtectionDomain)
                {
                    std::memcpy(DomainBytes, reinterpret_cast<const UInt8*>(request.Arguments[3U]),
                                sizeof(DomainBytes));
                }
                ++MemoryCreates;
                std::memcpy(LastMemoryBytes[type & 1U], reinterpret_cast<const UInt8*>(request.Arguments[3U]),
                            sizeof(LastMemoryBytes[0U]));
                response.Error = MemoryCreateError;
                response.Value0 = 0x100U + type;
                response.Value1 = 0x200U + type;
                LiveByType[type] = MemoryCreateError == Abi::SystemCallError::None;
                return;
            }
            ++Creates;
            std::memcpy(LastCreateBytes, reinterpret_cast<const UInt8*>(request.Arguments[3U]),
                        sizeof(LastCreateBytes));
            LastCreate = request;
            response.Error = CreateError;
            response.Value0 = (CreateError == Abi::SystemCallError::None) ? CreatedSelector : 0U;
            response.Value1 = (CreateError == Abi::SystemCallError::None) ? CreatedSelector + Creates : 0U;
            Live = CreateError == Abi::SystemCallError::None;
            return;
        }
        case Abi::SystemCallNumber::ObjectDestroy:
            ++Destroys;
            if (request.Arguments[2U] == Abi::ObjectTypeThread)
            {
                // The thread created last.
                const Size thread = (ThreadCreates - 1U) % 8U;
                ++ThreadDestroys;
                if (ThreadBusyPolls[thread] > 0U)
                {
                    --ThreadBusyPolls[thread];
                    response.Error = Abi::SystemCallError::WouldBlock;
                    return;
                }
                response.Value0 = ThreadExitCodes[thread];
                LiveByType[Abi::ObjectTypeThread] = false;
                return;
            }
            if (request.Arguments[2U] != Abi::ObjectTypeEndpoint)
            {
                DestroySequence[MemoryDestroys % 16U] = request.Arguments[2U];
                DestroyOrder[MemoryDestroys++ & 1U] = request.Arguments[2U];
                LiveByType[request.Arguments[2U] & 0xFU] = false;
                return;
            }
            LastDestroy = request;
            Live = false;
            return;
        case Abi::SystemCallNumber::ThreadTerminate:
            ++Kills;
            LastKill = request;
            response.Error = KillError;
            return;
        case Abi::SystemCallNumber::CapabilityQuery:
            ++Queries;
            if ((request.Arguments[0U] >= 0x100U) && !LiveByType[request.Arguments[0U] & 0xFU])
            {
                response.Error = Abi::SystemCallError::ObjectNotFound;
            }
            return;
        case Abi::SystemCallNumber::IpcSend:
            ++Sends;
            if (!Live)
            {
                response.Error = Abi::SystemCallError::ObjectNotFound;
                return;
            }
            std::memcpy(Looped, reinterpret_cast<const UInt8*>(request.Arguments[2U]), sizeof(Looped));
            response.Value0 = 1U;
            return;
        case Abi::SystemCallNumber::IpcReceive:
            if (request.Arguments[0U] == CreatedSelector)
            {
                std::memset(reinterpret_cast<UInt8*>(request.Arguments[2U]), 0, 48U);
                std::memcpy(reinterpret_cast<UInt8*>(request.Arguments[2U]) + 48U, Looped, sizeof(Looped));
                reinterpret_cast<UInt8*>(request.Arguments[2U])[48U] ^= CorruptLoopback ? 1U : 0U;
                response.Value0 = 1U;
                response.Value1 = 48U + sizeof(Looped);
                return;
            }
            if (Delivered >= FaultCount)
            {
                response.Error = Abi::SystemCallError::WouldBlock;
                return;
            }
            WriteDelivery(reinterpret_cast<UInt8*>(request.Arguments[2U]), Faults[Delivered]);
            ++Delivered;
            response.Value0 = Delivered;
            response.Value1 = DeliverySize;
            return;
        case Abi::SystemCallNumber::FaultResolve:
            LastResolve = request;
            if (request.Arguments[2U] == static_cast<UInt64>(Abi::FaultResolveKind::Terminate))
            {
                ++Terminates;
                response.Error = TerminateError;
                response.Value0 = (TerminateError == Abi::SystemCallError::None) ? Abi::FaultTerminatedExitCode : 0U;
                return;
            }
            std::memcpy(LastRequestBytes, reinterpret_cast<const UInt8*>(request.Arguments[3U]),
                        sizeof(LastRequestBytes));
            ++Resolves;
            response.Error = ResolveError;
            return;
        case Abi::SystemCallNumber::CapabilityClose:
            LastClosed = {request.Arguments[0U], request.Arguments[1U]};
            ++Closes;
            return;
        case Abi::SystemCallNumber::MemoryUnmap:
            ++Unmaps;
            LastUnmap = request;
            response.Error = UnmapError;
            return;
        case Abi::SystemCallNumber::DebugWrite:
            if (Lines == 0U)
            {
                std::memcpy(FirstLine, reinterpret_cast<const char*>(request.Arguments[2U]),
                            request.Arguments[3U] < sizeof(FirstLine) ? request.Arguments[3U] : sizeof(FirstLine) - 1U);
            }
            ++Lines;
            std::memset(LastLine, 0, sizeof(LastLine));
            std::memcpy(LastLine, reinterpret_cast<const char*>(request.Arguments[2U]),
                        request.Arguments[3U] < sizeof(LastLine) ? request.Arguments[3U] : sizeof(LastLine) - 1U);
            return;
        case Abi::SystemCallNumber::MemoryMap:
            ++Maps;
            LastMap = request;
            std::memcpy(LastMapBytes, reinterpret_cast<const UInt8*>(request.Arguments[3U]), sizeof(LastMapBytes));
            response.Error = MapError;
            return;
        default:
            ++Other;
            return;
        }
    }

    void WriteDelivery(UInt8* const delivery, const UInt64 faultAddress) const noexcept
    {
        std::memset(delivery, 0, DeliverySize);
        std::memcpy(delivery + 48U, "AIPC", 4U);
        delivery[48U + 12U] = 48U;
        delivery[48U + 36U] = 1U;
        delivery[48U + 38U] = 2U;
        delivery[48U + 81U] = 1U;
        StoreWord(delivery + 136U, 0x0F0FU);
        StoreWord(delivery + 144U, 0x7A7AU);
        StoreWord(delivery + 168U, faultAddress);
        delivery[192U] = Access;
        delivery[193U] = Cause;
        StoreWord(delivery + 200U, Tag);
        StoreWord(delivery + 208U, 0xA001U);
        if (CorruptMagic)
        {
            delivery[48U] = 'X';
        }
    }

    UInt64 ReadinessTag{Abi::PagerReadinessTag};
    UInt64 Faults[4U]{};
    Size FaultCount{0U};
    Size Delivered{0U};
    UInt64 DeliverySize{216U};
    // The supervision tag every delivered fault carries (pager protocol 1.2).
    UInt64 Tag{0U};
    // The record's access and cause: a missing translation of a read.
    UInt8 Access{1U};
    UInt8 Cause{1U};
    bool CorruptMagic{false};
    Abi::SystemCallError ResolveError{Abi::SystemCallError::None};
    Abi::SystemCallError TerminateError{Abi::SystemCallError::None};
    Size Waits{0U};
    UInt64 LastWaitTimeout{0U};
    Size Resolves{0U};
    Size Terminates{0U};
    Size Closes{0U};
    Size Other{0U};
    Abi::SystemCallRequest LastResolve{};
    UInt8 LastRequestBytes[Abi::FaultResolveRequestSizeBytes]{};
    CapabilityHandle LastClosed{};
    // The management self-test: created Endpoints answer with this selector.
    static constexpr UInt64 CreatedSelector{0x77U};
    Abi::SystemCallError CreateError{Abi::SystemCallError::None};
    bool CorruptLoopback{false};
    bool Live{false};
    Size Creates{0U};
    Size Destroys{0U};
    Size Sends{0U};
    Abi::SystemCallRequest LastCreate{};
    Abi::SystemCallRequest LastDestroy{};
    UInt8 LastCreateBytes[Abi::ObjectCreateEndpointRequestSizeBytes]{};
    UInt8 Looped[88U]{};
    // Frames and MemoryObjects: created handles are 0x100 + type.
    Abi::SystemCallError MemoryCreateError{Abi::SystemCallError::None};
    bool LiveByType[16U]{};
    Size MemoryCreates{0U};
    Size MemoryDestroys{0U};
    Size Queries{0U};
    UInt64 DestroyOrder[2U]{};
    // Every type's creations, the last ProtectionDomain request, and the
    // types destroyed in order (the last 16).
    Size CreatesByType[16U]{};
    UInt8 DomainBytes[Abi::ObjectCreateProtectionDomainRequestSizeBytes]{};
    UInt64 DestroySequence[16U]{};
    // Index 0 the Frame's request (type 8), 1 the MemoryObject's (type 7).
    UInt8 LastMemoryBytes[2U][Abi::ObjectCreateMemoryObjectRequestSizeBytes]{};
    // MemoryMap: the window is the test's own memory, which the call leaves
    // as it is.
    Abi::SystemCallError MapError{Abi::SystemCallError::None};
    Abi::SystemCallError UnmapError{Abi::SystemCallError::None};
    Size Maps{0U};
    Size Unmaps{0U};
    Abi::SystemCallRequest LastUnmap{};
    Abi::SystemCallRequest LastMap{};
    UInt8 LastMapBytes[Abi::FaultResolveRequestSizeBytes]{};
    // Threads: a created thread's handle is 0x104; ObjectDestroy refuses the
    // first (second, third, fourth) thread ThreadBusyPolls[0] ([1], [2], [3])
    // times, then returns its exit code. A thread created at SpinEntry counts
    // once in the word its argument names; ThreadTerminate answers KillError.
    Abi::SystemCallError ThreadCreateError{Abi::SystemCallError::None};
    Size ThreadBusyPolls[8U]{};
    UInt64 ThreadExitCodes[8U]{PagerThreadExitCode,
                               Abi::FaultTerminatedExitCode,
                               Abi::FaultTerminatedExitCode,
                               Abi::KilledExitCode,
                               User::Pager::PagerWaitingThreadExitCode,
                               Abi::KilledExitCode};
    Size ThreadCreates{0U};
    Size ThreadDestroys{0U};
    Abi::SystemCallRequest LastThreadCreate{};
    UInt8 ThreadBytes[8U][Abi::ObjectCreateThreadRequestSizeBytes]{};
    // A thread created at WaitEntry "waits": its control block receives
    // WaitResult.
    UInt64 WaitEntry{0U};
    Abi::SystemCallError WaitResult{Abi::SystemCallError::TimedOut};
    UInt64 SpinEntry{0U};
    Abi::SystemCallError KillError{Abi::SystemCallError::None};
    Size Kills{0U};
    Abi::SystemCallRequest LastKill{};
    // DebugWrite: the first and the last line written.
    Size Lines{0U};
    char FirstLine[72U]{};
    char LastLine[64U]{};
};

[[nodiscard]] PagerBootstrap Bootstrap() noexcept
{
    return PagerBootstrap{
        .Endpoint = {0x11U, 0x1111U},
        .WaitSet = {0x22U, 0x2222U},
        .Diagnostic = {0x33U, 0x3333U},
        .ClientSpace = {0x44U, 0x4444U},
        .LazyMemory = {0x55U, 0x5555U},
        .LazyFrame = {0x66U, 0x6666U},
        .LazyStart = LazyStart,
        .LazyPages = 2U,
        .Management = {},
        .ManagementFeatures = 0U,
        .OwnSpace = {},
        .WindowStart = 0U,
        .WindowPages = 0U,
        .Devices = {},
        .Scanout = {},
    };
}

void TestServesAFault(TestContext& runner) noexcept
{
    ScriptedKernel kernel;
    kernel.Faults[0U] = LazyStart + 0x1000U + 0x234U;
    kernel.FaultCount = 1U;
    const SystemCallClient client{SystemCallTransport{&ScriptedKernel::Transport, &kernel}};
    const PagerBootstrap bootstrap = Bootstrap();
    PagerProgram program{client, bootstrap};
    const PagerServeOutcome outcome = program.ServeOnce();
    const UInt8* const bytes = kernel.LastRequestBytes;
    ASTERIA_EXPECT(
        runner, "the pager maps the faulting lazy page from the matching frame page",
        outcome.Error == PagerServeError::None, outcome.Resolved == 1U, kernel.Waits == 1U, kernel.Resolves == 1U,
        kernel.Closes == 1U, kernel.Other == 0U, kernel.LastResolve.Arguments[0U] == 0x0F0FU,
        kernel.LastResolve.Arguments[1U] == 0x7A7AU, kernel.LastResolve.Arguments[2U] == 1U,
        kernel.LastResolve.Arguments[4U] == Abi::FaultResolveRequestSizeBytes,
        LoadWord(bytes + Abi::FaultResolveAddressSpaceOffset) == 0x44U,
        LoadWord(bytes + Abi::FaultResolveAddressSpaceOffset + 8U) == 0x4444U,
        LoadWord(bytes + Abi::FaultResolveMemoryObjectOffset) == 0x55U,
        LoadWord(bytes + Abi::FaultResolveFrameOffset + 8U) == 0x6666U,
        LoadWord(bytes + Abi::FaultResolveVirtualStartOffset) == LazyStart + 0x1000U,
        LoadWord(bytes + Abi::FaultResolvePageCountOffset) == 1U,
        LoadWord(bytes + Abi::FaultResolveMemoryObjectPageOffset) == 1U,
        LoadWord(bytes + Abi::FaultResolveFramePageOffset) == 1U, bytes[Abi::FaultResolvePermissionsOffset] == 0x0BU,
        kernel.LastClosed.Selector == 0x0F0FU, kernel.LastClosed.Authenticator == 0x7A7AU);
}

// A fault outside the region ends the faulting thread, and the pager keeps serving the faults after
// it.
void TestTerminatesFaultsOutsideTheRegion(TestContext& runner) noexcept
{
    ScriptedKernel kernel;
    kernel.Faults[0U] = 0U;
    kernel.Faults[1U] = LazyStart + 0x1000U;
    kernel.FaultCount = 2U;
    const SystemCallClient client{SystemCallTransport{&ScriptedKernel::Transport, &kernel}};
    const PagerBootstrap bootstrap = Bootstrap();
    PagerProgram program{client, bootstrap};
    const PagerServeOutcome outcome = program.ServeOnce();

    ScriptedKernel refused;
    refused.Faults[0U] = LazyStart + 0x2000U;
    refused.FaultCount = 1U;
    refused.TerminateError = Abi::SystemCallError::ObjectNotFound;
    const SystemCallClient refusedClient{SystemCallTransport{&ScriptedKernel::Transport, &refused}};
    PagerProgram refusedProgram{refusedClient, bootstrap};
    const PagerServeOutcome refusedOutcome = refusedProgram.ServeOnce();
    ASTERIA_EXPECT(runner, "a fault outside the region is terminated, its token closed, and serving goes on",
                   outcome.Error == PagerServeError::None, outcome.Resolved == 2U, kernel.Terminates == 1U,
                   kernel.Resolves == 1U, kernel.Closes == 2U, program.GetTerminated() == 1U,
                   refusedOutcome.Error == PagerServeError::TerminateFailed,
                   refusedOutcome.SystemCall == Abi::SystemCallError::ObjectNotFound, refused.Closes == 0U,
                   refusedProgram.GetTerminated() == 0U);
}

// A fault of a domain this pager created (a nonzero tag) is a crash, terminated even inside the
// lazy region it backs.
void TestTerminatesTaggedFaults(TestContext& runner) noexcept
{
    ScriptedKernel kernel;
    kernel.Faults[0U] = LazyStart + 0x10U;
    kernel.FaultCount = 1U;
    kernel.Tag = 2U;
    const SystemCallClient client{SystemCallTransport{&ScriptedKernel::Transport, &kernel}};
    const PagerBootstrap bootstrap = Bootstrap();
    PagerProgram program{client, bootstrap};
    const PagerServeOutcome outcome = program.ServeOnce();
    ASTERIA_EXPECT(runner, "a tagged fault inside the region is terminated, never mapped",
                   outcome.Error == PagerServeError::None, outcome.Resolved == 1U, kernel.Terminates == 1U,
                   kernel.Resolves == 0U, program.GetTerminated() == 1U, program.GetComponentCrashes() == 1U);
}

// A boot thread's fault in the lazy region that a lazy data page would not
// resolve (an exception, a protection fault, an instruction fetch) is
// terminated, never mapped; a missing translation of a write is mapped.
void TestTerminatesUnmappableFaultsInsideTheRegion(TestContext& runner) noexcept
{
    struct Case
    {
        UInt8 Access;
        UInt8 Cause;
        bool Mapped;
    };
    static constexpr Case cases[]{
        {1U, 4U, false}, // An exception at an instruction in the region
        {2U, 2U, false}, // A write to a mapped read-only page
        {3U, 1U, false}, // An instruction fetch from the data region
        {2U, 1U, true},  // A lazy page's first write
    };
    for (const Case& item : cases)
    {
        ScriptedKernel kernel;
        kernel.Faults[0U] = LazyStart + 0x10U;
        kernel.FaultCount = 1U;
        kernel.Access = item.Access;
        kernel.Cause = item.Cause;
        const SystemCallClient client{SystemCallTransport{&ScriptedKernel::Transport, &kernel}};
        const PagerBootstrap bootstrap = Bootstrap();
        PagerProgram program{client, bootstrap};
        const PagerServeOutcome outcome = program.ServeOnce();
        ASTERIA_EXPECT(runner, "a boot thread's fault in the region is mapped only if a lazy data page resolves it",
                       outcome.Error == PagerServeError::None, outcome.Resolved == 1U,
                       kernel.Terminates == (item.Mapped ? 0U : 1U), kernel.Resolves == (item.Mapped ? 1U : 0U),
                       program.GetTerminated() == (item.Mapped ? 0U : 1U), program.GetComponentCrashes() == 0U);
    }
}

void TestStopsOnRejectedFaults(TestContext& runner) noexcept
{
    const PagerBootstrap bootstrap = Bootstrap();

    ScriptedKernel corrupt;
    corrupt.Faults[0U] = LazyStart;
    corrupt.FaultCount = 1U;
    corrupt.CorruptMagic = true;
    const SystemCallClient corruptClient{SystemCallTransport{&ScriptedKernel::Transport, &corrupt}};
    PagerProgram corruptProgram{corruptClient, bootstrap};

    ScriptedKernel refused;
    refused.Faults[0U] = LazyStart;
    refused.FaultCount = 1U;
    refused.ResolveError = Abi::SystemCallError::RightsDenied;
    const SystemCallClient refusedClient{SystemCallTransport{&ScriptedKernel::Transport, &refused}};
    PagerProgram refusedProgram{refusedClient, bootstrap};

    ScriptedKernel stranger;
    stranger.ReadinessTag = 0x1234U;
    const SystemCallClient strangerClient{SystemCallTransport{&ScriptedKernel::Transport, &stranger}};
    PagerProgram strangerProgram{strangerClient, bootstrap};

    const PagerServeOutcome corruptOutcome = corruptProgram.ServeOnce();
    const PagerServeOutcome refusedOutcome = refusedProgram.ServeOnce();
    const PagerServeOutcome strangerOutcome = strangerProgram.ServeOnce();
    ASTERIA_EXPECT(runner, "a malformed message, a refused Map and a foreign wake stop the pager",
                   corruptOutcome.Error == PagerServeError::MalformedMessage, corrupt.Resolves == 0U,
                   refusedOutcome.Error == PagerServeError::ResolveFailed,
                   refusedOutcome.SystemCall == Abi::SystemCallError::RightsDenied, refused.Closes == 0U,
                   strangerOutcome.Error == PagerServeError::UnexpectedReadiness, stranger.Delivered == 0U);
}

// With a grant the pager creates, loops through, destroys and closes an Endpoint, then creates and
// destroys a second one; without one it makes no call.
void TestManagementSelfTest(TestContext& runner) noexcept
{
    ScriptedKernel kernel;
    const SystemCallClient client{SystemCallTransport{&ScriptedKernel::Transport, &kernel}};
    PagerBootstrap bootstrap = Bootstrap();
    bootstrap.Management = {0x99U, 0x9999U};
    PagerProgram program{client, bootstrap};
    const PagerServeOutcome outcome = program.TestManagement();
    const Kernel::Ipc::EndpointConfiguration expected = Kernel::Ipc::EndpointConfiguration::Default();
    const UInt8* const bytes = kernel.LastCreateBytes;
    bool zeroReserved{true};
    for (Size index{0U}; index < Abi::ObjectCreateEndpointQueuedBytesOffset; ++index)
    {
        zeroReserved = zeroReserved && (bytes[index] == 0U);
    }
    for (Size index{Abi::ObjectCreateEndpointReservedOffset}; index < sizeof(kernel.LastCreateBytes); ++index)
    {
        zeroReserved = zeroReserved && (bytes[index] == 0U);
    }
    ASTERIA_EXPECT(
        runner, "the self-test creates two Endpoints, loops one message, destroys and closes both",
        outcome.Error == PagerServeError::None, outcome.Resolved == 2U, kernel.Creates == 2U, kernel.Destroys == 2U,
        kernel.Sends == 3U, kernel.Closes == 2U, kernel.Other == 0U, kernel.Waits == 0U,
        kernel.LastCreate.Arguments[0U] == 0x99U, kernel.LastCreate.Arguments[1U] == 0x9999U,
        kernel.LastCreate.Arguments[2U] == Abi::ObjectTypeEndpoint,
        kernel.LastCreate.Arguments[4U] == Abi::ObjectCreateEndpointRequestSizeBytes,
        kernel.LastDestroy.Arguments[0U] == ScriptedKernel::CreatedSelector,
        kernel.LastDestroy.Arguments[2U] == Abi::ObjectTypeEndpoint,
        kernel.LastClosed.Selector == ScriptedKernel::CreatedSelector, zeroReserved,
        (bytes[Abi::ObjectCreateEndpointQueuedBytesOffset] |
         (UInt64{bytes[Abi::ObjectCreateEndpointQueuedBytesOffset + 1U]} << 8U)) == expected.MaximumQueuedBytes,
        (bytes[Abi::ObjectCreateEndpointReservedControlBytesOffset] |
         (UInt64{bytes[Abi::ObjectCreateEndpointReservedControlBytesOffset + 1U]} << 8U)) ==
            expected.ReservedControlBytes,
        (bytes[Abi::ObjectCreateEndpointBytesPerSenderOffset] |
         (UInt64{bytes[Abi::ObjectCreateEndpointBytesPerSenderOffset + 1U]} << 8U)) == expected.MaximumBytesPerSender,
        bytes[Abi::ObjectCreateEndpointQueuedMessagesOffset] == expected.MaximumQueuedMessages,
        bytes[Abi::ObjectCreateEndpointReservedControlMessagesOffset] == expected.ReservedControlMessages,
        bytes[Abi::ObjectCreateEndpointMessagesPerSenderOffset] == expected.MaximumMessagesPerSender,
        bytes[Abi::ObjectCreateEndpointQueuedAttachmentsOffset] == expected.MaximumQueuedAttachments,
        bytes[Abi::ObjectCreateEndpointReservedControlAttachmentsOffset] == expected.ReservedControlAttachments,
        bytes[Abi::ObjectCreateEndpointAttachmentsPerSenderOffset] == expected.MaximumAttachmentsPerSender);

    ScriptedKernel ungranted;
    const SystemCallClient ungrantedClient{SystemCallTransport{&ScriptedKernel::Transport, &ungranted}};
    const PagerBootstrap plain = Bootstrap();
    PagerProgram ungrantedProgram{ungrantedClient, plain};
    const PagerServeOutcome skipped = ungrantedProgram.TestManagement();

    ScriptedKernel exhausted;
    exhausted.CreateError = Abi::SystemCallError::ResourceExhausted;
    const SystemCallClient exhaustedClient{SystemCallTransport{&ScriptedKernel::Transport, &exhausted}};
    PagerProgram exhaustedProgram{exhaustedClient, bootstrap};
    const PagerServeOutcome refused = exhaustedProgram.TestManagement();

    ScriptedKernel corrupt;
    corrupt.CorruptLoopback = true;
    const SystemCallClient corruptClient{SystemCallTransport{&ScriptedKernel::Transport, &corrupt}};
    PagerProgram corruptProgram{corruptClient, bootstrap};
    const PagerServeOutcome mismatched = corruptProgram.TestManagement();
    ASTERIA_EXPECT(runner, "no grant makes no call, and a refused creation or a changed message fails the test",
                   skipped.Error == PagerServeError::None, skipped.Resolved == 0U, ungranted.Creates == 0U,
                   ungranted.Sends == 0U, ungranted.Other == 0U, refused.Error == PagerServeError::ManagementFailed,
                   refused.SystemCall == Abi::SystemCallError::ResourceExhausted, exhausted.Destroys == 0U,
                   mismatched.Error == PagerServeError::ManagementFailed,
                   mismatched.SystemCall == Abi::SystemCallError::None, corrupt.Destroys == 0U);
}

// With the frame pool, the self-test also creates a Frame and a MemoryObject and destroys both, the
// MemoryObject first.
void TestMemorySelfTest(TestContext& runner) noexcept
{
    ScriptedKernel kernel;
    const SystemCallClient client{SystemCallTransport{&ScriptedKernel::Transport, &kernel}};
    PagerBootstrap bootstrap = Bootstrap();
    bootstrap.Management = {0x99U, 0x9999U};
    bootstrap.ManagementFeatures = Abi::PagerManagementFrames;
    PagerProgram program{client, bootstrap};
    const PagerServeOutcome outcome = program.TestManagement();
    const UInt8* const frame = kernel.LastMemoryBytes[0U];
    const UInt8* const memory = kernel.LastMemoryBytes[1U];
    ASTERIA_EXPECT(
        runner, "the self-test creates and destroys a Frame and a MemoryObject", outcome.Error == PagerServeError::None,
        outcome.Resolved == 4U, kernel.Creates == 2U, kernel.MemoryCreates == 2U, kernel.MemoryDestroys == 2U,
        kernel.DestroyOrder[0U] == Abi::ObjectTypeMemoryObject, kernel.DestroyOrder[1U] == Abi::ObjectTypeFrame,
        kernel.Queries == 2U, kernel.Closes == 4U, kernel.Other == 0U,
        LoadWord(frame + Abi::ObjectCreatePagesOffset) == 2U, frame[Abi::ObjectCreatePermissionsOffset] == 0U,
        LoadWord(memory + Abi::ObjectCreatePagesOffset) == 2U, memory[Abi::ObjectCreatePermissionsOffset] == 0x0BU);

    ScriptedKernel exhausted;
    exhausted.MemoryCreateError = Abi::SystemCallError::ResourceExhausted;
    const SystemCallClient exhaustedClient{SystemCallTransport{&ScriptedKernel::Transport, &exhausted}};
    PagerProgram exhaustedProgram{exhaustedClient, bootstrap};
    const PagerServeOutcome refused = exhaustedProgram.TestManagement();

    ScriptedKernel withoutPool;
    const SystemCallClient withoutPoolClient{SystemCallTransport{&ScriptedKernel::Transport, &withoutPool}};
    PagerBootstrap endpointsOnly = bootstrap;
    endpointsOnly.ManagementFeatures = 0U;
    PagerProgram withoutPoolProgram{withoutPoolClient, endpointsOnly};
    const PagerServeOutcome skipped = withoutPoolProgram.TestManagement();
    ASTERIA_EXPECT(runner, "a refused Frame fails the test, and without the frame pool none is created",
                   refused.Error == PagerServeError::ManagementFailed,
                   refused.SystemCall == Abi::SystemCallError::ResourceExhausted, exhausted.MemoryDestroys == 0U,
                   skipped.Error == PagerServeError::None, skipped.Resolved == 2U, withoutPool.MemoryCreates == 0U);
}

// The pager maps a created pair into its own window, uses it, unmaps and
// destroys it; here the window is test memory.
void TestMappingSelfTest(TestContext& runner) noexcept
{
    alignas(4096) static UInt8 window[2U * 4096U];
    ScriptedKernel kernel;
    const SystemCallClient client{SystemCallTransport{&ScriptedKernel::Transport, &kernel}};
    PagerBootstrap bootstrap = Bootstrap();
    bootstrap.Management = {0x99U, 0x9999U};
    bootstrap.ManagementFeatures = Abi::PagerManagementFrames | Abi::PagerManagementMapping;
    bootstrap.OwnSpace = {0xAAU, 0xAAAAU};
    bootstrap.WindowStart = reinterpret_cast<UInt64>(window);
    bootstrap.WindowPages = 16U;
    PagerProgram program{client, bootstrap};
    const PagerServeOutcome outcome = program.TestManagement();
    const UInt8* const block = kernel.LastMapBytes;
    ASTERIA_EXPECT(
        runner, "the self-test maps a created pair into its window, uses, unmaps and destroys it",
        outcome.Error == PagerServeError::None, outcome.Resolved == 6U, kernel.MemoryCreates == 4U,
        kernel.MemoryDestroys == 4U, kernel.Maps == 1U, kernel.Unmaps == 1U, kernel.Other == 0U,
        kernel.LastUnmap.Arguments[0U] == 0xAAU, kernel.LastUnmap.Arguments[1U] == 0xAAAAU,
        kernel.LastUnmap.Arguments[2U] == bootstrap.WindowStart, kernel.LastUnmap.Arguments[3U] == 2U,
        kernel.DestroyOrder[0U] == Abi::ObjectTypeMemoryObject, kernel.DestroyOrder[1U] == Abi::ObjectTypeFrame,
        kernel.LastMap.Arguments[0U] == 0U, kernel.LastMap.Arguments[1U] == 0U, kernel.LastMap.Arguments[2U] == 0U,
        kernel.LastMap.Arguments[4U] == Abi::FaultResolveRequestSizeBytes, kernel.LastMap.Arguments[5U] == 0U,
        LoadWord(block + Abi::FaultResolveAddressSpaceOffset) == 0xAAU,
        LoadWord(block + Abi::FaultResolveAddressSpaceOffset + 8U) == 0xAAAAU,
        LoadWord(block + Abi::FaultResolveMemoryObjectOffset) == 0x100U + Abi::ObjectTypeMemoryObject,
        LoadWord(block + Abi::FaultResolveFrameOffset) == 0x100U + Abi::ObjectTypeFrame,
        LoadWord(block + Abi::FaultResolveVirtualStartOffset) == bootstrap.WindowStart,
        LoadWord(block + Abi::FaultResolvePageCountOffset) == 2U,
        LoadWord(block + Abi::FaultResolveMemoryObjectPageOffset) == 0U,
        LoadWord(block + Abi::FaultResolveFramePageOffset) == 0U, block[Abi::FaultResolvePermissionsOffset] == 0x0BU,
        !kernel.LiveByType[Abi::ObjectTypeFrame], !kernel.LiveByType[Abi::ObjectTypeMemoryObject],
        LoadWord(window) == Abi::MappedPagePattern, LoadWord(window + 8U) == 0U,
        LoadWord(window + 4096U) == Abi::MappedPagePattern, LoadWord(window + 4096U + 8U) == 1U);

    // The window now holds the pattern, as pages a previous user left dirty would.
    ScriptedKernel dirty;
    const SystemCallClient dirtyClient{SystemCallTransport{&ScriptedKernel::Transport, &dirty}};
    PagerProgram dirtyProgram{dirtyClient, bootstrap};
    const PagerServeOutcome unclean = dirtyProgram.TestManagement();
    std::memset(window, 0, sizeof(window));
    ScriptedKernel refusing;
    refusing.MapError = Abi::SystemCallError::InvalidArguments;
    const SystemCallClient refusingClient{SystemCallTransport{&ScriptedKernel::Transport, &refusing}};
    PagerProgram refusingProgram{refusingClient, bootstrap};
    const PagerServeOutcome refused = refusingProgram.TestManagement();
    const UInt64 untouched = LoadWord(window);
    PagerBootstrap narrow = bootstrap;
    narrow.WindowPages = 1U;
    ScriptedKernel small;
    const SystemCallClient smallClient{SystemCallTransport{&ScriptedKernel::Transport, &small}};
    PagerProgram smallProgram{smallClient, narrow};
    const PagerServeOutcome tooSmall = smallProgram.TestManagement();
    std::memset(window, 0, sizeof(window));
    ScriptedKernel stuck;
    stuck.UnmapError = Abi::SystemCallError::WouldBlock;
    const SystemCallClient stuckClient{SystemCallTransport{&ScriptedKernel::Transport, &stuck}};
    PagerProgram stuckProgram{stuckClient, bootstrap};
    const PagerServeOutcome unremoved = stuckProgram.TestManagement();
    ASTERIA_EXPECT(runner, "pages that do not arrive zeroed, a refused map or unmap or a small window fail the test",
                   unclean.Error == PagerServeError::ManagementFailed, unclean.SystemCall == Abi::SystemCallError::None,
                   refused.Error == PagerServeError::ManagementFailed,
                   refused.SystemCall == Abi::SystemCallError::InvalidArguments, untouched == 0U,
                   tooSmall.Error == PagerServeError::ManagementFailed, small.Maps == 0U,
                   unremoved.Error == PagerServeError::ManagementFailed,
                   unremoved.SystemCall == Abi::SystemCallError::WouldBlock, stuck.MemoryDestroys == 2U);
}

// Thread creation ABI and: with threads and a launch the pager creates a thread in its own domain
// from the launch, polls ObjectDestroy while it runs, counting the refusals in the block the thread
// receives, requires its exit code and closes its handle; then it creates the faulting thread and
// serves between its ObjectDestroy calls until it returns the termination's exit code, and does the
// same with the excepting thread where the launch names one.
void TestThreadSelfTest(TestContext& runner) noexcept
{
    constexpr PagerThreadLaunch launch{.Entry = 0x4000'1000U,
                                       .StackTop = 0x4010'0000U,
                                       .SpinLimit = 77U,
                                       .FaultEntry = 0x4000'2000U,
                                       .ExceptionEntry = 0U,
                                       .SpinningEntry = 0U,
                                       .WaitingEntry = 0U,
                                       .ComponentImages = nullptr,
                                       .ComponentImagesBytes = 0U,
                                       .ComponentSpinTicks = 0U,
                                       .Component = {},
                                       .DomainCycles = 0U,
                                       .ServiceDemo = nullptr,
                                       .LoaderDemo = nullptr,
                                       .DriverDemo = nullptr,
                                       .BlockDemo = nullptr,
                                       .FsDemo = nullptr,
                                       .NetDemo = nullptr};
    ScriptedKernel kernel;
    kernel.ThreadBusyPolls[0U] = 3U;
    kernel.ThreadBusyPolls[1U] = 2U;
    const SystemCallClient client{SystemCallTransport{&ScriptedKernel::Transport, &kernel}};
    PagerBootstrap bootstrap = Bootstrap();
    bootstrap.Management = {0x99U, 0x9999U};
    bootstrap.ManagementFeatures = Abi::PagerManagementThreads;
    PagerProgram program{client, bootstrap, &launch};
    const PagerServeOutcome outcome = program.TestManagement();
    const UInt8* const bytes = kernel.ThreadBytes[0U];
    const UInt8* const faulting = kernel.ThreadBytes[1U];
    bool zeroReserved{true};
    for (Size index{0U}; index < Abi::ObjectCreateThreadEntryOffset; ++index)
    {
        zeroReserved = zeroReserved && (bytes[index] == 0U);
    }
    for (Size index{Abi::ObjectCreateThreadBudgetOffset}; index < Abi::ObjectCreateThreadRequestSizeBytes; ++index)
    {
        zeroReserved = zeroReserved && (bytes[index] == 0U);
    }
    const auto* const control = reinterpret_cast<const PagerThreadControl*>(
        static_cast<Foundation::UIntPointer>(LoadWord(bytes + Abi::ObjectCreateThreadArgumentOffset)));
    ASTERIA_EXPECT(runner, "the self-test creates two threads from the launch, reclaims both and closes them",
                   outcome.Error == PagerServeError::None, outcome.Resolved == 4U, kernel.ThreadCreates == 2U,
                   kernel.ThreadDestroys == 7U, kernel.Waits == 2U,
                   LoadWord(faulting + Abi::ObjectCreateThreadEntryOffset) == launch.FaultEntry,
                   LoadWord(faulting + Abi::ObjectCreateThreadStackOffset) == launch.StackTop,
                   LoadWord(faulting + Abi::ObjectCreateThreadArgumentOffset) == 0U,
                   kernel.LastThreadCreate.Arguments[0U] == 0x99U, kernel.LastThreadCreate.Arguments[1U] == 0x9999U,
                   kernel.LastThreadCreate.Arguments[2U] == Abi::ObjectTypeThread,
                   kernel.LastThreadCreate.Arguments[4U] == Abi::ObjectCreateThreadRequestSizeBytes,
                   LoadWord(bytes + Abi::ObjectCreateThreadEntryOffset) == launch.Entry,
                   LoadWord(bytes + Abi::ObjectCreateThreadStackOffset) == launch.StackTop, zeroReserved,
                   control != nullptr, AtomicUInt64Reference::LoadAcquire(control->Polls) == 3U,
                   control->SpinLimit == launch.SpinLimit,
                   control->Diagnostic.Selector == bootstrap.Diagnostic.Selector, kernel.LastClosed.Selector == 0x104U);

    ScriptedKernel unlaunched;
    const SystemCallClient unlaunchedClient{SystemCallTransport{&ScriptedKernel::Transport, &unlaunched}};
    PagerProgram unlaunchedProgram{unlaunchedClient, bootstrap};
    const PagerServeOutcome skipped = unlaunchedProgram.TestManagement();

    ScriptedKernel refused;
    refused.ThreadCreateError = Abi::SystemCallError::ResourceExhausted;
    const SystemCallClient refusedClient{SystemCallTransport{&ScriptedKernel::Transport, &refused}};
    PagerProgram refusedProgram{refusedClient, bootstrap, &launch};
    const PagerServeOutcome exhausted = refusedProgram.TestManagement();

    ScriptedKernel lost;
    lost.ThreadExitCodes[0U] = User::Pager::PagerThreadStateLostExitCode;
    const SystemCallClient lostClient{SystemCallTransport{&ScriptedKernel::Transport, &lost}};
    PagerProgram lostProgram{lostClient, bootstrap, &launch};
    const PagerServeOutcome mismatched = lostProgram.TestManagement();
    ScriptedKernel survived;
    survived.ThreadExitCodes[1U] = PagerThreadExitCode;
    const SystemCallClient survivedClient{SystemCallTransport{&ScriptedKernel::Transport, &survived}};
    PagerProgram survivedProgram{survivedClient, bootstrap, &launch};
    const PagerServeOutcome unterminated = survivedProgram.TestManagement();
    constexpr PagerThreadLaunch excepting{.Entry = 0x4000'1000U,
                                          .StackTop = 0x4010'0000U,
                                          .SpinLimit = 77U,
                                          .FaultEntry = 0x4000'2000U,
                                          .ExceptionEntry = 0x4000'3000U,
                                          .SpinningEntry = 0U,
                                          .WaitingEntry = 0U,
                                          .ComponentImages = nullptr,
                                          .ComponentImagesBytes = 0U,
                                          .ComponentSpinTicks = 0U,
                                          .Component = {},
                                          .DomainCycles = 0U,
                                          .ServiceDemo = nullptr,
                                          .LoaderDemo = nullptr,
                                          .DriverDemo = nullptr,
                                          .BlockDemo = nullptr,
                                          .FsDemo = nullptr,
                                          .NetDemo = nullptr};
    ScriptedKernel third;
    third.ThreadBusyPolls[2U] = 2U;
    const SystemCallClient thirdClient{SystemCallTransport{&ScriptedKernel::Transport, &third}};
    PagerProgram thirdProgram{thirdClient, bootstrap, &excepting};
    const PagerServeOutcome three = thirdProgram.TestManagement();
    ScriptedKernel resumed;
    resumed.ThreadExitCodes[2U] = PagerThreadExitCode;
    const SystemCallClient resumedClient{SystemCallTransport{&ScriptedKernel::Transport, &resumed}};
    PagerProgram resumedProgram{resumedClient, bootstrap, &excepting};
    const PagerServeOutcome unexcepted = resumedProgram.TestManagement();
    ASTERIA_EXPECT(runner, "an exception entry adds a third thread, which the pager serves until it is terminated",
                   three.Error == PagerServeError::None, three.Resolved == 5U, third.ThreadCreates == 3U,
                   third.ThreadDestroys == 5U, third.Waits == 2U,
                   LoadWord(third.ThreadBytes[2U] + Abi::ObjectCreateThreadEntryOffset) == excepting.ExceptionEntry,
                   LoadWord(third.ThreadBytes[2U] + Abi::ObjectCreateThreadArgumentOffset) == 0U,
                   unexcepted.Error == PagerServeError::ManagementFailed, unexcepted.Resolved == 4U,
                   resumed.ThreadCreates == 3U);

    // Domain teardown: a spinning entry adds a fourth thread, which the pager kills once it
    // counted, twice, and reclaims with KilledExitCode.
    constexpr PagerThreadLaunch spinning{.Entry = 0x4000'1000U,
                                         .StackTop = 0x4010'0000U,
                                         .SpinLimit = 77U,
                                         .FaultEntry = 0x4000'2000U,
                                         .ExceptionEntry = 0x4000'3000U,
                                         .SpinningEntry = 0x4000'4000U,
                                         .WaitingEntry = 0U,
                                         .ComponentImages = nullptr,
                                         .ComponentImagesBytes = 0U,
                                         .ComponentSpinTicks = 0U,
                                         .Component = {},
                                         .DomainCycles = 0U,
                                         .ServiceDemo = nullptr,
                                         .LoaderDemo = nullptr,
                                         .DriverDemo = nullptr,
                                         .BlockDemo = nullptr,
                                         .FsDemo = nullptr,
                                         .NetDemo = nullptr};
    ScriptedKernel fourth;
    fourth.SpinEntry = spinning.SpinningEntry;
    const SystemCallClient fourthClient{SystemCallTransport{&ScriptedKernel::Transport, &fourth}};
    PagerProgram fourthProgram{fourthClient, bootstrap, &spinning};
    const PagerServeOutcome four = fourthProgram.TestManagement();
    ScriptedKernel blockedKill;
    blockedKill.SpinEntry = spinning.SpinningEntry;
    blockedKill.KillError = Abi::SystemCallError::WouldBlock;
    const SystemCallClient blockedClient{SystemCallTransport{&ScriptedKernel::Transport, &blockedKill}};
    PagerProgram blockedProgram{blockedClient, bootstrap, &spinning};
    const PagerServeOutcome unkilled = blockedProgram.TestManagement();
    ASTERIA_EXPECT(
        runner, "a spinning entry adds a fourth thread, which the pager kills and reclaims",
        four.Error == PagerServeError::None, four.Resolved == 6U, fourth.ThreadCreates == 4U, fourth.Kills == 2U,
        LoadWord(fourth.ThreadBytes[3U] + Abi::ObjectCreateThreadEntryOffset) == spinning.SpinningEntry,
        LoadWord(fourth.ThreadBytes[3U] + Abi::ObjectCreateThreadArgumentOffset) != 0U,
        fourth.LastKill.Arguments[0U] == 0x104U, fourth.LastKill.Arguments[2U] == 0U,
        fourth.LastClosed.Selector == 0x104U, unkilled.Error == PagerServeError::ManagementFailed,
        unkilled.SystemCall == Abi::SystemCallError::WouldBlock, unkilled.Resolved == 5U, blockedKill.Kills == 2U);

    // With WaitSets, a waiting entry adds a fifth thread on a WaitSet the pager creates, reclaimed
    // once its wait timed out, while the pager itself waits with timeouts.
    constexpr PagerThreadLaunch waiting{.Entry = 0x4000'1000U,
                                        .StackTop = 0x4010'0000U,
                                        .SpinLimit = 77U,
                                        .FaultEntry = 0x4000'2000U,
                                        .ExceptionEntry = 0x4000'3000U,
                                        .SpinningEntry = 0x4000'4000U,
                                        .WaitingEntry = 0x4000'5000U,
                                        .ComponentImages = nullptr,
                                        .ComponentImagesBytes = 0U,
                                        .ComponentSpinTicks = 0U,
                                        .Component = {},
                                        .DomainCycles = 0U,
                                        .ServiceDemo = nullptr,
                                        .LoaderDemo = nullptr,
                                        .DriverDemo = nullptr,
                                        .BlockDemo = nullptr,
                                        .FsDemo = nullptr,
                                        .NetDemo = nullptr};
    PagerBootstrap waitSets = bootstrap;
    waitSets.ManagementFeatures = Abi::PagerManagementThreads | Abi::PagerManagementWaitSets;
    ScriptedKernel fifth;
    fifth.SpinEntry = waiting.SpinningEntry;
    fifth.WaitEntry = waiting.WaitingEntry;
    fifth.ThreadBusyPolls[4U] = 2U;
    const SystemCallClient fifthClient{SystemCallTransport{&ScriptedKernel::Transport, &fifth}};
    PagerProgram fifthProgram{fifthClient, waitSets, &waiting};
    const PagerServeOutcome five = fifthProgram.TestManagement();
    ScriptedKernel woken;
    woken.SpinEntry = waiting.SpinningEntry;
    woken.WaitEntry = waiting.WaitingEntry;
    woken.WaitResult = Abi::SystemCallError::None;
    const SystemCallClient wokenClient{SystemCallTransport{&ScriptedKernel::Transport, &woken}};
    PagerProgram wokenProgram{wokenClient, waitSets, &waiting};
    const PagerServeOutcome early = wokenProgram.TestManagement();
    ASTERIA_EXPECT(runner, "a waiting entry adds a timed waiter on a created WaitSet and a waiter the pager kills",
                   five.Error == PagerServeError::None, five.Resolved == 9U, fifth.ThreadCreates == 6U,
                   fifth.CreatesByType[Abi::ObjectTypeWaitSet] == 1U, fifth.Waits == 3U, fifth.Kills == 3U,
                   LoadWord(fifth.ThreadBytes[4U] + Abi::ObjectCreateThreadEntryOffset) == waiting.WaitingEntry,
                   LoadWord(fifth.ThreadBytes[5U] + Abi::ObjectCreateThreadEntryOffset) == waiting.WaitingEntry,
                   fifth.LastWaitTimeout == 100'000U, fifth.LastKill.Arguments[0U] == 0x104U,
                   fifth.DestroySequence[0U] == Abi::ObjectTypeWaitSet,
                   early.Error == PagerServeError::ManagementFailed, early.Resolved == 6U);

    ASTERIA_EXPECT(runner, "no launch makes no thread, and a refused creation or another exit code fails the test",
                   skipped.Error == PagerServeError::None, skipped.Resolved == 2U, unlaunched.ThreadCreates == 0U,
                   unterminated.Error == PagerServeError::ManagementFailed, unterminated.Resolved == 3U,
                   survived.ThreadCreates == 2U, exhausted.Error == PagerServeError::ManagementFailed,
                   exhausted.SystemCall == Abi::SystemCallError::ResourceExhausted, refused.ThreadDestroys == 0U,
                   mismatched.Error == PagerServeError::ManagementFailed,
                   mismatched.SystemCall == Abi::SystemCallError::None, lost.ThreadDestroys == 1U, lost.Closes == 3U);
}

// The thread's clock advances one tick per read; the pager "runs" (Polls
// Advances) once the clock reached PreemptAt.
struct ThreadScript
{
    static UInt64 Clock() noexcept
    {
        ++Ticks;
        if ((Ticks == PreemptAt) && (Control != nullptr))
        {
            AtomicUInt64Reference{Control->Polls}.IncrementRelease();
        }
        return Ticks;
    }

    static bool Intact() noexcept
    {
        return StateIntact;
    }

    static inline UInt64 Ticks{0U};
    static inline UInt64 PreemptAt{0U};
    static inline bool StateIntact{true};
    static inline PagerThreadControl* Control{nullptr};
};

[[nodiscard]] UInt64 RunThread(ScriptedKernel& kernel, const UInt64 preemptAt, const bool intact) noexcept
{
    static PagerThreadControl control{};
    AtomicUInt64Reference{control.Polls}.StoreRelease(5U);
    control.Diagnostic = {0x3U, 0x33U};
    control.SpinLimit = 100U;
    ThreadScript::Ticks = 0U;
    ThreadScript::PreemptAt = preemptAt;
    ThreadScript::StateIntact = intact;
    ThreadScript::Control = &control;
    return User::Pager::RunPagerThread(SystemCallTransport{&ScriptedKernel::Transport, &kernel}, control,
                                       &ThreadScript::Intact, &ThreadScript::Clock);
}

// The thread reports done only once the pager ran while it waited and its
// registers held; otherwise it reports which of the two failed.
void TestThreadBody(TestContext& runner) noexcept
{
    ScriptedKernel done;
    const UInt64 doneCode = RunThread(done, 10U, true);
    const bool doneLine = std::strcmp(done.LastLine, User::Pager::PagerThreadDoneLine) == 0;
    ScriptedKernel lost;
    const UInt64 lostCode = RunThread(lost, 10U, false);
    const bool lostLine = std::strcmp(lost.LastLine, User::Pager::PagerThreadStateLostLine) == 0;
    ScriptedKernel alone;
    const UInt64 aloneCode = RunThread(alone, 0U, true);
    const bool aloneLine = std::strcmp(alone.LastLine, User::Pager::PagerThreadTimeoutLine) == 0;
    const UInt64 aloneTicks = ThreadScript::Ticks;
    ASTERIA_EXPECT(runner, "the thread reports done, lost registers or no preemption with its line and exit code",
                   doneCode == PagerThreadExitCode, doneLine, done.Lines == 1U,
                   lostCode == User::Pager::PagerThreadStateLostExitCode, lostLine,
                   aloneCode == User::Pager::PagerThreadTimeoutExitCode, aloneLine, aloneTicks > 100U,
                   alone.Other == 0U);
}

Size g_rootRuns{0U};

[[nodiscard]] PagerServeOutcome FailingRootService(const SystemCallClient&, const PagerBootstrap&, PagerProgram&,
                                                   const PagerThreadLaunch&) noexcept
{
    ++g_rootRuns;
    return PagerServeOutcome{.Error = PagerServeError::ManagementFailed,
                             .SystemCall = Abi::SystemCallError::InvalidArguments,
                             .Resolved = 0U};
}

// A root service that fails is reported, but the pager goes on serving
// faults: it waits again, and only its
// own failure, a foreign wake here, ends it.
void TestServesAfterTheRootService(TestContext& runner) noexcept
{
    UInt64 words[Abi::PagerBootstrapWords]{};
    for (Size index{0U}; index < 12U; ++index)
    {
        words[index] = 0x100U + index;
    }
    words[Abi::PagerBootstrapLazyStartWord] = LazyStart;
    words[Abi::PagerBootstrapLazyPagesWord] = 2U;
    ScriptedKernel kernel;
    kernel.ReadinessTag = 0x1234U;
    const PagerThreadLaunch launch{};
    User::Pager::RunPager(SystemCallTransport{&ScriptedKernel::Transport, &kernel}, words, &launch,
                          &FailingRootService);
    ASTERIA_EXPECT(runner, "a failed root service is reported and the pager still serves faults", g_rootRuns == 1U,
                   kernel.Lines == 2U, kernel.Waits == 1U,
                   std::strcmp(kernel.FirstLine, "Bail out! root service stopped serving: error 0x08 call 0x02\n") == 0,
                   std::strncmp(kernel.LastLine, "Bail out! pager stopped serving faults: error 0x", 48U) == 0);
}

void TestDecodesTheStartBlock(TestContext& runner) noexcept
{
    UInt64 words[Abi::PagerBootstrapWords]{};
    for (Size index{0U}; index < 12U; ++index)
    {
        words[index] = 0x100U + index;
    }
    words[Abi::PagerBootstrapManagementWord] = 0x10EU;
    words[Abi::PagerBootstrapManagementWord + 1U] = 0x10FU;
    words[Abi::PagerBootstrapManagementFeaturesWord] = Abi::PagerManagementFrames;
    words[Abi::PagerBootstrapLazyStartWord] = LazyStart;
    words[Abi::PagerBootstrapLazyPagesWord] = 2U;
    words[Abi::PagerBootstrapOwnSpaceWord] = 0x112U;
    words[Abi::PagerBootstrapOwnSpaceWord + 1U] = 0x113U;
    words[Abi::PagerBootstrapWindowStartWord] = 0x4C00'0000U;
    words[Abi::PagerBootstrapWindowPagesWord] = 16U;
    words[Abi::PagerBootstrapDeviceFunctionWord(Abi::PagerDeviceAhci)] = 0x11AU;
    words[Abi::PagerBootstrapDeviceFunctionWord(Abi::PagerDeviceAhci) + 1U] = 0x11BU;
    words[Abi::PagerBootstrapDeviceRegionWord(Abi::PagerDeviceAhci)] = 0x11CU;
    words[Abi::PagerBootstrapDeviceRegionWord(Abi::PagerDeviceAhci) + 1U] = 0x11DU;
    words[Abi::PagerBootstrapDeviceFunctionWord(Abi::PagerDeviceNic)] = 0x120U;
    words[Abi::PagerBootstrapDeviceFunctionWord(Abi::PagerDeviceNic) + 1U] = 0x121U;
    words[Abi::PagerBootstrapDeviceRegionWord(Abi::PagerDeviceNic)] = 0x122U;
    words[Abi::PagerBootstrapDeviceRegionWord(Abi::PagerDeviceNic) + 1U] = 0x123U;
    words[Abi::PagerBootstrapInventoryFrameWord] = 0x12EU;
    words[Abi::PagerBootstrapInventoryFrameWord + 1U] = 0x12FU;
    words[Abi::PagerBootstrapInventoryPagesWord] = 4U;
    words[Abi::PagerBootstrapModuleBaseWord] = Abi::Amd64PagerModuleBase;
    words[Abi::PagerBootstrapModuleBytesWord] = 8192U;
    words[Abi::PagerBootstrapDeviceFunctionWord(Abi::PagerDeviceXhci)] = 0x151U;
    words[Abi::PagerBootstrapDeviceFunctionWord(Abi::PagerDeviceXhci) + 1U] = 0x152U;
    words[Abi::PagerBootstrapDeviceRegionWord(Abi::PagerBootstrapDevices - 1U)] = 0x165U;
    words[Abi::PagerBootstrapDeviceRegionWord(Abi::PagerBootstrapDevices - 1U) + 1U] = 0x166U;
    words[Abi::PagerBootstrapClockTicksPerMicrosecondWord] = 123U;
    words[Abi::PagerBootstrapScanoutRegionWord] = 0x11EU;
    words[Abi::PagerBootstrapScanoutRegionWord + 1U] = 0x11FU;
    words[Abi::PagerBootstrapScanoutModeWord] = 1280U | (UInt64{800U} << 32U);
    words[Abi::PagerBootstrapScanoutModeWord + 1U] = 1280U | (UInt64{2U} << 32U);
    PagerBootstrap decoded{};
    const bool accepted = User::Pager::DecodePagerBootstrap(words, decoded);
    words[Abi::PagerBootstrapWindowStartWord] = 0x4C00'0800U;
    PagerBootstrap window{};
    const bool rejectedWindow = !User::Pager::DecodePagerBootstrap(words, window);
    words[Abi::PagerBootstrapWindowStartWord] = 0x4C00'0000U;
    words[Abi::PagerBootstrapLazyStartWord] = LazyStart + 1U;
    PagerBootstrap misaligned{};
    const bool rejectedMisaligned = !User::Pager::DecodePagerBootstrap(words, misaligned);
    words[Abi::PagerBootstrapLazyStartWord] = LazyStart;
    words[Abi::PagerBootstrapLazyPagesWord] = 0U;
    const bool rejectedEmpty = !User::Pager::DecodePagerBootstrap(words, misaligned);
    ASTERIA_EXPECT(runner, "the start block decodes six handles and an aligned, nonempty lazy region", accepted,
                   decoded.Endpoint.Selector == 0x100U, decoded.WaitSet.Authenticator == 0x103U,
                   decoded.LazyFrame.Selector == 0x10AU, decoded.LazyFrame.Authenticator == 0x10BU,
                   decoded.LazyStart == LazyStart, decoded.LazyPages == 2U, decoded.Management.Selector == 0x10EU,
                   decoded.Management.Authenticator == 0x10FU, decoded.ManagementFeatures == Abi::PagerManagementFrames,
                   decoded.OwnSpace.Selector == 0x112U, decoded.OwnSpace.Authenticator == 0x113U,
                   decoded.WindowStart == 0x4C00'0000U, decoded.WindowPages == 16U, rejectedMisaligned, rejectedEmpty,
                   rejectedWindow);
    const User::Pager::PagerDevice& edu = decoded.Devices[Abi::PagerDeviceEdu];
    const User::Pager::PagerDevice& ahci = decoded.Devices[Abi::PagerDeviceAhci];
    const User::Pager::PagerDevice& nic = decoded.Devices[Abi::PagerDeviceNic];
    ASTERIA_EXPECT(
        runner, "the device table decodes by kind, an entry naming none as zero handles",
        Abi::PagerBootstrapDeviceFunctionWord(Abi::PagerDeviceEdu) == 22U,
        Abi::PagerBootstrapDeviceRegionWord(Abi::PagerDeviceAhci) == 28U,
        Abi::PagerBootstrapDeviceRegionWord(Abi::PagerDeviceNic) == 32U, Abi::PagerBootstrapScanoutRegionWord == 34U,
        Abi::PagerBootstrapWords == 88U, Abi::PagerBootstrapModuleBaseWord == 49U,
        Abi::PagerBootstrapModuleBytesWord == 50U, Abi::PagerBootstrapDeviceFunctionWord(Abi::PagerDeviceXhci) == 51U,
        Abi::PagerBootstrapDeviceRegionWord(Abi::PagerDeviceUsbHost - 1U) == 65U,
        Abi::PagerBootstrapDeviceFunctionWord(Abi::PagerDeviceUsbHost) == 69U,
        Abi::PagerBootstrapDeviceRegionWord(Abi::PagerBootstrapDevices - 1U) == 83U,
        Abi::PagerBootstrapClockTicksPerMicrosecondWord == 67U, edu.Function.Selector == 0U,
        edu.Region.Authenticator == 0U, ahci.Function.Selector == 0x11AU, ahci.Function.Authenticator == 0x11BU,
        ahci.Region.Selector == 0x11CU, ahci.Region.Authenticator == 0x11DU, nic.Function.Selector == 0x120U,
        nic.Function.Authenticator == 0x121U, nic.Region.Selector == 0x122U, nic.Region.Authenticator == 0x123U);
    ASTERIA_EXPECT(runner, "the appended inventory capability and page extent survive decoding",
                   decoded.InventoryFrame.Selector == 0x12EU, decoded.InventoryFrame.Authenticator == 0x12FU,
                   decoded.InventoryPages == 4U);
    ASTERIA_EXPECT(runner, "the module slice survives decoding", decoded.ModuleBase == Abi::Amd64PagerModuleBase,
                   decoded.ModuleBytes == 8192U);
    ASTERIA_EXPECT(runner, "appended xHCI capabilities and clock scale preserve the original module slice",
                   decoded.Devices[Abi::PagerDeviceXhci].Function.Selector == 0x151U,
                   decoded.Devices[Abi::PagerDeviceXhci].Function.Authenticator == 0x152U,
                   decoded.Devices[Abi::PagerBootstrapDevices - 1U].Region.Selector == 0x165U,
                   decoded.Devices[Abi::PagerBootstrapDevices - 1U].Region.Authenticator == 0x166U,
                   decoded.ClockTicksPerMicrosecond == 123U);
    words[Abi::PagerBootstrapLazyPagesWord] = 2U;
    constexpr UInt64 invalidModules[][2]{
        {0U, 8192U},
        {Abi::Amd64PagerModuleBase, 0U},
        {Abi::Amd64PagerModuleBase + 1U, 8192U},
        {Abi::Amd64PagerModuleBase, 8193U},
        {Abi::Amd64PagerModuleBase, Abi::PagerModuleMaximumBytes + 4096U},
        {~UInt64{4095U}, 8192U},
    };
    for (const auto& extent : invalidModules)
    {
        words[Abi::PagerBootstrapModuleBaseWord] = extent[0U];
        words[Abi::PagerBootstrapModuleBytesWord] = extent[1U];
        ASTERIA_EXPECT(runner, "invalid module start-block extent is refused without changing output",
                       !User::Pager::DecodePagerBootstrap(words, decoded),
                       decoded.ModuleBase == Abi::Amd64PagerModuleBase, decoded.ModuleBytes == 8192U);
        ASTERIA_EXPECT(runner, "appended xHCI capabilities and clock scale preserve the original module slice",
                       decoded.Devices[Abi::PagerDeviceXhci].Function.Selector == 0x151U,
                       decoded.Devices[Abi::PagerDeviceXhci].Function.Authenticator == 0x152U,
                       decoded.Devices[Abi::PagerBootstrapDevices - 1U].Region.Selector == 0x165U,
                       decoded.Devices[Abi::PagerBootstrapDevices - 1U].Region.Authenticator == 0x166U,
                       decoded.ClockTicksPerMicrosecond == 123U);
    }
    ASTERIA_EXPECT(runner, "the scanout decodes its region and its packed mode",
                   decoded.Scanout.Region.Selector == 0x11EU, decoded.Scanout.Region.Authenticator == 0x11FU,
                   decoded.Scanout.Width == 1280U, decoded.Scanout.Height == 800U, decoded.Scanout.Stride == 1280U,
                   decoded.Scanout.Format == 2U);
}
// Where the launch asks for it and the boot grants domains and mapping, the self-test creates and
// destroys whole domains: an AddressSpace, a CapabilitySpace and a ProtectionDomain tagged one more
// each cycle, a pair mapped into the new space and unmapped, then the pair, the domain and its
// spaces destroyed.
void TestDomainSelfTest(TestContext& runner) noexcept
{
    alignas(4096) static UInt8 window[2U * 4096U];
    std::memset(window, 0, sizeof(window));
    constexpr PagerThreadLaunch launch{.Entry = 0U,
                                       .StackTop = 0U,
                                       .SpinLimit = 0U,
                                       .FaultEntry = 0U,
                                       .ExceptionEntry = 0U,
                                       .SpinningEntry = 0U,
                                       .WaitingEntry = 0U,
                                       .ComponentImages = nullptr,
                                       .ComponentImagesBytes = 0U,
                                       .ComponentSpinTicks = 0U,
                                       .Component = {},
                                       .DomainCycles = 3U,
                                       .ServiceDemo = nullptr,
                                       .LoaderDemo = nullptr,
                                       .DriverDemo = nullptr,
                                       .BlockDemo = nullptr,
                                       .FsDemo = nullptr,
                                       .NetDemo = nullptr};
    ScriptedKernel kernel;
    const SystemCallClient client{SystemCallTransport{&ScriptedKernel::Transport, &kernel}};
    PagerBootstrap bootstrap = Bootstrap();
    bootstrap.Management = {0x99U, 0x9999U};
    bootstrap.ManagementFeatures = Abi::PagerManagementMapping | Abi::PagerManagementDomains;
    bootstrap.OwnSpace = {0xAAU, 0xAAAAU};
    bootstrap.WindowStart = reinterpret_cast<UInt64>(window);
    bootstrap.WindowPages = 16U;
    PagerProgram program{client, bootstrap, &launch};
    const PagerServeOutcome outcome = program.TestManagement();
    const UInt64 expected[]{Abi::ObjectTypeMemoryObject, Abi::ObjectTypeFrame, Abi::ObjectTypeProtectionDomain,
                            Abi::ObjectTypeCapabilitySpace, Abi::ObjectTypeAddressSpace};
    bool ordered{true};
    for (Size index{0U}; index < 5U; ++index)
    {
        ordered = ordered && (kernel.DestroySequence[(kernel.MemoryDestroys - 5U + index) % 16U] == expected[index]);
    }
    ASTERIA_EXPECT(
        runner, "three domains are created, given a mapping they lose, and destroyed domain first",
        outcome.Error == PagerServeError::None, outcome.Resolved == 19U,
        kernel.CreatesByType[Abi::ObjectTypeAddressSpace] == 3U,
        kernel.CreatesByType[Abi::ObjectTypeCapabilitySpace] == 3U,
        kernel.CreatesByType[Abi::ObjectTypeProtectionDomain] == 3U, kernel.Maps == 4U, kernel.Unmaps == 4U,
        kernel.MemoryDestroys == 17U, ordered, kernel.LastUnmap.Arguments[0U] == 0x100U + Abi::ObjectTypeAddressSpace,
        kernel.LastUnmap.Arguments[1U] == 0x200U + Abi::ObjectTypeAddressSpace,
        kernel.LastUnmap.Arguments[2U] == bootstrap.WindowStart, kernel.LastUnmap.Arguments[3U] == 2U,
        LoadWord(kernel.LastMapBytes + Abi::FaultResolveAddressSpaceOffset) == 0x100U + Abi::ObjectTypeAddressSpace,
        LoadWord(kernel.DomainBytes + Abi::ObjectCreateDomainAddressSpaceOffset) ==
            0x100U + Abi::ObjectTypeAddressSpace,
        LoadWord(kernel.DomainBytes + Abi::ObjectCreateDomainAddressSpaceOffset + 8U) ==
            0x200U + Abi::ObjectTypeAddressSpace,
        LoadWord(kernel.DomainBytes + Abi::ObjectCreateDomainCapabilitySpaceOffset) ==
            0x100U + Abi::ObjectTypeCapabilitySpace,
        LoadWord(kernel.DomainBytes + Abi::ObjectCreateDomainSupervisionTagOffset) == 0x5E1F'0003U,
        LoadWord(kernel.DomainBytes + 40U) == 0U, LoadWord(kernel.DomainBytes + 48U) == 0U,
        LoadWord(kernel.DomainBytes + 56U) == 0U, !kernel.LiveByType[Abi::ObjectTypeAddressSpace],
        !kernel.LiveByType[Abi::ObjectTypeProtectionDomain]);

    // No cycles without the domains feature or without a launch asking.
    std::memset(window, 0, sizeof(window));
    PagerBootstrap mappingOnly = bootstrap;
    mappingOnly.ManagementFeatures = Abi::PagerManagementMapping;
    ScriptedKernel plain;
    const SystemCallClient plainClient{SystemCallTransport{&ScriptedKernel::Transport, &plain}};
    PagerProgram plainProgram{plainClient, mappingOnly, &launch};
    const PagerServeOutcome skipped = plainProgram.TestManagement();
    std::memset(window, 0, sizeof(window));
    ScriptedKernel refusing;
    refusing.MemoryCreateError = Abi::SystemCallError::ResourceExhausted;
    PagerBootstrap domainsOnly = bootstrap;
    domainsOnly.ManagementFeatures = Abi::PagerManagementMapping | Abi::PagerManagementDomains;
    const SystemCallClient refusingClient{SystemCallTransport{&ScriptedKernel::Transport, &refusing}};
    PagerProgram refusingProgram{refusingClient, domainsOnly, &launch};
    const PagerServeOutcome refused = refusingProgram.TestManagement();
    ASTERIA_EXPECT(runner, "without the domains feature nothing is created, and a refused creation fails the test",
                   skipped.Error == PagerServeError::None, plain.CreatesByType[Abi::ObjectTypeAddressSpace] == 0U,
                   refused.Error == PagerServeError::ManagementFailed,
                   refused.SystemCall == Abi::SystemCallError::ResourceExhausted);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"PagerProgramTests"};
    Asteria::Test::TestServesAFault(runner);
    Asteria::Test::TestTerminatesFaultsOutsideTheRegion(runner);
    Asteria::Test::TestTerminatesTaggedFaults(runner);
    Asteria::Test::TestTerminatesUnmappableFaultsInsideTheRegion(runner);
    Asteria::Test::TestStopsOnRejectedFaults(runner);
    Asteria::Test::TestManagementSelfTest(runner);
    Asteria::Test::TestMemorySelfTest(runner);
    Asteria::Test::TestMappingSelfTest(runner);
    Asteria::Test::TestThreadSelfTest(runner);
    Asteria::Test::TestDomainSelfTest(runner);
    Asteria::Test::TestThreadBody(runner);
    Asteria::Test::TestDecodesTheStartBlock(runner);
    Asteria::Test::TestServesAfterTheRootService(runner);
    return runner.Finish();
}
