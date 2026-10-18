// SPDX-License-Identifier: Apache-2.0

#include "User/Platform/Ia64/Runtime/Ia64MinimalUserImageRuntime.hpp"

#include "User/Platform/Ia64/Runtime/Ia64BssZeroInitializationProbe.hpp"

namespace Asteria::User::Platform::Ia64::Runtime
{
namespace
{
constexpr Foundation::UInt64 ExpectedSendTransferredBytes{0U};
constexpr Foundation::UInt64 ExpectedReceiveTransferredBytes{Ia64MinimalUserImageIpcDeliverySizeBytes};

constexpr Foundation::UInt8 CanonicalIpcFrame[Ia64MinimalUserImageIpcFrameSizeBytes]{
    0x41U, 0x49U, 0x50U, 0x43U, 0x01U, 0x00U, 0x58U, 0x00U, 0x58U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};

constexpr Foundation::UInt8 CanonicalIpcDelivery[Ia64MinimalUserImageIpcDeliverySizeBytes]{
    0x41U, 0x49U, 0x50U, 0x44U, 0x01U, 0x00U, 0x30U, 0x00U, 0x88U, 0x00U, 0x00U, 0x00U, 0x58U, 0x00U, 0x07U, 0x00U,
    0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x41U, 0x49U, 0x50U, 0x43U, 0x01U, 0x00U, 0x58U, 0x00U, 0x58U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};

[[nodiscard]] Foundation::UInt64 AddressOf(const void* const storage) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(storage));
}

void ResetResponse(Abi::SystemCallResponse& response) noexcept
{
    response = Abi::SystemCallResponse{
        .Error = Abi::SystemCallError::InvalidConfiguration,
        .Value0 = 0U,
        .Value1 = 0U,
    };
}

void ResetEvidence() noexcept
{
    ResetResponse(g_querySystemCallResponse);
    ResetResponse(g_sendSystemCallResponse);
    ResetResponse(g_receiveSystemCallResponse);
    ResetResponse(g_lastSystemCallResponse);
    for (Foundation::Size index{0U}; index < sizeof(g_ipcFrame); ++index)
    {
        g_ipcFrame[index] = CanonicalIpcFrame[index];
    }
    for (Foundation::Size index{0U}; index < sizeof(g_ipcDelivery); ++index)
    {
        g_ipcDelivery[index] = 0U;
    }
    g_ipcCompletionSentinel = 0U;
}

[[nodiscard]] bool DeliveryMatches() noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(g_ipcDelivery); ++index)
    {
        if (g_ipcDelivery[index] != CanonicalIpcDelivery[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr Foundation::UInt64 FailureValue(const Ia64MinimalUserImageRuntimeFailure failure) noexcept
{
    return static_cast<Foundation::UInt64>(failure);
}
} // namespace

constinit Abi::SystemCallResponse g_querySystemCallResponse{
    .Error = Abi::SystemCallError::InvalidConfiguration,
    .Value0 = 0U,
    .Value1 = 0U,
};
constinit Abi::SystemCallResponse g_sendSystemCallResponse{
    .Error = Abi::SystemCallError::InvalidConfiguration,
    .Value0 = 0U,
    .Value1 = 0U,
};
constinit Abi::SystemCallResponse g_receiveSystemCallResponse{
    .Error = Abi::SystemCallError::InvalidConfiguration,
    .Value0 = 0U,
    .Value1 = 0U,
};
alignas(Foundation::UInt64) constinit Foundation::UInt8 g_ipcFrame[Ia64MinimalUserImageIpcFrameSizeBytes]{
    0x41U, 0x49U, 0x50U, 0x43U, 0x01U, 0x00U, 0x58U, 0x00U, 0x58U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};
alignas(Foundation::UInt64) constinit Foundation::UInt8 g_ipcDelivery[Ia64MinimalUserImageIpcDeliverySizeBytes]{};
constinit Foundation::UInt64 g_ipcCompletionSentinel{0U};

constinit Abi::SystemCallResponse g_lastSystemCallResponse{
    .Error = Abi::SystemCallError::InvalidConfiguration,
    .Value0 = 0U,
    .Value1 = 0U,
};

[[gnu::noinline]] Foundation::UInt64 RunIa64MinimalUserImage(const Foundation::UInt64 bootstrapSelector,
                                                             const Foundation::UInt64 bootstrapAuthenticator,
                                                             const User::System::SystemCallTransport transport) noexcept
{
    ObserveBssZeroInitialization();
    ResetEvidence();
    if (g_bssZeroInitializationProbe != BssZeroInitializationPassedSentinel)
    {
        return FailureValue(Ia64MinimalUserImageRuntimeFailure::BssInitializationRejected);
    }

    const User::System::SystemCallClient client{transport};
    const User::System::CapabilityHandle endpoint{
        .Selector = bootstrapSelector,
        .Authenticator = bootstrapAuthenticator,
    };

    g_querySystemCallResponse = client.QueryCapability(endpoint);
    g_lastSystemCallResponse = g_querySystemCallResponse;
    if (g_querySystemCallResponse.Error != Abi::SystemCallError::None)
    {
        return static_cast<Foundation::UInt64>(g_querySystemCallResponse.Error);
    }
    if ((g_querySystemCallResponse.Value0 != Ia64MinimalUserImageExpectedEndpointInformation) ||
        (g_querySystemCallResponse.Value1 != Ia64MinimalUserImageInfiniteLeaseDeadline))
    {
        return FailureValue(Ia64MinimalUserImageRuntimeFailure::QueryResponseRejected);
    }

    g_sendSystemCallResponse = client.Send(endpoint, AddressOf(g_ipcFrame), sizeof(g_ipcFrame));
    if (g_sendSystemCallResponse.Error != Abi::SystemCallError::None)
    {
        return static_cast<Foundation::UInt64>(g_sendSystemCallResponse.Error);
    }
    if ((g_sendSystemCallResponse.Value0 != Ia64MinimalUserImageIpcSequence) ||
        (g_sendSystemCallResponse.Value1 != ExpectedSendTransferredBytes))
    {
        return FailureValue(Ia64MinimalUserImageRuntimeFailure::SendResponseRejected);
    }

    g_receiveSystemCallResponse = client.Receive(endpoint, AddressOf(g_ipcDelivery), sizeof(g_ipcDelivery));
    if (g_receiveSystemCallResponse.Error != Abi::SystemCallError::None)
    {
        return static_cast<Foundation::UInt64>(g_receiveSystemCallResponse.Error);
    }
    if ((g_receiveSystemCallResponse.Value0 != Ia64MinimalUserImageIpcSequence) ||
        (g_receiveSystemCallResponse.Value1 != ExpectedReceiveTransferredBytes))
    {
        return FailureValue(Ia64MinimalUserImageRuntimeFailure::ReceiveResponseRejected);
    }
    if (!DeliveryMatches())
    {
        return FailureValue(Ia64MinimalUserImageRuntimeFailure::DeliveryRejected);
    }

    g_ipcCompletionSentinel = Ia64MinimalUserImageIpcCompletionValue;
    return FailureValue(Ia64MinimalUserImageRuntimeFailure::None);
}

static_assert(sizeof(CanonicalIpcFrame) == sizeof(g_ipcFrame));
static_assert(sizeof(CanonicalIpcDelivery) == sizeof(g_ipcDelivery));
} // namespace Asteria::User::Platform::Ia64::Runtime
