// SPDX-License-Identifier: Apache-2.0

#include "Kernel/System/IpcSystemCallDispatcher.hpp"

#include "Kernel/Diagnostics/DiagnosticChannel.hpp"
#include "Abi/ComponentBootstrap.hpp"
#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Ipc/IpcDeliveryCodec.hpp"
#include "Kernel/Memory/IPagerResolution.hpp"
#include "Kernel/Memory/FaultChannel.hpp"
#include "Kernel/Memory/FramePool.hpp"
#include "Kernel/Memory/IUserMemoryAccess.hpp"
#include "Kernel/System/IObjectManagement.hpp"
#include "Kernel/System/SystemCallScratch.hpp"

static_assert((1U << Asteria::Abi::ObjectCreateRamFrameMaximumAlignmentLog2) ==
              Asteria::Kernel::Memory::FramePool::MaximumRunPages);

namespace Asteria::Kernel::System
{
namespace
{
using DispatchResult = Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>;

class ScratchScope final
{
public:
    explicit ScratchScope(SystemCallScratch& scratch) noexcept : m_scratch{scratch}
    {
        m_scratch.Clear();
    }

    ~ScratchScope() noexcept
    {
        m_scratch.Clear();
    }

    ScratchScope(const ScratchScope&) = delete;
    ScratchScope& operator=(const ScratchScope&) = delete;
    ScratchScope(ScratchScope&&) = delete;
    ScratchScope& operator=(ScratchScope&&) = delete;

private:
    SystemCallScratch& m_scratch;
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

[[nodiscard]] constexpr IpcSystemCallDispatchFailure
MakeFailure(const IpcSystemCallDispatchError error,
            const Ipc::EndpointErrorCode endpoint = Ipc::EndpointErrorCode::None,
            const Memory::UserMemoryAccessError userMemory = Memory::UserMemoryAccessError::None) noexcept
{
    return IpcSystemCallDispatchFailure{
        .Error = error,
        .Endpoint = endpoint,
        .UserMemory = userMemory,
        .Reserved = 0U,
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

[[nodiscard]] constexpr Object::CapabilityHandle DecodeHandle(const SystemCallRequest& request) noexcept
{
    return Object::CapabilityHandle{
        request.Arguments[0U],
        request.Arguments[1U],
    };
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

[[nodiscard]] constexpr bool IsUserMemoryProtocolViolation(const Memory::UserMemoryAccessError error) noexcept
{
    switch (error)
    {
    case Memory::UserMemoryAccessError::ForeignReservation:
    case Memory::UserMemoryAccessError::StaleReservation:
    case Memory::UserMemoryAccessError::BackendProtocolViolation:
        return true;

    case Memory::UserMemoryAccessError::None:
    case Memory::UserMemoryAccessError::InvalidRange:
    case Memory::UserMemoryAccessError::NullKernelBuffer:
    case Memory::UserMemoryAccessError::NonCanonicalEmptyBuffer:
    case Memory::UserMemoryAccessError::KernelBufferOverflow:
    case Memory::UserMemoryAccessError::SizeMismatch:
    case Memory::UserMemoryAccessError::NotReady:
    case Memory::UserMemoryAccessError::AccessDenied:
    case Memory::UserMemoryAccessError::PinFailed:
    case Memory::UserMemoryAccessError::Fault:
    case Memory::UserMemoryAccessError::ReservationExhausted:
        return false;
    }
    return true;
}

[[nodiscard]] constexpr SystemCallError MapUserMemoryError(const Memory::UserMemoryAccessError error) noexcept
{
    switch (error)
    {
    case Memory::UserMemoryAccessError::None:
        return SystemCallError::None;
    case Memory::UserMemoryAccessError::InvalidRange:
    case Memory::UserMemoryAccessError::NullKernelBuffer:
    case Memory::UserMemoryAccessError::NonCanonicalEmptyBuffer:
    case Memory::UserMemoryAccessError::KernelBufferOverflow:
    case Memory::UserMemoryAccessError::SizeMismatch:
        return SystemCallError::InvalidArguments;
    case Memory::UserMemoryAccessError::NotReady:
        return SystemCallError::InvalidConfiguration;
    case Memory::UserMemoryAccessError::AccessDenied:
    case Memory::UserMemoryAccessError::Fault:
        return SystemCallError::UserMemoryFault;
    case Memory::UserMemoryAccessError::PinFailed:
    case Memory::UserMemoryAccessError::ReservationExhausted:
        return SystemCallError::ResourceExhausted;
    case Memory::UserMemoryAccessError::ForeignReservation:
    case Memory::UserMemoryAccessError::StaleReservation:
    case Memory::UserMemoryAccessError::BackendProtocolViolation:
        return SystemCallError::InvalidConfiguration;
    }
    return SystemCallError::InvalidConfiguration;
}

[[nodiscard]] constexpr bool IsEndpointProtocolViolation(const Ipc::EndpointErrorCode error) noexcept
{
    switch (error)
    {
    case Ipc::EndpointErrorCode::QueueCorrupt:
    case Ipc::EndpointErrorCode::InvalidReservation:
    case Ipc::EndpointErrorCode::StaleReservation:
        return true;

    case Ipc::EndpointErrorCode::None:
    case Ipc::EndpointErrorCode::InvalidConfiguration:
    case Ipc::EndpointErrorCode::InvalidArgument:
    case Ipc::EndpointErrorCode::InvalidMessage:
    case Ipc::EndpointErrorCode::WouldBlock:
    case Ipc::EndpointErrorCode::CapabilityTransferFailed:
    case Ipc::EndpointErrorCode::SequenceExhausted:
    case Ipc::EndpointErrorCode::ReservationActive:
    case Ipc::EndpointErrorCode::InvocationRejected:
        return false;
    }
    return true;
}

[[nodiscard]] constexpr SystemCallError MapEndpointError(const Ipc::EndpointError& error) noexcept
{
    switch (error.Code)
    {
    case Ipc::EndpointErrorCode::None:
        return SystemCallError::None;
    case Ipc::EndpointErrorCode::InvalidConfiguration:
        return SystemCallError::InvalidConfiguration;
    case Ipc::EndpointErrorCode::InvalidArgument:
    case Ipc::EndpointErrorCode::InvalidMessage:
        return SystemCallError::InvalidArguments;
    case Ipc::EndpointErrorCode::WouldBlock:
    case Ipc::EndpointErrorCode::ReservationActive:
        return SystemCallError::WouldBlock;
    case Ipc::EndpointErrorCode::CapabilityTransferFailed:
    case Ipc::EndpointErrorCode::InvocationRejected:
        return MapCapabilityError(error.Capability);
    case Ipc::EndpointErrorCode::SequenceExhausted:
        return SystemCallError::ResourceExhausted;
    case Ipc::EndpointErrorCode::QueueCorrupt:
    case Ipc::EndpointErrorCode::InvalidReservation:
    case Ipc::EndpointErrorCode::StaleReservation:
        return SystemCallError::InvalidConfiguration;
    }
    return SystemCallError::InvalidConfiguration;
}

[[nodiscard]] constexpr SystemCallError MapRangeError(const Memory::UserMemoryRangeError error) noexcept
{
    switch (error)
    {
    case Memory::UserMemoryRangeError::None:
        return SystemCallError::None;
    case Memory::UserMemoryRangeError::NullAddress:
    case Memory::UserMemoryRangeError::NonCanonicalEmptyRange:
    case Memory::UserMemoryRangeError::RangeTooLarge:
    case Memory::UserMemoryRangeError::AddressOverflow:
        return SystemCallError::InvalidArguments;
    }
    return SystemCallError::InvalidArguments;
}

[[nodiscard]] bool ContextIsValid(const SystemCallContext& context) noexcept
{
    if ((context.Capabilities == nullptr) || (context.UserMemory == nullptr) || (context.Scratch == nullptr) ||
        !context.Capabilities->IsReady() || !context.ResourceDomain.IsValid() || !context.AddressSpace.IsValid() ||
        !context.UserMemory->IsReady())
    {
        return false;
    }
    const Memory::AddressSpaceId memoryAddressSpace = context.UserMemory->GetAddressSpaceId();
    return memoryAddressSpace.IsValid() && memoryAddressSpace.IsEqualTo(context.AddressSpace);
}

[[nodiscard]] IpcSystemCallDispatchFailure EndpointFailure(const Ipc::EndpointErrorCode endpoint) noexcept
{
    return MakeFailure(IpcSystemCallDispatchError::EndpointProtocolViolation, endpoint);
}

[[nodiscard]] IpcSystemCallDispatchFailure UserMemoryFailure(const Memory::UserMemoryAccessError userMemory) noexcept
{
    return MakeFailure(IpcSystemCallDispatchError::UserMemoryProtocolViolation, Ipc::EndpointErrorCode::None,
                       userMemory);
}

[[nodiscard]] bool CancelPreparedReceive(Ipc::Endpoint& endpoint, const Ipc::EndpointReceiveReservation reservation,
                                         IpcSystemCallDispatchFailure& failure) noexcept
{
    const Ipc::EndpointError cancelled = endpoint.CancelPreparedReceive(reservation);
    if (cancelled.Code == Ipc::EndpointErrorCode::None)
    {
        return true;
    }
    failure = EndpointFailure(cancelled.Code);
    return false;
}

[[nodiscard]] [[gnu::noinline]] DispatchResult PrepareSendFrame(const SystemCallContext& context,
                                                                const SystemCallRequest& request) noexcept
{
    const Foundation::Size frameSize = static_cast<Foundation::Size>(request.Arguments[3U]);
    const auto sourceRange = Memory::UserMemoryRange::Create(request.Arguments[2U], frameSize);
    if (!sourceRange.HasValue())
    {
        return DispatchResult::Success(MakeResponse(MapRangeError(sourceRange.GetError())));
    }

    SystemCallScratch& scratch = *context.Scratch;
    const Ipc::WritableByteView wire = scratch.GetWritableWireBuffer();
    const Memory::UserMemoryAccessError copied =
        context.UserMemory->CopyFromUser(sourceRange.GetValue(), Memory::WritableKernelBuffer{
                                                                     .Data = wire.Data,
                                                                     .SizeBytes = frameSize,
                                                                 });
    if (IsUserMemoryProtocolViolation(copied))
    {
        return DispatchResult::Failure(UserMemoryFailure(copied));
    }
    if (copied != Memory::UserMemoryAccessError::None)
    {
        return DispatchResult::Success(MakeResponse(MapUserMemoryError(copied)));
    }

    const Ipc::MessageCodecError decoded = Ipc::MessageCodec::Decode(
        Ipc::ReadOnlyByteView{
            .Data = wire.Data,
            .SizeBytes = frameSize,
        },
        scratch.GetMessage());
    if (decoded != Ipc::MessageCodecError::None)
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
    }
    return DispatchResult::Success(MakeResponse(SystemCallError::None));
}

[[nodiscard]] [[gnu::noinline]] DispatchResult SendPreparedFrame(const SystemCallContext& context,
                                                                 const SystemCallRequest& request) noexcept
{
    Object::CapabilitySpace& capabilities = *context.Capabilities;
    auto endpointLookup =
        capabilities.Lookup(DecodeHandle(request), Object::ObjectType::Endpoint, Object::CapabilityOperation::Invoke);
    if (!endpointLookup.HasValue())
    {
        return DispatchResult::Success(MakeResponse(MapCapabilityError(endpointLookup.GetError())));
    }

    // The lease pins the Endpoint; the send authorizes the same handle again
    // where it publishes, so a Revoke ordered before publication is observed.
    Ipc::Endpoint& endpoint = static_cast<Ipc::Endpoint&>(endpointLookup.GetValue().GetObject());
    const auto sent = endpoint.SendFromInvocation(capabilities, DecodeHandle(request), context.ResourceDomain,
                                                  Ipc::EndpointSendClass::Normal, context.Scratch->GetMessage());
    if (!sent.HasValue())
    {
        if (IsEndpointProtocolViolation(sent.GetError().Code))
        {
            return DispatchResult::Failure(EndpointFailure(sent.GetError().Code));
        }
        return DispatchResult::Success(MakeResponse(MapEndpointError(sent.GetError())));
    }
    return DispatchResult::Success(MakeResponse(SystemCallError::None, sent.GetValue().Sequence));
}

[[nodiscard]] [[gnu::noinline]] DispatchResult CommitPreparedDelivery(
    const SystemCallContext& context, Ipc::Endpoint& endpoint, const Ipc::EndpointReceivePreparation& preparation,
    const Memory::UserMemoryWriteReservation userReservation, const Foundation::Size deliverySize) noexcept
{
    const auto committed = endpoint.CommitPreparedReceive(preparation.Reservation);
    if (!committed.HasValue())
    {
        const Memory::UserMemoryAccessError cancelled = context.UserMemory->CancelCopyToUser(userReservation);
        if (cancelled != Memory::UserMemoryAccessError::None)
        {
            return DispatchResult::Failure(UserMemoryFailure(cancelled));
        }
        if (IsEndpointProtocolViolation(committed.GetError().Code))
        {
            return DispatchResult::Failure(EndpointFailure(committed.GetError().Code));
        }
        // The Endpoint authority ended after the head was reserved: nothing
        // was consumed or written, and the reservation is released.
        IpcSystemCallDispatchFailure failure{};
        if (!CancelPreparedReceive(endpoint, preparation.Reservation, failure))
        {
            return DispatchResult::Failure(failure);
        }
        return DispatchResult::Success(MakeResponse(MapEndpointError(committed.GetError())));
    }

    const Ipc::ReadOnlyByteView wire = context.Scratch->GetWireBuffer(deliverySize);
    const Memory::UserMemoryAccessError copied =
        context.UserMemory->CommitCopyToUser(userReservation, Memory::ReadOnlyKernelBuffer{
                                                                  .Data = wire.Data,
                                                                  .SizeBytes = wire.SizeBytes,
                                                              });
    if (copied != Memory::UserMemoryAccessError::None)
    {
        return DispatchResult::Failure(UserMemoryFailure(copied));
    }
    return DispatchResult::Success(MakeResponse(SystemCallError::None, committed.GetValue().Sequence,
                                                static_cast<Foundation::UInt64>(deliverySize)));
}

[[nodiscard]] [[gnu::noinline]] DispatchResult
PrepareUserDelivery(const SystemCallContext& context, const SystemCallRequest& request, Ipc::Endpoint& endpoint,
                    const Ipc::EndpointReceivePreparation& preparation, const Foundation::Size deliverySize) noexcept
{
    IpcSystemCallDispatchFailure failure{};
    const auto destinationRange = Memory::UserMemoryRange::Create(request.Arguments[2U], deliverySize);
    if (!destinationRange.HasValue())
    {
        if (!CancelPreparedReceive(endpoint, preparation.Reservation, failure))
        {
            return DispatchResult::Failure(failure);
        }
        return DispatchResult::Failure(MakeFailure(IpcSystemCallDispatchError::InvalidContext));
    }

    const auto userReservation = context.UserMemory->PrepareCopyToUser(destinationRange.GetValue());
    if (!userReservation.HasValue())
    {
        if (!CancelPreparedReceive(endpoint, preparation.Reservation, failure))
        {
            return DispatchResult::Failure(failure);
        }
        if (IsUserMemoryProtocolViolation(userReservation.GetError()))
        {
            return DispatchResult::Failure(UserMemoryFailure(userReservation.GetError()));
        }
        return DispatchResult::Success(MakeResponse(MapUserMemoryError(userReservation.GetError())));
    }

    return CommitPreparedDelivery(context, endpoint, preparation, userReservation.GetValue(), deliverySize);
}

[[nodiscard]] [[gnu::noinline]] DispatchResult
EncodePreparedDelivery(const SystemCallContext& context, const SystemCallRequest& request, Ipc::Endpoint& endpoint,
                       const Ipc::EndpointReceivePreparation& preparation, const Foundation::Size capacity) noexcept
{
    IpcSystemCallDispatchFailure failure{};
    SystemCallScratch& scratch = *context.Scratch;
    const auto encoded =
        Ipc::IpcDeliveryCodec::Encode(preparation.Metadata, scratch.GetMessage(), scratch.GetWritableWireBuffer());
    if (!encoded.HasValue())
    {
        if (!CancelPreparedReceive(endpoint, preparation.Reservation, failure))
        {
            return DispatchResult::Failure(failure);
        }
        return DispatchResult::Failure(EndpointFailure(Ipc::EndpointErrorCode::QueueCorrupt));
    }

    const Foundation::Size deliverySize = encoded.GetValue();
    if (deliverySize > capacity)
    {
        if (!CancelPreparedReceive(endpoint, preparation.Reservation, failure))
        {
            return DispatchResult::Failure(failure);
        }
        return DispatchResult::Success(MakeResponse(SystemCallError::SpaceExhausted));
    }
    return PrepareUserDelivery(context, request, endpoint, preparation, deliverySize);
}

[[nodiscard]] [[gnu::noinline]] DispatchResult PrepareEndpointDelivery(const SystemCallContext& context,
                                                                       const SystemCallRequest& request,
                                                                       Ipc::Endpoint& endpoint,
                                                                       const Foundation::Size capacity) noexcept
{
    const auto prepared = endpoint.PrepareReceiveFromInvocation(*context.Capabilities, DecodeHandle(request),
                                                                context.Scratch->GetMessage());
    if (!prepared.HasValue())
    {
        if (IsEndpointProtocolViolation(prepared.GetError().Code))
        {
            return DispatchResult::Failure(EndpointFailure(prepared.GetError().Code));
        }
        return DispatchResult::Success(MakeResponse(MapEndpointError(prepared.GetError())));
    }
    return EncodePreparedDelivery(context, request, endpoint, prepared.GetValue(), capacity);
}

[[nodiscard]] [[gnu::noinline]] DispatchResult ReceiveFromEndpoint(const SystemCallContext& context,
                                                                   const SystemCallRequest& request,
                                                                   const Foundation::Size capacity) noexcept
{
    Object::CapabilitySpace& capabilities = *context.Capabilities;
    auto endpointLookup =
        capabilities.Lookup(DecodeHandle(request), Object::ObjectType::Endpoint, Object::CapabilityOperation::Read);
    if (!endpointLookup.HasValue())
    {
        if (endpointLookup.GetError() != Object::CapabilityError::TypeMismatch)
        {
            return DispatchResult::Success(MakeResponse(MapCapabilityError(endpointLookup.GetError())));
        }
        const auto channel = capabilities.Lookup(DecodeHandle(request), Object::ObjectType::FaultChannel,
                                                 Object::CapabilityOperation::Read);
        if (!channel.HasValue())
        {
            return DispatchResult::Success(MakeResponse(MapCapabilityError(channel.GetError())));
        }
        return PrepareEndpointDelivery(context, request,
                                       static_cast<Memory::FaultChannel&>(channel.GetValue().GetObject()).GetEndpoint(),
                                       capacity);
    }

    // PrepareReceiveFromInvocation requires this space to be the Endpoint's
    // receiver space (RightsDenied) and authorizes the handle again before
    // the reservation and when the head is consumed.
    Ipc::Endpoint& endpoint = static_cast<Ipc::Endpoint&>(endpointLookup.GetValue().GetObject());
    return PrepareEndpointDelivery(context, request, endpoint, capacity);
}
} // Namespace

Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
IpcSystemCallDispatcher::Dispatch(const SystemCallContext& context, const SystemCallRequest& request) noexcept
{
    if (!ContextIsValid(context))
    {
        return DispatchResult::Failure(MakeFailure(IpcSystemCallDispatchError::InvalidContext));
    }

    switch (static_cast<SystemCallNumber>(request.Number))
    {
    case SystemCallNumber::IpcSend:
        return DispatchSend(context, request);
    case SystemCallNumber::IpcReceive:
        return DispatchReceive(context, request);
    case SystemCallNumber::DebugWrite:
        return DispatchDebugWrite(context, request);
    case SystemCallNumber::FaultResolve:
        return DispatchFaultResolve(context, request);
    case SystemCallNumber::ObjectCreate:
        return DispatchObjectCreate(context, request);
    case SystemCallNumber::ObjectDestroy:
        return DispatchObjectDestroy(context, request);
    case SystemCallNumber::MemoryMap:
        return DispatchMemoryMap(context, request);
    case SystemCallNumber::MemoryUnmap:
        return DispatchMemoryUnmap(context, request);
    case SystemCallNumber::ThreadTerminate:
        return DispatchThreadTerminate(context, request);
    case SystemCallNumber::WaitSetBind:
        return DispatchWaitSetBind(context, request);
    case SystemCallNumber::DeviceControl:
        return DispatchDeviceControl(context, request);
    case SystemCallNumber::IommuControl:
        return DispatchIommuControl(context, request);
    case SystemCallNumber::PortIo:
        return DispatchResult::Success((context.ObjectManagement != nullptr)
                                           ? context.ObjectManagement->AccessPorts(*context.Capabilities, request)
                                           : MakeResponse(SystemCallError::InvalidConfiguration));
    case SystemCallNumber::InterruptControl:
        return DispatchInterruptControl(context, request);

    case SystemCallNumber::CapabilityClose:
    case SystemCallNumber::CapabilityQuery:
    case SystemCallNumber::CapabilityDerive:
    case SystemCallNumber::CapabilityDuplicate:
    case SystemCallNumber::CapabilityRevoke:
    case SystemCallNumber::WaitSetWait:
    case SystemCallNumber::ThreadExit:
    case SystemCallNumber::CapabilityGrant:
        return DispatchResult::Success(MakeResponse(SystemCallError::UnknownSystemCall));
    }
    return DispatchResult::Success(MakeResponse(SystemCallError::UnknownSystemCall));
}

Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
IpcSystemCallDispatcher::DispatchSend(const SystemCallContext& context, const SystemCallRequest& request) noexcept
{
    ScratchScope scratchScope{*context.Scratch};
    if (!ArgumentsAreZero(request, 4U) || (request.Arguments[3U] < Ipc::MessageCodec::WireHeaderSizeBytes) ||
        (request.Arguments[3U] > Ipc::MessageCodec::MaximumWireSizeBytes))
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
    }

    const DispatchResult prepared = PrepareSendFrame(context, request);
    if (!prepared.HasValue() || (prepared.GetValue().Error != SystemCallError::None))
    {
        return prepared;
    }
    return SendPreparedFrame(context, request);
}

Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
IpcSystemCallDispatcher::DispatchReceive(const SystemCallContext& context, const SystemCallRequest& request) noexcept
{
    ScratchScope scratchScope{*context.Scratch};
    constexpr Foundation::UInt64 minimumDeliverySize{Ipc::IpcDeliveryCodec::WireHeaderSizeBytes +
                                                     Ipc::MessageCodec::WireHeaderSizeBytes};
    if (!ArgumentsAreZero(request, 4U) || (request.Arguments[3U] < minimumDeliverySize) ||
        (request.Arguments[3U] > Ipc::IpcDeliveryCodec::MaximumWireSizeBytes))
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
    }

