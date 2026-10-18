// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "User/Platform/Ia64/Runtime/Ia64BssZeroInitializationProbe.hpp"
#include "User/Platform/Ia64/Runtime/Ia64MinimalUserImageRuntime.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{

using User::Platform::Ia64::Runtime::Ia64MinimalUserImageIpcDeliverySizeBytes;
using User::Platform::Ia64::Runtime::Ia64MinimalUserImageIpcFrameSizeBytes;

constexpr Foundation::UInt64 BootstrapSelector{0x100U};
constexpr Foundation::UInt64 BootstrapAuthenticator{0xA5A5'0123'4567'89ABULL};

constexpr Foundation::UInt8 ExpectedFrame[Ia64MinimalUserImageIpcFrameSizeBytes]{
    0x41U, 0x49U, 0x50U, 0x43U, 0x01U, 0x00U, 0x58U, 0x00U, 0x58U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};

constexpr Foundation::UInt8 ExpectedDeliveryHeader[48U]{
    0x41U, 0x49U, 0x50U, 0x44U, 0x01U, 0x00U, 0x30U, 0x00U, 0x88U, 0x00U, 0x00U, 0x00U, 0x58U, 0x00U, 0x07U, 0x00U,
    0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};

[[nodiscard]] constexpr Abi::SystemCallResponse QuerySuccess() noexcept
{
    return Abi::SystemCallResponse{
        .Error = Abi::SystemCallError::None,
        .Value0 = User::Platform::Ia64::Runtime::Ia64MinimalUserImageExpectedEndpointInformation,
        .Value1 = User::Platform::Ia64::Runtime::Ia64MinimalUserImageInfiniteLeaseDeadline,
    };
}

[[nodiscard]] constexpr Abi::SystemCallResponse SendSuccess() noexcept
{
    return Abi::SystemCallResponse{
        .Error = Abi::SystemCallError::None,
        .Value0 = User::Platform::Ia64::Runtime::Ia64MinimalUserImageIpcSequence,
        .Value1 = 0U,
    };
}

[[nodiscard]] constexpr Abi::SystemCallResponse ReceiveSuccess() noexcept
{
    return Abi::SystemCallResponse{
        .Error = Abi::SystemCallError::None,
        .Value0 = User::Platform::Ia64::Runtime::Ia64MinimalUserImageIpcSequence,
        .Value1 = Ia64MinimalUserImageIpcDeliverySizeBytes,
    };
}

[[nodiscard]] constexpr Abi::SystemCallResponse ResetResponse() noexcept
{
    return Abi::SystemCallResponse{
        .Error = Abi::SystemCallError::InvalidConfiguration,
        .Value0 = 0U,
        .Value1 = 0U,
    };
}

struct ScriptedTransportContext
{
    Abi::SystemCallResponse Responses[3U];
    Abi::SystemCallRequest Requests[3U];
    Foundation::Size CallCount;
    Foundation::Size DeliveryMutationOffset;
    Foundation::UInt8 DeliveryMutationMask;
    bool PublishDelivery;
    bool ProbeWasObserved[3U];
    bool CompletionWasClear[3U];
    bool SendFrameMatched;
};

[[nodiscard]] bool BytesMatch(const Foundation::UInt8* const left, const Foundation::UInt8* const right,
                              const Foundation::Size count) noexcept
{
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (left[index] != right[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool BytesAreZero(const Foundation::UInt8* const bytes, const Foundation::Size count) noexcept
{
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

void PublishDelivery(Foundation::UInt8* const destination, const ScriptedTransportContext& script) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(ExpectedDeliveryHeader); ++index)
    {
        destination[index] = ExpectedDeliveryHeader[index];
    }
    for (Foundation::Size index{0U}; index < sizeof(ExpectedFrame); ++index)
    {
        destination[sizeof(ExpectedDeliveryHeader) + index] = ExpectedFrame[index];
    }
    if (script.DeliveryMutationMask != 0U)
    {
        destination[script.DeliveryMutationOffset] ^= script.DeliveryMutationMask;
    }
}

void InvokeScriptedTransport(void* const context, const Abi::SystemCallRequest* const request,
                             Abi::SystemCallResponse* const response) noexcept
{
    auto* const script = static_cast<ScriptedTransportContext*>(context);
    const Foundation::Size callIndex = script->CallCount;
    if (callIndex >= 3U)
    {
        *response = Abi::SystemCallResponse{
            .Error = Abi::SystemCallError::InvalidArguments,
            .Value0 = 0U,
            .Value1 = 0U,
        };
        ++script->CallCount;
        return;
    }

    script->Requests[callIndex] = *request;
    script->ProbeWasObserved[callIndex] = User::Platform::Ia64::Runtime::g_bssZeroInitializationProbe ==
                                          User::Platform::Ia64::Runtime::BssZeroInitializationPassedSentinel;
    script->CompletionWasClear[callIndex] = User::Platform::Ia64::Runtime::g_ipcCompletionSentinel == 0U;

    if (request->Number == static_cast<Foundation::UInt64>(Abi::SystemCallNumber::IpcSend))
    {
        const auto* const frame =
            reinterpret_cast<const Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(request->Arguments[2U]));
        script->SendFrameMatched = (request->Arguments[3U] == Ia64MinimalUserImageIpcFrameSizeBytes) &&
                                   BytesMatch(frame, ExpectedFrame, sizeof(ExpectedFrame));
    }
    if ((request->Number == static_cast<Foundation::UInt64>(Abi::SystemCallNumber::IpcReceive)) &&
        script->PublishDelivery)
    {
        auto* const delivery =
            reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(request->Arguments[2U]));
        PublishDelivery(delivery, *script);
    }

    *response = script->Responses[callIndex];
    ++script->CallCount;
}

[[nodiscard]] ScriptedTransportContext MakeSuccessfulScript() noexcept
{
    return ScriptedTransportContext{
        .Responses = {QuerySuccess(), SendSuccess(), ReceiveSuccess()},
        .Requests = {},
        .CallCount = 0U,
        .DeliveryMutationOffset = 0U,
        .DeliveryMutationMask = 0U,
        .PublishDelivery = true,
        .ProbeWasObserved = {},
        .CompletionWasClear = {},
        .SendFrameMatched = false,
    };
}

void SeedEvidence(const Foundation::UInt64 bssValue) noexcept
{
    User::Platform::Ia64::Runtime::g_bssZeroInitializationProbe = bssValue;
    User::Platform::Ia64::Runtime::g_querySystemCallResponse = {
        .Error = Abi::SystemCallError::Expired,
        .Value0 = 0x1111U,
        .Value1 = 0x2222U,
    };
    User::Platform::Ia64::Runtime::g_sendSystemCallResponse = {
        .Error = Abi::SystemCallError::Revoked,
        .Value0 = 0x3333U,
        .Value1 = 0x4444U,
    };
    User::Platform::Ia64::Runtime::g_receiveSystemCallResponse = {
        .Error = Abi::SystemCallError::RightsDenied,
        .Value0 = 0x5555U,
        .Value1 = 0x6666U,
    };
    User::Platform::Ia64::Runtime::g_lastSystemCallResponse = {
        .Error = Abi::SystemCallError::ObjectNotFound,
        .Value0 = 0x7777U,
        .Value1 = 0x8888U,
    };
    for (Foundation::Size index{0U}; index < Ia64MinimalUserImageIpcFrameSizeBytes; ++index)
    {
        User::Platform::Ia64::Runtime::g_ipcFrame[index] = 0x5AU;
    }
    for (Foundation::Size index{0U}; index < Ia64MinimalUserImageIpcDeliverySizeBytes; ++index)
    {
        User::Platform::Ia64::Runtime::g_ipcDelivery[index] = 0xA5U;
    }
    User::Platform::Ia64::Runtime::g_ipcCompletionSentinel = 0xDEAD'BEEF'CAFE'BABEULL;
}

[[nodiscard]] Foundation::UInt64 Run(ScriptedTransportContext& script, const Foundation::UInt64 bssValue = 0U) noexcept
{
    SeedEvidence(bssValue);
    const User::System::SystemCallTransport transport{
        .Function = InvokeScriptedTransport,
        .Context = &script,
    };
    return User::Platform::Ia64::Runtime::RunIa64MinimalUserImage(BootstrapSelector, BootstrapAuthenticator, transport);
}

[[nodiscard]] bool ResponseMatches(const Abi::SystemCallResponse& response,
                                   const Abi::SystemCallResponse& expected) noexcept
{
    return (response.Error == expected.Error) && (response.Value0 == expected.Value0) &&
           (response.Value1 == expected.Value1);
}

[[nodiscard]] bool RequestMatches(const Abi::SystemCallRequest& request, const Abi::SystemCallNumber number,
                                  const Foundation::UInt64 argument2, const Foundation::UInt64 argument3) noexcept
{
    return (request.Number == static_cast<Foundation::UInt64>(number)) &&
           (request.Arguments[0U] == BootstrapSelector) && (request.Arguments[1U] == BootstrapAuthenticator) &&
           (request.Arguments[2U] == argument2) && (request.Arguments[3U] == argument3) &&
           (request.Arguments[4U] == 0U) && (request.Arguments[5U] == 0U);
}

[[nodiscard]] Foundation::UInt64 AddressOf(const void* const storage) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(storage));
}

