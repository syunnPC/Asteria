// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "User/System/SystemCallClient.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{

struct FakeTransportContext
{
    Abi::SystemCallRequest LastRequest;
    Abi::SystemCallResponse NextResponse;
    Foundation::Size CallCount;
};

void InvokeFakeTransport(void* const context, const Abi::SystemCallRequest* const request,
                         Abi::SystemCallResponse* const response) noexcept
{
    auto* const fake = static_cast<FakeTransportContext*>(context);
    fake->LastRequest = *request;
    ++fake->CallCount;
    *response = fake->NextResponse;
}

[[nodiscard]] bool RequestMatches(const Abi::SystemCallRequest& request, const Abi::SystemCallNumber number,
                                  const User::System::CapabilityHandle handle, const Foundation::UInt64 argument2,
                                  const Foundation::UInt64 argument3, const Foundation::UInt64 argument4 = 0U) noexcept
{
    return (request.Number == static_cast<Foundation::UInt64>(number)) && (request.Arguments[0U] == handle.Selector) &&
           (request.Arguments[1U] == handle.Authenticator) && (request.Arguments[2U] == argument2) &&
           (request.Arguments[3U] == argument3) && (request.Arguments[4U] == argument4) &&
           (request.Arguments[5U] == 0U);
}

[[nodiscard]] bool ResponseMatches(const Abi::SystemCallResponse& response,
                                   const Abi::SystemCallResponse& expected) noexcept
{
    return (response.Error == expected.Error) && (response.Value0 == expected.Value0) &&
           (response.Value1 == expected.Value1);
}

void TestUnavailableTransport(TestContext& runner) noexcept
{
    const User::System::SystemCallClient client{User::System::SystemCallTransport{
        .Function = nullptr,
        .Context = nullptr,
    }};
    const Abi::SystemCallResponse response = client.QueryCapability(User::System::CapabilityHandle{
        .Selector = 1U,
        .Authenticator = 2U,
    });

    ASTERIA_EXPECT(runner, "null transport is not ready", !client.IsReady());
    ASTERIA_EXPECT(runner, "null transport returns one canonical configuration error",
                   response.Error == Abi::SystemCallError::InvalidConfiguration, response.Value0 == 0U,
                   response.Value1 == 0U);
}