    const auto capacityRange =
        Memory::UserMemoryRange::Create(request.Arguments[2U], static_cast<Foundation::Size>(request.Arguments[3U]));
    if (!capacityRange.HasValue())
    {
        return DispatchResult::Success(MakeResponse(MapRangeError(capacityRange.GetError())));
    }
    return ReceiveFromEndpoint(context, request, capacityRange.GetValue().GetSizeBytes());
}

Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
IpcSystemCallDispatcher::DispatchDebugWrite(const SystemCallContext& context, const SystemCallRequest& request) noexcept
{
    ScratchScope scratchScope{*context.Scratch};
    if (!ArgumentsAreZero(request, 4U) || (request.Arguments[3U] == 0U) ||
        (request.Arguments[3U] > Abi::DebugWriteMaximumBytes))
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
    }
    const Foundation::Size sizeBytes = static_cast<Foundation::Size>(request.Arguments[3U]);

    auto channelLookup = context.Capabilities->Lookup(DecodeHandle(request), Object::ObjectType::DiagnosticChannel,
                                                      Object::CapabilityOperation::Write);
    if (!channelLookup.HasValue())
    {
        return DispatchResult::Success(MakeResponse(MapCapabilityError(channelLookup.GetError())));
    }

    const auto sourceRange = Memory::UserMemoryRange::Create(request.Arguments[2U], sizeBytes);
    if (!sourceRange.HasValue())
    {
        return DispatchResult::Success(MakeResponse(MapRangeError(sourceRange.GetError())));
    }
    const Ipc::WritableByteView text = context.Scratch->GetWritableWireBuffer();
    const Memory::UserMemoryAccessError copied =
        context.UserMemory->CopyFromUser(sourceRange.GetValue(), Memory::WritableKernelBuffer{
                                                                     .Data = text.Data,
                                                                     .SizeBytes = sizeBytes,
                                                                 });
    if (IsUserMemoryProtocolViolation(copied))
    {
        return DispatchResult::Failure(UserMemoryFailure(copied));
    }
    if (copied != Memory::UserMemoryAccessError::None)
    {
        return DispatchResult::Success(MakeResponse(MapUserMemoryError(copied)));
    }

