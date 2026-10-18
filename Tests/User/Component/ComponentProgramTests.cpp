// SPDX-License-Identifier: Apache-2.0
#include "Abi/ComponentBootstrap.hpp"
#include "Abi/IpcCanonicalFrame.hpp"
#include "Abi/PagerProtocol.hpp"
#include "User/Component/ComponentProgram.hpp"
#include "User/Component/EchoFrame.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Foundation::UInt8;

// Records the one call the component makes and copies the frame it names.
struct FakeKernel
{
    Abi::SystemCallRequest LastRequest;
    Abi::SystemCallError Answer;
    Size Calls;
    UInt8 Frame[sizeof(Abi::CanonicalAsynchronousFrame)];

    static void Transport(void* const context, const Abi::SystemCallRequest* const request,
                          Abi::SystemCallResponse* const response) noexcept
    {
        auto& kernel = *static_cast<FakeKernel*>(context);
        kernel.LastRequest = *request;
        ++kernel.Calls;
        const auto* const frame =
            reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(request->Arguments[2U]));
        for (Size index{0U}; (index < sizeof(kernel.Frame)) && (index < request->Arguments[3U]); ++index)
        {
            kernel.Frame[index] = frame[index];
        }
        *response = Abi::SystemCallResponse{.Error = kernel.Answer, .Value0 = 0U, .Value1 = 0U};
    }
};

// One tick per read; Crashed records the crash, which returns here.
struct Script
{
    static UInt64 Clock() noexcept
    {
        return ++Ticks;
    }

    static void Crash(const UInt64 kind) noexcept
    {
        CrashedAt = Ticks;
        CrashKind = kind;
    }

    static inline UInt64 Ticks{0U};
    static inline UInt64 CrashedAt{0U};
    static inline UInt64 CrashKind{~UInt64{0U}};
};

[[nodiscard]] constexpr UInt64 Word(const Size index, const UInt64 behavior) noexcept
{
    switch (index)
    {
    case Abi::ComponentStartMagicWord:
        return Abi::ComponentStartMagic;
    case Abi::ComponentIncarnationWord:
        return 7U;
    case Abi::ComponentBehaviorWord:
        return behavior;
    case Abi::ComponentReportWord:
        return 0x31U;
    case Abi::ComponentReportWord + 1U:
        return 0x32U;
    case Abi::ComponentSpinTicksWord:
        return 100U;
    default:
        return 0U;
    }
}

void TestIncarnations(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    for (Size index{0U}; index < Abi::ComponentBlockWords; ++index)
    {
        block[index] = Word(index, Abi::ComponentBehaviorReport);
    }
    FakeKernel kernel{.LastRequest = {}, .Answer = Abi::SystemCallError::None, .Calls = 0U, .Frame = {}};
    const User::System::SystemCallTransport transport{&FakeKernel::Transport, &kernel};
    const UInt64 reported = User::Component::RunComponent(transport, block, Script::Clock, Script::Crash);
    bool canonical{true};
    for (Size index{0U}; index < sizeof(kernel.Frame); ++index)
    {
        canonical = canonical && (kernel.Frame[index] == Abi::CanonicalAsynchronousFrame[index]);
    }
    ASTERIA_EXPECT(runner, "a reporting incarnation computes for its ticks, sends the canonical frame and exits",
                   reported == Abi::ComponentExitCode, Script::Ticks >= 100U, kernel.Calls == 1U,
                   kernel.LastRequest.Number == static_cast<UInt64>(Abi::SystemCallNumber::IpcSend),
                   kernel.LastRequest.Arguments[0U] == 0x31U, kernel.LastRequest.Arguments[1U] == 0x32U,
                   kernel.LastRequest.Arguments[3U] == sizeof(Abi::CanonicalAsynchronousFrame), canonical,
                   Script::CrashedAt == 0U);

    kernel.Answer = Abi::SystemCallError::RightsDenied;
    const UInt64 refused = User::Component::RunComponent(transport, block, Script::Clock, Script::Crash);
    block[Abi::ComponentBehaviorWord] = Abi::ComponentBehaviorCrash;
    block[Abi::ComponentCrashKindWord] = Abi::ComponentCrashFloatingPoint;
    const UInt64 crashed = User::Component::RunComponent(transport, block, Script::Clock, Script::Crash);
    const UInt64 spun = Script::CrashedAt;
    block[Abi::ComponentBehaviorWord] = 0U;
    const UInt64 unknown = User::Component::RunComponent(transport, block, Script::Clock, Script::Crash);
    block[Abi::ComponentStartMagicWord] = 0U;
    block[Abi::ComponentBehaviorWord] = Abi::ComponentBehaviorReport;
    const UInt64 unmarked = User::Component::RunComponent(transport, block, Script::Clock, Script::Crash);
    ASTERIA_EXPECT(runner, "a crashing incarnation spins its ticks before it crashes, and bad starts make no call",
                   refused == Abi::ComponentReportFailedExitCode, crashed == Abi::ComponentCrashReturnedExitCode,
                   spun >= 100U, Script::CrashKind == Abi::ComponentCrashFloatingPoint,
                   unknown == Abi::ComponentBadStartExitCode, unmarked == Abi::ComponentBadStartExitCode,
                   kernel.Calls == 2U);
}