void TestStableWrappers(TestContext& runner) noexcept
{
    FakeTransportContext fake{
        .LastRequest = {},
        .NextResponse =
            {
                .Error = Abi::SystemCallError::None,
                .Value0 = 0x1122'3344'5566'7788ULL,
                .Value1 = 0x8877'6655'4433'2211ULL,
            },
        .CallCount = 0U,
    };
    const User::System::SystemCallClient client{User::System::SystemCallTransport{
        .Function = InvokeFakeTransport,
        .Context = &fake,
    }};
    constexpr User::System::CapabilityHandle handle{
        .Selector = 0x0123'4567'89AB'CDEFULL,
        .Authenticator = 0xFEDC'BA98'7654'3210ULL,
    };

    ASTERIA_EXPECT(runner, "fake transport is ready", client.IsReady());

    const Abi::SystemCallResponse close = client.CloseCapability(handle);
    ASTERIA_EXPECT(runner, "close wrapper emits canonical arguments",
                   RequestMatches(fake.LastRequest, Abi::SystemCallNumber::CapabilityClose, handle, 0U, 0U));
    ASTERIA_EXPECT(runner, "close wrapper returns the transport response", ResponseMatches(close, fake.NextResponse));

    static_cast<void>(client.QueryCapability(handle));
    ASTERIA_EXPECT(runner, "query wrapper emits canonical arguments",
                   RequestMatches(fake.LastRequest, Abi::SystemCallNumber::CapabilityQuery, handle, 0U, 0U));

    static_cast<void>(client.DeriveCapability(handle, 0x1357U, 0x2468'ACE0U));
    ASTERIA_EXPECT(
        runner, "derive wrapper emits rights and deadline",
        RequestMatches(fake.LastRequest, Abi::SystemCallNumber::CapabilityDerive, handle, 0x1357U, 0x2468'ACE0U));

    static_cast<void>(
        client.DeriveCapability(handle, 0x1357U, 0x2468'ACE0U, Abi::SystemCallDeriveChildRevocationDomain));
    ASTERIA_EXPECT(runner, "derive wrapper emits its flags in argument 4",
                   RequestMatches(fake.LastRequest, Abi::SystemCallNumber::CapabilityDerive, handle, 0x1357U,
                                  0x2468'ACE0U, Abi::SystemCallDeriveChildRevocationDomain));

    static_cast<void>(client.DuplicateCapability(handle, 0xAAAAU, 0xBBBB'CCCCU));
    ASTERIA_EXPECT(
        runner, "duplicate wrapper emits rights and deadline",
        RequestMatches(fake.LastRequest, Abi::SystemCallNumber::CapabilityDuplicate, handle, 0xAAAAU, 0xBBBB'CCCCU));

    static_cast<void>(client.RevokeCapability(handle));
    ASTERIA_EXPECT(runner, "revoke wrapper emits canonical arguments",
                   RequestMatches(fake.LastRequest, Abi::SystemCallNumber::CapabilityRevoke, handle, 0U, 0U));

    static_cast<void>(client.Send(handle, 0x2000U, 88U));
    ASTERIA_EXPECT(runner, "send wrapper emits frame address and size",
                   RequestMatches(fake.LastRequest, Abi::SystemCallNumber::IpcSend, handle, 0x2000U, 88U));

    static_cast<void>(client.Receive(handle, 0x3000U, 136U));
    ASTERIA_EXPECT(runner, "receive wrapper emits delivery address and capacity",
                   RequestMatches(fake.LastRequest, Abi::SystemCallNumber::IpcReceive, handle, 0x3000U, 136U));

    static_cast<void>(client.Wait(handle));
    ASTERIA_EXPECT(runner, "wait wrapper emits canonical arguments",
                   RequestMatches(fake.LastRequest, Abi::SystemCallNumber::WaitSetWait, handle, 0U, 0U));
    static_cast<void>(client.FaultResolve(handle, Abi::FaultResolveKind::Map, 0x7000'1000U));
    ASTERIA_EXPECT(runner, "fault-resolve wrapper emits its kind, request address and request size",
                   RequestMatches(fake.LastRequest, Abi::SystemCallNumber::FaultResolve, handle, 1U, 0x7000'1000U,
                                  Abi::FaultResolveRequestSizeBytes));
    static_cast<void>(client.FaultTerminate(handle));
    ASTERIA_EXPECT(runner, "fault-terminate wrapper emits kind 2 and no request",
                   RequestMatches(fake.LastRequest, Abi::SystemCallNumber::FaultResolve, handle, 2U, 0U, 0U));
    static_cast<void>(client.CreateEndpoint(handle, 0x7000'2000U));
    ASTERIA_EXPECT(runner, "create-endpoint wrapper emits the Endpoint type, request address and request size",
                   RequestMatches(fake.LastRequest, Abi::SystemCallNumber::ObjectCreate, handle,
                                  Abi::ObjectTypeEndpoint, 0x7000'2000U, Abi::ObjectCreateEndpointRequestSizeBytes));
    static_cast<void>(client.DestroyObject(handle, Abi::ObjectTypeEndpoint));
    ASTERIA_EXPECT(runner, "destroy wrapper emits the object type and no request",
                   RequestMatches(fake.LastRequest, Abi::SystemCallNumber::ObjectDestroy, handle,
                                  Abi::ObjectTypeEndpoint, 0U, 0U));
    static_cast<void>(
        client.CreateObject(handle, Abi::ObjectTypeFrame, 0x7000'3000U, Abi::ObjectCreateFrameRequestSizeBytes));
    ASTERIA_EXPECT(runner, "create-object wrapper emits the type, request address and request size",
                   RequestMatches(fake.LastRequest, Abi::SystemCallNumber::ObjectCreate, handle, Abi::ObjectTypeFrame,
                                  0x7000'3000U, Abi::ObjectCreateFrameRequestSizeBytes));
    static_cast<void>(client.MapMemory(0x7000'4000U));
    ASTERIA_EXPECT(runner, "map-memory wrapper emits only the request address and size",
                   RequestMatches(fake.LastRequest, Abi::SystemCallNumber::MemoryMap, User::System::CapabilityHandle{},
                                  0U, 0x7000'4000U, Abi::FaultResolveRequestSizeBytes));
    static_cast<void>(client.UnmapMemory(handle, 0x4C00'0000U, 2U));
    ASTERIA_EXPECT(runner, "unmap-memory wrapper emits the space, the start and the page count",
                   RequestMatches(fake.LastRequest, Abi::SystemCallNumber::MemoryUnmap, handle, 0x4C00'0000U, 2U, 0U));
    static_cast<void>(client.CreateThread(handle, 0x7000'5000U));
    ASTERIA_EXPECT(runner, "create-thread wrapper emits the Thread type, request address and request size",
                   RequestMatches(fake.LastRequest, Abi::SystemCallNumber::ObjectCreate, handle, Abi::ObjectTypeThread,
                                  0x7000'5000U, Abi::ObjectCreateThreadRequestSizeBytes));
    static_cast<void>(client.ExitThread(0x55U));
    ASTERIA_EXPECT(runner, "exit-thread wrapper emits the exit code in argument 0 and nothing else",
                   RequestMatches(fake.LastRequest, Abi::SystemCallNumber::ThreadExit,
                                  User::System::CapabilityHandle{0x55U, 0U}, 0U, 0U, 0U));
    static_cast<void>(client.GrantCapability(handle, User::System::CapabilityHandle{0x31U, 0x32U}, 0x0101U, 9U,
                                             Abi::CapabilityGrantCopy));
    ASTERIA_EXPECT(
        runner, "grant wrapper emits the destination, the rights and type, and the disposition",
        fake.LastRequest.Number == static_cast<Foundation::UInt64>(Abi::SystemCallNumber::CapabilityGrant),
        fake.LastRequest.Arguments[0U] == handle.Selector, fake.LastRequest.Arguments[1U] == handle.Authenticator,
        fake.LastRequest.Arguments[2U] == 0x31U, fake.LastRequest.Arguments[3U] == 0x32U,
        fake.LastRequest.Arguments[4U] == (0x0101U | (Foundation::UInt64{9U} << Abi::CapabilityGrantTypeShift)),
        fake.LastRequest.Arguments[5U] == Abi::CapabilityGrantCopy);
    static_cast<void>(client.TerminateThread(handle));
    ASTERIA_EXPECT(runner, "terminate wrapper emits the thread handle and nothing else",
                   RequestMatches(fake.LastRequest, Abi::SystemCallNumber::ThreadTerminate, handle, 0U, 0U, 0U));
    ASTERIA_EXPECT(runner, "each wrapper call invokes transport once", fake.CallCount == 20U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"SystemCallClientTests"};
    Asteria::Test::TestUnavailableTransport(runner);
    Asteria::Test::TestStableWrappers(runner);
    return runner.Finish();
}
