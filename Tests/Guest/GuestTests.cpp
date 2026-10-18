// SPDX-License-Identifier: Apache-2.0
#include "Guest/GuestTests.hpp"

#include "Abi/BootstrapFixture.hpp"

namespace Asteria::Guest
{
namespace
{
using Abi::SystemCallError;
using Abi::SystemCallResponse;
using Foundation::Size;
using Foundation::UInt64;
using Foundation::UInt8;
using User::System::SystemCallClient;

// Query value 0 packs the object type in bits 0-7 and the rights from bit
// 16 (SystemCallAbi.md): Endpoint (9) with Inspect|Invoke|Read, and
// DiagnosticChannel (22) with Inspect|Write.
constexpr UInt64 EndpointInformation{9U | (0x7U << Abi::SystemCallQueryRightsShift)};
constexpr UInt64 DiagnosticInformation{22U | (0x9U << Abi::SystemCallQueryRightsShift)};
constexpr UInt64 InfiniteLease{~UInt64{0U}};
constexpr UInt64 WaitTag{Abi::BootstrapWaitTag};
constexpr UInt64 WaitRegistrationSequence{1U};
constexpr UInt64 WaitCount{2U};

// One attachment-free asynchronous AIPC frame and the exact AIPD delivery of
// it through a self-receiving Endpoint (SystemCallAbi.md, IpcWireFormat.md).
constexpr UInt8 CanonicalFrame[88]{
    0x41U, 0x49U, 0x50U, 0x43U, 0x01U, 0x00U, 0x58U, 0x00U, 0x58U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};
constexpr UInt8 DeliveryHeader[48]{
    0x41U, 0x49U, 0x50U, 0x44U, 0x01U, 0x00U, 0x30U, 0x00U, 0x88U, 0x00U, 0x00U, 0x00U, 0x58U, 0x00U, 0x07U, 0x00U,
    0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};
constexpr Size DeliverySizeBytes{sizeof(DeliveryHeader) + sizeof(CanonicalFrame)};

alignas(UInt64) constinit UInt8 g_frame[sizeof(CanonicalFrame)]{};
alignas(UInt64) constinit UInt8 g_delivery[DeliverySizeBytes]{};

struct Output
{
    const SystemCallClient* Client;
    User::System::CapabilityHandle Channel;
};

[[nodiscard]] UInt64 AddressOf(const void* const storage) noexcept
{
    return static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(storage));
}

[[nodiscard]] bool WriteLine(void* const context, const char* const text, const Size sizeBytes) noexcept
{
    const auto& output = *static_cast<const Output*>(context);
    const SystemCallResponse response = output.Client->DebugWrite(output.Channel, AddressOf(text), sizeBytes);
    return (response.Error == SystemCallError::None) && (response.Value0 == sizeBytes);
}

[[nodiscard]] bool ResponseIs(const SystemCallResponse& response, const SystemCallError error, const UInt64 value0,
                              const UInt64 value1) noexcept
{
    return (response.Error == error) && (response.Value0 == value0) && (response.Value1 == value1);
}

[[nodiscard]] bool BytesEqual(const UInt8* const first, const UInt8* const second, const Size sizeBytes) noexcept
{
    for (Size index{0U}; index < sizeBytes; ++index)
    {
        if (first[index] != second[index])
        {
            return false;
        }
    }
    return true;
}

void CheckResponse(Tap& tap, const char* const description, const SystemCallResponse& response,
                   const SystemCallError error, const UInt64 value0, const UInt64 value1) noexcept
{
    if (tap.Check(ResponseIs(response, error, value0, value1), description))
    {
        return;
    }
    tap.Detail("error", static_cast<UInt64>(error), static_cast<UInt64>(response.Error));
    tap.Detail("value0", value0, response.Value0);
    tap.Detail("value1", value1, response.Value1);
}
} // Namespace

// Zero-initialized storage lands in.bss; the loader must clear it. The
// accesses are volatile so the compiler cannot assume the initial value.
constinit volatile UInt64 g_bssProbe[4]{};