void CheckIncompleteEvidence(TestContext& runner, const char* const description) noexcept
{
    ASTERIA_EXPECT(runner, description, User::Platform::Ia64::Runtime::g_ipcCompletionSentinel == 0U);
}

void CheckCallPrefix(TestContext& runner, const ScriptedTransportContext& script,
                     const Foundation::Size expectedCalls) noexcept
{
    ASTERIA_EXPECT(runner, "runtime stops at the exact scripted failure cut", script.CallCount == expectedCalls);
    for (Foundation::Size index{0U}; index < expectedCalls; ++index)
    {
        ASTERIA_EXPECT(runner, "BSS pass sentinel precedes every system call", script.ProbeWasObserved[index]);
        ASTERIA_EXPECT(runner, "IPC completion remains clear during every system call",
                       script.CompletionWasClear[index]);
    }
}

void TestBssFailureShortCircuits(TestContext& runner) noexcept
{
    ScriptedTransportContext script = MakeSuccessfulScript();
    const Foundation::UInt64 result = Run(script, 1U);

    ASTERIA_EXPECT(
        runner, "nonzero BSS returns the dedicated first failure",
        result == static_cast<Foundation::UInt64>(
                      User::Platform::Ia64::Runtime::Ia64MinimalUserImageRuntimeFailure::BssInitializationRejected));
    ASTERIA_EXPECT(runner, "BSS failure issues no system call", script.CallCount == 0U);
    ASTERIA_EXPECT(runner, "BSS failure sentinel is retained",
                   User::Platform::Ia64::Runtime::g_bssZeroInitializationProbe ==
                       User::Platform::Ia64::Runtime::BssZeroInitializationFailedSentinel);
    ASTERIA_EXPECT(runner, "BSS failure resets all operation responses",
                   ResponseMatches(User::Platform::Ia64::Runtime::g_querySystemCallResponse, ResetResponse()),
                   ResponseMatches(User::Platform::Ia64::Runtime::g_sendSystemCallResponse, ResetResponse()),
                   ResponseMatches(User::Platform::Ia64::Runtime::g_receiveSystemCallResponse, ResetResponse()));
    ASTERIA_EXPECT(runner, "BSS failure still retains the canonical frame",
                   BytesMatch(User::Platform::Ia64::Runtime::g_ipcFrame, ExpectedFrame, sizeof(ExpectedFrame)));
    ASTERIA_EXPECT(
        runner, "BSS failure clears stale delivery evidence",
        BytesAreZero(User::Platform::Ia64::Runtime::g_ipcDelivery, Ia64MinimalUserImageIpcDeliverySizeBytes));
    CheckIncompleteEvidence(runner, "BSS failure does not publish IPC completion");
}