    // The capability lookup checked the object type, and only DiagnosticChannel instances report
    // ObjectType::DiagnosticChannel.
    Diagnostics::DiagnosticChannel& channel =
        static_cast<Diagnostics::DiagnosticChannel&>(channelLookup.GetValue().GetObject());
    if (channel.Write(text.Data, sizeBytes) != Diagnostics::DiagnosticWriteError::None)
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
    }
    return DispatchResult::Success(MakeResponse(SystemCallError::None, sizeBytes, 0U));
}
namespace
{
[[nodiscard]] Foundation::UInt64 LoadWord(const Foundation::UInt8* const bytes, const Foundation::Size offset) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[offset + index]) << (8U * index);
    }
    return value;
}

[[nodiscard]] Object::CapabilityHandle LoadHandle(const Foundation::UInt8* const bytes,
                                                  const Foundation::Size offset) noexcept
{
    return Object::CapabilityHandle{LoadWord(bytes, offset), LoadWord(bytes, offset + 8U)};
}

/// Decodes the permission byte, or returns false for an undefined bit.
[[nodiscard]] bool DecodePagePermissions(const Foundation::UInt8 bits, Memory::PagePermissions& output) noexcept
{
    constexpr Memory::PagePermission Known[]{
        Memory::PagePermission::Read,
        Memory::PagePermission::Write,
        Memory::PagePermission::Execute,
        Memory::PagePermission::User,
    };
    Memory::PagePermissions permissions{};
    Foundation::UInt8 remaining = bits;
    for (const Memory::PagePermission permission : Known)
    {
        const auto bit = static_cast<Foundation::UInt8>(permission);
        if ((remaining & bit) != 0U)
        {
            permissions = permissions.With(permission);
            remaining = static_cast<Foundation::UInt8>(remaining & static_cast<Foundation::UInt8>(~bit));
        }
    }
    if (remaining != 0U)
    {
        return false;
    }
    output = permissions;
    return true;
}