UInt64 RunGuestTests(const Bootstrap& bootstrap, const User::System::SystemCallTransport transport,
                     const PlatformChecks platformChecks) noexcept
{
    const SystemCallClient client{transport};
    // Static storage keeps the writer and its 256-byte line buffer off the
    // small user stack, and needs no memset (the IA-64 user runtime has
    // none). The program runs once, so nothing else uses them.
    static constinit Output output{};
    static constinit Tap tap{WriteLine, &output};
    output = Output{&client, bootstrap.Diagnostic};
    tap.Begin();

    // The bootstrap handles as facts: the runner compares their
    // authenticators across boots of the same image (the validation keys
    // come from boot entropy, CapabilityModel.md), never with fixed values.
    tap.Handle("endpoint", bootstrap.Endpoint.Selector, bootstrap.Endpoint.Authenticator);
    tap.Handle("diagnostic", bootstrap.Diagnostic.Selector, bootstrap.Diagnostic.Authenticator);
    if (bootstrap.HasWaitSet)
    {
        tap.Handle("waitset", bootstrap.WaitSet.Selector, bootstrap.WaitSet.Authenticator);
    }

    tap.Check((g_bssProbe[0] | g_bssProbe[1] | g_bssProbe[2] | g_bssProbe[3]) == 0U,
              "bss.zero: the loader clears the image's zero-initialized storage");
    g_bssProbe[0] = 1U;

    CheckResponse(tap, "query.endpoint: the bootstrap Endpoint is Inspect|Invoke|Read with an infinite lease",
                  client.QueryCapability(bootstrap.Endpoint), SystemCallError::None, EndpointInformation,
                  InfiniteLease);
    CheckResponse(tap, "query.diagnostic: the DiagnosticChannel is Inspect|Write with an infinite lease",
                  client.QueryCapability(bootstrap.Diagnostic), SystemCallError::None, DiagnosticInformation,
                  InfiniteLease);

    static constexpr char ControlBytes[] = "bad\r\n";
    CheckResponse(tap, "debugwrite.rejects-control-bytes",
                  client.DebugWrite(bootstrap.Diagnostic, AddressOf(ControlBytes), sizeof(ControlBytes) - 1U),
                  SystemCallError::InvalidArguments, 0U, 0U);
    static constexpr char Line[] = "x\n";
    CheckResponse(tap, "debugwrite.rejects-a-handle-without-write",
                  client.DebugWrite(bootstrap.Endpoint, AddressOf(Line), sizeof(Line) - 1U),
                  SystemCallError::RightsDenied, 0U, 0U);

    for (Size index{0U}; index < sizeof(g_frame); ++index)
    {
        g_frame[index] = CanonicalFrame[index];
    }
    CheckResponse(tap, "ipc.send: the canonical 88-byte frame is queued as sequence 1",
                  client.Send(bootstrap.Endpoint, AddressOf(g_frame), sizeof(g_frame)), SystemCallError::None, 1U, 0U);
    CheckResponse(tap, "ipc.receive: the same sequence is delivered in 136 bytes",
                  client.Receive(bootstrap.Endpoint, AddressOf(g_delivery), sizeof(g_delivery)), SystemCallError::None,
                  1U, DeliverySizeBytes);
    tap.Check(BytesEqual(g_delivery, DeliveryHeader, sizeof(DeliveryHeader)) &&
                  BytesEqual(g_delivery + sizeof(DeliveryHeader), CanonicalFrame, sizeof(CanonicalFrame)),
              "ipc.delivery: header and payload equal the canonical delivery");

    for (UInt64 wait{1U}; wait <= WaitCount; ++wait)
    {
        if (!bootstrap.HasWaitSet)
        {
            tap.Skip("waitset.wait", "this image receives no WaitSet");
            continue;
        }
        CheckResponse(tap, "waitset.wait: the timer notification is selected with its registration sequence",
                      client.Wait(bootstrap.WaitSet), SystemCallError::None, WaitTag, WaitRegistrationSequence);
    }

    if (platformChecks != nullptr)
    {
        platformChecks(tap);
    }
    tap.End();
    return tap.GetFailures();
}
} // Namespace Asteria::Guest