// A kernel for an account incarnation: each ObjectCreate answers handle
// 0x100 + n, and the fourth, past the domain's pages, ResourceExhausted
// (Refuse); the fifth requests executable memory and answers CodeAnswer.
// MemoryMap and IpcSend succeed. It records what each call named.
struct AccountKernel
{
    static void Transport(void* const context, const Abi::SystemCallRequest* const request,
                          Abi::SystemCallResponse* const response) noexcept
    {
        auto& kernel = *static_cast<AccountKernel*>(context);
        *response = Abi::SystemCallResponse{.Error = Abi::SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        const auto* const bytes =
            reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(request->Arguments[3U]));
        switch (static_cast<Abi::SystemCallNumber>(request->Number))
        {
        case Abi::SystemCallNumber::ObjectCreate:
            if (kernel.Creates < 7U)
            {
                kernel.Types[kernel.Creates] = request->Arguments[2U];
                kernel.Authorities[kernel.Creates] = request->Arguments[0U];
                kernel.Pages[kernel.Creates] = (request->Arguments[2U] == Abi::ObjectTypeEndpoint) ? 0U : bytes[0U];
                kernel.Permissions[kernel.Creates] = (request->Arguments[2U] == Abi::ObjectTypeMemoryObject)
                                                         ? bytes[Abi::ObjectCreatePermissionsOffset]
                                                         : 0U;
            }
            ++kernel.Creates;
            response->Value0 = 0x100U + kernel.Creates;
            response->Value1 = 0x200U + kernel.Creates;
            if (kernel.Creates == kernel.Refuse)
            {
                response->Error = Abi::SystemCallError::ResourceExhausted;
            }
            if (kernel.Creates == 5U || kernel.Creates == 6U)
            {
                response->Error = kernel.CodeAnswer;
            }
            if (kernel.Creates == 7U)
            {
                response->Error = Abi::SystemCallError::ResourceExhausted;
            }
            return;
        case Abi::SystemCallNumber::MemoryMap:
            ++kernel.Maps;
            for (Size index{0U}; index < sizeof(kernel.Map); ++index)
            {
                kernel.Map[index] = bytes[index];
            }
            return;
        case Abi::SystemCallNumber::IpcSend:
            ++kernel.Sends;
            return;
        default:
            ++kernel.Other;
            return;
        }
    }

    Size Refuse{4U};
    Size Creates{0U};
    UInt64 Types[7U]{};
    UInt64 Authorities[7U]{};
    UInt64 Pages[7U]{};
    UInt8 Permissions[7U]{};
    Abi::SystemCallError CodeAnswer{Abi::SystemCallError::RightsDenied};
    Size Maps{0U};
    UInt8 Map[Abi::FaultResolveRequestSizeBytes]{};
    Size Sends{0U};
    Size Other{0U};
};