[[nodiscard]] constexpr SystemCallResponse MapPagerResolution(const Memory::PagerResolutionResult& result) noexcept
{
    switch (result.Error)
    {
    case Memory::PagerResolutionError::None:
        return SystemCallResponse{.Error = SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
    case Memory::PagerResolutionError::InvalidRequest:
    case Memory::PagerResolutionError::MappingRejected:
        return SystemCallResponse{.Error = SystemCallError::InvalidArguments, .Value0 = 0U, .Value1 = 0U};
    case Memory::PagerResolutionError::CapabilityRejected:
        return SystemCallResponse{.Error = MapCapabilityError(result.Capability), .Value0 = 0U, .Value1 = 0U};
    case Memory::PagerResolutionError::NotPending:
        return SystemCallResponse{.Error = SystemCallError::ObjectNotFound, .Value0 = 0U, .Value1 = 0U};
    case Memory::PagerResolutionError::AddressSpaceMismatch:
        return SystemCallResponse{.Error = SystemCallError::RightsDenied, .Value0 = 0U, .Value1 = 0U};
    case Memory::PagerResolutionError::Busy:
        return SystemCallResponse{.Error = SystemCallError::WouldBlock, .Value0 = 0U, .Value1 = 0U};
    case Memory::PagerResolutionError::Faulted:
        break;
    }
    return SystemCallResponse{.Error = SystemCallError::InvalidConfiguration, .Value0 = 0U, .Value1 = 0U};
}
} // Namespace

namespace
{
/// Copies the mapping block FaultResolve and MemoryMap share (argument 3
/// names it, argument 4 must equal its size) from the caller's memory and
/// decodes it. False when the call completes with outcome instead.
[[nodiscard, gnu::noinline]] bool DecodeMappingBlock(const SystemCallContext& context, const SystemCallRequest& request,
                                                     Memory::MemoryMapRequest& decoded,
                                                     DispatchResult& outcome) noexcept
{
    if (request.Arguments[4U] != Abi::FaultResolveRequestSizeBytes)
    {
        outcome = DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
        return false;
    }
    const auto sourceRange = Memory::UserMemoryRange::Create(request.Arguments[3U], Abi::FaultResolveRequestSizeBytes);
    if (!sourceRange.HasValue())
    {
        outcome = DispatchResult::Success(MakeResponse(MapRangeError(sourceRange.GetError())));
        return false;
    }
    const Ipc::WritableByteView wire = context.Scratch->GetWritableWireBuffer();
    const Memory::UserMemoryAccessError copied =
        context.UserMemory->CopyFromUser(sourceRange.GetValue(), Memory::WritableKernelBuffer{
                                                                     .Data = wire.Data,
                                                                     .SizeBytes = Abi::FaultResolveRequestSizeBytes,
                                                                 });
    if (IsUserMemoryProtocolViolation(copied))
    {
        outcome = DispatchResult::Failure(UserMemoryFailure(copied));
        return false;
    }
    if (copied != Memory::UserMemoryAccessError::None)
    {
        outcome = DispatchResult::Success(MakeResponse(MapUserMemoryError(copied)));
        return false;
    }

    const Foundation::UInt8* const bytes = wire.Data;
    Memory::PagePermissions permissions{};
    bool reservedZero{true};
    for (Foundation::Size index{Abi::FaultResolvePermissionsOffset + 1U}; index < Abi::FaultResolveRequestSizeBytes;
         ++index)
    {
        reservedZero = reservedZero && (bytes[index] == 0U);
    }
    if (!reservedZero || !DecodePagePermissions(bytes[Abi::FaultResolvePermissionsOffset], permissions))
    {
        outcome = DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
        return false;
    }
    decoded = Memory::MemoryMapRequest{
        .AddressSpace = LoadHandle(bytes, Abi::FaultResolveAddressSpaceOffset),
        .MemoryObject = LoadHandle(bytes, Abi::FaultResolveMemoryObjectOffset),
        .Frame = LoadHandle(bytes, Abi::FaultResolveFrameOffset),
        .Mapping =
            Memory::PinnedFrameMappingRequest{
                .VirtualStart = Memory::VirtualAddress{LoadWord(bytes, Abi::FaultResolveVirtualStartOffset)},
                .Pages = Memory::PageCount{LoadWord(bytes, Abi::FaultResolvePageCountOffset)},
                .MemoryObjectOffsetPages = LoadWord(bytes, Abi::FaultResolveMemoryObjectPageOffset),
                .FrameOffsetPages = LoadWord(bytes, Abi::FaultResolveFramePageOffset),
                .Permissions = permissions,
                .Reserved = {},
            },
    };
    return true;
}
} // Namespace

namespace
{
[[nodiscard, gnu::noinline]] SystemCallResponse ResolveByToken(const SystemCallContext& context,
                                                               const Memory::PagerResolveRequest& request) noexcept
{
    Memory::IPagerResolution* resolution{nullptr};
    Object::ObjectPin channelPin;
    {
        const auto token = context.Capabilities->Lookup(request.FaultToken, Object::ObjectType::FaultToken,
                                                        Object::CapabilityOperation::Invoke);
        if (!token.HasValue())
        {
            return MakeResponse(MapCapabilityError(token.GetError()));
        }
        const auto& storage = static_cast<const Memory::FaultToken&>(token.GetValue().GetObject());
        resolution = storage.GetResolution();
        if (resolution == nullptr)
        {
            return MakeResponse(SystemCallError::InvalidConfiguration);
        }
        if (storage.PinResolutionChannel(channelPin) != Object::ObjectPinError::Ok)
        {
            return MakeResponse(SystemCallError::ObjectNotFound);
        }
    }
    return MapPagerResolution(resolution->Resolve(*context.Capabilities, request));
}
} // Namespace

Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
IpcSystemCallDispatcher::DispatchFaultResolve(const SystemCallContext& context,
                                              const SystemCallRequest& request) noexcept
{
    ScratchScope scratchScope{*context.Scratch};
    if (request.Arguments[2U] == static_cast<Foundation::UInt64>(Abi::FaultResolveKind::Terminate))
    {
        return DispatchFaultTerminate(context, request);
    }
    if ((request.Arguments[2U] != static_cast<Foundation::UInt64>(Abi::FaultResolveKind::Map)) ||
        (request.Arguments[4U] != Abi::FaultResolveRequestSizeBytes) || (request.Arguments[5U] != 0U))
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
    }

    Memory::MemoryMapRequest block{};
    DispatchResult outcome = DispatchResult::Success(MakeResponse(SystemCallError::None));
    if (!DecodeMappingBlock(context, request, block, outcome))
    {
        return outcome;
    }
    const Memory::PagerResolveRequest decoded{
        .FaultToken = DecodeHandle(request),
        .AddressSpace = block.AddressSpace,
        .MemoryObject = block.MemoryObject,
        .Frame = block.Frame,
        .Mapping =
            Memory::FaultFrameMappingRequest{
                .VirtualStart = block.Mapping.VirtualStart,
                .Pages = block.Mapping.Pages,
                .MemoryObjectOffsetPages = block.Mapping.MemoryObjectOffsetPages,
                .FrameOffsetPages = block.Mapping.FrameOffsetPages,
                .Permissions = block.Mapping.Permissions,
                .Reserved = {},
            },
        .Kind = Memory::FaultResolutionKind::Map,
        .Reserved = {},
    };
    return DispatchResult::Success(ResolveByToken(context, decoded));
}

