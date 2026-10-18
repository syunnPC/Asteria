// SPDX-License-Identifier: Apache-2.0

#include "User/Runtime/KernelStatus.hpp"

#include "User/Runtime/Include/AsKernelStatus.h"

namespace Asteria::User::Runtime
{
static_assert(sizeof(AsKernelStatus) == sizeof(KernelStatus));
static_assert(KernelFacility.Number() == AsKernelFacilityNumber);
static_assert(Status{KernelStatus::UnknownSystemCall}.Raw() == AsKernelUnknownSystemCall);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::UnknownSystemCall) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::UnknownSystemCall));
static_assert(Status{KernelStatus::InvalidArguments}.Raw() == AsKernelInvalidArguments);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::InvalidArguments) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::InvalidArguments));
static_assert(Status{KernelStatus::InvalidConfiguration}.Raw() == AsKernelInvalidConfiguration);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::InvalidConfiguration) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::InvalidConfiguration));
static_assert(Status{KernelStatus::InvalidHandle}.Raw() == AsKernelInvalidHandle);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::InvalidHandle) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::InvalidHandle));
static_assert(Status{KernelStatus::ObjectNotFound}.Raw() == AsKernelObjectNotFound);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::ObjectNotFound) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::ObjectNotFound));
static_assert(Status{KernelStatus::ResourceExhausted}.Raw() == AsKernelResourceExhausted);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::ResourceExhausted) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::ResourceExhausted));
static_assert(Status{KernelStatus::SpaceExhausted}.Raw() == AsKernelSpaceExhausted);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::SpaceExhausted) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::SpaceExhausted));
static_assert(Status{KernelStatus::RightsDenied}.Raw() == AsKernelRightsDenied);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::RightsDenied) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::RightsDenied));
static_assert(Status{KernelStatus::RightsAmplification}.Raw() == AsKernelRightsAmplification);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::RightsAmplification) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::RightsAmplification));
static_assert(Status{KernelStatus::NoAttenuation}.Raw() == AsKernelNoAttenuation);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::NoAttenuation) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::NoAttenuation));
static_assert(Status{KernelStatus::TypeMismatch}.Raw() == AsKernelTypeMismatch);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::TypeMismatch) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::TypeMismatch));
static_assert(Status{KernelStatus::Revoked}.Raw() == AsKernelRevoked);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::Revoked) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::Revoked));
static_assert(Status{KernelStatus::Expired}.Raw() == AsKernelExpired);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::Expired) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::Expired));
static_assert(Status{KernelStatus::LeaseExtension}.Raw() == AsKernelLeaseExtension);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::LeaseExtension) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::LeaseExtension));
static_assert(Status{KernelStatus::RevocationDomainExhausted}.Raw() == AsKernelRevocationDomainExhausted);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::RevocationDomainExhausted) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::RevocationDomainExhausted));
static_assert(Status{KernelStatus::WouldBlock}.Raw() == AsKernelWouldBlock);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::WouldBlock) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::WouldBlock));
static_assert(Status{KernelStatus::UserMemoryFault}.Raw() == AsKernelUserMemoryFault);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::UserMemoryFault) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::UserMemoryFault));
static_assert(Status{KernelStatus::TimedOut}.Raw() == AsKernelTimedOut);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::TimedOut) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::TimedOut));
static_assert(Status{KernelStatus::WriteValueDenied}.Raw() == AsKernelWriteValueDenied);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::WriteValueDenied) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::WriteValueDenied));
static_assert(Status{KernelStatus::ReservedMemory}.Raw() == AsKernelReservedMemory);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::ReservedMemory) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::ReservedMemory));
static_assert(Status{KernelStatus::DisplayReserved}.Raw() == AsKernelDisplayReserved);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::DisplayReserved) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::DisplayReserved));
static_assert(Status{KernelStatus::BridgeReserved}.Raw() == AsKernelBridgeReserved);
static_assert(static_cast<Foundation::UInt64>(KernelStatus::BridgeReserved) ==
              static_cast<Foundation::UInt64>(Abi::SystemCallError::BridgeReserved));
static_assert(LastKernelStatus == static_cast<Foundation::UInt32>(Abi::SystemCallError::Last));

Foundation::Result<Status, Status> FromKernelError(const Foundation::UInt64 error) noexcept
{
    if (error == 0U)
    {
        return Foundation::Result<Status, Status>::Success(Status{});
    }
    if (error > 0x00FF'FFFFULL)
    {
        return Foundation::Result<Status, Status>::Failure(KernelStatus::InvalidArguments);
    }
    return Status::FromRaw(0x8000'0000U | (KernelFacility.Number() << 24U) | static_cast<Foundation::UInt32>(error));
}

const char* Name(const KernelStatus value) noexcept
{
    switch (value)
    {
    case KernelStatus::UnknownSystemCall:
        return "UnknownSystemCall";
    case KernelStatus::InvalidArguments:
        return "InvalidArguments";
    case KernelStatus::InvalidConfiguration:
        return "InvalidConfiguration";
    case KernelStatus::InvalidHandle:
        return "InvalidHandle";
    case KernelStatus::ObjectNotFound:
        return "ObjectNotFound";
    case KernelStatus::ResourceExhausted:
        return "ResourceExhausted";
    case KernelStatus::SpaceExhausted:
        return "SpaceExhausted";
    case KernelStatus::RightsDenied:
        return "RightsDenied";
    case KernelStatus::RightsAmplification:
        return "RightsAmplification";
    case KernelStatus::NoAttenuation:
        return "NoAttenuation";
    case KernelStatus::TypeMismatch:
        return "TypeMismatch";
    case KernelStatus::Revoked:
        return "Revoked";
    case KernelStatus::Expired:
        return "Expired";
    case KernelStatus::LeaseExtension:
        return "LeaseExtension";
    case KernelStatus::RevocationDomainExhausted:
        return "RevocationDomainExhausted";
    case KernelStatus::WouldBlock:
        return "WouldBlock";
    case KernelStatus::UserMemoryFault:
        return "UserMemoryFault";
    case KernelStatus::TimedOut:
        return "TimedOut";
    case KernelStatus::ReservedMemory:
        return "ReservedMemory";
    case KernelStatus::DisplayReserved:
        return "DisplayReserved";
    case KernelStatus::BridgeReserved:
        return "BridgeReserved";
    case KernelStatus::WriteValueDenied:
        return "WriteValueDenied";
    }
    return "Unknown";
}
} // namespace Asteria::User::Runtime