void TestInvalidTransportShortCircuitsAtQuery(TestContext& runner) noexcept
{
    SeedEvidence(0U);
    const Foundation::UInt64 result =
        User::Platform::Ia64::Runtime::RunIa64MinimalUserImage(BootstrapSelector, BootstrapAuthenticator,
                                                               User::System::SystemCallTransport{
                                                                   .Function = nullptr,
                                                                   .Context = nullptr,
                                                               });

    ASTERIA_EXPECT(runner, "invalid transport returns the Query system-call error",
                   result == static_cast<Foundation::UInt64>(Abi::SystemCallError::InvalidConfiguration));
    ASTERIA_EXPECT(runner, "invalid transport retains the Query error in both query records",
                   ResponseMatches(User::Platform::Ia64::Runtime::g_querySystemCallResponse, ResetResponse()),
                   ResponseMatches(User::Platform::Ia64::Runtime::g_lastSystemCallResponse, ResetResponse()));
    ASTERIA_EXPECT(runner, "invalid transport leaves later responses reset",
                   ResponseMatches(User::Platform::Ia64::Runtime::g_sendSystemCallResponse, ResetResponse()),
                   ResponseMatches(User::Platform::Ia64::Runtime::g_receiveSystemCallResponse, ResetResponse()));
    CheckIncompleteEvidence(runner, "invalid transport does not publish IPC completion");
}