// Terminate carries no request: arguments 3 to 5 are zero.
Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
IpcSystemCallDispatcher::DispatchFaultTerminate(const SystemCallContext& context,
                                                const SystemCallRequest& request) noexcept
{
    if ((request.Arguments[3U] != 0U) || (request.Arguments[4U] != 0U) || (request.Arguments[5U] != 0U))
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
    }
    const Memory::PagerResolveRequest decoded{
        .FaultToken = DecodeHandle(request),
        .AddressSpace = {},
        .MemoryObject = {},
        .Frame = {},
        .Mapping = {},
        .Kind = Memory::FaultResolutionKind::Terminate,
        .Reserved = {},
    };
    SystemCallResponse response = ResolveByToken(context, decoded);
    if (response.Error == SystemCallError::None)
    {
        response.Value0 = Abi::FaultTerminatedExitCode;
    }
    return DispatchResult::Success(response);
}
namespace
{
[[nodiscard]] Foundation::UInt16 LoadHalf(const Foundation::UInt8* const bytes, const Foundation::Size offset) noexcept
{
    return static_cast<Foundation::UInt16>(
        static_cast<Foundation::UInt16>(bytes[offset]) |
        static_cast<Foundation::UInt16>(static_cast<Foundation::UInt16>(bytes[offset + 1U]) << 8U));
}

[[nodiscard]] Ipc::EndpointConfiguration DecodeEndpointConfiguration(const Foundation::UInt8* const bytes) noexcept
{
    return Ipc::EndpointConfiguration{
        .MaximumQueuedBytes = LoadHalf(bytes, Abi::ObjectCreateEndpointQueuedBytesOffset),
        .ReservedControlBytes = LoadHalf(bytes, Abi::ObjectCreateEndpointReservedControlBytesOffset),
        .MaximumBytesPerSender = LoadHalf(bytes, Abi::ObjectCreateEndpointBytesPerSenderOffset),
        .MaximumQueuedMessages = bytes[Abi::ObjectCreateEndpointQueuedMessagesOffset],
        .ReservedControlMessages = bytes[Abi::ObjectCreateEndpointReservedControlMessagesOffset],
        .MaximumMessagesPerSender = bytes[Abi::ObjectCreateEndpointMessagesPerSenderOffset],
        .MaximumQueuedAttachments = bytes[Abi::ObjectCreateEndpointQueuedAttachmentsOffset],
        .ReservedControlAttachments = bytes[Abi::ObjectCreateEndpointReservedControlAttachmentsOffset],
        .MaximumAttachmentsPerSender = bytes[Abi::ObjectCreateEndpointAttachmentsPerSenderOffset],
        .Reserved = 0U,
    };
}

/// Maps a management result to its response, or false for a result the
/// kernel could neither complete nor undo.
[[nodiscard]] bool MapObjectManagement(const ObjectManagementResult& result, SystemCallResponse& response) noexcept
{
    switch (result.Error)
    {
    case ObjectManagementError::None:
        // A creation returns the new capability; a Thread's destruction its
        // exit code.
        response = result.Created.IsValid()
            ? SystemCallResponse{
                .Error = SystemCallError::None,
                .Value0 = result.Created.GetSelector(),
                .Value1 = result.Created.GetAuthenticator(),
            }
            : SystemCallResponse{.Error = SystemCallError::None, .Value0 = result.Value, .Value1 = result.Value1};
        return true;
    case ObjectManagementError::UnsupportedType:
    case ObjectManagementError::InvalidRequest:
        response = SystemCallResponse{.Error = SystemCallError::InvalidArguments, .Value0 = 0U, .Value1 = 0U};
        return true;
    case ObjectManagementError::CapabilityRejected:
        response = SystemCallResponse{.Error = MapCapabilityError(result.Capability), .Value0 = 0U, .Value1 = 0U};
        return true;
    case ObjectManagementError::NotOwned:
        response = SystemCallResponse{.Error = SystemCallError::RightsDenied, .Value0 = 0U, .Value1 = 0U};
        return true;
    case ObjectManagementError::ResourceExhausted:
        response = SystemCallResponse{.Error = SystemCallError::ResourceExhausted, .Value0 = 0U, .Value1 = 0U};
        return true;
    case ObjectManagementError::Busy:
        response = SystemCallResponse{.Error = SystemCallError::WouldBlock, .Value0 = 0U, .Value1 = 0U};
        return true;
    case ObjectManagementError::NotConfigured:
        response = SystemCallResponse{.Error = SystemCallError::InvalidConfiguration, .Value0 = 0U, .Value1 = 0U};
        return true;
    case ObjectManagementError::ReservedMemory:
        response = {.Error = SystemCallError::ReservedMemory, .Value0 = 0U, .Value1 = 0U};
        return true;
    case ObjectManagementError::BridgeReserved:
        response = {.Error = SystemCallError::BridgeReserved, .Value0 = 0U, .Value1 = 0U};
        return true;
    case ObjectManagementError::DisplayReserved:
        response = {.Error = SystemCallError::DisplayReserved, .Value0 = 0U, .Value1 = 0U};
        return true;
    case ObjectManagementError::Faulted:
        break;
    }
    return false;
}
} // Namespace

