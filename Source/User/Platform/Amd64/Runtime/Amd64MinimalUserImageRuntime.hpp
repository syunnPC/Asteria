// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PLATFORM_AMD64_RUNTIME_AMD64_MINIMAL_USER_IMAGE_RUNTIME_HPP_INCLUDED
#define ASTERIA_USER_PLATFORM_AMD64_RUNTIME_AMD64_MINIMAL_USER_IMAGE_RUNTIME_HPP_INCLUDED

#include "Abi/SystemCallAbi.hpp"
#include "Foundation/Types.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Platform::Amd64::Runtime
{
inline constexpr Foundation::Size Amd64MinimalUserImageIpcFrameSizeBytes{88U};
inline constexpr Foundation::Size Amd64MinimalUserImageIpcDeliverySizeBytes{136U};
inline constexpr Foundation::UInt64 Amd64MinimalUserImageExpectedEndpointInformation{0x0000'0000'0007'0009ULL};
inline constexpr Foundation::UInt64 Amd64MinimalUserImageInfiniteLeaseDeadline{~Foundation::UInt64{0U}};
inline constexpr Foundation::UInt64 Amd64MinimalUserImageIpcSequence{1U};
inline constexpr Foundation::UInt64 Amd64MinimalUserImageIpcCompletionValue{0x4153'5445'5249'4134ULL};

enum class Amd64MinimalUserImageRuntimeFailure : Foundation::UInt64
{
    None = 0U,
    BssInitializationRejected = 0x100U,
    QueryResponseRejected = 0x101U,
    SendResponseRejected = 0x102U,
    ReceiveResponseRejected = 0x103U,
    DeliveryRejected = 0x104U,
};

extern Abi::SystemCallResponse g_querySystemCallResponse;
extern Abi::SystemCallResponse g_sendSystemCallResponse;
extern Abi::SystemCallResponse g_receiveSystemCallResponse;
extern Foundation::UInt8 g_ipcFrame[Amd64MinimalUserImageIpcFrameSizeBytes];
extern Foundation::UInt8 g_ipcDelivery[Amd64MinimalUserImageIpcDeliverySizeBytes];
extern Foundation::UInt64 g_ipcCompletionSentinel;

/// Retained compatibility copy of the Query response. New diagnostics use the
/// three operation-specific response records above.
extern Abi::SystemCallResponse g_lastSystemCallResponse;

/// Observes image BSS, validates the bootstrap Endpoint with Query, sends one
/// canonical attachment-free asynchronous AIPC frame, and receives and checks
/// its exact canonical AIPD loopback delivery. Each syscall response and both
/// wire buffers remain in global storage for target diagnostics. The completion
/// sentinel is published only after every check succeeds.
///
/// A syscall failure returns its SystemCallError value. A local validation
/// failure returns the corresponding Amd64MinimalUserImageRuntimeFailure value.
/// The transport and its context must remain valid for the duration of this
/// call. This function allocates no memory and is not thread-safe.
[[nodiscard, gnu::noinline]] Foundation::UInt64
RunAmd64MinimalUserImage(Foundation::UInt64 bootstrapSelector, Foundation::UInt64 bootstrapAuthenticator,
                         User::System::SystemCallTransport transport) noexcept;

static_assert(Amd64MinimalUserImageIpcFrameSizeBytes == 88U);
static_assert(Amd64MinimalUserImageIpcDeliverySizeBytes == 48U + 88U);
static_assert(Amd64MinimalUserImageIpcCompletionValue != 0U);
static_assert(sizeof(Amd64MinimalUserImageRuntimeFailure) == 8U);
} // namespace Asteria::User::Platform::Amd64::Runtime

#endif // ASTERIA_USER_PLATFORM_AMD64_RUNTIME_AMD64_MINIMAL_USER_IMAGE_RUNTIME_HPP_INCLUDED