void TestQuerySystemCallFailure(TestContext& runner) noexcept
{
    ScriptedTransportContext script = MakeSuccessfulScript();
    script.Responses[0U] = Abi::SystemCallResponse{
        .Error = Abi::SystemCallError::RightsDenied,
        .Value0 = 0x1111U,
        .Value1 = 0x2222U,
    };
    const Foundation::UInt64 result = Run(script);

    ASTERIA_EXPECT(runner, "Query syscall failure is returned unchanged",
                   result == static_cast<Foundation::UInt64>(Abi::SystemCallError::RightsDenied));
    CheckCallPrefix(runner, script, 1U);
    ASTERIA_EXPECT(runner, "Query syscall failure is retained in both query records",
                   ResponseMatches(User::Platform::Ia64::Runtime::g_querySystemCallResponse, script.Responses[0U]),
                   ResponseMatches(User::Platform::Ia64::Runtime::g_lastSystemCallResponse, script.Responses[0U]));
    ASTERIA_EXPECT(runner, "Query syscall failure prevents Send",
                   ResponseMatches(User::Platform::Ia64::Runtime::g_sendSystemCallResponse, ResetResponse()));
    CheckIncompleteEvidence(runner, "Query syscall failure does not publish IPC completion");
}

void ExerciseRejectedQueryMetadata(TestContext& runner, const Abi::SystemCallResponse query,
                                   const char* const description) noexcept
{
    ScriptedTransportContext script = MakeSuccessfulScript();
    script.Responses[0U] = query;
    const Foundation::UInt64 result = Run(script);

    ASTERIA_EXPECT(runner, description,
                   result ==
                       static_cast<Foundation::UInt64>(
                           User::Platform::Ia64::Runtime::Ia64MinimalUserImageRuntimeFailure::QueryResponseRejected));
    CheckCallPrefix(runner, script, 1U);
    ASTERIA_EXPECT(runner, "rejected Query metadata remains observable",
                   ResponseMatches(User::Platform::Ia64::Runtime::g_querySystemCallResponse, query));
    ASTERIA_EXPECT(runner, "rejected Query metadata prevents Send",
                   ResponseMatches(User::Platform::Ia64::Runtime::g_sendSystemCallResponse, ResetResponse()));
    CheckIncompleteEvidence(runner, "rejected Query metadata does not publish IPC completion");
}