namespace
{
/// The exact request size of a creatable type, or zero for another type.
[[nodiscard]] constexpr Foundation::Size ObjectCreateRequestSize(const Foundation::UInt64 type) noexcept
{
    switch (type)
    {
    case Abi::ObjectTypeEndpoint:
        return Abi::ObjectCreateEndpointRequestSizeBytes;
    case Abi::ObjectTypeFrame:
        return Abi::ObjectCreateFrameRequestSizeBytes;
    case Abi::ObjectTypeMemoryObject:
        return Abi::ObjectCreateMemoryObjectRequestSizeBytes;
    case Abi::ObjectTypeThread:
        return Abi::ObjectCreateThreadRequestSizeBytes;
    case Abi::ObjectTypeCapabilitySpace:
        return Abi::ObjectCreateCapabilitySpaceRequestSizeBytes;
    case Abi::ObjectTypeAddressSpace:
        return Abi::ObjectCreateAddressSpaceRequestSizeBytes;
    case Abi::ObjectTypeProtectionDomain:
        return Abi::ObjectCreateProtectionDomainRequestSizeBytes;
    case Abi::ObjectTypeFaultChannel:
        return Abi::ObjectCreateFaultChannelRequestSizeBytes;
    case Abi::ObjectTypeWaitSet:
        return Abi::ObjectCreateWaitSetRequestSizeBytes;
    case Abi::ObjectTypeResourceDomain:
        return Abi::ObjectCreateResourceDomainRequestSizeBytes;
    case Abi::ObjectTypeIommuDomain:
        return Abi::ObjectCreateIommuDomainRequestSizeBytes;
    case Abi::ObjectTypeInterrupt:
        return Abi::ObjectCreateInterruptRequestSizeBytes;
    default:
        return 0U;
    }
}

[[nodiscard]] constexpr bool IsDestroyableType(const Foundation::UInt64 type) noexcept
{
    return ObjectCreateRequestSize(type) != 0U;
}

/// The size of a request of type whose caller gave size: a Frame's request is a RAM Frame's or a
/// device Frame's.
[[nodiscard]] constexpr Foundation::Size ObjectCreateRequestSize(const Foundation::UInt64 type,
                                                                 const Foundation::UInt64 size) noexcept
{
    if (type == Abi::ObjectTypeProtectionDomain && size == Abi::ObjectCreateProtectionDomainChannelRequestSizeBytes)
    {
        return static_cast<Foundation::Size>(size);
    }
    if (type == Abi::ObjectTypeResourceDomain && (size == Abi::ObjectCreateResourceDomainExtendedRequestSizeBytes ||
                                                  size == Abi::ObjectCreateResourceDomainArenaRequestSizeBytes))
    {
        return static_cast<Foundation::Size>(size);
    }
    if (type == Abi::ObjectTypeFrame && size == Abi::ObjectCreateRamFrameRequestSizeBytes)
    {
        return static_cast<Foundation::Size>(size);
    }
    return ((type == Abi::ObjectTypeFrame) && (size == Abi::ObjectCreateDeviceFrameRequestSizeBytes))
               ? Abi::ObjectCreateDeviceFrameRequestSizeBytes
               : ObjectCreateRequestSize(type);
}

/// A MemoryObject's cache-policy byte; false for an undefined one.
[[nodiscard]] constexpr bool DecodeCachePolicy(const Foundation::UInt8 value,
                                               Memory::MemoryCachePolicy& policy) noexcept
{
    switch (value)
    {
    case Abi::ObjectCreateCacheWriteBack:
        policy = Memory::MemoryCachePolicy::WriteBack;
        return true;
    case Abi::ObjectCreateCacheUncached:
        policy = Memory::MemoryCachePolicy::Uncached;
        return true;
    case Abi::ObjectCreateCacheWriteCombining:
        policy = Memory::MemoryCachePolicy::WriteCombining;
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool BytesAreZero(const Foundation::UInt8* const bytes, const Foundation::Size first,
                                const Foundation::Size end) noexcept
{
    bool zero{true};
    for (Foundation::Size index{first}; index < end; ++index)
    {
        zero = zero && (bytes[index] == 0U);
    }
    return zero;
}

/// Decodes a validated request of type and size into decoded; false for a
/// nonzero reserved byte or an undefined permission bit or cache policy.
[[nodiscard]] bool DecodeObjectCreateRequest(const Foundation::UInt64 type, const Foundation::UInt8* const bytes,
                                             const Foundation::Size size, ObjectCreateRequest& decoded) noexcept
{
    switch (type)
    {
    case Abi::ObjectTypeCapabilitySpace:
        // Every byte is reserved.
        if (!BytesAreZero(bytes, 0U, Abi::ObjectCreateCapabilitySpaceRequestSizeBytes))
        {
            return false;
        }
        decoded.Type = Object::ObjectType::CapabilitySpace;
        return true;
    case Abi::ObjectTypeAddressSpace:
        if ((bytes[0U] & ~Abi::ObjectCreateAddressSpaceFlagLargeArena) != 0U ||
            !BytesAreZero(bytes, 1U, Abi::ObjectCreateAddressSpaceRequestSizeBytes))
        {
            return false;
        }
        decoded.Type = Object::ObjectType::AddressSpace;
        decoded.LargePageTableArena = (bytes[0U] & Abi::ObjectCreateAddressSpaceFlagLargeArena) != 0U;
        return true;
    case Abi::ObjectTypeFaultChannel:
        if (!BytesAreZero(bytes, 0U, size))
        {
            return false;
        }
        decoded.Type = Object::ObjectType::FaultChannel;
        return true;
    case Abi::ObjectTypeWaitSet:
        // Every byte is reserved.
        if (!BytesAreZero(bytes, 0U, Abi::ObjectCreateWaitSetRequestSizeBytes))
        {
            return false;
        }
        decoded.Type = Object::ObjectType::WaitSet;
        return true;
    case Abi::ObjectTypeResourceDomain:
        // The child's limits; the pool refuses nonzero metadata and handles.
        decoded.Type = Object::ObjectType::ResourceDomain;
        decoded.DomainLimits = Resource::ResourceDomainLimits{
            .CommittedMemoryPages = LoadWord(bytes, Abi::ObjectCreateDomainCommittedPagesOffset),
            .ResidentMemoryPages = LoadWord(bytes, Abi::ObjectCreateDomainResidentPagesOffset),
            .KernelMetadataBytes = LoadWord(bytes, Abi::ObjectCreateDomainMetadataOffset),
            .CapabilityHandles = LoadWord(bytes, Abi::ObjectCreateDomainHandlesOffset),
            .IpcQueueBytes = LoadWord(bytes, Abi::ObjectCreateDomainQueueBytesOffset),
            .Threads = LoadWord(bytes, Abi::ObjectCreateDomainThreadsOffset),
            .ChildDomains = LoadWord(bytes, Abi::ObjectCreateDomainChildrenOffset),
        };
        decoded.DomainLimits.Objects.LargePageTableArenas = 0U;
        if (size != Abi::ObjectCreateResourceDomainRequestSizeBytes)
        {
            decoded.DomainFlags = LoadWord(bytes, Abi::ObjectCreateDomainFlagsOffset);
            if ((decoded.DomainFlags & ~Abi::ObjectCreateDomainFlagCode) != 0U)
            {
                return false;
            }
            decoded.ExplicitObjectLimits = true;
            for (Foundation::Size i{0U}; i < Abi::LimitedObjectCount; ++i)
            {
                decoded.DomainLimits.Objects.Counts[i] =
                    LoadHalf(bytes, Abi::ObjectCreateDomainObjectLimitsOffset + i * 2U);
            }
            if (size == Abi::ObjectCreateResourceDomainArenaRequestSizeBytes)
            {
                if (!BytesAreZero(bytes, Abi::ObjectCreateDomainLargeArenaLimitOffset + 2U, size))
                {
                    return false;
                }
                decoded.DomainLimits.Objects.LargePageTableArenas =
                    LoadHalf(bytes, Abi::ObjectCreateDomainLargeArenaLimitOffset);
            }
        }
        return true;
    case Abi::ObjectTypeIommuDomain:
        // The device address width; the domain checks its range.
        if (!BytesAreZero(bytes, Abi::ObjectCreateIommuAddressBitsOffset + 1U,
                          Abi::ObjectCreateIommuDomainRequestSizeBytes))
        {
            return false;
        }
        decoded.Type = Object::ObjectType::IommuDomain;
        decoded.IommuInputAddressBits = bytes[Abi::ObjectCreateIommuAddressBitsOffset];
        return true;
    case Abi::ObjectTypeInterrupt:
        // The function and its message source; the pool refuses a source but the MSI.
        decoded.Type = Object::ObjectType::Interrupt;
        decoded.InterruptFunction =
            Object::CapabilityHandle{LoadWord(bytes, Abi::ObjectCreateInterruptFunctionOffset),
                                     LoadWord(bytes, Abi::ObjectCreateInterruptFunctionOffset + 8U)};
        decoded.InterruptSource = LoadWord(bytes, Abi::ObjectCreateInterruptSourceOffset);
        return true;
    case Abi::ObjectTypeProtectionDomain:
    {
        // A billing domain other than the authorizing one is reserved, and the tag names the domain
        // to its supervisor.
        const auto componentOffset = size == Abi::ObjectCreateProtectionDomainRequestSizeBytes
                                         ? Abi::ObjectCreateDomainComponentKindOffset
                                         : Abi::ObjectCreateDomainChannelComponentKindOffset;
        if (!BytesAreZero(bytes, Abi::ObjectCreateDomainBillingOffset, Abi::ObjectCreateDomainComponentKindOffset) ||
            (LoadWord(bytes, Abi::ObjectCreateDomainSupervisionTagOffset) == 0U) ||
            LoadWord(bytes, componentOffset) > Abi::ComponentBehaviorMaximum)
        {
            return false;
        }
        decoded.Type = Object::ObjectType::ProtectionDomain;
        decoded.DomainAddressSpace =
            Object::CapabilityHandle{LoadWord(bytes, Abi::ObjectCreateDomainAddressSpaceOffset),
                                     LoadWord(bytes, Abi::ObjectCreateDomainAddressSpaceOffset + 8U)};
        decoded.DomainCapabilities =
            Object::CapabilityHandle{LoadWord(bytes, Abi::ObjectCreateDomainCapabilitySpaceOffset),
                                     LoadWord(bytes, Abi::ObjectCreateDomainCapabilitySpaceOffset + 8U)};
        decoded.SupervisionTag = LoadWord(bytes, Abi::ObjectCreateDomainSupervisionTagOffset);
        if (size == Abi::ObjectCreateProtectionDomainChannelRequestSizeBytes)
        {
            decoded.DomainChannel = LoadHandle(bytes, Abi::ObjectCreateDomainFaultChannelOffset);
            if (!decoded.DomainChannel.IsValid())
            {
                return false;
            }
        }
        static_assert(Abi::ComponentBehaviorMaximum <= Foundation::UInt8{0xFFU});
        decoded.DiagnosticComponentKind = static_cast<Foundation::UInt8>(LoadWord(bytes, componentOffset));
        return true;
    }
    case Abi::ObjectTypeThread:
        // Budgets, periods and the static TLS thread pointer are reserved and must be zero; the
        // domain is the caller's (zero) or one it manages. The stack top is 16-byte aligned, as
        // both ISAs' ABIs require at a call: compiled user code may keep SSE or FP spills there.
        if (!BytesAreZero(bytes, Abi::ObjectCreateThreadBudgetOffset, Abi::ObjectCreateThreadRequestSizeBytes) ||
            ((LoadWord(bytes, Abi::ObjectCreateThreadStackOffset) & 15U) != 0U))
        {
            return false;
        }
        decoded.Type = Object::ObjectType::Thread;
        decoded.Thread = Scheduler::UserThreadStart{
            .Entry = LoadWord(bytes, Abi::ObjectCreateThreadEntryOffset),
            .UserStackTop = LoadWord(bytes, Abi::ObjectCreateThreadStackOffset),
            .Argument = LoadWord(bytes, Abi::ObjectCreateThreadArgumentOffset),
            .Domain = nullptr,
        };
        decoded.ThreadDomain = Object::CapabilityHandle{LoadWord(bytes, Abi::ObjectCreateThreadDomainOffset),
                                                        LoadWord(bytes, Abi::ObjectCreateThreadDomainOffset + 8U)};
        return true;
    case Abi::ObjectTypeEndpoint:
        // The receiver space: zero for the caller's, or a created CapabilitySpace.
        if (!BytesAreZero(bytes, Abi::ObjectCreateEndpointReservedOffset,
                          Abi::ObjectCreateEndpointSealedSelectorOffset))
        {
            return false;
        }
        decoded.Type = Object::ObjectType::Endpoint;
        decoded.Endpoint = DecodeEndpointConfiguration(bytes);
        decoded.EndpointReceiver =
            Object::CapabilityHandle{LoadWord(bytes, Abi::ObjectCreateEndpointReceiverSpaceOffset),
                                     LoadWord(bytes, Abi::ObjectCreateEndpointReceiverSpaceOffset + 8U)};
        decoded.EndpointSealedSelector = LoadWord(bytes, Abi::ObjectCreateEndpointSealedSelectorOffset);
        return true;
    case Abi::ObjectTypeFrame:
    {
        // The new size carries log2 alignment and an address limit; the legacy and device forms
        // stay unchanged.
        if (size == Abi::ObjectCreateRamFrameRequestSizeBytes)
        {
            const Foundation::UInt8 alignmentLog2 = bytes[Abi::ObjectCreateFrameAlignmentOffset];
            const Foundation::UInt8 limit = bytes[Abi::ObjectCreateRamFrameLimitOffset];
            const Foundation::UInt64 pages = LoadWord(bytes, Abi::ObjectCreatePagesOffset);
            if (alignmentLog2 > Abi::ObjectCreateRamFrameMaximumAlignmentLog2 || (limit != 0U && limit != 32U) ||
                !BytesAreZero(bytes, 10U, size) || pages == 0U || pages > Memory::FramePool::MaximumRunPages)
            {
                return false;
            }
            decoded.Type = Object::ObjectType::Frame;
            decoded.Pages = Memory::PageCount{pages};
            decoded.FrameAlignmentPages = static_cast<Foundation::UInt16>(1U << alignmentLog2);
            decoded.FrameAddressLimitBits = limit;
            return true;
        }
        // A device Frame names its region and offset; a zero handle there is refused by the
        // region's lookup. A Frame of RAM may ask for a physical alignment, a power of two of
        // pages.
        const Foundation::UInt8 alignment = bytes[Abi::ObjectCreateFrameAlignmentOffset];
        if (!BytesAreZero(bytes, Abi::ObjectCreateFrameAlignmentOffset + 1U, Abi::ObjectCreateFrameRequestSizeBytes) ||
            (alignment > Abi::ObjectCreateFrameMaximumAlignmentPages) || ((alignment & (alignment - 1U)) != 0U) ||
            ((size == Abi::ObjectCreateDeviceFrameRequestSizeBytes) && (alignment > 1U)))
        {
            return false;
        }
        decoded.Type = Object::ObjectType::Frame;
        decoded.Pages = Memory::PageCount{LoadWord(bytes, Abi::ObjectCreatePagesOffset)};
        if (size == Abi::ObjectCreateFrameRequestSizeBytes &&
            decoded.Pages.GetValue() > Abi::ObjectCreateFrameLegacyMaximumPages)
        {
            return false;
        }
        decoded.FrameAlignmentPages = alignment;
        if (size == Abi::ObjectCreateDeviceFrameRequestSizeBytes)
        {
            decoded.FrameRegion =
                Object::CapabilityHandle{LoadWord(bytes, Abi::ObjectCreateDeviceFrameRegionOffset),
                                         LoadWord(bytes, Abi::ObjectCreateDeviceFrameRegionOffset + 8U)};
            decoded.FrameRegionOffsetPages = LoadWord(bytes, Abi::ObjectCreateDeviceFrameRegionPageOffset);
            return decoded.FrameRegion.IsValid();
        }
        return true;
    }
    default:
        if (!BytesAreZero(bytes, Abi::ObjectCreateCachePolicyOffset + 1U,
                          Abi::ObjectCreateMemoryObjectRequestSizeBytes) ||
            !DecodePagePermissions(bytes[Abi::ObjectCreatePermissionsOffset], decoded.MaximumPermissions) ||
            !DecodeCachePolicy(bytes[Abi::ObjectCreateCachePolicyOffset], decoded.MemoryCachePolicy))
        {
            return false;
        }
        decoded.Type = Object::ObjectType::MemoryObject;
        decoded.Pages = Memory::PageCount{LoadWord(bytes, Abi::ObjectCreatePagesOffset)};
        return true;
    }
}
// The decoded request, too large to share DispatchObjectCreate's frame with
// the copy's bookkeeping at -O0, lives in a frame of its own.
[[nodiscard]] [[gnu::noinline]] DispatchResult CreateDecoded(const SystemCallContext& context,
                                                             const SystemCallRequest& request,
                                                             const Foundation::UInt8* const wire,
                                                             const Foundation::Size size) noexcept
{
    ObjectCreateRequest decoded{};
    decoded.Authority = DecodeHandle(request);
    decoded.Caller = context.ProtectionDomain;
    if (!DecodeObjectCreateRequest(request.Arguments[2U], wire, size, decoded))
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
    }
    SystemCallResponse response{};
    if (!MapObjectManagement(context.ObjectManagement->Create(*context.Capabilities, decoded), response))
    {
        return DispatchResult::Failure(MakeFailure(IpcSystemCallDispatchError::ObjectManagementViolation));
    }
    return DispatchResult::Success(response);
}
} // Namespace

// The type selects the request's layout, so it is checked before user memory is read.
Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
IpcSystemCallDispatcher::DispatchObjectCreate(const SystemCallContext& context,
                                              const SystemCallRequest& request) noexcept
{
    ScratchScope scratchScope{*context.Scratch};
    const Foundation::Size requestSize = ObjectCreateRequestSize(request.Arguments[2U], request.Arguments[4U]);
    if ((requestSize == 0U) || (request.Arguments[4U] != requestSize) || (request.Arguments[5U] != 0U))
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
    }
    if (context.ObjectManagement == nullptr)
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidConfiguration));
    }

    const auto sourceRange = Memory::UserMemoryRange::Create(request.Arguments[3U], requestSize);
    if (!sourceRange.HasValue())
    {
        return DispatchResult::Success(MakeResponse(MapRangeError(sourceRange.GetError())));
    }
    const Ipc::WritableByteView wire = context.Scratch->GetWritableWireBuffer();
    const Memory::UserMemoryAccessError copied =
        context.UserMemory->CopyFromUser(sourceRange.GetValue(), Memory::WritableKernelBuffer{
                                                                     .Data = wire.Data,
                                                                     .SizeBytes = requestSize,
                                                                 });
    if (IsUserMemoryProtocolViolation(copied))
    {
        return DispatchResult::Failure(UserMemoryFailure(copied));
    }
    if (copied != Memory::UserMemoryAccessError::None)
    {
        return DispatchResult::Success(MakeResponse(MapUserMemoryError(copied)));
    }

    return CreateDecoded(context, request, wire.Data, requestSize);
}

