// SPDX-License-Identifier: Apache-2.0

#include "Kernel/System/SystemCallDispatcher.hpp"
#include "Kernel/Object/CapabilitySpacePool.hpp"
#include "Kernel/Object/CapabilityTransfer.hpp"
#include "Kernel/Object/CapabilityWireTypes.hpp"

namespace Asteria::Kernel::System
{
namespace
{
enum class CapabilityCreationKind : Foundation::UInt8
{
    Derived,
    Duplicate,
};

[[nodiscard]] constexpr SystemCallResponse MakeResponse(const SystemCallError error,
                                                        const Foundation::UInt64 value0 = 0U,
                                                        const Foundation::UInt64 value1 = 0U) noexcept
{
    return SystemCallResponse{
        .Error = error,
        .Value0 = value0,
        .Value1 = value1,
    };
}

[[nodiscard]] constexpr bool ArgumentsAreZero(const SystemCallRequest& request, const Foundation::Size first) noexcept
{
    for (Foundation::Size index{first}; index < SystemCallArgumentCount; ++index)
    {
        if (request.Arguments[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr SystemCallError MapCapabilityError(const Object::CapabilityError error) noexcept
{
    switch (error)
    {
    case Object::CapabilityError::Ok:
        return SystemCallError::None;
    case Object::CapabilityError::InvalidArgument:
        return SystemCallError::InvalidArguments;
    case Object::CapabilityError::InvalidConfiguration:
        return SystemCallError::InvalidConfiguration;
    case Object::CapabilityError::InvalidHandle:
        return SystemCallError::InvalidHandle;
    case Object::CapabilityError::ObjectNotFound:
        return SystemCallError::ObjectNotFound;
    case Object::CapabilityError::ResourceExhausted:
        return SystemCallError::ResourceExhausted;
    case Object::CapabilityError::SpaceExhausted:
        return SystemCallError::SpaceExhausted;
    case Object::CapabilityError::RightsDenied:
        return SystemCallError::RightsDenied;
    case Object::CapabilityError::RightsAmplification:
        return SystemCallError::RightsAmplification;
    case Object::CapabilityError::NoAttenuation:
        return SystemCallError::NoAttenuation;
    case Object::CapabilityError::TypeMismatch:
        return SystemCallError::TypeMismatch;
    case Object::CapabilityError::Revoked:
        return SystemCallError::Revoked;
    case Object::CapabilityError::Expired:
        return SystemCallError::Expired;
    case Object::CapabilityError::LeaseExtension:
        return SystemCallError::LeaseExtension;
    case Object::CapabilityError::RevocationDomainExhausted:
        return SystemCallError::RevocationDomainExhausted;
    }
    return SystemCallError::InvalidConfiguration;
}

[[nodiscard]] constexpr Object::CapabilityHandle DecodeHandle(const SystemCallRequest& request) noexcept
{
    return Object::CapabilityHandle{request.Arguments[0U], request.Arguments[1U]};
}

[[nodiscard]] SystemCallResponse DispatchCapabilityCreation(Object::CapabilitySpace& caller,
                                                            const SystemCallRequest& request,
                                                            const CapabilityCreationKind kind) noexcept
{
    // Derive argument 4 selects the result's revocation domain; Duplicate has
    // no flags.
    const Foundation::UInt64 flags = request.Arguments[4U];
    const Foundation::UInt64 permittedFlags =
        (kind == CapabilityCreationKind::Derived) ? SystemCallDeriveChildRevocationDomain : 0U;
    if (!ArgumentsAreZero(request, 5U) || ((flags & ~permittedFlags) != 0U) || (request.Arguments[2U] == 0U) ||
        (request.Arguments[2U] > 0xFFFFU))
    {
        return MakeResponse(SystemCallError::InvalidArguments);
    }
    const Foundation::UInt16 rawRights = static_cast<Foundation::UInt16>(request.Arguments[2U]);
    Object::CapabilityRights rights{};
    if (!Object::TryDecodeCapabilityRights(rawRights, rights))
    {
        return MakeResponse(SystemCallError::InvalidArguments);
    }

    const Object::CapabilityHandle source = DecodeHandle(request);
    const Object::MonotonicTime deadline{request.Arguments[3U]};
    const auto created =
        (kind == CapabilityCreationKind::Duplicate)
            ? caller.Duplicate(source, rights, deadline)
            : caller.Derive(source, rights, deadline,
                            (flags == SystemCallDeriveChildRevocationDomain) ? Object::DerivedRevocationDomain::Child
                                                                             : Object::DerivedRevocationDomain::Source);
    if (!created.HasValue())
    {
        return MakeResponse(MapCapabilityError(created.GetError()));
    }
    return MakeResponse(SystemCallError::None, created.GetValue().GetSelector(), created.GetValue().GetAuthenticator());
}
} // Namespace

SystemCallResponse SystemCallDispatcher::Dispatch(Object::CapabilitySpace& caller,
                                                  const SystemCallRequest& request) noexcept
{
    switch (static_cast<SystemCallNumber>(request.Number))
    {
    case SystemCallNumber::CapabilityClose:
        return DispatchClose(caller, request);
    case SystemCallNumber::CapabilityQuery:
        return DispatchQuery(caller, request);
    case SystemCallNumber::CapabilityDerive:
        return DispatchDerive(caller, request);
    case SystemCallNumber::CapabilityDuplicate:
        return DispatchDuplicate(caller, request);
    case SystemCallNumber::CapabilityRevoke:
        return DispatchRevoke(caller, request);
    case SystemCallNumber::CapabilityGrant:
        return DispatchGrant(caller, request);
    case SystemCallNumber::IpcSend:
    case SystemCallNumber::IpcReceive:
    case SystemCallNumber::WaitSetWait:
    case SystemCallNumber::DebugWrite:
    case SystemCallNumber::FaultResolve:
    case SystemCallNumber::ObjectCreate:
    case SystemCallNumber::ObjectDestroy:
    case SystemCallNumber::MemoryMap:
    case SystemCallNumber::MemoryUnmap:
    case SystemCallNumber::ThreadExit:
    case SystemCallNumber::ThreadTerminate:
    case SystemCallNumber::WaitSetBind:
    case SystemCallNumber::DeviceControl:
    case SystemCallNumber::IommuControl:
    case SystemCallNumber::PortIo:
    case SystemCallNumber::InterruptControl:
        return MakeResponse(SystemCallError::UnknownSystemCall);
    }
    return MakeResponse(SystemCallError::UnknownSystemCall);
}

bool SystemCallDispatcher::DecodeThreadExit(const SystemCallRequest& request, Foundation::UInt64& exitCode) noexcept
{
    if ((request.Number != static_cast<Foundation::UInt64>(SystemCallNumber::ThreadExit)) ||
        ((request.Arguments[0U] >> 63U) != 0U))
    {
        return false;
    }
    for (Foundation::Size index{1U}; index < SystemCallArgumentCount; ++index)
    {
        if (request.Arguments[index] != 0U)
        {
            return false;
        }
    }
    exitCode = request.Arguments[0U];
    return true;
}

SystemCallResponse SystemCallDispatcher::DispatchClose(Object::CapabilitySpace& caller,
                                                       const SystemCallRequest& request) noexcept
{
    if (!ArgumentsAreZero(request, 2U))
    {
        return MakeResponse(SystemCallError::InvalidArguments);
    }
    return MakeResponse(MapCapabilityError(caller.Close(DecodeHandle(request))));
}

SystemCallResponse SystemCallDispatcher::DispatchQuery(Object::CapabilitySpace& caller,
                                                       const SystemCallRequest& request) noexcept
{
    if (!ArgumentsAreZero(request, 2U))
    {
        return MakeResponse(SystemCallError::InvalidArguments);
    }
    const auto descriptor = caller.Query(DecodeHandle(request));
    if (!descriptor.HasValue())
    {
        return MakeResponse(MapCapabilityError(descriptor.GetError()));
    }

    const Foundation::UInt8 objectType = Object::EncodeObjectType(descriptor.GetValue().Type);
    if (objectType == 0U)
    {
        return MakeResponse(SystemCallError::InvalidConfiguration);
    }
    const Foundation::UInt64 information =
        static_cast<Foundation::UInt64>(objectType) |
        (static_cast<Foundation::UInt64>(descriptor.GetValue().Rights.GetRawValue()) << SystemCallQueryRightsShift);
    return MakeResponse(SystemCallError::None, information, descriptor.GetValue().LeaseDeadline.GetTicks());
}

SystemCallResponse SystemCallDispatcher::DispatchDerive(Object::CapabilitySpace& caller,
                                                        const SystemCallRequest& request) noexcept
{
    return DispatchCapabilityCreation(caller, request, CapabilityCreationKind::Derived);
}

SystemCallResponse SystemCallDispatcher::DispatchDuplicate(Object::CapabilitySpace& caller,
                                                           const SystemCallRequest& request) noexcept
{
    return DispatchCapabilityCreation(caller, request, CapabilityCreationKind::Duplicate);
}

SystemCallResponse SystemCallDispatcher::DispatchRevoke(Object::CapabilitySpace& caller,
                                                        const SystemCallRequest& request) noexcept
{
    if (!ArgumentsAreZero(request, 2U))
    {
        return MakeResponse(SystemCallError::InvalidArguments);
    }
    return MakeResponse(MapCapabilityError(caller.Revoke(DecodeHandle(request))));
}
// One capability into a CapabilitySpace the caller manages: the transfer moves or copies it
// atomically with the source's lease, and a copy never carries Revoke.
SystemCallResponse SystemCallDispatcher::DispatchGrant(Object::CapabilitySpace& caller,
                                                       const SystemCallRequest& request) noexcept
{
    const Foundation::UInt64 encoded = request.Arguments[4U];
    const Foundation::UInt64 disposition = request.Arguments[5U];
    const Foundation::UInt64 type = (encoded >> Abi::CapabilityGrantTypeShift) & 0xFFU;
    Object::CapabilityRights rights{};
    if (((encoded >> (Abi::CapabilityGrantTypeShift + 8U)) != 0U) || (type == 0U) ||
        !Object::TryDecodeCapabilityRights(static_cast<Foundation::UInt16>(encoded & 0xFFFFU), rights) ||
        ((disposition != Abi::CapabilityGrantMove) && (disposition != Abi::CapabilityGrantCopy)))
    {
        return MakeResponse(SystemCallError::InvalidArguments);
    }
    const bool move = disposition == Abi::CapabilityGrantMove;
    const auto destination = caller.Lookup(Object::CapabilityHandle{request.Arguments[2U], request.Arguments[3U]},
                                           Object::ObjectType::CapabilitySpace, Object::CapabilityOperation::Manage);
    if (!destination.HasValue())
    {
        return MakeResponse(MapCapabilityError(destination.GetError()));
    }
    // Only CapabilitySpaceObject reports ObjectType::CapabilitySpace; the lease keeps the
    // destination registered through the transfer.
    Object::CapabilitySpace& target =
        static_cast<Object::CapabilitySpaceObject&>(destination.GetValue().GetObject()).GetSpace();
    const Object::CapabilityHandle source = DecodeHandle(request);
    Object::MonotonicTime deadline{};
    {
        const auto lease =
            caller.Lookup(source, static_cast<Object::ObjectType>(type),
                          move ? Object::CapabilityOperation::Transfer : Object::CapabilityOperation::Duplicate);
        if (!lease.HasValue())
        {
            return MakeResponse(MapCapabilityError(lease.GetError()));
        }
        deadline = lease.GetValue().GetDescriptor().LeaseDeadline;
    }
    const Object::CapabilityTransferRequest transfer{
        .Source = source,
        .RequestedDeadline = deadline,
        .RequestedRights = rights,
        .ExpectedType = static_cast<Object::ObjectType>(type),
        .Disposition = move ? Object::CapabilityTransferDisposition::Move : Object::CapabilityTransferDisposition::Copy,
        .Reserved = 0U,
    };
    const auto granted = caller.TransferBatchTo(target, Object::CapabilityTransferRequestView{&transfer, 1U});
    if (!granted.HasValue())
    {
        return MakeResponse(MapCapabilityError(granted.GetError()));
    }
    return MakeResponse(SystemCallError::None, granted.GetValue().DestinationHandles[0U].GetSelector(),
                        granted.GetValue().DestinationHandles[0U].GetAuthenticator());
}
} // Namespace Asteria::Kernel::System