void TestQueryMetadataFailures(TestContext& runner) noexcept
{
    Abi::SystemCallResponse wrongType = QuerySuccess();
    wrongType.Value0 = 0x0000'0000'0007'0008ULL;
    ExerciseRejectedQueryMetadata(runner, wrongType, "Query rejects a non-Endpoint object type");

    Abi::SystemCallResponse wrongRights = QuerySuccess();
    wrongRights.Value0 = 0x0000'0000'0006'0009ULL;
    ExerciseRejectedQueryMetadata(runner, wrongRights, "Query rejects rights other than raw Inspect|Invoke|Read");

    Abi::SystemCallResponse reservedBits = QuerySuccess();
    reservedBits.Value0 |= Foundation::UInt64{1U} << 8U;
    ExerciseRejectedQueryMetadata(runner, reservedBits, "Query rejects nonzero packed-metadata reserved bits");

    Abi::SystemCallResponse finiteLease = QuerySuccess();
    --finiteLease.Value1;
    ExerciseRejectedQueryMetadata(runner, finiteLease, "Query rejects a finite bootstrap lease");
}

void TestSendSystemCallFailure(TestContext& runner) noexcept
{
    ScriptedTransportContext script = MakeSuccessfulScript();
    script.Responses[1U] = Abi::SystemCallResponse{
        .Error = Abi::SystemCallError::WouldBlock,
        .Value0 = 0x3333U,
        .Value1 = 0x4444U,
    };
    const Foundation::UInt64 result = Run(script);

    ASTERIA_EXPECT(runner, "Send syscall failure is returned unchanged",
                   result == static_cast<Foundation::UInt64>(Abi::SystemCallError::WouldBlock));
    CheckCallPrefix(runner, script, 2U);
    ASTERIA_EXPECT(runner, "Send observes the exact AIPC frame", script.SendFrameMatched);
    ASTERIA_EXPECT(runner, "Send failure retains successful Query and exact Send response",
                   ResponseMatches(User::Platform::Ia64::Runtime::g_querySystemCallResponse, QuerySuccess()),
                   ResponseMatches(User::Platform::Ia64::Runtime::g_sendSystemCallResponse, script.Responses[1U]));
    ASTERIA_EXPECT(runner, "Send syscall failure prevents Receive",
                   ResponseMatches(User::Platform::Ia64::Runtime::g_receiveSystemCallResponse, ResetResponse()));
    CheckIncompleteEvidence(runner, "Send syscall failure does not publish IPC completion");
}

void ExerciseRejectedSendResponse(TestContext& runner, const Abi::SystemCallResponse send,
                                  const char* const description) noexcept
{
    ScriptedTransportContext script = MakeSuccessfulScript();
    script.Responses[1U] = send;
    const Foundation::UInt64 result = Run(script);

    ASTERIA_EXPECT(runner, description,
                   result ==
                       static_cast<Foundation::UInt64>(
                           User::Platform::Ia64::Runtime::Ia64MinimalUserImageRuntimeFailure::SendResponseRejected));
    CheckCallPrefix(runner, script, 2U);
    ASTERIA_EXPECT(runner, "rejected Send used exact AIPC bytes", script.SendFrameMatched);
    ASTERIA_EXPECT(runner, "rejected Send response remains observable",
                   ResponseMatches(User::Platform::Ia64::Runtime::g_sendSystemCallResponse, send));
    ASTERIA_EXPECT(runner, "rejected Send response prevents Receive",
                   ResponseMatches(User::Platform::Ia64::Runtime::g_receiveSystemCallResponse, ResetResponse()));
    CheckIncompleteEvidence(runner, "rejected Send response does not publish IPC completion");
}

void TestSendResponseFailures(TestContext& runner) noexcept
{
    Abi::SystemCallResponse wrongSequence = SendSuccess();
    ++wrongSequence.Value0;
    ExerciseRejectedSendResponse(runner, wrongSequence, "Send rejects the wrong Endpoint sequence");

    Abi::SystemCallResponse wrongTransferredBytes = SendSuccess();
    wrongTransferredBytes.Value1 = Ia64MinimalUserImageIpcFrameSizeBytes;
    ExerciseRejectedSendResponse(runner, wrongTransferredBytes, "Send rejects a nonzero transferred-byte result");
}