[[nodiscard]] UInt64 Load(const UInt8* const bytes) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (8U * index);
    }
    return value;
}

// An account incarnation creates an Endpoint, a Frame and a MemoryObject in its own domain, maps
// them at its window, writes and reads each page, is refused one page more and reports.
void TestAccount(TestContext& runner) noexcept
{
    alignas(4096) static UInt64 window[2U * 512U]{};
    UInt64 block[Abi::ComponentBlockWords]{};
    for (Size index{0U}; index < Abi::ComponentStartBlockWords; ++index)
    {
        block[index] = Word(index, Abi::ComponentBehaviorAccount);
    }
    block[Abi::ComponentAccountWord] = 0x61U;
    block[Abi::ComponentAccountWord + 1U] = 0x62U;
    block[Abi::ComponentAccountSpaceWord] = 0x63U;
    block[Abi::ComponentAccountSpaceWord + 1U] = 0x64U;
    block[Abi::ComponentWindowStartWord] = reinterpret_cast<Foundation::UIntPointer>(window);
    block[Abi::ComponentWindowPagesWord] = 2U;
    AccountKernel kernel{};
    const User::System::SystemCallTransport transport{&AccountKernel::Transport, &kernel};
    const UInt64 used = User::Component::RunComponent(transport, block, Script::Clock, Script::Crash);
    ASTERIA_EXPECT(
        runner, "an account incarnation creates in its own domain, maps its window and is refused beyond",
        used == Abi::ComponentAccountExitCode, kernel.Creates == 7U, kernel.Types[0U] == Abi::ObjectTypeEndpoint,
        kernel.Types[1U] == Abi::ObjectTypeFrame, kernel.Types[2U] == Abi::ObjectTypeMemoryObject,
        kernel.Types[3U] == Abi::ObjectTypeFrame, kernel.Authorities[0U] == 0x61U, kernel.Authorities[3U] == 0x61U,
        kernel.Pages[1U] == 2U, kernel.Pages[2U] == 2U, kernel.Pages[3U] == 1U, kernel.Maps == 1U,
        kernel.Types[4U] == Abi::ObjectTypeMemoryObject, kernel.Authorities[4U] == 0x61U, kernel.Pages[4U] == 1U,
        kernel.Permissions[4U] == 0x0DU, Load(kernel.Map + Abi::FaultResolveAddressSpaceOffset) == 0x63U,
        Load(kernel.Map + Abi::FaultResolveMemoryObjectOffset) == 0x103U,
        Load(kernel.Map + Abi::FaultResolveFrameOffset) == 0x102U,
        Load(kernel.Map + Abi::FaultResolveVirtualStartOffset) == block[Abi::ComponentWindowStartWord],
        Load(kernel.Map + Abi::FaultResolvePageCountOffset) == 2U,
        kernel.Map[Abi::FaultResolvePermissionsOffset] == 0x0BU, window[0U] == 0x574F'4E50'4147'4500ULL,
        window[512U] == 0x574F'4E50'4147'4501ULL, kernel.Sends == 1U, kernel.Other == 0U);

    // A refusal before the domain's pages are spent, or none past them,
    // fails the incarnation before it reports.
    AccountKernel early{};
    early.Refuse = 2U;
    const User::System::SystemCallTransport earlyTransport{&AccountKernel::Transport, &early};
    const UInt64 refused = User::Component::RunComponent(earlyTransport, block, Script::Clock, Script::Crash);
    AccountKernel unbounded{};
    unbounded.Refuse = 0U;
    const User::System::SystemCallTransport unboundedTransport{&AccountKernel::Transport, &unbounded};
    const UInt64 unrefused = User::Component::RunComponent(unboundedTransport, block, Script::Clock, Script::Crash);
    constexpr auto Failure = [](const UInt64 step, const Abi::SystemCallError error) noexcept
    {
        return Abi::ComponentAccountFailedExitCode | (step << 32U) | (static_cast<UInt64>(error) << 40U);
    };
    AccountKernel codeProducer{};
    codeProducer.CodeAnswer = Abi::SystemCallError::None;
    const User::System::SystemCallTransport codeTransport{&AccountKernel::Transport, &codeProducer};
    const UInt64 codeAdmitted = User::Component::RunComponent(codeTransport, block, Script::Clock, Script::Crash);
    ASTERIA_EXPECT(runner, "admitting executable memory fails the account incarnation before it reports",
                   codeAdmitted == Failure(Abi::ComponentAccountCodeRefusalStep, Abi::SystemCallError::None),
                   codeProducer.Creates == 5U, codeProducer.Sends == 0U);
    ASTERIA_EXPECT(runner, "a refusal too early, or none past its pages, fails the account incarnation with its step",
                   refused == Failure(Abi::ComponentAccountFrameStep, Abi::SystemCallError::ResourceExhausted),
                   early.Maps == 0U, early.Sends == 0U,
                   unrefused == Failure(Abi::ComponentAccountRefusalStep, Abi::SystemCallError::None),
                   unbounded.Creates == 4U, unbounded.Sends == 0U);
}

// A kernel for a driver: records each ObjectCreate's type, size and request,
// each MemoryMap and each DeviceControl, and answers handles 0x300 + n; it
// refuses bus mastering with Mastering, as without an IommuDomain.
struct DriverKernel
{
    static void Transport(void* const context, const Abi::SystemCallRequest* const request,
                          Abi::SystemCallResponse* const response) noexcept
    {
        auto& kernel = *static_cast<DriverKernel*>(context);
        *response = Abi::SystemCallResponse{.Error = Abi::SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        const auto* const bytes =
            reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(request->Arguments[3U]));
        switch (static_cast<Abi::SystemCallNumber>(request->Number))
        {
        case Abi::SystemCallNumber::ObjectCreate:
            if (kernel.Creates < 2U)
            {
                kernel.Types[kernel.Creates] = request->Arguments[2U];
                kernel.Sizes[kernel.Creates] = request->Arguments[4U];
                for (Size index{0U}; index < request->Arguments[4U]; ++index)
                {
                    kernel.Requests[kernel.Creates][index] = bytes[index];
                }
            }
            ++kernel.Creates;
            response->Value0 = 0x300U + kernel.Creates;
            response->Value1 = 0x400U + kernel.Creates;
            return;
        case Abi::SystemCallNumber::MemoryMap:
            ++kernel.Maps;
            for (Size index{0U}; index < sizeof(kernel.Map); ++index)
            {
                kernel.Map[index] = bytes[index];
            }
            return;
        case Abi::SystemCallNumber::IpcSend:
            ++kernel.Sends;
            return;
        case Abi::SystemCallNumber::DeviceControl:
            if (kernel.Controls < 2U)
            {
                kernel.Operations[kernel.Controls] = request->Arguments[2U];
            }
            ++kernel.Controls;
            kernel.Function = request->Arguments[0U];
            response->Error =
                (request->Arguments[2U] == Abi::DeviceEnableBusMaster) ? kernel.Mastering : Abi::SystemCallError::None;
            return;
        default:
            ++kernel.Other;
            return;
        }
    }

    Abi::SystemCallError Mastering{Abi::SystemCallError::InvalidConfiguration};
    Size Controls{0U};
    UInt64 Operations[2U]{};
    UInt64 Function{0U};
    Size Creates{0U};
    UInt64 Types[2U]{};
    UInt64 Sizes[2U]{};
    UInt8 Requests[2U][Abi::ObjectCreateDeviceFrameRequestSizeBytes]{};
    Size Maps{0U};
    UInt8 Map[Abi::FaultResolveRequestSizeBytes]{};
    Size Sends{0U};
    Size Other{0U};
};

// A driver finds bus mastering refused and disables it, then maps the device's first register page
// through a device Frame of its region and an uncached MemoryObject of its own domain, and checks
// the device's identification and liveness word; plain memory in place of the device stops it at
// the liveness check, and a granted bus master before anything is created.
void TestDriver(TestContext& runner) noexcept
{
    alignas(4096) static Foundation::UInt32 registers[1024U]{};
    UInt64 block[Abi::ComponentBlockWords]{};
    for (Size index{0U}; index < Abi::ComponentStartBlockWords; ++index)
    {
        block[index] = Word(index, Abi::ComponentBehaviorDriver);
    }
    block[Abi::ComponentAccountWord] = 0x61U;
    block[Abi::ComponentAccountSpaceWord] = 0x63U;
    block[Abi::ComponentDeviceRegionWord] = 0x71U;
    block[Abi::ComponentDeviceRegionWord + 1U] = 0x72U;
    block[Abi::ComponentDeviceFunctionWord] = 0x73U;
    block[Abi::ComponentWindowStartWord] = reinterpret_cast<Foundation::UIntPointer>(registers);
    block[Abi::ComponentWindowPagesWord] = 2U;
    constexpr auto Failure = [](const UInt64 step, const UInt64 error = 0U) noexcept
    {
        return Abi::ComponentDriverFailedExitCode | (step << 32U) | (error << 40U);
    };
    DriverKernel kernel{};
    const User::System::SystemCallTransport transport{&DriverKernel::Transport, &kernel};
    const UInt64 unidentified = User::Component::RunComponent(transport, block, Script::Clock, Script::Crash);
    registers[0U] = 0x0100'00EDU;
    DriverKernel alive{};
    const User::System::SystemCallTransport aliveTransport{&DriverKernel::Transport, &alive};
    const UInt64 inert = User::Component::RunComponent(aliveTransport, block, Script::Clock, Script::Crash);
    ASTERIA_EXPECT(runner, "a driver maps its device's registers uncached and checks identification and liveness",
                   unidentified == Failure(Abi::ComponentDriverIdentityStep),
                   inert == Failure(Abi::ComponentDriverLivenessStep), registers[1U] == 0x5A5A'1234U,
                   kernel.Creates == 2U, kernel.Types[0U] == Abi::ObjectTypeFrame,
                   kernel.Sizes[0U] == Abi::ObjectCreateDeviceFrameRequestSizeBytes,
                   Load(kernel.Requests[0U] + Abi::ObjectCreatePagesOffset) == 1U,
                   Load(kernel.Requests[0U] + Abi::ObjectCreateDeviceFrameRegionOffset) == 0x71U,
                   Load(kernel.Requests[0U] + Abi::ObjectCreateDeviceFrameRegionOffset + 8U) == 0x72U,
                   Load(kernel.Requests[0U] + Abi::ObjectCreateDeviceFrameRegionPageOffset) == 0U,
                   kernel.Types[1U] == Abi::ObjectTypeMemoryObject,
                   kernel.Requests[1U][Abi::ObjectCreateCachePolicyOffset] == Abi::ObjectCreateCacheUncached,
                   kernel.Maps == 1U, Load(kernel.Map + Abi::FaultResolveFrameOffset) == 0x301U,
                   Load(kernel.Map + Abi::FaultResolveMemoryObjectOffset) == 0x302U,
                   Load(kernel.Map + Abi::FaultResolvePageCountOffset) == 1U, kernel.Sends == 0U, kernel.Other == 0U,
                   kernel.Controls == 2U, kernel.Operations[0U] == Abi::DeviceEnableBusMaster,
                   kernel.Operations[1U] == Abi::DeviceDisableBusMaster, kernel.Function == 0x73U);

    DriverKernel mastering{};
    mastering.Mastering = Abi::SystemCallError::None;
    const User::System::SystemCallTransport masteringTransport{&DriverKernel::Transport, &mastering};
    const UInt64 unconfined = User::Component::RunComponent(masteringTransport, block, Script::Clock, Script::Crash);
    ASTERIA_EXPECT(runner, "a driver granted bus mastering without confinement stops before mapping",
                   unconfined == Failure(Abi::ComponentDriverBusMasterStep), mastering.Controls == 1U,
                   mastering.Creates == 0U);
}
// Serves a scripted echo: the first request came through the wrong
// capability, the second's reply cannot be sent, then the Endpoint is empty
// and the next wait fails. Records every Close and Send.
struct EchoKernel
{
    UInt8 WrongSelector[User::Component::EchoDeliveryHeaderBytes + User::Component::EchoRequestBytes + 8U]{};
    UInt8 Valid[User::Component::EchoDeliveryHeaderBytes + User::Component::EchoRequestBytes + 8U]{};
    Size Waits{0U};
    Size Receives{0U};
    Size Sends{0U};
    UInt64 Answer{0U};
    UInt64 Closed[8U]{};
    Size Closes{0U};

    static void Transport(void* const context, const Abi::SystemCallRequest* const request,
                          Abi::SystemCallResponse* const response) noexcept
    {
        auto& kernel = *static_cast<EchoKernel*>(context);
        *response = Abi::SystemCallResponse{.Error = Abi::SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        switch (static_cast<Abi::SystemCallNumber>(request->Number))
        {
        case Abi::SystemCallNumber::WaitSetWait:
            response->Value0 = User::Component::EchoWaitTag;
            response->Error = (kernel.Waits++ == 0U) ? Abi::SystemCallError::None : Abi::SystemCallError::InvalidHandle;
            return;
        case Abi::SystemCallNumber::IpcReceive:
        {
            const UInt8* const source = (kernel.Receives == 0U) ? kernel.WrongSelector : kernel.Valid;
            if (kernel.Receives++ >= 2U)
            {
                response->Error = Abi::SystemCallError::WouldBlock;
                return;
            }
            auto* const target = reinterpret_cast<UInt8*>(static_cast<Foundation::UIntPointer>(request->Arguments[2U]));
            for (Size index{0U}; index < sizeof(kernel.Valid); ++index)
            {
                target[index] = source[index];
            }
            response->Value1 = sizeof(kernel.Valid);
            return;
        }
        case Abi::SystemCallNumber::IpcSend:
            ++kernel.Sends;
            kernel.Answer = request->Arguments[0U];
            response->Error = Abi::SystemCallError::ObjectNotFound;
            return;
        case Abi::SystemCallNumber::CapabilityClose:
            kernel.Closed[kernel.Closes++ % 8U] = request->Arguments[0U];
            response->Error = (kernel.Closes == 1U) ? Abi::SystemCallError::InvalidHandle : Abi::SystemCallError::None;
            return;
        default:
            response->Error = Abi::SystemCallError::UnknownSystemCall;
            return;
        }
    }
};

void WriteDelivery(UInt8* const delivery, const UInt64 selector) noexcept
{
    const User::Component::EchoAttachment attachments[2U]{
        {.Handle = {0x0101U, 0x11U},
         .Rights = User::Component::EchoInvokeRight,
         .Mode = User::Component::EchoCopy,
         .Type = User::Component::EchoEndpointType},
        {.Handle = {0x0202U, 0x22U},
         .Rights = User::Component::EchoFrameRights,
         .Mode = User::Component::EchoMove,
         .Type = User::Component::EchoFrameType},
    };
    User::Component::WriteEchoFrame(delivery + User::Component::EchoDeliveryHeaderBytes, attachments, 2U);
    auto* const frame = delivery + User::Component::EchoDeliveryHeaderBytes;
    for (Size byte{User::Component::EchoRequestBytes}; byte > User::Component::EchoHeaderBytes; --byte)
    {
        frame[byte + 7U] = frame[byte - 1U];
    }
    for (Size byte{0U}; byte < 8U; ++byte)
    {
        frame[User::Component::EchoHeaderBytes + byte] = 0U;
    }
    frame[80U] = 1U; // One canonical empty variable-payload descriptor.
    frame[8U] = static_cast<UInt8>(User::Component::EchoRequestBytes + 8U);
    for (Size byte{0U}; byte < 8U; ++byte)
    {
        delivery[User::Component::EchoDeliverySealedSelectorOffset + byte] =
            static_cast<UInt8>(selector >> (8U * byte));
    }
}

// Service IPC: a request the echo cannot serve is dropped with its capabilities closed, unanswered,
// and a reply it cannot send is dropped with the capability it would have moved back; neither ends
// the service, whose own failed wait does. A stale handle's Close is survived.
void TestEchoSurvivesItsRequests(TestContext& runner) noexcept
{
    EchoKernel kernel;
    WriteDelivery(kernel.WrongSelector, 0x5EA1U);
    WriteDelivery(kernel.Valid, User::Component::EchoSealedSelector);
    const User::System::SystemCallTransport transport{&EchoKernel::Transport, &kernel};
    UInt64 block[Abi::ComponentBlockWords]{};
    for (Size index{0U}; index < Abi::ComponentBlockWords; ++index)
    {
        block[index] = Word(index, Abi::ComponentBehaviorEcho);
    }
    const UInt64 ended = User::Component::RunComponent(transport, block, Script::Clock, Script::Crash);
    ASTERIA_EXPECT(runner, "the echo drops what it cannot serve or answer and ends only on its own failure",
                   ended == Abi::ComponentEchoFailedExitCode, kernel.Waits == 2U, kernel.Receives == 3U,
                   kernel.Sends == 1U, kernel.Answer == 0x0101U, kernel.Closes == 4U, kernel.Closed[0U] == 0x0101U,
                   kernel.Closed[1U] == 0x0202U, kernel.Closed[2U] == 0x0202U, kernel.Closed[3U] == 0x0101U);
}

// Component launch: the supervisor's own channel receives the kernel identity before the local tag
// and resolves through the received token.
struct SupervisorKernel
{
    static constexpr Size Payload{User::Component::EchoDeliveryHeaderBytes + User::Component::EchoHeaderBytes +
                                  User::Component::EchoAttachmentBytes};
    UInt8 Delivery[Payload + Abi::PagerFaultPayloadBytes]{};
    Size Calls{0U};
    Size FailCall{0U};
    bool Authority{true};
    bool Terminated{false};

    static void Transport(void* const context, const Abi::SystemCallRequest* const request,
                          Abi::SystemCallResponse* const response) noexcept
    {
        auto& kernel = *static_cast<SupervisorKernel*>(context);
        ++kernel.Calls;
        *response = {.Error = kernel.Calls == kernel.FailCall ? Abi::SystemCallError::RightsDenied
                                                              : Abi::SystemCallError::None,
                     .Value0 = 0U,
                     .Value1 = 0U};
        if (response->Error != Abi::SystemCallError::None)
        {
            return;
        }
        switch (static_cast<Abi::SystemCallNumber>(request->Number))
        {
        case Abi::SystemCallNumber::ObjectCreate:
            kernel.Authority =
                kernel.Authority && request->Arguments[0U] == 0x61U &&
                request->Arguments[2U] == (kernel.Calls == 1U ? Abi::ObjectTypeFaultChannel : Abi::ObjectTypeWaitSet) &&
                request->Arguments[4U] == 8U;
            response->Value0 = 0x100U + kernel.Calls;
            response->Value1 = 0x200U + kernel.Calls;
            return;
        case Abi::SystemCallNumber::WaitSetBind:
            kernel.Authority = kernel.Authority && request->Arguments[0U] == 0x102U && request->Arguments[2U] == 0x101U;
            return;
        case Abi::SystemCallNumber::IpcSend:
            kernel.Authority = kernel.Authority && request->Arguments[0U] == 0x31U;
            return;
        case Abi::SystemCallNumber::WaitSetWait:
            kernel.Authority = kernel.Authority && request->Arguments[0U] == 0x102U;
            response->Value0 = User::Component::EchoWaitTag;
            return;
        case Abi::SystemCallNumber::IpcReceive:
        {
            kernel.Authority = kernel.Authority && request->Arguments[0U] == 0x101U;
            auto* const output = reinterpret_cast<UInt8*>(static_cast<Foundation::UIntPointer>(request->Arguments[2U]));
            for (Size i{0U}; i < sizeof(kernel.Delivery); ++i)
            {
                output[i] = kernel.Delivery[i];
            }
            response->Value1 = sizeof(kernel.Delivery);
            return;
        }
        case Abi::SystemCallNumber::FaultResolve:
            kernel.Terminated = request->Arguments[0U] == 0x103U && request->Arguments[1U] == 0x203U &&
                                request->Arguments[2U] == static_cast<UInt64>(Abi::FaultResolveKind::Terminate);
            return;
        default:
            kernel.Authority = false;
            return;
        }
    }
};

void TestFaultSupervisor(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    for (Size i{0U}; i < Abi::ComponentBlockWords; ++i)
    {
        block[i] = Word(i, Abi::ComponentBehaviorFaultSupervisor);
    }
    block[Abi::ComponentAccountWord] = 0x61U;
    block[Abi::ComponentAccountWord + 1U] = 0x62U;
    SupervisorKernel kernel{};
    constexpr Size frame{User::Component::EchoDeliveryHeaderBytes};
    const User::Component::EchoAttachment token{
        {0x103U, 0x203U}, User::Component::EchoInvokeRight, User::Component::EchoMove, 20U};
    User::Component::WriteEchoFrame(kernel.Delivery + frame, &token, 1U);
    kernel.Delivery[frame + 36U] = Abi::PagerProtocolMajor;
    kernel.Delivery[frame + 38U] = Abi::PagerProtocolMinor;
    kernel.Delivery[SupervisorKernel::Payload + Abi::PagerFaultTagOffset] = 8U;
    kernel.Delivery[SupervisorKernel::Payload + Abi::PagerFaultIdentityOffset] = 0x71U;
    const User::System::SystemCallTransport transport{&SupervisorKernel::Transport, &kernel};
    const auto result = User::Component::RunComponent(transport, block, Script::Clock, Script::Crash);
    ASTERIA_EXPECT(runner, "a created supervisor creates, binds, receives and terminates through its own channel",
                   result == Abi::ComponentFaultSupervisorExitCode, kernel.Authority, kernel.Terminated,
                   kernel.Calls == 7U);
    bool refusals{true};
    for (Size fail{1U}; fail <= 7U; ++fail)
    {
        kernel.Calls = 0U;
        kernel.FailCall = fail;
        kernel.Terminated = false;
        const auto refused = User::Component::RunComponent(transport, block, Script::Clock, Script::Crash);
        const UInt64 step = fail == 7U ? 7U : fail == 3U ? 2U : fail > 3U ? fail - 1U : fail;
        refusals = refusals && refused == (Abi::ComponentFaultSupervisorFailedExitCode | (step << 32U)) &&
                   kernel.Calls == fail && !kernel.Terminated;
    }
    ASTERIA_EXPECT(runner, "each failed supervisor operation stops the fault exchange", refusals);
    kernel.FailCall = 0U;
    bool invalid{true};
    const Size offsets[]{frame + 38U, SupervisorKernel::Payload + Abi::PagerFaultIdentityOffset,
                         SupervisorKernel::Payload + Abi::PagerFaultTagOffset};
    for (const Size offset : offsets)
    {
        const UInt8 saved = kernel.Delivery[offset];
        kernel.Delivery[offset] = 0U;
        kernel.Calls = 0U;
        kernel.Terminated = false;
        const auto refused = User::Component::RunComponent(transport, block, Script::Clock, Script::Crash);
        invalid = invalid && refused != Abi::ComponentFaultSupervisorExitCode && !kernel.Terminated;
        kernel.Delivery[offset] = saved;
    }
    ASTERIA_EXPECT(runner, "protocol, identity and tag refusals preserve the received token", invalid);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"ComponentProgramTests"};
    Asteria::Test::TestIncarnations(runner);
    Asteria::Test::TestAccount(runner);
    Asteria::Test::TestDriver(runner);
    Asteria::Test::TestEchoSurvivesItsRequests(runner);
    Asteria::Test::TestFaultSupervisor(runner);
    return runner.Finish();
}
