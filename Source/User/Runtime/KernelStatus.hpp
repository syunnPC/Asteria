// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_RUNTIME_KERNEL_STATUS_HPP_INCLUDED
#define ASTERIA_USER_RUNTIME_KERNEL_STATUS_HPP_INCLUDED

#include "Abi/SystemCallAbi.hpp"
#include "User/Runtime/Status.hpp"

namespace Asteria::User::Runtime
{
inline constexpr Facility KernelFacility{1U};

/// SystemCallError codes by identity; None explicitly uses FromKernelError.
enum class KernelStatus : Foundation::UInt32
{
    UnknownSystemCall = 1U,
    InvalidArguments = 2U,
    InvalidConfiguration = 3U,
    InvalidHandle = 4U,
    ObjectNotFound = 5U,
    ResourceExhausted = 6U,
    SpaceExhausted = 7U,
    RightsDenied = 8U,
    RightsAmplification = 9U,
    NoAttenuation = 10U,
    TypeMismatch = 11U,
    Revoked = 12U,
    Expired = 13U,
    LeaseExtension = 14U,
    RevocationDomainExhausted = 15U,
    WouldBlock = 16U,
    UserMemoryFault = 17U,
    TimedOut = 18U,
    WriteValueDenied = 19U,
    ReservedMemory = 20U,
    DisplayReserved = 21U,
    BridgeReserved = 22U,
};
inline constexpr Foundation::UInt32 LastKernelStatus{22U};

[[nodiscard]] constexpr Facility FacilityOf(KernelStatus) noexcept
{
    return KernelFacility;
}

[[nodiscard]] constexpr bool IsKnownStatusCode(KernelStatus, const Foundation::UInt32 code) noexcept
{
    return (code >= 1U) && (code <= LastKernelStatus);
}

/// Returns a static name; the value must be a known code (use Status::TryAs for external data).
[[nodiscard]] const char* Name(KernelStatus value) noexcept;

/// Validates the ABI word before narrowing. None explicitly produces success;
/// newer kernel errors retain their identity and take TryAs's unknown path.
[[nodiscard]] Foundation::Result<Status, Status> FromKernelError(Foundation::UInt64 error) noexcept;
[[nodiscard]] inline Foundation::Result<Status, Status> FromKernelError(const Abi::SystemCallError error) noexcept
{
    return FromKernelError(static_cast<Foundation::UInt64>(error));
}
} // namespace Asteria::User::Runtime

#endif