void TestReceiveSystemCallFailure(TestContext& runner) noexcept
{
    ScriptedTransportContext script = MakeSuccessfulScript();
    script.Responses[2U] = Abi::SystemCallResponse{
        .Error = Abi::SystemCallError::WouldBlock,
        .Value0 = 0x5555U,
        .Value1 = 0x6666U,
    };
    script.PublishDelivery = false;
    const Foundation::UInt64 result = Run(script);

    ASTERIA_EXPECT(runner, "Receive syscall failure is returned unchanged",
                   result == static_cast<Foundation::UInt64>(Abi::SystemCallError::WouldBlock));
    CheckCallPrefix(runner, script, 3U);
    ASTERIA_EXPECT(runner, "Receive syscall failure remains observable",
                   ResponseMatches(User::Platform::Ia64::Runtime::g_receiveSystemCallResponse, script.Responses[2U]));
    ASTERIA_EXPECT(
        runner, "Receive syscall failure retains the cleared delivery buffer",
        BytesAreZero(User::Platform::Ia64::Runtime::g_ipcDelivery, Ia64MinimalUserImageIpcDeliverySizeBytes));
    CheckIncompleteEvidence(runner, "Receive syscall failure does not publish IPC completion");
}

void ExerciseRejectedReceiveResponse(TestContext& runner, const Abi::SystemCallResponse receive,
                                     const char* const description) noexcept
{
    ScriptedTransportContext script = MakeSuccessfulScript();
    script.Responses[2U] = receive;
    const Foundation::UInt64 result = Run(script);

    ASTERIA_EXPECT(runner, description,
                   result ==
                       static_cast<Foundation::UInt64>(
                           User::Platform::Ia64::Runtime::Ia64MinimalUserImageRuntimeFailure::ReceiveResponseRejected));
    CheckCallPrefix(runner, script, 3U);
    ASTERIA_EXPECT(runner, "rejected Receive response remains observable",
                   ResponseMatches(User::Platform::Ia64::Runtime::g_receiveSystemCallResponse, receive));
    CheckIncompleteEvidence(runner, "rejected Receive response does not publish IPC completion");
}

void TestReceiveResponseFailures(TestContext& runner) noexcept
{
    Abi::SystemCallResponse wrongSequence = ReceiveSuccess();
    ++wrongSequence.Value0;
    ExerciseRejectedReceiveResponse(runner, wrongSequence, "Receive rejects a sequence different from Send");

    Abi::SystemCallResponse wrongDeliverySize = ReceiveSuccess();
    --wrongDeliverySize.Value1;
    ExerciseRejectedReceiveResponse(runner, wrongDeliverySize,
                                    "Receive rejects a delivery extent other than 136 bytes");
}

void ExerciseRejectedDelivery(TestContext& runner, const Foundation::Size mutationOffset,
                              const char* const description) noexcept
{
    ScriptedTransportContext script = MakeSuccessfulScript();
    script.DeliveryMutationOffset = mutationOffset;
    script.DeliveryMutationMask = 1U;
    const Foundation::UInt64 result = Run(script);

    ASTERIA_EXPECT(runner, description,
                   result == static_cast<Foundation::UInt64>(
                                 User::Platform::Ia64::Runtime::Ia64MinimalUserImageRuntimeFailure::DeliveryRejected));
    CheckCallPrefix(runner, script, 3U);
    ASTERIA_EXPECT(runner, "delivery rejection follows an exact successful Receive response",
                   ResponseMatches(User::Platform::Ia64::Runtime::g_receiveSystemCallResponse, ReceiveSuccess()));
    CheckIncompleteEvidence(runner, "delivery rejection does not publish IPC completion");
}

void TestDeliveryFailures(TestContext& runner) noexcept
{
    ExerciseRejectedDelivery(runner, 0U, "delivery rejects a mutated AIPD header byte");
    ExerciseRejectedDelivery(runner, sizeof(ExpectedDeliveryHeader) + 32U,
                             "delivery rejects a mutated embedded AIPC frame byte");
}