// ObjectDestroy carries no request: arguments 3 to 5 are zero.
Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
IpcSystemCallDispatcher::DispatchObjectDestroy(const SystemCallContext& context,
                                               const SystemCallRequest& request) noexcept
{
    ScratchScope scratchScope{*context.Scratch};
    if (!IsDestroyableType(request.Arguments[2U]) || !ArgumentsAreZero(request, 3U))
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
    }
    if (context.ObjectManagement == nullptr)
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidConfiguration));
    }
    SystemCallResponse response{};
    if (!MapObjectManagement(context.ObjectManagement->Destroy(*context.Capabilities, DecodeHandle(request),
                                                               static_cast<Object::ObjectType>(request.Arguments[2U])),
                             response))
    {
        return DispatchResult::Failure(MakeFailure(IpcSystemCallDispatchError::ObjectManagementViolation));
    }
    return DispatchResult::Success(response);
}

// ThreadTerminate names its Thread in arguments 0 and 1; arguments 2 to 5 are zero.
Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
IpcSystemCallDispatcher::DispatchThreadTerminate(const SystemCallContext& context,
                                                 const SystemCallRequest& request) noexcept
{
    ScratchScope scratchScope{*context.Scratch};
    if (!ArgumentsAreZero(request, 2U))
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
    }
    if (context.ObjectManagement == nullptr)
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidConfiguration));
    }
    SystemCallResponse response{};
    if (!MapObjectManagement(context.ObjectManagement->Terminate(*context.Capabilities, DecodeHandle(request)),
                             response))
    {
        return DispatchResult::Failure(MakeFailure(IpcSystemCallDispatchError::ObjectManagementViolation));
    }
    return DispatchResult::Success(response);
}

// DeviceControl names its PCI function in arguments 0 and 1 and its operation in 2; DeviceAssign's
// ProtectionDomain is arguments 3 and 4, DeviceQuiesced's assignment sequence argument 3 with
// argument 4 zero, DeviceReadConfiguration's dword offset argument 3 with argument 4 zero,
// DeviceWriteLegacySupport's offset and 32-bit value arguments 3 and 4, and both are zero for
// every other operation; argument 5 is zero.
Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
IpcSystemCallDispatcher::DispatchDeviceControl(const SystemCallContext& context,
                                               const SystemCallRequest& request) noexcept
{
    ScratchScope scratchScope{*context.Scratch};
    const Foundation::UInt64 operation = request.Arguments[2U];
    const bool sequence = operation == Abi::DeviceQuiesced;
    const bool configuration =
        (operation == Abi::DeviceReadConfiguration) || (operation == Abi::DeviceWriteLegacySupport);
    if ((operation < Abi::DeviceAssign) || (operation > Abi::DeviceReboot) ||
        ((operation > Abi::DeviceWriteLegacySupport) && (operation < Abi::DevicePowerQuery)) ||
        (request.Arguments[5U] != 0U) || (sequence && ((request.Arguments[3U] == 0U) || (request.Arguments[4U] != 0U))) ||
        (configuration && ((request.Arguments[3U] > 0xFCU) || ((request.Arguments[3U] & 3U) != 0U) ||
                           (request.Arguments[4U] > 0xFFFF'FFFFU) ||
                           ((operation == Abi::DeviceReadConfiguration) && (request.Arguments[4U] != 0U)))) ||
        ((operation != Abi::DeviceAssign) && !sequence && !configuration &&
         ((request.Arguments[3U] != 0U) || (request.Arguments[4U] != 0U))))
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
    }
    if (context.ObjectManagement == nullptr)
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidConfiguration));
    }
    SystemCallResponse response{};
    if (!MapObjectManagement(context.ObjectManagement->ControlDevice(
                                 *context.Capabilities, context.ProtectionDomain, DecodeHandle(request), operation,
                                 Object::CapabilityHandle{request.Arguments[3U], request.Arguments[4U]}),
                             response))
    {
        return DispatchResult::Failure(MakeFailure(IpcSystemCallDispatchError::ObjectManagementViolation));
    }
    return DispatchResult::Success(response);
}