void TestSuccessfulLoopback(TestContext& runner) noexcept
{
    ScriptedTransportContext script = MakeSuccessfulScript();
    const Foundation::UInt64 result = Run(script);

    ASTERIA_EXPECT(runner, "exact loopback completes successfully",
                   result == static_cast<Foundation::UInt64>(
                                 User::Platform::Ia64::Runtime::Ia64MinimalUserImageRuntimeFailure::None));
    CheckCallPrefix(runner, script, 3U);
    ASTERIA_EXPECT(runner, "Query request uses the exact bootstrap handle and zero unused fields",
                   RequestMatches(script.Requests[0U], Abi::SystemCallNumber::CapabilityQuery, 0U, 0U));
    ASTERIA_EXPECT(runner, "Send request names the retained frame and exact 88-byte extent",
                   RequestMatches(script.Requests[1U], Abi::SystemCallNumber::IpcSend,
                                  AddressOf(User::Platform::Ia64::Runtime::g_ipcFrame),
                                  Ia64MinimalUserImageIpcFrameSizeBytes));
    ASTERIA_EXPECT(runner, "Receive request names the retained delivery and exact 136-byte capacity",
                   RequestMatches(script.Requests[2U], Abi::SystemCallNumber::IpcReceive,
                                  AddressOf(User::Platform::Ia64::Runtime::g_ipcDelivery),
                                  Ia64MinimalUserImageIpcDeliverySizeBytes));
    ASTERIA_EXPECT(runner, "Send frame is the exact canonical AIPC frame", script.SendFrameMatched);
    ASTERIA_EXPECT(runner, "success retains three distinct exact syscall responses",
                   ResponseMatches(User::Platform::Ia64::Runtime::g_querySystemCallResponse, QuerySuccess()),
                   ResponseMatches(User::Platform::Ia64::Runtime::g_sendSystemCallResponse, SendSuccess()),
                   ResponseMatches(User::Platform::Ia64::Runtime::g_receiveSystemCallResponse, ReceiveSuccess()));
    ASTERIA_EXPECT(runner, "compatibility response remains the exact Query response",
                   ResponseMatches(User::Platform::Ia64::Runtime::g_lastSystemCallResponse, QuerySuccess()));
    ASTERIA_EXPECT(runner, "success retains exact canonical AIPC bytes",
                   BytesMatch(User::Platform::Ia64::Runtime::g_ipcFrame, ExpectedFrame, sizeof(ExpectedFrame)));
    ASTERIA_EXPECT(runner, "success retains exact AIPD header and embedded AIPC bytes",
                   BytesMatch(User::Platform::Ia64::Runtime::g_ipcDelivery, ExpectedDeliveryHeader,
                              sizeof(ExpectedDeliveryHeader)),
                   BytesMatch(User::Platform::Ia64::Runtime::g_ipcDelivery + sizeof(ExpectedDeliveryHeader),
                              ExpectedFrame, sizeof(ExpectedFrame)));
    ASTERIA_EXPECT(runner, "completion sentinel is published only after exact delivery validation",
                   User::Platform::Ia64::Runtime::g_ipcCompletionSentinel ==
                       User::Platform::Ia64::Runtime::Ia64MinimalUserImageIpcCompletionValue);
}

static_assert(sizeof(ExpectedFrame) == 88U);
static_assert(sizeof(ExpectedDeliveryHeader) + sizeof(ExpectedFrame) == Ia64MinimalUserImageIpcDeliverySizeBytes);
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64MinimalUserImageRuntimeTests"};
    Asteria::Test::TestBssFailureShortCircuits(runner);
    Asteria::Test::TestInvalidTransportShortCircuitsAtQuery(runner);
    Asteria::Test::TestQuerySystemCallFailure(runner);
    Asteria::Test::TestQueryMetadataFailures(runner);
    Asteria::Test::TestSendSystemCallFailure(runner);
    Asteria::Test::TestSendResponseFailures(runner);
    Asteria::Test::TestReceiveSystemCallFailure(runner);
    Asteria::Test::TestReceiveResponseFailures(runner);
    Asteria::Test::TestDeliveryFailures(runner);
    Asteria::Test::TestSuccessfulLoopback(runner);
    return runner.Finish();
}