namespace
{
// IommuMap's block, read and decoded in a frame of its own; false with outcome set when it cannot
// Be.
[[nodiscard, gnu::noinline]] bool DecodeIommuMap(const SystemCallContext& context, const SystemCallRequest& request,
                                                 IommuControlRequest& decoded, DispatchResult& outcome) noexcept
{
    const auto sourceRange = Memory::UserMemoryRange::Create(request.Arguments[3U], Abi::IommuMapRequestSizeBytes);
    if (!sourceRange.HasValue())
    {
        outcome = DispatchResult::Success(MakeResponse(MapRangeError(sourceRange.GetError())));
        return false;
    }
    const Ipc::WritableByteView wire = context.Scratch->GetWritableWireBuffer();
    const Memory::UserMemoryAccessError copied = context.UserMemory->CopyFromUser(
        sourceRange.GetValue(),
        Memory::WritableKernelBuffer{.Data = wire.Data, .SizeBytes = Abi::IommuMapRequestSizeBytes});
    if (IsUserMemoryProtocolViolation(copied))
    {
        outcome = DispatchResult::Failure(UserMemoryFailure(copied));
        return false;
    }
    if (copied != Memory::UserMemoryAccessError::None)
    {
        outcome = DispatchResult::Success(MakeResponse(MapUserMemoryError(copied)));
        return false;
    }
    const Foundation::UInt8* const bytes = wire.Data;
    const Foundation::UInt8 permissions = bytes[Abi::IommuMapPermissionsOffset];
    if ((permissions == 0U) ||
        ((permissions & static_cast<Foundation::UInt8>(~(Abi::IommuMapDeviceRead | Abi::IommuMapDeviceWrite))) != 0U) ||
        !BytesAreZero(bytes, Abi::IommuMapPermissionsOffset + 1U, Abi::IommuMapRequestSizeBytes))
    {
        outcome = DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
        return false;
    }
    decoded.Frame = Object::CapabilityHandle{LoadWord(bytes, Abi::IommuMapFrameOffset),
                                             LoadWord(bytes, Abi::IommuMapFrameOffset + 8U)};
    decoded.FramePageOffset = LoadWord(bytes, Abi::IommuMapFramePageOffset);
    decoded.Pages = LoadWord(bytes, Abi::IommuMapPagesOffset);
    decoded.DeviceAddress = LoadWord(bytes, Abi::IommuMapDeviceAddressOffset);
    decoded.Permissions = permissions;
    return true;
}

// Which arguments past the operation each IommuControl operation leaves zero.
[[nodiscard]] bool IommuArgumentsAreValid(const SystemCallRequest& request) noexcept
{
    switch (request.Arguments[2U])
    {
    case Abi::IommuAttach:
    case Abi::IommuDetach:
        return request.Arguments[5U] == 0U;
    case Abi::IommuMap:
        return (request.Arguments[4U] == Abi::IommuMapRequestSizeBytes) && (request.Arguments[5U] == 0U);
    case Abi::IommuUnmap:
        return (request.Arguments[4U] == 0U) && (request.Arguments[5U] == 0U);
    case Abi::IommuFaults:
        return ArgumentsAreZero(request, 3U);
    default:
        return false;
    }
}
} // Namespace

// IommuControl names its IommuDomain in arguments 0 and 1 and its operation in 2.
Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
IpcSystemCallDispatcher::DispatchIommuControl(const SystemCallContext& context,
                                              const SystemCallRequest& request) noexcept
{
    ScratchScope scratchScope{*context.Scratch};
    if (!IommuArgumentsAreValid(request))
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
    }
    if (context.ObjectManagement == nullptr)
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidConfiguration));
    }
    const Foundation::UInt64 operation = request.Arguments[2U];
    IommuControlRequest decoded{};
    decoded.Domain = DecodeHandle(request);
    decoded.Operation = operation;
    if ((operation == Abi::IommuAttach) || (operation == Abi::IommuDetach))
    {
        decoded.Function = Object::CapabilityHandle{request.Arguments[3U], request.Arguments[4U]};
    }
    else if (operation == Abi::IommuUnmap)
    {
        decoded.DeviceAddress = request.Arguments[3U];
    }
    DispatchResult outcome = DispatchResult::Success(MakeResponse(SystemCallError::None));
    if ((operation == Abi::IommuMap) && !DecodeIommuMap(context, request, decoded, outcome))
    {
        return outcome;
    }
    SystemCallResponse response{};
    if (!MapObjectManagement(
            context.ObjectManagement->ControlIommu(*context.Capabilities, context.ProtectionDomain, decoded), response))
    {
        return DispatchResult::Failure(MakeFailure(IpcSystemCallDispatchError::ObjectManagementViolation));
    }
    return DispatchResult::Success(response);
}

// InterruptControl names its Interrupt in arguments 0 and 1 and its operation in 2; arguments 3 to
// 5 are zero.
Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
IpcSystemCallDispatcher::DispatchInterruptControl(const SystemCallContext& context,
                                                  const SystemCallRequest& request) noexcept
{
    ScratchScope scratchScope{*context.Scratch};
    const Foundation::UInt64 operation = request.Arguments[2U];
    if (((operation != Abi::InterruptRearm) && (operation != Abi::InterruptKeepMasked)) ||
        !ArgumentsAreZero(request, 3U))
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
    }
    if (context.ObjectManagement == nullptr)
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidConfiguration));
    }
    SystemCallResponse response{};
    if (!MapObjectManagement(
            context.ObjectManagement->ControlInterrupt(*context.Capabilities, DecodeHandle(request), operation),
            response))
    {
        return DispatchResult::Failure(MakeFailure(IpcSystemCallDispatchError::ObjectManagementViolation));
    }
    return DispatchResult::Success(response);
}

// WaitSetBind names its WaitSet in arguments 0 and 1, its Endpoint in 2 and 3 and the nonzero tag
// in 4; argument 5 is zero.
Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
IpcSystemCallDispatcher::DispatchWaitSetBind(const SystemCallContext& context,
                                             const SystemCallRequest& request) noexcept
{
    ScratchScope scratchScope{*context.Scratch};
    if ((request.Arguments[4U] == 0U) || (request.Arguments[5U] != 0U))
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
    }
    if (context.ObjectManagement == nullptr)
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidConfiguration));
    }
    SystemCallResponse response{};
    if (!MapObjectManagement(context.ObjectManagement->BindWaitSet(
                                 *context.Capabilities, DecodeHandle(request),
                                 Object::CapabilityHandle{request.Arguments[2U], request.Arguments[3U]},
                                 request.Arguments[4U]),
                             response))
    {
        return DispatchResult::Failure(MakeFailure(IpcSystemCallDispatchError::ObjectManagementViolation));
    }
    return DispatchResult::Success(response);
}

// MemoryMap: argument 3 names the mapping block and argument 4 sizes it;
// the other arguments are zero.
Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
IpcSystemCallDispatcher::DispatchMemoryMap(const SystemCallContext& context, const SystemCallRequest& request) noexcept
{
    ScratchScope scratchScope{*context.Scratch};
    if ((request.Arguments[0U] != 0U) || (request.Arguments[1U] != 0U) || (request.Arguments[2U] != 0U) ||
        (request.Arguments[4U] != Abi::FaultResolveRequestSizeBytes) || (request.Arguments[5U] != 0U))
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
    }
    if (context.ObjectManagement == nullptr)
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidConfiguration));
    }
    Memory::MemoryMapRequest decoded{};
    DispatchResult outcome = DispatchResult::Success(MakeResponse(SystemCallError::None));
    if (!DecodeMappingBlock(context, request, decoded, outcome))
    {
        return outcome;
    }
    SystemCallResponse response{};
    if (!MapObjectManagement(context.ObjectManagement->Map(*context.Capabilities, decoded), response))
    {
        return DispatchResult::Failure(MakeFailure(IpcSystemCallDispatchError::ObjectManagementViolation));
    }
    return DispatchResult::Success(response);
}

// MemoryUnmap carries no request: arguments 4 and 5 are zero.
Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>
IpcSystemCallDispatcher::DispatchMemoryUnmap(const SystemCallContext& context,
                                             const SystemCallRequest& request) noexcept
{
    ScratchScope scratchScope{*context.Scratch};
    if (!ArgumentsAreZero(request, 4U))
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidArguments));
    }
    if (context.ObjectManagement == nullptr)
    {
        return DispatchResult::Success(MakeResponse(SystemCallError::InvalidConfiguration));
    }
    SystemCallResponse response{};
    if (!MapObjectManagement(context.ObjectManagement->Unmap(*context.Capabilities, DecodeHandle(request),
                                                             Memory::VirtualAddress{request.Arguments[2U]},
                                                             Memory::PageCount{request.Arguments[3U]}),
                             response))
    {
        return DispatchResult::Failure(MakeFailure(IpcSystemCallDispatchError::ObjectManagementViolation));
    }
    return DispatchResult::Success(response);
}
} // Namespace Asteria::Kernel::System
