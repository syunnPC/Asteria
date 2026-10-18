// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Kernel/Diagnostics/DiagnosticChannel.hpp"
#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Ipc/IpcDeliveryCodec.hpp"
#include "Kernel/Memory/IUserMemoryAccess.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Memory/FaultTokenFactory.hpp"
#include "Kernel/Memory/IPagerResolution.hpp"
#include "Kernel/System/IObjectManagement.hpp"
#include "Kernel/System/IpcSystemCallDispatcher.hpp"
#include "Abi/ObjectCreationLimits.hpp"
#include "Abi/ComponentBootstrap.hpp"
#include "Kernel/System/SystemCallScratch.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt64;
using Kernel::Diagnostics::DiagnosticChannel;
using Kernel::Diagnostics::IDiagnosticLineSink;
using Kernel::Ipc::CancellationState;
using Kernel::Ipc::CapabilityAttachment;
using Kernel::Ipc::CapabilityTransferMode;
using Kernel::Ipc::Endpoint;
using Kernel::Ipc::EndpointConfiguration;
using Kernel::Ipc::EndpointErrorCode;
using Kernel::Ipc::EndpointSendClass;
using Kernel::Ipc::InterfaceIdentifier;
using Kernel::Ipc::IpcDeliveryCodec;
using Kernel::Ipc::IpcDeliveryCodecError;
using Kernel::Ipc::MessageCodec;
using Kernel::Ipc::MessageCodecError;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Ipc::MessageKind;
using Kernel::Ipc::ReadOnlyByteView;
using Kernel::Ipc::WritableByteView;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::IUserMemoryAccess;
using Kernel::Memory::ReadOnlyKernelBuffer;
using Kernel::Memory::UserMemoryAccessError;
using Kernel::Memory::UserMemoryRange;
using Kernel::Memory::UserMemoryWriteReservation;
using Kernel::Memory::WritableKernelBuffer;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityOperation;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::IKernelObject;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationDomainId;
using Kernel::Object::RevocationError;
using Kernel::Object::RevocationRegistry;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::System::IpcSystemCallDispatchError;
using Kernel::System::IpcSystemCallDispatchFailure;
using Kernel::System::IpcSystemCallDispatcher;
using Kernel::System::SystemCallContext;
using Kernel::System::SystemCallError;
using Kernel::System::SystemCallNumber;
using Kernel::System::SystemCallRequest;
using Kernel::System::SystemCallResponse;
using Kernel::System::SystemCallScratch;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

using DispatchResult = Foundation::Result<SystemCallResponse, IpcSystemCallDispatchFailure>;

constexpr UInt64 SendAddress{0x0000'0001'0000'0000ULL};
constexpr UInt64 ReceiveAddress{SendAddress + 4'096U};

class TestKernelObject final : public IKernelObject
{
public:
    constexpr TestKernelObject(const ObjectType type, const ResourceDomainId chargeOwner) noexcept
        : m_type{type}, m_chargeOwner{chargeOwner}
    {
    }

    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return m_type;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return m_chargeOwner;
    }

private:
    ObjectType m_type;
    ResourceDomainId m_chargeOwner;
};

/// Records the lines a DiagnosticChannel writes.
class RecordingLineSink final : public IDiagnosticLineSink
{
public:
    void WriteLine(const UInt8* const text, const Size lengthBytes) noexcept override
    {
        if (m_lineCount < MaximumLines)
        {
            Line& line = m_lines[m_lineCount];
            line.SizeBytes = lengthBytes < sizeof(line.Text) ? lengthBytes : sizeof(line.Text);
            for (Size index{0U}; index < line.SizeBytes; ++index)
            {
                line.Text[index] = text[index];
            }
        }
        ++m_lineCount;
    }

    [[nodiscard]] Size GetLineCount() const noexcept
    {
        return m_lineCount;
    }

    [[nodiscard]] bool LineIs(const Size index, const char* const expected) const noexcept
    {
        if (index >= m_lineCount || index >= MaximumLines)
        {
            return false;
        }
        const Line& line = m_lines[index];
        Size length{0U};
        while (expected[length] != '\0')
        {
            if (length >= line.SizeBytes || line.Text[length] != static_cast<UInt8>(expected[length]))
            {
                return false;
            }
            ++length;
        }
        return length == line.SizeBytes;
    }

private:
    static constexpr Size MaximumLines{8U};

    struct Line
    {
        UInt8 Text[64];
        Size SizeBytes;
    };

    Line m_lines[MaximumLines]{};
    Size m_lineCount{0U};
};

class DispatcherFixture final
{
public:
    explicit DispatcherFixture(const EndpointConfiguration configuration = EndpointConfiguration::Default()) noexcept
        : m_objects{m_synchronization}, m_revocations{m_synchronization}, m_issuer{m_objects},
          m_senderSpace{m_objects, m_revocations, m_clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL},
          m_receiverSpace{m_objects, m_revocations, m_clock, 0x1111'2222'3333'4444ULL, 0xAAAA'BBBB'CCCC'DDDDULL},
          m_foreignSpace{m_objects, m_revocations, m_clock, 0x1357'9BDF'2468'ACE0ULL, 0x0ECA'8642'FDB9'7531ULL},
          m_endpoint{m_receiverSpace, ResourceDomainId{90U}, configuration},
          m_attachmentObject{ObjectType::MemoryObject, ResourceDomainId{91U}},
          m_addressSpaceObject{ObjectType::AddressSpace, ResourceDomainId{92U}},
          m_channel{m_channelSink, ResourceDomainId{93U}}
    {
    }

    ~DispatcherFixture() noexcept
    {
        if (m_endpoint.IsReady())
        {
            static_cast<void>(m_endpoint.DiscardUnread());
        }
        for (Size index{0U}; index < m_handleCount; ++index)
        {
            static_cast<void>(m_handles[index].Space->Close(m_handles[index].Handle));
        }
        DestroyObject(m_endpointId);
        DestroyObject(m_attachmentObjectId);
        DestroyObject(m_addressSpaceObjectId);
        DestroyObject(m_channelId);
        for (Size index{0U}; index < 4U; ++index)
        {
            static_cast<void>(m_issuer.CollectDestroyedObject());
        }
    }

    DispatcherFixture(const DispatcherFixture&) = delete;
    DispatcherFixture& operator=(const DispatcherFixture&) = delete;
    DispatcherFixture(DispatcherFixture&&) = delete;
    DispatcherFixture& operator=(DispatcherFixture&&) = delete;

    [[nodiscard]] bool Initialize() noexcept
    {
        const auto endpoint = m_issuer.CreateObject(m_endpoint);
        const auto attachment = m_issuer.CreateObject(m_attachmentObject);
        const auto addressSpace = m_issuer.CreateObject(m_addressSpaceObject);
        const auto channel = m_issuer.CreateObject(m_channel);
        if (!endpoint.HasValue() || !attachment.HasValue() || !addressSpace.HasValue() || !channel.HasValue())
        {
            return false;
        }
        m_endpointId = endpoint.GetValue();
        m_attachmentObjectId = attachment.GetValue();
        m_addressSpaceObjectId = addressSpace.GetValue();
        m_channelId = channel.GetValue();
        return m_endpoint.IsReady() && m_senderSpace.IsReady() && m_receiverSpace.IsReady() && m_foreignSpace.IsReady();
    }

    [[nodiscard]] RevocationDomainId CreateDomain() noexcept
    {
        const auto domain = m_revocations.CreateRoot();
        return domain.HasValue() ? domain.GetValue() : RevocationDomainId{};
    }

    [[nodiscard]] Foundation::Result<CapabilityHandle, CapabilityError>
    MintEndpoint(CapabilitySpace& destination, const CapabilityRights rights, const RevocationDomainId domain,
                 const MonotonicTime deadline = MonotonicTime::Infinite(),
                 const UInt64 sealedObjectSelector = 0U) noexcept
    {
        return Mint(destination, m_endpointId, rights, domain, deadline, sealedObjectSelector);
    }

    [[nodiscard]] Foundation::Result<CapabilityHandle, CapabilityError>
    MintAttachment(CapabilitySpace& destination, const CapabilityRights rights, const RevocationDomainId domain,
                   const MonotonicTime deadline = MonotonicTime::Infinite()) noexcept
    {
        return Mint(destination, m_attachmentObjectId, rights, domain, deadline, 0xA77A'0001U);
    }

    [[nodiscard]] Foundation::Result<CapabilityHandle, CapabilityError>
    MintChannel(CapabilitySpace& destination, const CapabilityRights rights, const RevocationDomainId domain) noexcept
    {
        return Mint(destination, m_channelId, rights, domain, MonotonicTime::Infinite(), 0U);
    }

    [[nodiscard]] RecordingLineSink& GetChannelSink() noexcept
    {
        return m_channelSink;
    }

    void TrackTransferredHandle(CapabilitySpace& space, const CapabilityHandle handle) noexcept
    {
        TrackHandle(space, handle);
    }

    [[nodiscard]] AddressSpaceId GetAddressSpaceId() const noexcept
    {
        return AddressSpaceId{m_objects, m_addressSpaceObjectId};
    }

    [[nodiscard]] ManualClock& GetClock() noexcept
    {
        return m_clock;
    }

    [[nodiscard]] RevocationRegistry& GetRevocations() noexcept
    {
        return m_revocations;
    }

    [[nodiscard]] CapabilitySpace& GetSenderSpace() noexcept
    {
        return m_senderSpace;
    }

    [[nodiscard]] CapabilitySpace& GetReceiverSpace() noexcept
    {
        return m_receiverSpace;
    }

    [[nodiscard]] CapabilitySpace& GetForeignSpace() noexcept
    {
        return m_foreignSpace;
    }

    [[nodiscard]] ObjectRegistry& GetObjects() noexcept
    {
        return m_objects;
    }
    [[nodiscard]] RootCapabilityIssuer& GetIssuer() noexcept
    {
        return m_issuer;
    }
    [[nodiscard]] Endpoint& GetEndpoint() noexcept
    {
        return m_endpoint;
    }

private:
    struct TrackedHandle
    {
        CapabilitySpace* Space;
        CapabilityHandle Handle;
    };

    [[nodiscard]] Foundation::Result<CapabilityHandle, CapabilityError>
    Mint(CapabilitySpace& destination, const ObjectId object, const CapabilityRights rights,
         const RevocationDomainId domain, const MonotonicTime deadline, const UInt64 sealedObjectSelector) noexcept
    {
        const auto result = m_issuer.Mint(destination, object, rights, domain, deadline, sealedObjectSelector);
        if (result.HasValue())
        {
            TrackHandle(destination, result.GetValue());
        }
        return result;
    }

    void TrackHandle(CapabilitySpace& space, const CapabilityHandle handle) noexcept
    {
        if (m_handleCount < MaximumTrackedHandles)
        {
            m_handles[m_handleCount] = TrackedHandle{
                .Space = &space,
                .Handle = handle,
            };
            ++m_handleCount;
        }
    }

    void DestroyObject(const ObjectId object) noexcept
    {
        if (object.IsValid())
        {
            static_cast<void>(m_issuer.DestroyObject(object));
        }
    }

    static constexpr Size MaximumTrackedHandles{64U};

    SpinLock m_synchronization;
    ManualClock m_clock;
    Asteria::TestSupport::TestObjectRegistry m_objects;
    RevocationRegistry m_revocations;
    RootCapabilityIssuer m_issuer;
    CapabilitySpace m_senderSpace;
    CapabilitySpace m_receiverSpace;
    CapabilitySpace m_foreignSpace;
    Endpoint m_endpoint;
    TestKernelObject m_attachmentObject;
    TestKernelObject m_addressSpaceObject;
    RecordingLineSink m_channelSink;
    DiagnosticChannel m_channel;
    ObjectId m_channelId{};
    ObjectId m_endpointId{};
    ObjectId m_attachmentObjectId{};
    ObjectId m_addressSpaceObjectId{};
    TrackedHandle m_handles[MaximumTrackedHandles]{};
    Size m_handleCount{0U};
};

class TestUserMemoryAccess final : public IUserMemoryAccess
{
public:
    explicit TestUserMemoryAccess(const AddressSpaceId addressSpace) noexcept : m_addressSpace{addressSpace}
    {
    }

    [[nodiscard]] AddressSpaceId GetAddressSpaceId() const noexcept override
    {
        return m_addressSpace;
    }

    [[nodiscard]] bool IsReady() const noexcept override
    {
        return m_isReady;
    }

    void Store(const UInt64 address, const UInt8* const bytes, const Size sizeBytes) noexcept
    {
        Size offset{0U};
        if (!Translate(address, sizeBytes, offset))
        {
            return;
        }
        for (Size index{0U}; index < sizeBytes; ++index)
        {
            m_bytes[offset + index] = bytes[index];
        }
    }

    void Fill(const UInt64 address, const Size sizeBytes, const UInt8 value) noexcept
    {
        Size offset{0U};
        if (!Translate(address, sizeBytes, offset))
        {
            return;
        }
        for (Size index{0U}; index < sizeBytes; ++index)
        {
            m_bytes[offset + index] = value;
        }
    }

    [[nodiscard]] const UInt8* GetBytes(const UInt64 address) const noexcept
    {
        Size offset{0U};
        return Translate(address, 1U, offset) ? m_bytes + offset : nullptr;
    }

    [[nodiscard]] bool RangeHasValue(const UInt64 address, const Size sizeBytes, const UInt8 value) const noexcept
    {
        Size offset{0U};
        if (!Translate(address, sizeBytes, offset))
        {
            return false;
        }
        for (Size index{0U}; index < sizeBytes; ++index)
        {
            if (m_bytes[offset + index] != value)
            {
                return false;
            }
        }
        return true;
    }

    void FailNextCopyFrom(const UserMemoryAccessError error) noexcept
    {
        m_nextCopyFromError = error;
    }

    void FailNextPrepare(const UserMemoryAccessError error) noexcept
    {
        m_nextPrepareError = error;
    }

    void FailNextCommit(const UserMemoryAccessError error) noexcept
    {
        m_nextCommitError = error;
    }

    void ObserveCommitAgainst(Endpoint& endpoint) noexcept
    {
        m_observedEndpoint = &endpoint;
        m_observedQueuedMessagesAtCommit = ~Size{0U};
    }

    /// Revokes domain inside the next write preparation, which a receive
    /// performs after reserving the Endpoint head and before consuming it.
    void RevokeDuringNextPrepare(RevocationRegistry& registry, const RevocationDomainId domain) noexcept
    {
        m_revokeRegistry = &registry;
        m_revokeDomain = domain;
    }

    [[nodiscard]] RevocationError GetDuringPrepareRevocation() const noexcept
    {
        return m_revokeResult;
    }

    [[nodiscard]] Size GetCopyFromCount() const noexcept
    {
        return m_copyFromCount;
    }

    [[nodiscard]] Size GetPrepareCount() const noexcept
    {
        return m_prepareCount;
    }

    [[nodiscard]] Size GetCancelCount() const noexcept
    {
        return m_cancelCount;
    }

    [[nodiscard]] Size GetCommitCount() const noexcept
    {
        return m_commitCount;
    }

    [[nodiscard]] Size GetObservedQueuedMessagesAtCommit() const noexcept
    {
        return m_observedQueuedMessagesAtCommit;
    }

protected:
    [[nodiscard]] UserMemoryAccessError CopyFromUserValidated(const UserMemoryRange source,
                                                              const WritableKernelBuffer destination) noexcept override
    {
        ++m_copyFromCount;
        Size offset{0U};
        if (!Translate(source.GetStartAddress(), source.GetSizeBytes(), offset))
        {
            return UserMemoryAccessError::AccessDenied;
        }
        if (m_nextCopyFromError != UserMemoryAccessError::None)
        {
            const UserMemoryAccessError error = m_nextCopyFromError;
            m_nextCopyFromError = UserMemoryAccessError::None;
            return error;
        }
        for (Size index{0U}; index < source.GetSizeBytes(); ++index)
        {
            destination.Data[index] = m_bytes[offset + index];
        }
        return UserMemoryAccessError::None;
    }

    [[nodiscard]] Foundation::Result<UserMemoryWriteReservation, UserMemoryAccessError>
    PrepareCopyToUserValidated(const UserMemoryRange destination) noexcept override
    {
        using ResultType = Foundation::Result<UserMemoryWriteReservation, UserMemoryAccessError>;

        ++m_prepareCount;
        if (m_revokeRegistry != nullptr)
        {
            m_revokeResult = m_revokeRegistry->Revoke(m_revokeDomain);
            m_revokeRegistry = nullptr;
        }
        Size offset{0U};
        if (!Translate(destination.GetStartAddress(), destination.GetSizeBytes(), offset))
        {
            return ResultType::Failure(UserMemoryAccessError::AccessDenied);
        }
        if (m_nextPrepareError != UserMemoryAccessError::None)
        {
            const UserMemoryAccessError error = m_nextPrepareError;
            m_nextPrepareError = UserMemoryAccessError::None;
            return ResultType::Failure(error);
        }
        if (m_hasReservation)
        {
            return ResultType::Failure(UserMemoryAccessError::ReservationExhausted);
        }
        m_hasReservation = true;
        m_reservedRange = destination;
        m_activeGeneration = m_nextGeneration;
        ++m_nextGeneration;
        return ResultType::Success(MakeWriteReservation(destination, m_activeGeneration));
    }

    [[nodiscard]] UserMemoryAccessError
    CancelCopyToUserValidated(const UserMemoryWriteReservation reservation) noexcept override
    {
        if (!IsCurrent(reservation))
        {
            return UserMemoryAccessError::StaleReservation;
        }
        ++m_cancelCount;
        ClearReservation();
        return UserMemoryAccessError::None;
    }

    [[nodiscard]] UserMemoryAccessError CommitCopyToUserValidated(const UserMemoryWriteReservation reservation,
                                                                  const ReadOnlyKernelBuffer source) noexcept override
    {
        if (!IsCurrent(reservation))
        {
            return UserMemoryAccessError::StaleReservation;
        }
        if (m_observedEndpoint != nullptr)
        {
            m_observedQueuedMessagesAtCommit = m_observedEndpoint->GetQueueState().QueuedMessages;
        }
        ++m_commitCount;
        if (m_nextCommitError != UserMemoryAccessError::None)
        {
            const UserMemoryAccessError error = m_nextCommitError;
            m_nextCommitError = UserMemoryAccessError::None;
            ClearReservation();
            return error;
        }

        Size offset{0U};
        if (!Translate(reservation.GetRange().GetStartAddress(), source.SizeBytes, offset))
        {
            ClearReservation();
            return UserMemoryAccessError::BackendProtocolViolation;
        }
        for (Size index{0U}; index < source.SizeBytes; ++index)
        {
            m_bytes[offset + index] = source.Data[index];
        }
        ClearReservation();
        return UserMemoryAccessError::None;
    }

private:
    [[nodiscard]] static bool Translate(const UInt64 address, const Size sizeBytes, Size& offset) noexcept
    {
        if (address < SendAddress)
        {
            return false;
        }
        const UInt64 difference = address - SendAddress;
        if (difference > static_cast<UInt64>(StorageSizeBytes))
        {
            return false;
        }
        offset = static_cast<Size>(difference);
        return (offset <= StorageSizeBytes) && (sizeBytes <= (StorageSizeBytes - offset));
    }

    [[nodiscard]] bool IsCurrent(const UserMemoryWriteReservation reservation) const noexcept
    {
        return m_hasReservation && (reservation.GetGeneration() == m_activeGeneration) &&
               reservation.GetRange().IsEqualTo(m_reservedRange);
    }

    void ClearReservation() noexcept
    {
        m_hasReservation = false;
        m_reservedRange = UserMemoryRange{};
        m_activeGeneration = 0U;
    }

    static constexpr Size StorageSizeBytes{8'192U};

    AddressSpaceId m_addressSpace;
    bool m_isReady{true};
    bool m_hasReservation{false};
    UserMemoryRange m_reservedRange{};
    UInt64 m_nextGeneration{1U};
    UInt64 m_activeGeneration{0U};
    UserMemoryAccessError m_nextCopyFromError{UserMemoryAccessError::None};
    UserMemoryAccessError m_nextPrepareError{UserMemoryAccessError::None};
    UserMemoryAccessError m_nextCommitError{UserMemoryAccessError::None};
    Endpoint* m_observedEndpoint{nullptr};
    RevocationRegistry* m_revokeRegistry{nullptr};
    RevocationDomainId m_revokeDomain{};
    RevocationError m_revokeResult{RevocationError::InvalidDomain};
    Size m_observedQueuedMessagesAtCommit{~Size{0U}};
    Size m_copyFromCount{0U};
    Size m_prepareCount{0U};
    Size m_cancelCount{0U};
    Size m_commitCount{0U};
    UInt8 m_bytes[StorageSizeBytes]{};
};

[[nodiscard]] bool MessageIsClear(const MessageEnvelope& message) noexcept
{
    bool clear = (message.Interface.GetLow() == 0U) && (message.Interface.GetHigh() == 0U) &&
                 (message.TransactionIdentifier == 0U) && (message.Deadline.GetTicks() == 0U) &&
                 (message.Trace.TraceIdentifierHigh == 0U) && (message.Trace.TraceIdentifierLow == 0U) &&
                 (message.Trace.SpanIdentifier == 0U) && (message.MethodIdentifier == 0U) &&
                 (message.InterfaceVersionMajor == 0U) && (message.InterfaceVersionMinor == 0U) &&
                 (message.FixedPayloadSizeBytes == 0U) && (message.PayloadSizeBytes == 0U) &&
                 (message.Kind == MessageKind::Invalid) && (message.Cancellation == CancellationState::None) &&
                 (message.VariablePayloadCount == 0U) && (message.AttachmentCount == 0U) && (message.Reserved == 0U) &&
                 (message.ReservedAlignment == 0U);
    for (Size index{0U}; index < MessageEnvelope::MaximumCapabilityAttachments; ++index)
    {
        const CapabilityAttachment& attachment = message.Attachments[index];
        clear = clear && (attachment.Handle.GetSelector() == 0U) && (attachment.Handle.GetAuthenticator() == 0U) &&
                (attachment.RequestedDeadline.GetTicks() == 0U) && (attachment.RequestedRights.GetRawValue() == 0U) &&
                (attachment.ExpectedType == ObjectType::Invalid) &&
                (attachment.TransferMode == CapabilityTransferMode::Invalid) && (attachment.Reserved == 0U);
    }
    for (Size index{0U}; index < MessageEnvelope::MaximumVariablePayloads; ++index)
    {
        clear = clear && (message.VariablePayloads[index].OffsetBytes == 0U) &&
                (message.VariablePayloads[index].SizeBytes == 0U) && (message.VariablePayloads[index].Reserved == 0U);
    }
    for (const UInt8 byte : message.Payload)
    {
        clear = clear && (byte == 0U);
    }
    return clear;
}

class DispatchHarness final
{
public:
    DispatchHarness(const AddressSpaceId addressSpace, const ResourceDomainId resourceDomain) noexcept
        : m_userMemory{addressSpace}, m_addressSpace{addressSpace}, m_resourceDomain{resourceDomain}
    {
    }

    [[nodiscard]] DispatchResult Dispatch(CapabilitySpace& capabilities, const SystemCallRequest& request) noexcept
    {
        PoisonScratch();
        const SystemCallContext context{
            .Capabilities = &capabilities,
            .UserMemory = &m_userMemory,
            .Scratch = &m_scratch,
            .ResourceDomain = m_resourceDomain,
            .AddressSpace = m_addressSpace,
            .PagerResolution = m_pagerResolution,
            .ObjectManagement = m_objectManagement,
        };
        const DispatchResult result = IpcSystemCallDispatcher::Dispatch(context, request);
        m_allScratchCleared = m_allScratchCleared && ScratchIsClear();
        return result;
    }

    [[nodiscard]] TestUserMemoryAccess& GetUserMemory() noexcept
    {
        return m_userMemory;
    }

    void SetPagerResolution(Asteria::Kernel::Memory::IPagerResolution* const resolution) noexcept
    {
        m_pagerResolution = resolution;
    }

    void SetObjectManagement(Asteria::Kernel::System::IObjectManagement* const management) noexcept
    {
        m_objectManagement = management;
    }

    [[nodiscard]] bool AllScratchWasCleared() const noexcept
    {
        return m_allScratchCleared;
    }

private:
    void PoisonScratch() noexcept
    {
        const WritableByteView wire = m_scratch.GetWritableWireBuffer();
        for (Size index{0U}; index < wire.SizeBytes; ++index)
        {
            wire.Data[index] = 0xA5U;
        }
        MessageEnvelope& message = m_scratch.GetMessage();
        message.TransactionIdentifier = 0xFFFFU;
        message.Payload[0U] = 0x5AU;
    }

    [[nodiscard]] bool ScratchIsClear() noexcept
    {
        const ReadOnlyByteView wire = m_scratch.GetWireBuffer(IpcDeliveryCodec::MaximumWireSizeBytes);
        bool clear{true};
        for (Size index{0U}; index < wire.SizeBytes; ++index)
        {
            clear = clear && (wire.Data[index] == 0U);
        }
        return clear && MessageIsClear(m_scratch.GetMessage());
    }

    TestUserMemoryAccess m_userMemory;
    Asteria::Kernel::Memory::IPagerResolution* m_pagerResolution{nullptr};
    Asteria::Kernel::System::IObjectManagement* m_objectManagement{nullptr};
    SystemCallScratch m_scratch;
    AddressSpaceId m_addressSpace;
    ResourceDomainId m_resourceDomain;
    bool m_allScratchCleared{true};
};

[[nodiscard]] constexpr CapabilityRights TrustedEndpointRights() noexcept
{
    return CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Read).With(CapabilityRight::Inspect);
}

[[nodiscard]] MessageEnvelope MakeMessage(const UInt64 transaction, const Foundation::UInt16 payloadSize = 0U) noexcept
{
    MessageEnvelope message{};
    message.Interface = InterfaceIdentifier{0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL};
    message.TransactionIdentifier = transaction;
    message.Deadline = MonotonicTime::Infinite();
    message.MethodIdentifier = 7U;
    message.InterfaceVersionMajor = 1U;
    message.FixedPayloadSizeBytes = payloadSize;
    message.PayloadSizeBytes = payloadSize;
    message.Kind = MessageKind::Asynchronous;
    message.Cancellation = CancellationState::None;
    for (Foundation::UInt16 index{0U}; index < payloadSize; ++index)
    {
        message.Payload[index] = static_cast<UInt8>((transaction + index) & 0xFFU);
    }
    return message;
}

void SetAttachment(MessageEnvelope& message, const CapabilityHandle handle) noexcept
{
    message.Attachments[0U] = CapabilityAttachment{
        .Handle = handle,
        .RequestedDeadline = MonotonicTime::Infinite(),
        .RequestedRights = CapabilityRights{CapabilityRight::Inspect},
        .ExpectedType = ObjectType::MemoryObject,
        .TransferMode = CapabilityTransferMode::Copy,
        .Reserved = 0U,
    };
    message.AttachmentCount = 1U;
}

[[nodiscard]] Size StoreMessage(TestUserMemoryAccess& userMemory, const MessageEnvelope& message) noexcept
{
    UInt8 frame[MessageCodec::MaximumWireSizeBytes]{};
    const auto encoded = MessageCodec::Encode(message, WritableByteView{.Data = frame, .SizeBytes = sizeof(frame)});
    if (!encoded.HasValue())
    {
        return 0U;
    }
    userMemory.Store(SendAddress, frame, encoded.GetValue());
    return encoded.GetValue();
}

[[nodiscard]] constexpr SystemCallRequest MakeIpcRequest(const SystemCallNumber number, const CapabilityHandle handle,
                                                         const UInt64 address, const Size sizeBytes) noexcept
{
    return SystemCallRequest{
        .Number = static_cast<UInt64>(number),
        .Arguments =
            {
                handle.GetSelector(),
                handle.GetAuthenticator(),
                address,
                static_cast<UInt64>(sizeBytes),
                0U,
                0U,
            },
    };
}

[[nodiscard]] constexpr SystemCallRequest MakeSendRequest(const CapabilityHandle handle, const Size frameSize) noexcept
{
    return MakeIpcRequest(SystemCallNumber::IpcSend, handle, SendAddress, frameSize);
}

[[nodiscard]] constexpr SystemCallRequest
MakeReceiveRequest(const CapabilityHandle handle, const Size capacity = IpcDeliveryCodec::MaximumWireSizeBytes) noexcept
{
    return MakeIpcRequest(SystemCallNumber::IpcReceive, handle, ReceiveAddress, capacity);
}

[[nodiscard]] constexpr bool ResponseIs(const DispatchResult& result, const SystemCallError error,
                                        const UInt64 value0 = 0U, const UInt64 value1 = 0U) noexcept
{
    return result.HasValue() && (result.GetValue().Error == error) && (result.GetValue().Value0 == value0) &&
           (result.GetValue().Value1 == value1);
}

[[nodiscard]] constexpr bool HandlesEqual(const CapabilityHandle first, const CapabilityHandle second) noexcept
{
    return (first.GetSelector() == second.GetSelector()) && (first.GetAuthenticator() == second.GetAuthenticator());
}

[[nodiscard]] UInt8* FindQueuedFrameMagic(Endpoint& endpoint) noexcept
{
    UInt8* const bytes = reinterpret_cast<UInt8*>(&endpoint);
    for (Size index{0U}; (index + sizeof(Foundation::UInt32)) <= sizeof(endpoint); ++index)
    {
        if ((bytes[index] == static_cast<UInt8>(MessageCodec::WireMagic)) &&
            (bytes[index + 1U] == static_cast<UInt8>(MessageCodec::WireMagic >> 8U)) &&
            (bytes[index + 2U] == static_cast<UInt8>(MessageCodec::WireMagic >> 16U)) &&
            (bytes[index + 3U] == static_cast<UInt8>(MessageCodec::WireMagic >> 24U)))
        {
            return bytes + index;
        }
    }
    return nullptr;
}

void TestCanonicalRoundTripAndAttachment(TestContext& runner) noexcept
{
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "canonical IPC fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x1234U}};
    const RevocationDomainId endpointDomain = fixture.CreateDomain();
    const RevocationDomainId attachmentDomain = fixture.CreateDomain();
    const UInt64 sealedSelector{0xCAFE'BEEF'0123'4567ULL};
    const auto senderHandle = fixture.MintEndpoint(fixture.GetSenderSpace(), TrustedEndpointRights(), endpointDomain,
                                                   MonotonicTime::Infinite(), sealedSelector);
    const auto receiverHandle =
        fixture.MintEndpoint(fixture.GetReceiverSpace(), CapabilityRights{CapabilityRight::Read}, endpointDomain);
    const auto attachmentHandle = fixture.MintAttachment(
        fixture.GetSenderSpace(), CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Duplicate),
        attachmentDomain);
    ASTERIA_EXPECT(runner, "canonical endpoint and attachment capabilities mint", senderHandle.HasValue(),
                   receiverHandle.HasValue(), attachmentHandle.HasValue());
    if (!senderHandle.HasValue() || !receiverHandle.HasValue() || !attachmentHandle.HasValue())
    {
        return;
    }

    MessageEnvelope message = MakeMessage(0x1111U, 6U);
    SetAttachment(message, attachmentHandle.GetValue());
    const Size frameSize = StoreMessage(harness.GetUserMemory(), message);
    ASTERIA_EXPECT(runner, "canonical attachment frame encodes", frameSize != 0U);
    const DispatchResult sent =
        harness.Dispatch(fixture.GetSenderSpace(), MakeSendRequest(senderHandle.GetValue(), frameSize));
    ASTERIA_EXPECT(runner, "canonical syscall send publishes one sequence", ResponseIs(sent, SystemCallError::None, 1U),
                   fixture.GetEndpoint().GetQueueState().QueuedMessages == 1U);

    harness.GetUserMemory().Fill(ReceiveAddress, IpcDeliveryCodec::MaximumWireSizeBytes, 0xCCU);
    harness.GetUserMemory().ObserveCommitAgainst(fixture.GetEndpoint());
    const DispatchResult received =
        harness.Dispatch(fixture.GetReceiverSpace(), MakeReceiveRequest(receiverHandle.GetValue()));
    ASTERIA_EXPECT(runner, "canonical syscall receive returns sequence and exact delivery size", received.HasValue(),
                   received.GetValue().Error == SystemCallError::None, received.GetValue().Value0 == 1U,
                   received.GetValue().Value1 <= IpcDeliveryCodec::MaximumWireSizeBytes);
    ASTERIA_EXPECT(runner, "copyout commits once and only after Endpoint consumption",
                   harness.GetUserMemory().GetCommitCount() == 1U,
                   harness.GetUserMemory().GetObservedQueuedMessagesAtCommit() == 0U,
                   fixture.GetEndpoint().GetQueueState().QueuedMessages == 0U);

    Kernel::Ipc::EndpointReceiveMetadata metadata{};
    MessageEnvelope decoded{};
    IpcDeliveryCodecError decodeError{IpcDeliveryCodecError::HeaderTooSmall};
    if (received.HasValue())
    {
        decodeError = IpcDeliveryCodec::Decode(
                          ReadOnlyByteView{
                              .Data = harness.GetUserMemory().GetBytes(ReceiveAddress),
                              .SizeBytes = static_cast<Size>(received.GetValue().Value1),
                          },
                          metadata, decoded)
                          .Error;
    }
    ASTERIA_EXPECT(runner, "delivery preserves exact trusted invoking-capability metadata",
                   decodeError == IpcDeliveryCodecError::None, metadata.Sequence == 1U,
                   metadata.Sender.GetValue() == 0x1234U,
                   metadata.SenderEndpointRights.IsEqualTo(TrustedEndpointRights()),
                   metadata.SenderEndpointSealedObjectSelector == sealedSelector, metadata.AttachmentCount == 1U,
                   metadata.SendClass == EndpointSendClass::Normal);
    ASTERIA_EXPECT(runner, "delivery contains the canonical message and transferred handle",
                   decoded.TransactionIdentifier == message.TransactionIdentifier,
                   decoded.PayloadSizeBytes == message.PayloadSizeBytes, decoded.Payload[0U] == message.Payload[0U],
                   decoded.AttachmentCount == 1U,
                   !HandlesEqual(decoded.Attachments[0U].Handle, attachmentHandle.GetValue()));

    auto destinationLookup = fixture.GetReceiverSpace().Lookup(decoded.Attachments[0U].Handle, ObjectType::MemoryObject,
                                                               CapabilityOperation::Inspect);
    auto sourceLookup = fixture.GetSenderSpace().Lookup(attachmentHandle.GetValue(), ObjectType::MemoryObject,
                                                        CapabilityOperation::Inspect);
    ASTERIA_EXPECT(
        runner, "copy attachment remains usable at source and is attenuated at receiver", destinationLookup.HasValue(),
        sourceLookup.HasValue(),
        destinationLookup.GetValue().GetDescriptor().Rights.IsEqualTo(CapabilityRights{CapabilityRight::Inspect}));
    fixture.TrackTransferredHandle(fixture.GetReceiverSpace(), decoded.Attachments[0U].Handle);
    ASTERIA_EXPECT(runner, "successful send and receive clear all processor-local scratch",
                   harness.GetUserMemory().GetCopyFromCount() == 1U, harness.AllScratchWasCleared());
}

void TestHandleAuthorizationFailures(TestContext& runner) noexcept
{
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "handle-failure IPC fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x2222U}};
    const MessageEnvelope message = MakeMessage(0x2222U, 1U);
    const Size frameSize = StoreMessage(harness.GetUserMemory(), message);
    const RevocationDomainId ordinaryDomain = fixture.CreateDomain();
    const RevocationDomainId revokedDomain = fixture.CreateDomain();
    const RevocationDomainId expiredDomain = fixture.CreateDomain();
    const RevocationDomainId wrongTypeDomain = fixture.CreateDomain();
    const auto valid =
        fixture.MintEndpoint(fixture.GetSenderSpace(), CapabilityRights{CapabilityRight::Invoke}, ordinaryDomain);
    const auto denied =
        fixture.MintEndpoint(fixture.GetSenderSpace(), CapabilityRights{CapabilityRight::Read}, ordinaryDomain);
    const auto revoked =
        fixture.MintEndpoint(fixture.GetSenderSpace(), CapabilityRights{CapabilityRight::Invoke}, revokedDomain);
    const auto expired = fixture.MintEndpoint(fixture.GetSenderSpace(), CapabilityRights{CapabilityRight::Invoke},
                                              expiredDomain, MonotonicTime{20U});
    const auto wrongType =
        fixture.MintAttachment(fixture.GetSenderSpace(), CapabilityRights{CapabilityRight::Invoke}, wrongTypeDomain);
    ASTERIA_EXPECT(runner, "authorization-failure capabilities and message initialize", frameSize != 0U,
                   valid.HasValue(), denied.HasValue(), revoked.HasValue(), expired.HasValue(), wrongType.HasValue());
    if (!valid.HasValue() || !denied.HasValue() || !revoked.HasValue() || !expired.HasValue() || !wrongType.HasValue())
    {
        return;
    }

    CapabilityHandle forged{valid.GetValue().GetSelector(), valid.GetValue().GetAuthenticator() ^ 1U};
    ASTERIA_EXPECT(runner, "forged endpoint handle returns an ordinary InvalidHandle response",
                   ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), MakeSendRequest(forged, frameSize)),
                              SystemCallError::InvalidHandle));
    ASTERIA_EXPECT(
        runner, "wrong object type returns an ordinary TypeMismatch response",
        ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), MakeSendRequest(wrongType.GetValue(), frameSize)),
                   SystemCallError::TypeMismatch));
    ASTERIA_EXPECT(runner, "invoke-right denial returns an ordinary RightsDenied response",
                   ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), MakeSendRequest(denied.GetValue(), frameSize)),
                              SystemCallError::RightsDenied));
    ASTERIA_EXPECT(runner, "endpoint capability revocation commits",
                   fixture.GetRevocations().Revoke(revokedDomain) == RevocationError::Ok);
    ASTERIA_EXPECT(
        runner, "revoked endpoint handle returns an ordinary Revoked response",
        ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), MakeSendRequest(revoked.GetValue(), frameSize)),
                   SystemCallError::Revoked));
    fixture.GetClock().Set(20U);
    ASTERIA_EXPECT(
        runner, "expired endpoint handle returns an ordinary Expired response",
        ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), MakeSendRequest(expired.GetValue(), frameSize)),
                   SystemCallError::Expired));
    ASTERIA_EXPECT(runner, "all authorization responses preserve the queue and clear scratch",
                   fixture.GetEndpoint().GetQueueState().QueuedMessages == 0U, harness.AllScratchWasCleared());
}

// SystemCallAbi.md: IPC send and receive are ordered where they take effect,
// so the Endpoint authority must still hold at publication and at
// consumption, not only at the syscall's lookup.
void TestAuthorityHoldsWhereIpcTakesEffect(TestContext& runner) noexcept
{
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "effect-authority IPC fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x8888U}};
    const RevocationDomainId sendDomain = fixture.CreateDomain();
    const RevocationDomainId receiveDomain = fixture.CreateDomain();
    fixture.GetClock().Set(100U);
    const auto expiring = fixture.MintEndpoint(fixture.GetSenderSpace(), CapabilityRights{CapabilityRight::Invoke},
                                               sendDomain, MonotonicTime{101U});
    const auto sender =
        fixture.MintEndpoint(fixture.GetSenderSpace(), CapabilityRights{CapabilityRight::Invoke}, sendDomain);
    const auto receiver =
        fixture.MintEndpoint(fixture.GetReceiverSpace(), CapabilityRights{CapabilityRight::Read}, receiveDomain);
    const Size frameSize = StoreMessage(harness.GetUserMemory(), MakeMessage(0x8888U, 2U));
    ASTERIA_EXPECT(runner, "effect-authority capabilities and message initialize", expiring.HasValue(),
                   sender.HasValue(), receiver.HasValue(), frameSize != 0U);
    if (!expiring.HasValue() || !sender.HasValue() || !receiver.HasValue())
    {
        return;
    }

    // Every clock read advances one tick: the Endpoint lookup observes tick
    // 100 and the publication tick 101, when the invoking lease has expired.
    fixture.GetClock().SetAdvanceOnRead(true);
    const DispatchResult expiredSend =
        harness.Dispatch(fixture.GetSenderSpace(), MakeSendRequest(expiring.GetValue(), frameSize));
    fixture.GetClock().SetAdvanceOnRead(false);
    ASTERIA_EXPECT(runner, "a send whose Endpoint lease expires before publication publishes nothing",
                   ResponseIs(expiredSend, SystemCallError::Expired),
                   fixture.GetEndpoint().GetQueueState().QueuedMessages == 0U);

    const DispatchResult sent =
        harness.Dispatch(fixture.GetSenderSpace(), MakeSendRequest(sender.GetValue(), frameSize));
    harness.GetUserMemory().Fill(ReceiveAddress, IpcDeliveryCodec::MaximumWireSizeBytes, 0xCCU);
    harness.GetUserMemory().RevokeDuringNextPrepare(fixture.GetRevocations(), receiveDomain);
    const DispatchResult revokedReceive =
        harness.Dispatch(fixture.GetReceiverSpace(), MakeReceiveRequest(receiver.GetValue()));
    ASTERIA_EXPECT(runner,
                   "a receive whose Endpoint authority is revoked before consumption consumes and writes nothing",
                   sent.HasValue(), sent.GetValue().Error == SystemCallError::None,
                   harness.GetUserMemory().GetDuringPrepareRevocation() == RevocationError::Ok,
                   ResponseIs(revokedReceive, SystemCallError::Revoked),
                   fixture.GetEndpoint().GetQueueState().QueuedMessages == 1U,
                   harness.GetUserMemory().GetCommitCount() == 0U, harness.GetUserMemory().GetCancelCount() == 1U,
                   harness.GetUserMemory().RangeHasValue(ReceiveAddress, IpcDeliveryCodec::MaximumWireSizeBytes, 0xCCU),
                   harness.AllScratchWasCleared());
}

void TestMalformedInputAndProtocolFailure(TestContext& runner) noexcept
{
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "malformed-input IPC fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x3333U}};
    const RevocationDomainId domain = fixture.CreateDomain();
    const auto senderHandle =
        fixture.MintEndpoint(fixture.GetSenderSpace(), CapabilityRights{CapabilityRight::Invoke}, domain);
    ASTERIA_EXPECT(runner, "malformed-input endpoint capability mints", senderHandle.HasValue());
    if (!senderHandle.HasValue())
    {
        return;
    }

    const MessageEnvelope message = MakeMessage(0x3333U, 2U);
    const Size frameSize = StoreMessage(harness.GetUserMemory(), message);
    SystemCallRequest unusedArgument = MakeSendRequest(senderHandle.GetValue(), frameSize);
    unusedArgument.Arguments[5U] = 1U;
    ASTERIA_EXPECT(
        runner, "nonzero unused send argument is rejected before copy-in",
        ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), unusedArgument), SystemCallError::InvalidArguments));

    SystemCallRequest nullAddress = MakeSendRequest(senderHandle.GetValue(), frameSize);
    nullAddress.Arguments[2U] = 0U;
    ASTERIA_EXPECT(
        runner, "null nonempty user frame range is rejected",
        ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), nullAddress), SystemCallError::InvalidArguments));
    ASTERIA_EXPECT(
        runner, "undersized MessageCodec frame argument is rejected",
        ResponseIs(harness.Dispatch(fixture.GetSenderSpace(),
                                    MakeSendRequest(senderHandle.GetValue(), MessageCodec::WireHeaderSizeBytes - 1U)),
                   SystemCallError::InvalidArguments));

    UInt8 malformed[MessageCodec::MaximumWireSizeBytes]{};
    const auto encoded =
        MessageCodec::Encode(message, WritableByteView{.Data = malformed, .SizeBytes = sizeof(malformed)});
    if (encoded.HasValue())
    {
        malformed[0U] ^= 1U;
        harness.GetUserMemory().Store(SendAddress, malformed, encoded.GetValue());
    }
    ASTERIA_EXPECT(runner, "malformed copied frame returns an ordinary InvalidArguments response", encoded.HasValue(),
                   ResponseIs(harness.Dispatch(fixture.GetSenderSpace(),
                                               MakeSendRequest(senderHandle.GetValue(), encoded.GetValue())),
                              SystemCallError::InvalidArguments));

    static_cast<void>(StoreMessage(harness.GetUserMemory(), message));
    harness.GetUserMemory().FailNextCopyFrom(UserMemoryAccessError::Fault);
    ASTERIA_EXPECT(
        runner, "copy-in fault returns an ordinary UserMemoryFault response",
        ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), MakeSendRequest(senderHandle.GetValue(), frameSize)),
                   SystemCallError::UserMemoryFault));

    harness.GetUserMemory().FailNextCopyFrom(UserMemoryAccessError::BackendProtocolViolation);
    const DispatchResult invariant =
        harness.Dispatch(fixture.GetSenderSpace(), MakeSendRequest(senderHandle.GetValue(), frameSize));
    ASTERIA_EXPECT(runner, "backend protocol fault is an invariant failure, not a syscall response",
                   !invariant.HasValue(),
                   invariant.GetError().Error == IpcSystemCallDispatchError::UserMemoryProtocolViolation,
                   invariant.GetError().Endpoint == EndpointErrorCode::None,
                   invariant.GetError().UserMemory == UserMemoryAccessError::BackendProtocolViolation,
                   invariant.GetError().Reserved == 0U);
    ASTERIA_EXPECT(runner, "malformed, ordinary-fault, and invariant exits all clear scratch",
                   fixture.GetEndpoint().GetQueueState().QueuedMessages == 0U, harness.AllScratchWasCleared());
}

void TestQueuePressureAndReservedControlBoundary(TestContext& runner) noexcept
{
    {
        DispatcherFixture fixture;
        ASTERIA_EXPECT(runner, "queue-pressure IPC fixture initializes", fixture.Initialize());
        DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x4444U}};
        const RevocationDomainId domain = fixture.CreateDomain();
        const auto handle =
            fixture.MintEndpoint(fixture.GetSenderSpace(), CapabilityRights{CapabilityRight::Invoke}, domain);
        const Size frameSize = StoreMessage(harness.GetUserMemory(), MakeMessage(0x4444U));
        bool firstFourSucceeded = handle.HasValue() && (frameSize != 0U);
        if (handle.HasValue())
        {
            for (UInt64 sequence{1U}; sequence <= 4U; ++sequence)
            {
                firstFourSucceeded =
                    firstFourSucceeded && ResponseIs(harness.Dispatch(fixture.GetSenderSpace(),
                                                                      MakeSendRequest(handle.GetValue(), frameSize)),
                                                     SystemCallError::None, sequence);
            }
        }
        ASTERIA_EXPECT(runner, "normal sends fill the exact per-sender message quota", firstFourSucceeded,
                       fixture.GetEndpoint().GetQueueState().QueuedMessages == 4U);
        ASTERIA_EXPECT(
            runner, "queue pressure returns WouldBlock without publishing a fifth send", handle.HasValue(),
            ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), MakeSendRequest(handle.GetValue(), frameSize)),
                       SystemCallError::WouldBlock),
            fixture.GetEndpoint().GetQueueState().QueuedMessages == 4U);
        ASTERIA_EXPECT(runner, "queue-pressure success and WouldBlock exits clear scratch",
                       harness.AllScratchWasCleared());
    }

    EndpointConfiguration reservedConfiguration{
        .MaximumQueuedBytes = 176U,
        .ReservedControlBytes = 88U,
        .MaximumBytesPerSender = 88U,
        .MaximumQueuedMessages = 2U,
        .ReservedControlMessages = 1U,
        .MaximumMessagesPerSender = 1U,
        .MaximumQueuedAttachments = 0U,
        .ReservedControlAttachments = 0U,
        .MaximumAttachmentsPerSender = 0U,
        .Reserved = 0U,
    };
    DispatcherFixture fixture{reservedConfiguration};
    ASTERIA_EXPECT(runner, "reserved-control IPC fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x4545U}};
    const RevocationDomainId domain = fixture.CreateDomain();
    const auto handle =
        fixture.MintEndpoint(fixture.GetSenderSpace(), CapabilityRights{CapabilityRight::Invoke}, domain);
    const MessageEnvelope message = MakeMessage(0x4545U);
    const Size frameSize = StoreMessage(harness.GetUserMemory(), message);
    ASTERIA_EXPECT(runner, "syscall uses the one normal queue slot", handle.HasValue(),
                   ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), MakeSendRequest(handle.GetValue(), frameSize)),
                              SystemCallError::None, 1U));
    if (!handle.HasValue())
    {
        return;
    }
    SystemCallRequest requestedControl = MakeSendRequest(handle.GetValue(), frameSize);
    requestedControl.Arguments[4U] = static_cast<UInt64>(EndpointSendClass::ReservedControl);
    ASTERIA_EXPECT(
        runner, "user arguments cannot select ReservedControl send class",
        ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), requestedControl), SystemCallError::InvalidArguments));
    ASTERIA_EXPECT(runner, "canonical syscall cannot consume the reserved-control slot",
                   ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), MakeSendRequest(handle.GetValue(), frameSize)),
                              SystemCallError::WouldBlock));
    const auto kernelControl = fixture.GetEndpoint().Send(fixture.GetSenderSpace(), ResourceDomainId{0x4545U},
                                                          EndpointSendClass::ReservedControl, message);
    ASTERIA_EXPECT(runner, "trusted kernel path can still use the reserved-control slot", kernelControl.HasValue(),
                   kernelControl.GetValue().Sequence == 2U, fixture.GetEndpoint().GetQueueState().QueuedMessages == 2U);
    ASTERIA_EXPECT(runner, "reserved-control boundary responses clear scratch", harness.AllScratchWasCleared());
}

void TestReceiveRetryAndForeignSpace(TestContext& runner) noexcept
{
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "receive-retry IPC fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x5555U}};
    const RevocationDomainId domain = fixture.CreateDomain();
    const auto senderHandle =
        fixture.MintEndpoint(fixture.GetSenderSpace(), CapabilityRights{CapabilityRight::Invoke}, domain);
    const auto receiverHandle =
        fixture.MintEndpoint(fixture.GetReceiverSpace(), CapabilityRights{CapabilityRight::Read}, domain);
    const auto foreignHandle =
        fixture.MintEndpoint(fixture.GetForeignSpace(), CapabilityRights{CapabilityRight::Read}, domain);
    const MessageEnvelope message = MakeMessage(0x5555U, 64U);
    const Size frameSize = StoreMessage(harness.GetUserMemory(), message);
    ASTERIA_EXPECT(
        runner, "receive-retry message is queued through the syscall", senderHandle.HasValue(),
        receiverHandle.HasValue(), foreignHandle.HasValue(),
        ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), MakeSendRequest(senderHandle.GetValue(), frameSize)),
                   SystemCallError::None, 1U));
    if (!receiverHandle.HasValue() || !foreignHandle.HasValue())
    {
        return;
    }

    harness.GetUserMemory().Fill(ReceiveAddress, IpcDeliveryCodec::MaximumWireSizeBytes, 0xCCU);
    ASTERIA_EXPECT(runner, "read-capable foreign receiver space is rejected without consumption",
                   ResponseIs(harness.Dispatch(fixture.GetForeignSpace(), MakeReceiveRequest(foreignHandle.GetValue())),
                              SystemCallError::RightsDenied),
                   fixture.GetEndpoint().GetQueueState().QueuedMessages == 1U);

    constexpr Size minimumDeliverySize{IpcDeliveryCodec::WireHeaderSizeBytes + MessageCodec::WireHeaderSizeBytes};
    ASTERIA_EXPECT(
        runner, "small receive buffer cancels reservation without consuming or writing",
        ResponseIs(harness.Dispatch(fixture.GetReceiverSpace(),
                                    MakeReceiveRequest(receiverHandle.GetValue(), minimumDeliverySize)),
                   SystemCallError::SpaceExhausted),
        fixture.GetEndpoint().GetQueueState().QueuedMessages == 1U,
        harness.GetUserMemory().RangeHasValue(ReceiveAddress, IpcDeliveryCodec::MaximumWireSizeBytes, 0xCCU));

    harness.GetUserMemory().FailNextPrepare(UserMemoryAccessError::Fault);
    ASTERIA_EXPECT(
        runner, "prepare-write fault leaves the exact delivery queued and user bytes unchanged",
        ResponseIs(harness.Dispatch(fixture.GetReceiverSpace(), MakeReceiveRequest(receiverHandle.GetValue())),
                   SystemCallError::UserMemoryFault),
        fixture.GetEndpoint().GetQueueState().QueuedMessages == 1U,
        harness.GetUserMemory().RangeHasValue(ReceiveAddress, IpcDeliveryCodec::MaximumWireSizeBytes, 0xCCU));

    harness.GetUserMemory().ObserveCommitAgainst(fixture.GetEndpoint());
    const DispatchResult retry =
        harness.Dispatch(fixture.GetReceiverSpace(), MakeReceiveRequest(receiverHandle.GetValue()));
    ASTERIA_EXPECT(runner, "exact retry consumes once and performs one guaranteed copyout commit", retry.HasValue(),
                   retry.GetValue().Error == SystemCallError::None, retry.GetValue().Value0 == 1U,
                   harness.GetUserMemory().GetPrepareCount() == 2U, harness.GetUserMemory().GetCommitCount() == 1U,
                   harness.GetUserMemory().GetObservedQueuedMessagesAtCommit() == 0U,
                   fixture.GetEndpoint().GetQueueState().QueuedMessages == 0U);
    ASTERIA_EXPECT(runner, "foreign, capacity, prepare-fault, and retry exits clear scratch",
                   harness.AllScratchWasCleared());
}

void TestInvariantFailuresAfterQueuePreparation(TestContext& runner) noexcept
{
    {
        DispatcherFixture fixture;
        ASTERIA_EXPECT(runner, "endpoint-invariant fixture initializes", fixture.Initialize());
        DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x6666U}};
        const RevocationDomainId domain = fixture.CreateDomain();
        const auto senderHandle =
            fixture.MintEndpoint(fixture.GetSenderSpace(), CapabilityRights{CapabilityRight::Invoke}, domain);
        const auto receiverHandle =
            fixture.MintEndpoint(fixture.GetReceiverSpace(), CapabilityRights{CapabilityRight::Read}, domain);
        const Size frameSize = StoreMessage(harness.GetUserMemory(), MakeMessage(0x6666U, 1U));
        if (senderHandle.HasValue())
        {
            static_cast<void>(
                harness.Dispatch(fixture.GetSenderSpace(), MakeSendRequest(senderHandle.GetValue(), frameSize)));
        }
        UInt8* const magic = FindQueuedFrameMagic(fixture.GetEndpoint());
        if (magic != nullptr)
        {
            magic[0U] ^= 1U;
        }
        DispatchResult corrupted = DispatchResult::Failure(IpcSystemCallDispatchFailure{});
        if (receiverHandle.HasValue())
        {
            corrupted = harness.Dispatch(fixture.GetReceiverSpace(), MakeReceiveRequest(receiverHandle.GetValue()));
        }
        ASTERIA_EXPECT(
            runner, "corrupt Endpoint head is reported as an invariant failure", magic != nullptr,
            !corrupted.HasValue(), corrupted.GetError().Error == IpcSystemCallDispatchError::EndpointProtocolViolation,
            corrupted.GetError().Endpoint == EndpointErrorCode::QueueCorrupt,
            corrupted.GetError().UserMemory == UserMemoryAccessError::None, corrupted.GetError().Reserved == 0U);
        if (magic != nullptr)
        {
            magic[0U] ^= 1U;
        }
        ASTERIA_EXPECT(runner, "Endpoint invariant failure preserves the queue and clears scratch",
                       fixture.GetEndpoint().GetQueueState().QueuedMessages == 1U, harness.AllScratchWasCleared());
    }

    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "copyout-invariant fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x7777U}};
    const RevocationDomainId domain = fixture.CreateDomain();
    const auto senderHandle =
        fixture.MintEndpoint(fixture.GetSenderSpace(), CapabilityRights{CapabilityRight::Invoke}, domain);
    const auto receiverHandle =
        fixture.MintEndpoint(fixture.GetReceiverSpace(), CapabilityRights{CapabilityRight::Read}, domain);
    const Size frameSize = StoreMessage(harness.GetUserMemory(), MakeMessage(0x7777U, 3U));
    if (senderHandle.HasValue())
    {
        static_cast<void>(
            harness.Dispatch(fixture.GetSenderSpace(), MakeSendRequest(senderHandle.GetValue(), frameSize)));
    }
    harness.GetUserMemory().Fill(ReceiveAddress, IpcDeliveryCodec::MaximumWireSizeBytes, 0xCCU);
    harness.GetUserMemory().ObserveCommitAgainst(fixture.GetEndpoint());
    harness.GetUserMemory().FailNextCommit(UserMemoryAccessError::BackendProtocolViolation);
    DispatchResult commitFailure = DispatchResult::Failure(IpcSystemCallDispatchFailure{});
    if (receiverHandle.HasValue())
    {
        commitFailure = harness.Dispatch(fixture.GetReceiverSpace(), MakeReceiveRequest(receiverHandle.GetValue()));
    }
    ASTERIA_EXPECT(runner, "post-consumption copyout contract breach is an invariant failure",
                   !commitFailure.HasValue(),
                   commitFailure.GetError().Error == IpcSystemCallDispatchError::UserMemoryProtocolViolation,
                   commitFailure.GetError().UserMemory == UserMemoryAccessError::BackendProtocolViolation,
                   harness.GetUserMemory().GetObservedQueuedMessagesAtCommit() == 0U,
                   fixture.GetEndpoint().GetQueueState().QueuedMessages == 0U);
    ASTERIA_EXPECT(runner, "copyout invariant failure writes no byte and clears scratch",
                   harness.GetUserMemory().RangeHasValue(ReceiveAddress, IpcDeliveryCodec::MaximumWireSizeBytes, 0xCCU),
                   harness.AllScratchWasCleared());
}

void TestDebugWrite(TestContext& runner) noexcept
{
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "DebugWrite fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x3333U}};
    const RevocationDomainId domain = fixture.CreateDomain();
    const auto writer = fixture.MintChannel(fixture.GetSenderSpace(), CapabilityRights{CapabilityRight::Write}, domain);
    const auto reader = fixture.MintChannel(fixture.GetSenderSpace(), CapabilityRights{CapabilityRight::Read}, domain);
    const auto endpoint =
        fixture.MintEndpoint(fixture.GetSenderSpace(), CapabilityRights{CapabilityRight::Write}, domain);
    ASTERIA_EXPECT(runner, "DebugWrite capabilities mint", writer.HasValue(), reader.HasValue(), endpoint.HasValue());
    if (!writer.HasValue() || !reader.HasValue() || !endpoint.HasValue())
    {
        return;
    }
    const auto store = [&harness](const char* const text) noexcept
    {
        Size size{0U};
        while (text[size] != '\0')
        {
            ++size;
        }
        harness.GetUserMemory().Store(SendAddress, reinterpret_cast<const UInt8*>(text), size);
        return size;
    };
    const auto write = [&](const CapabilityHandle handle, const Size size, const UInt64 address = SendAddress) noexcept
    {
        return harness.Dispatch(fixture.GetSenderSpace(),
                                MakeIpcRequest(SystemCallNumber::DebugWrite, handle, address, size));
    };
    RecordingLineSink& sink = fixture.GetChannelSink();

    const Size twoLines = store("ok 1 - first\n\nnot ok 2\n");
    ASTERIA_EXPECT(runner, "DebugWrite emits every line and returns the byte count",
                   ResponseIs(write(writer.GetValue(), twoLines), SystemCallError::None, twoLines),
                   sink.GetLineCount() == 3U, sink.LineIs(0U, "ok 1 - first"), sink.LineIs(1U, ""),
                   sink.LineIs(2U, "not ok 2"));

    const Size carriageReturn = store("ok\r\n");
    const Size unterminated = store("partial");
    ASTERIA_EXPECT(runner, "DebugWrite rejects control bytes and unterminated text without output",
                   ResponseIs(write(writer.GetValue(), carriageReturn), SystemCallError::InvalidArguments),
                   ResponseIs(write(writer.GetValue(), unterminated), SystemCallError::InvalidArguments),
                   sink.GetLineCount() == 3U);

    SystemCallRequest reserved = MakeIpcRequest(SystemCallNumber::DebugWrite, writer.GetValue(), SendAddress, 3U);
    reserved.Arguments[5U] = 1U;
    ASTERIA_EXPECT(runner, "DebugWrite rejects empty, oversized and reserved-argument requests",
                   ResponseIs(write(writer.GetValue(), 0U, 0U), SystemCallError::InvalidArguments),
                   ResponseIs(write(writer.GetValue(), Asteria::Abi::DebugWriteMaximumBytes + 1U),
                              SystemCallError::InvalidArguments),
                   ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), reserved), SystemCallError::InvalidArguments),
                   sink.GetLineCount() == 3U);

    const Size line = store("x\n");
    ASTERIA_EXPECT(runner, "DebugWrite needs a DiagnosticChannel with the Write right",
                   ResponseIs(write(reader.GetValue(), line), SystemCallError::RightsDenied),
                   ResponseIs(write(endpoint.GetValue(), line), SystemCallError::TypeMismatch),
                   sink.GetLineCount() == 3U);
    ASTERIA_EXPECT(runner, "DebugWrite reports an inaccessible user buffer as a fault",
                   ResponseIs(write(writer.GetValue(), line, 0x1000U), SystemCallError::UserMemoryFault),
                   sink.GetLineCount() == 3U, harness.AllScratchWasCleared());
}

// Records the decoded request and answers with a chosen result.
class FakePagerResolution final : public Asteria::Kernel::Memory::IPagerResolution
{
public:
    [[nodiscard]] Asteria::Kernel::Memory::PagerResolutionResult
    Resolve(CapabilitySpace& caller, const Asteria::Kernel::Memory::PagerResolveRequest& request) noexcept override
    {
        ++Calls;
        Caller = &caller;
        Last = request;
        return Result;
    }

    Size Calls{0U};
    CapabilitySpace* Caller{nullptr};
    Asteria::Kernel::Memory::PagerResolveRequest Last{};
    Asteria::Kernel::Memory::PagerResolutionResult Result{};
};

void StoreWord(UInt8* const bytes, const Size offset, const UInt64 value) noexcept
{
    for (Size index{0U}; index < 8U; ++index)
    {
        bytes[offset + index] = static_cast<UInt8>(value >> (8U * index));
    }
}

void TestFaultResolve(TestContext& runner) noexcept
{
    using Asteria::Kernel::Memory::PagePermission;
    using Asteria::Kernel::Memory::PagerResolutionError;
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "FaultResolve fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x3333U}};
    FakePagerResolution pager;
    TestKernelObject thread{ObjectType::Thread, ResourceDomainId{0x3333U}};
    const auto registeredThread = fixture.GetIssuer().CreateObject(thread);
    Asteria::Kernel::Memory::FaultTokenFactory factory{fixture.GetObjects(), fixture.GetIssuer()};
    const auto createdToken = factory.Create(Asteria::Kernel::Memory::FaultTokenCreateRequest{
        .Destination = &fixture.GetSenderSpace(),
        .ChargeOwner = ResourceDomainId{0x3333U},
        .Thread = Asteria::Kernel::Scheduler::ThreadId{fixture.GetObjects(), registeredThread.GetValue()},
        .AddressSpace = fixture.GetAddressSpaceId(),
        .Fault = {.VirtualAddress = 0x4000U,
                  .InstructionPointer = 0x1000U,
                  .ArchitectureSyndrome = 0U,
                  .Access = Asteria::Kernel::Memory::FaultAccess::Read,
                  .Cause = Asteria::Kernel::Memory::FaultCause::MissingMapping,
                  .Privilege = Asteria::Kernel::Memory::FaultPrivilege::User,
                  .Reserved = {}},
        .RevocationDomain = fixture.CreateDomain(),
        .LeaseDeadline = MonotonicTime::Infinite(),
        .Resolution = &pager});
    const CapabilityHandle token = createdToken.HasValue() ? createdToken.GetValue().Capability : CapabilityHandle{};
    UInt8 wire[Asteria::Abi::FaultResolveRequestSizeBytes]{};
    StoreWord(wire, Asteria::Abi::FaultResolveAddressSpaceOffset, 0x0202U);
    StoreWord(wire, Asteria::Abi::FaultResolveAddressSpaceOffset + 8U, 0xB0B0U);
    StoreWord(wire, Asteria::Abi::FaultResolveMemoryObjectOffset, 0x0303U);
    StoreWord(wire, Asteria::Abi::FaultResolveMemoryObjectOffset + 8U, 0xC0C0U);
    StoreWord(wire, Asteria::Abi::FaultResolveFrameOffset, 0x0404U);
    StoreWord(wire, Asteria::Abi::FaultResolveFrameOffset + 8U, 0xD0D0U);
    StoreWord(wire, Asteria::Abi::FaultResolveVirtualStartOffset, 0x4000'0000U);
    StoreWord(wire, Asteria::Abi::FaultResolvePageCountOffset, 1U);
    StoreWord(wire, Asteria::Abi::FaultResolveMemoryObjectPageOffset, 2U);
    StoreWord(wire, Asteria::Abi::FaultResolveFramePageOffset, 3U);
    wire[Asteria::Abi::FaultResolvePermissionsOffset] = 0x0BU;
    harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
    const auto resolve = [&](const UInt64 kind, const UInt64 address, const UInt64 size) noexcept
    {
        SystemCallRequest request = MakeIpcRequest(SystemCallNumber::FaultResolve, token, kind, 0U);
        request.Arguments[3U] = address;
        request.Arguments[4U] = size;
        return harness.Dispatch(fixture.GetSenderSpace(), request);
    };
    constexpr UInt64 Map{static_cast<UInt64>(Asteria::Abi::FaultResolveKind::Map)};

    auto forgedRequest = MakeIpcRequest(SystemCallNumber::FaultResolve, CapabilityHandle{0x0101U, 0xA0A0U}, Map, 0U);
    forgedRequest.Arguments[3U] = SendAddress;
    forgedRequest.Arguments[4U] = sizeof(wire);
    const auto forged = harness.Dispatch(fixture.GetSenderSpace(), forgedRequest);
    const auto unconfigured = resolve(Map, SendAddress, sizeof(wire));
    FakePagerResolution otherPager;
    harness.SetPagerResolution(&otherPager);
    pager.Calls = 0U;
    const auto resolved = resolve(Map, SendAddress, sizeof(wire));
    const auto& last = pager.Last;
    ASTERIA_EXPECT(runner, "FaultResolve decodes the request and passes the caller's space",
                   ResponseIs(unconfigured, SystemCallError::None), ResponseIs(forged, SystemCallError::InvalidHandle),
                   ResponseIs(resolved, SystemCallError::None), pager.Calls == 1U,
                   pager.Caller == &fixture.GetSenderSpace(), last.FaultToken.GetSelector() == token.GetSelector(),
                   last.FaultToken.GetAuthenticator() == token.GetAuthenticator(), otherPager.Calls == 0U,
                   last.AddressSpace.GetSelector() == 0x0202U, last.AddressSpace.GetAuthenticator() == 0xB0B0U,
                   last.MemoryObject.GetSelector() == 0x0303U, last.Frame.GetAuthenticator() == 0xD0D0U,
                   last.Mapping.VirtualStart.GetValue() == 0x4000'0000U, last.Mapping.Pages.GetValue() == 1U,
                   last.Mapping.MemoryObjectOffsetPages == 2U, last.Mapping.FrameOffsetPages == 3U,
                   last.Mapping.Permissions.Contains(PagePermission::Read),
                   last.Mapping.Permissions.Contains(PagePermission::Write),
                   !last.Mapping.Permissions.Contains(PagePermission::Execute),
                   last.Mapping.Permissions.Contains(PagePermission::User));

    SystemCallRequest reserved = MakeIpcRequest(SystemCallNumber::FaultResolve, token, Map, 0U);
    reserved.Arguments[3U] = SendAddress;
    reserved.Arguments[4U] = sizeof(wire);
    reserved.Arguments[5U] = 1U;
    ASTERIA_EXPECT(runner, "FaultResolve rejects another kind, size or reserved argument before the pager",
                   ResponseIs(resolve(3U, SendAddress, sizeof(wire)), SystemCallError::InvalidArguments),
                   ResponseIs(resolve(0U, SendAddress, sizeof(wire)), SystemCallError::InvalidArguments),
                   ResponseIs(resolve(Map, SendAddress, sizeof(wire) - 1U), SystemCallError::InvalidArguments),
                   ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), reserved), SystemCallError::InvalidArguments),
                   ResponseIs(resolve(Map, 0x1000U, sizeof(wire)), SystemCallError::UserMemoryFault),
                   pager.Calls == 1U);

    wire[Asteria::Abi::FaultResolvePermissionsOffset] = 0x13U;
    harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
    const auto undefinedPermission = resolve(Map, SendAddress, sizeof(wire));
    wire[Asteria::Abi::FaultResolvePermissionsOffset] = 0x03U;
    wire[Asteria::Abi::FaultResolveRequestSizeBytes - 1U] = 1U;
    harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
    const auto reservedByte = resolve(Map, SendAddress, sizeof(wire));
    ASTERIA_EXPECT(runner, "FaultResolve rejects an undefined permission bit and a nonzero reserved byte",
                   ResponseIs(undefinedPermission, SystemCallError::InvalidArguments),
                   ResponseIs(reservedByte, SystemCallError::InvalidArguments), pager.Calls == 1U);

    wire[Asteria::Abi::FaultResolveRequestSizeBytes - 1U] = 0U;
    harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
    const auto answer = [&](const PagerResolutionError error, const CapabilityError capability) noexcept
    {
        pager.Result = {.Error = error, .Capability = capability, .Reserved = {}};
        return resolve(Map, SendAddress, sizeof(wire));
    };
    ASTERIA_EXPECT(
        runner, "FaultResolve maps each pager result onto the system-call errors",
        ResponseIs(answer(PagerResolutionError::NotPending, CapabilityError::Ok), SystemCallError::ObjectNotFound),
        ResponseIs(answer(PagerResolutionError::AddressSpaceMismatch, CapabilityError::Ok),
                   SystemCallError::RightsDenied),
        ResponseIs(answer(PagerResolutionError::CapabilityRejected, CapabilityError::Revoked),
                   SystemCallError::Revoked),
        ResponseIs(answer(PagerResolutionError::MappingRejected, CapabilityError::Ok),
                   SystemCallError::InvalidArguments),
        ResponseIs(answer(PagerResolutionError::InvalidRequest, CapabilityError::Ok),
                   SystemCallError::InvalidArguments),
        ResponseIs(answer(PagerResolutionError::Faulted, CapabilityError::Ok), SystemCallError::InvalidConfiguration),
        harness.AllScratchWasCleared());

    // Terminate names only the token; arguments 3 to 5 are zero.
    constexpr UInt64 Terminate{static_cast<UInt64>(Asteria::Abi::FaultResolveKind::Terminate)};
    pager.Result = {.Error = PagerResolutionError::None, .Capability = CapabilityError::Ok, .Reserved = {}};
    const UInt64 callsBefore = pager.Calls;
    const auto terminated = resolve(Terminate, 0U, 0U);
    const auto& request = pager.Last;
    ASTERIA_EXPECT(runner, "FaultResolve Terminate passes only the token and returns the exit code",
                   ResponseIs(terminated, SystemCallError::None, Asteria::Abi::FaultTerminatedExitCode),
                   pager.Calls == callsBefore + 1U,
                   request.Kind == Asteria::Kernel::Memory::FaultResolutionKind::Terminate,
                   request.FaultToken.GetSelector() == token.GetSelector(),
                   request.FaultToken.GetAuthenticator() == token.GetAuthenticator(), !request.AddressSpace.IsValid(),
                   !request.MemoryObject.IsValid(), !request.Frame.IsValid(), request.Mapping.Pages.GetValue() == 0U);
    SystemCallRequest terminateReserved = MakeIpcRequest(SystemCallNumber::FaultResolve, token, Terminate, 0U);
    terminateReserved.Arguments[5U] = 1U;
    pager.Result = {.Error = PagerResolutionError::NotPending, .Capability = CapabilityError::Ok, .Reserved = {}};
    const auto stale = resolve(Terminate, 0U, 0U);
    ASTERIA_EXPECT(
        runner, "FaultResolve Terminate rejects a request or reserved argument and maps the pager result",
        ResponseIs(resolve(Terminate, SendAddress, 0U), SystemCallError::InvalidArguments),
        ResponseIs(resolve(Terminate, 0U, sizeof(wire)), SystemCallError::InvalidArguments),
        ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), terminateReserved), SystemCallError::InvalidArguments),
        ResponseIs(stale, SystemCallError::ObjectNotFound), pager.Calls == callsBefore + 2U,
        harness.AllScratchWasCleared());
}
// Records the decoded requests and answers with a chosen result.
class FakeObjectManagement final : public Asteria::Kernel::System::IObjectManagement
{
public:
    [[nodiscard]] Asteria::Abi::SystemCallResponse AccessPorts(Asteria::Kernel::Object::CapabilitySpace&,
                                                               const Asteria::Abi::SystemCallRequest&) noexcept override
    {
        return {.Error = Asteria::Abi::SystemCallError::InvalidConfiguration, .Value0 = 0U, .Value1 = 0U};
    }

    [[nodiscard]] Asteria::Kernel::System::ObjectManagementResult
    Create(CapabilitySpace& caller, const Asteria::Kernel::System::ObjectCreateRequest& request) noexcept override
    {
        ++Creates;
        Caller = &caller;
        LastCreate = request;
        return Result;
    }

    [[nodiscard]] Asteria::Kernel::System::ObjectManagementResult
    Destroy(CapabilitySpace& caller, const CapabilityHandle object, const ObjectType type) noexcept override
    {
        ++Destroys;
        Caller = &caller;
        LastDestroyed = object;
        LastDestroyedType = type;
        return Result;
    }

    [[nodiscard]] Asteria::Kernel::System::ObjectManagementResult
    Map(CapabilitySpace& caller, const Asteria::Kernel::Memory::MemoryMapRequest& request) noexcept override
    {
        ++Maps;
        Caller = &caller;
        LastMap = request;
        return Result;
    }

    [[nodiscard]] Asteria::Kernel::System::ObjectManagementResult
    Unmap(CapabilitySpace& caller, const CapabilityHandle space,
          const Asteria::Kernel::Memory::VirtualAddress virtualStart,
          const Asteria::Kernel::Memory::PageCount pages) noexcept override
    {
        ++Unmaps;
        Caller = &caller;
        LastUnmapSpace = space;
        LastUnmapStart = virtualStart.GetValue();
        LastUnmapPages = pages.GetValue();
        return Result;
    }

    [[nodiscard]] Asteria::Kernel::System::ObjectManagementResult
    Terminate(CapabilitySpace& caller, const CapabilityHandle thread) noexcept override
    {
        ++Terminations;
        Caller = &caller;
        LastTerminated = thread;
        return Result;
    }

    [[nodiscard]] Asteria::Kernel::System::ObjectManagementResult BindWaitSet(CapabilitySpace& caller,
                                                                              const CapabilityHandle waitSet,
                                                                              const CapabilityHandle endpoint,
                                                                              const UInt64 tag) noexcept override
    {
        ++Binds;
        Caller = &caller;
        LastWaitSet = waitSet;
        LastEndpoint = endpoint;
        LastTag = tag;
        return Result;
    }

    [[nodiscard]] Asteria::Kernel::System::ObjectManagementResult
    ControlDevice(CapabilitySpace& caller, const Asteria::Kernel::Protection::ProtectionDomainId callerDomain,
                  const CapabilityHandle function, const UInt64 operation,
                  const CapabilityHandle domain) noexcept override
    {
        ++DeviceControls;
        Caller = &caller;
        LastCallerDomain = callerDomain;
        LastFunction = function;
        LastOperation = operation;
        LastDeviceDomain = domain;
        return Result;
    }

    [[nodiscard]] Asteria::Kernel::System::ObjectManagementResult
    ControlIommu(CapabilitySpace& caller, const Asteria::Kernel::Protection::ProtectionDomainId callerDomain,
                 const Asteria::Kernel::System::IommuControlRequest& request) noexcept override
    {
        ++IommuControls;
        Caller = &caller;
        LastCallerDomain = callerDomain;
        LastIommu = request;
        return Result;
    }

    [[nodiscard]] Asteria::Kernel::System::ObjectManagementResult
    ControlInterrupt(CapabilitySpace& caller, const CapabilityHandle interrupt,
                     const UInt64 operation) noexcept override
    {
        ++InterruptControls;
        Caller = &caller;
        LastInterrupt = interrupt;
        LastOperation = operation;
        return Result;
    }

    Size InterruptControls{0U};
    CapabilityHandle LastInterrupt{};
    Size IommuControls{0U};
    Asteria::Kernel::System::IommuControlRequest LastIommu{};
    Size DeviceControls{0U};
    Asteria::Kernel::Protection::ProtectionDomainId LastCallerDomain{};
    CapabilityHandle LastFunction{};
    UInt64 LastOperation{0U};
    CapabilityHandle LastDeviceDomain{};
    Size Binds{0U};
    CapabilityHandle LastWaitSet{};
    CapabilityHandle LastEndpoint{};
    UInt64 LastTag{0U};
    Size Creates{0U};
    Size Destroys{0U};
    Size Maps{0U};
    Size Unmaps{0U};
    Size Terminations{0U};
    CapabilityHandle LastTerminated{};
    CapabilityHandle LastUnmapSpace{};
    UInt64 LastUnmapStart{0U};
    UInt64 LastUnmapPages{0U};
    CapabilitySpace* Caller{nullptr};
    Asteria::Kernel::Memory::MemoryMapRequest LastMap{};
    Asteria::Kernel::System::ObjectCreateRequest LastCreate{};
    CapabilityHandle LastDestroyed{};
    ObjectType LastDestroyedType{ObjectType::Invalid};
    Asteria::Kernel::System::ObjectManagementResult Result{};
};

void TestObjectManagement(TestContext& runner) noexcept
{
    using Asteria::Kernel::System::ObjectManagementError;
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "object management fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x3333U}};
    FakeObjectManagement management;
    const CapabilityHandle domain{0x0505U, 0xE0E0U};
    UInt8 wire[Asteria::Abi::ObjectCreateEndpointRequestSizeBytes]{};
    wire[Asteria::Abi::ObjectCreateEndpointQueuedBytesOffset] = 0xC0U;
    wire[Asteria::Abi::ObjectCreateEndpointQueuedBytesOffset + 1U] = 0x0FU;
    wire[Asteria::Abi::ObjectCreateEndpointReservedControlBytesOffset] = 0xF0U;
    wire[Asteria::Abi::ObjectCreateEndpointReservedControlBytesOffset + 1U] = 0x03U;
    wire[Asteria::Abi::ObjectCreateEndpointBytesPerSenderOffset] = 0xE0U;
    wire[Asteria::Abi::ObjectCreateEndpointBytesPerSenderOffset + 1U] = 0x07U;
    wire[Asteria::Abi::ObjectCreateEndpointQueuedMessagesOffset] = 8U;
    wire[Asteria::Abi::ObjectCreateEndpointReservedControlMessagesOffset] = 2U;
    wire[Asteria::Abi::ObjectCreateEndpointMessagesPerSenderOffset] = 4U;
    wire[Asteria::Abi::ObjectCreateEndpointQueuedAttachmentsOffset] = 32U;
    wire[Asteria::Abi::ObjectCreateEndpointReservedControlAttachmentsOffset] = 8U;
    wire[Asteria::Abi::ObjectCreateEndpointAttachmentsPerSenderOffset] = 16U;
    harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
    const auto create = [&](const UInt64 type, const UInt64 address, const UInt64 size) noexcept
    {
        SystemCallRequest request = MakeIpcRequest(SystemCallNumber::ObjectCreate, domain, type, 0U);
        request.Arguments[3U] = address;
        request.Arguments[4U] = size;
        return harness.Dispatch(fixture.GetSenderSpace(), request);
    };
    const auto destroy = [&](const UInt64 type) noexcept
    {
        return harness.Dispatch(fixture.GetSenderSpace(),
                                MakeIpcRequest(SystemCallNumber::ObjectDestroy, domain, type, 0U));
    };
    constexpr UInt64 EndpointType{Asteria::Abi::ObjectTypeEndpoint};

    const auto unconfigured = create(EndpointType, SendAddress, sizeof(wire));
    const auto unconfiguredDestroy = destroy(EndpointType);
    harness.SetObjectManagement(&management);
    management.Result = {.Error = Kernel::System::ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = CapabilityHandle{0x0707U, 0xF0F0U},
                         .Value = 0U};
    const auto created = create(EndpointType, SendAddress, sizeof(wire));
    const auto& last = management.LastCreate;
    const EndpointConfiguration expected = EndpointConfiguration::Default();
    ASTERIA_EXPECT(runner, "ObjectCreate decodes the Endpoint request and returns the new capability",
                   ResponseIs(unconfigured, SystemCallError::InvalidConfiguration),
                   ResponseIs(unconfiguredDestroy, SystemCallError::InvalidConfiguration),
                   ResponseIs(created, SystemCallError::None, 0x0707U, 0xF0F0U), management.Creates == 1U,
                   management.Caller == &fixture.GetSenderSpace(), last.Authority.GetSelector() == 0x0505U,
                   last.Authority.GetAuthenticator() == 0xE0E0U, last.Type == ObjectType::Endpoint,
                   last.Endpoint.MaximumQueuedBytes == expected.MaximumQueuedBytes,
                   last.Endpoint.ReservedControlBytes == expected.ReservedControlBytes,
                   last.Endpoint.MaximumBytesPerSender == expected.MaximumBytesPerSender,
                   last.Endpoint.MaximumQueuedMessages == expected.MaximumQueuedMessages,
                   last.Endpoint.ReservedControlMessages == expected.ReservedControlMessages,
                   last.Endpoint.MaximumMessagesPerSender == expected.MaximumMessagesPerSender,
                   last.Endpoint.MaximumQueuedAttachments == expected.MaximumQueuedAttachments,
                   last.Endpoint.ReservedControlAttachments == expected.ReservedControlAttachments,
                   last.Endpoint.MaximumAttachmentsPerSender == expected.MaximumAttachmentsPerSender,
                   last.Endpoint.Reserved == 0U);

    SystemCallRequest reserved = MakeIpcRequest(SystemCallNumber::ObjectCreate, domain, EndpointType, 0U);
    reserved.Arguments[3U] = SendAddress;
    reserved.Arguments[4U] = sizeof(wire);
    reserved.Arguments[5U] = 1U;
    ASTERIA_EXPECT(runner, "ObjectCreate rejects another type, size or reserved argument before reading memory",
                   ResponseIs(create(static_cast<UInt64>(ObjectType::Notification), SendAddress, sizeof(wire)),
                              SystemCallError::InvalidArguments),
                   ResponseIs(create(EndpointType, SendAddress, sizeof(wire) - 1U), SystemCallError::InvalidArguments),
                   ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), reserved), SystemCallError::InvalidArguments),
                   ResponseIs(create(EndpointType, 0x1000U, sizeof(wire)), SystemCallError::UserMemoryFault),
                   management.Creates == 1U);

    // A receiver space handle passes to the object manager.
    wire[Asteria::Abi::ObjectCreateEndpointReceiverSpaceOffset] = 0x21U;
    wire[Asteria::Abi::ObjectCreateEndpointReceiverSpaceOffset + 8U] = 0x43U;
    harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
    const auto receiverSpace = create(EndpointType, SendAddress, sizeof(wire));
    const auto receiverRequest = management.LastCreate;
    wire[Asteria::Abi::ObjectCreateEndpointReceiverSpaceOffset] = 0U;
    wire[Asteria::Abi::ObjectCreateEndpointReceiverSpaceOffset + 8U] = 0U;
    wire[Asteria::Abi::ObjectCreateEndpointSealedSelectorOffset - 1U] = 1U;
    harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
    const auto reservedByte = create(EndpointType, SendAddress, sizeof(wire));
    wire[Asteria::Abi::ObjectCreateEndpointSealedSelectorOffset - 1U] = 0U;
    // Service IPC: the last word is the sealed selector.
    StoreWord(wire, Asteria::Abi::ObjectCreateEndpointSealedSelectorOffset, 0x5EA1'ED00'0000'0042ULL);
    harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
    const auto sealed = create(EndpointType, SendAddress, sizeof(wire));
    const auto sealedRequest = management.LastCreate;
    StoreWord(wire, Asteria::Abi::ObjectCreateEndpointSealedSelectorOffset, 0U);
    harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
    const auto shortRequest = create(EndpointType, SendAddress, Asteria::Abi::ObjectCreateEndpointSealedSelectorOffset);
    ASTERIA_EXPECT(runner, "ObjectCreate passes a receiver space and a sealed selector and rejects a reserved byte",
                   receiverSpace.HasValue(), receiverRequest.EndpointReceiver.GetSelector() == 0x21U,
                   receiverRequest.EndpointReceiver.GetAuthenticator() == 0x43U,
                   receiverRequest.EndpointSealedSelector == 0U,
                   ResponseIs(reservedByte, SystemCallError::InvalidArguments), sealed.HasValue(),
                   sealedRequest.EndpointSealedSelector == 0x5EA1'ED00'0000'0042ULL,
                   ResponseIs(shortRequest, SystemCallError::InvalidArguments), management.Creates == 3U);

    const auto answer = [&](const ObjectManagementError error, const CapabilityError capability) noexcept
    {
        management.Result = {.Error = error, .Capability = capability, .Reserved = {}, .Created = {}, .Value = 0U};
        return create(EndpointType, SendAddress, sizeof(wire));
    };
    ASTERIA_EXPECT(
        runner, "ObjectCreate maps each management result onto the system-call errors",
        ResponseIs(answer(ObjectManagementError::UnsupportedType, CapabilityError::Ok),
                   SystemCallError::InvalidArguments),
        ResponseIs(answer(ObjectManagementError::InvalidRequest, CapabilityError::Ok),
                   SystemCallError::InvalidArguments),
        ResponseIs(answer(ObjectManagementError::CapabilityRejected, CapabilityError::RightsDenied),
                   SystemCallError::RightsDenied),
        ResponseIs(answer(ObjectManagementError::CapabilityRejected, CapabilityError::SpaceExhausted),
                   SystemCallError::SpaceExhausted),
        ResponseIs(answer(ObjectManagementError::NotOwned, CapabilityError::Ok), SystemCallError::RightsDenied),
        ResponseIs(answer(ObjectManagementError::ResourceExhausted, CapabilityError::Ok),
                   SystemCallError::ResourceExhausted));
    const auto faulted = answer(ObjectManagementError::Faulted, CapabilityError::Ok);
    ASTERIA_EXPECT(
        runner, "ObjectCreate reports an operation the kernel could not undo as a violation", !faulted.HasValue(),
        !faulted.HasValue() && (faulted.GetError().Error == IpcSystemCallDispatchError::ObjectManagementViolation));

    // Frames and MemoryObjects take 16-byte requests.
    management.Result = {.Error = ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = CapabilityHandle{0x0808U, 0xF1F1U},
                         .Value = 0U};
    UInt8 memory[Asteria::Abi::ObjectCreateMemoryObjectRequestSizeBytes]{};
    StoreWord(memory, Asteria::Abi::ObjectCreatePagesOffset, 3U);
    memory[Asteria::Abi::ObjectCreatePermissionsOffset] = 0x0BU;
    harness.GetUserMemory().Store(SendAddress, memory, sizeof(memory));
    const auto createdMemory = create(Asteria::Abi::ObjectTypeMemoryObject, SendAddress, sizeof(memory));
    const auto memoryRequest = management.LastCreate;
    const auto frameSized = create(Asteria::Abi::ObjectTypeFrame, SendAddress, sizeof(memory));
    const auto frameRequest = management.LastCreate;
    memory[Asteria::Abi::ObjectCreatePermissionsOffset] = 0x10U;
    harness.GetUserMemory().Store(SendAddress, memory, sizeof(memory));
    const auto undefinedPermission = create(Asteria::Abi::ObjectTypeMemoryObject, SendAddress, sizeof(memory));
    const Size creates = management.Creates;
    const auto wrongSize = create(Asteria::Abi::ObjectTypeFrame, SendAddress, sizeof(wire));
    ASTERIA_EXPECT(runner, "ObjectCreate decodes Frame and MemoryObject requests",
                   ResponseIs(createdMemory, SystemCallError::None, 0x0808U, 0xF1F1U),
                   memoryRequest.Type == ObjectType::MemoryObject, memoryRequest.Pages.GetValue() == 3U,
                   memoryRequest.MaximumPermissions.GetBits() == 0x0BU,
                   ResponseIs(frameSized, SystemCallError::InvalidArguments),
                   frameRequest.Type == ObjectType::MemoryObject,
                   ResponseIs(undefinedPermission, SystemCallError::InvalidArguments),
                   ResponseIs(wrongSize, SystemCallError::InvalidArguments), management.Creates == creates);
    UInt8 frameBlock[Asteria::Abi::ObjectCreateFrameRequestSizeBytes]{};
    StoreWord(frameBlock, Asteria::Abi::ObjectCreatePagesOffset, 2U);
    harness.GetUserMemory().Store(SendAddress, frameBlock, sizeof(frameBlock));
    const auto createdFrame = create(Asteria::Abi::ObjectTypeFrame, SendAddress, sizeof(frameBlock));
    const auto createdFrameRequest = management.LastCreate;
    ASTERIA_EXPECT(runner, "ObjectCreate passes a Frame's page count",
                   ResponseIs(createdFrame, SystemCallError::None, 0x0808U, 0xF1F1U),
                   createdFrameRequest.Type == ObjectType::Frame, createdFrameRequest.Pages.GetValue() == 2U,
                   createdFrameRequest.FrameAlignmentPages == 0U);
    // A Frame of RAM may ask for a physical alignment of a power of two up to 16 pages.
    frameBlock[Asteria::Abi::ObjectCreateFrameAlignmentOffset] = 4U;
    harness.GetUserMemory().Store(SendAddress, frameBlock, sizeof(frameBlock));
    const auto alignedFrame = create(Asteria::Abi::ObjectTypeFrame, SendAddress, sizeof(frameBlock));
    const auto alignedRequest = management.LastCreate;
    const Size alignedCreates = management.Creates;
    frameBlock[Asteria::Abi::ObjectCreateFrameAlignmentOffset] = 3U;
    harness.GetUserMemory().Store(SendAddress, frameBlock, sizeof(frameBlock));
    const auto oddAlignment = create(Asteria::Abi::ObjectTypeFrame, SendAddress, sizeof(frameBlock));
    frameBlock[Asteria::Abi::ObjectCreateFrameAlignmentOffset] = 32U;
    harness.GetUserMemory().Store(SendAddress, frameBlock, sizeof(frameBlock));
    const auto wideAlignment = create(Asteria::Abi::ObjectTypeFrame, SendAddress, sizeof(frameBlock));
    frameBlock[Asteria::Abi::ObjectCreateFrameAlignmentOffset] = 16U;
    frameBlock[Asteria::Abi::ObjectCreateFrameAlignmentOffset + 1U] = 1U;
    harness.GetUserMemory().Store(SendAddress, frameBlock, sizeof(frameBlock));
    const auto alignmentTail = create(Asteria::Abi::ObjectTypeFrame, SendAddress, sizeof(frameBlock));
    ASTERIA_EXPECT(runner, "ObjectCreate passes a Frame's alignment and refuses one that is no power of two to 16",
                   ResponseIs(alignedFrame, SystemCallError::None, 0x0808U, 0xF1F1U),
                   alignedRequest.Type == ObjectType::Frame, alignedRequest.FrameAlignmentPages == 4U,
                   ResponseIs(oddAlignment, SystemCallError::InvalidArguments),
                   ResponseIs(wideAlignment, SystemCallError::InvalidArguments),
                   ResponseIs(alignmentTail, SystemCallError::InvalidArguments), management.Creates == alignedCreates);

    // A device Frame's 40-byte request names its region and page offset, and a MemoryObject's
    // cache-policy byte picks uncached.
    UInt8 deviceBlock[Asteria::Abi::ObjectCreateDeviceFrameRequestSizeBytes]{};
    StoreWord(deviceBlock, Asteria::Abi::ObjectCreatePagesOffset, 1U);
    StoreWord(deviceBlock, Asteria::Abi::ObjectCreateDeviceFrameRegionOffset, 0x0909U);
    StoreWord(deviceBlock, Asteria::Abi::ObjectCreateDeviceFrameRegionOffset + 8U, 0xA1A1U);
    StoreWord(deviceBlock, Asteria::Abi::ObjectCreateDeviceFrameRegionPageOffset, 3U);
    harness.GetUserMemory().Store(SendAddress, deviceBlock, sizeof(deviceBlock));
    const auto createdDevice = create(Asteria::Abi::ObjectTypeFrame, SendAddress, sizeof(deviceBlock));
    const auto deviceRequest = management.LastCreate;
    const Size devices = management.Creates;
    const auto deviceAsMemory = create(Asteria::Abi::ObjectTypeMemoryObject, SendAddress, sizeof(deviceBlock));
    StoreWord(deviceBlock, Asteria::Abi::ObjectCreateDeviceFrameRegionOffset, 0U);
    StoreWord(deviceBlock, Asteria::Abi::ObjectCreateDeviceFrameRegionOffset + 8U, 0U);
    harness.GetUserMemory().Store(SendAddress, deviceBlock, sizeof(deviceBlock));
    const auto noRegion = create(Asteria::Abi::ObjectTypeFrame, SendAddress, sizeof(deviceBlock));
    StoreWord(deviceBlock, Asteria::Abi::ObjectCreateDeviceFrameRegionOffset, 0x0909U);
    StoreWord(deviceBlock, Asteria::Abi::ObjectCreateDeviceFrameRegionOffset + 8U, 0xA1A1U);
    deviceBlock[Asteria::Abi::ObjectCreateFrameAlignmentOffset] = 2U;
    harness.GetUserMemory().Store(SendAddress, deviceBlock, sizeof(deviceBlock));
    const auto alignedDevice = create(Asteria::Abi::ObjectTypeFrame, SendAddress, sizeof(deviceBlock));
    memory[Asteria::Abi::ObjectCreatePermissionsOffset] = 0x0BU;
    memory[Asteria::Abi::ObjectCreateCachePolicyOffset] = Asteria::Abi::ObjectCreateCacheUncached;
    harness.GetUserMemory().Store(SendAddress, memory, sizeof(memory));
    const auto uncachedMemory = create(Asteria::Abi::ObjectTypeMemoryObject, SendAddress, sizeof(memory));
    const auto uncachedRequest = management.LastCreate;
    memory[Asteria::Abi::ObjectCreateCachePolicyOffset] = 3U;
    harness.GetUserMemory().Store(SendAddress, memory, sizeof(memory));
    const auto undefinedPolicy = create(Asteria::Abi::ObjectTypeMemoryObject, SendAddress, sizeof(memory));
    ASTERIA_EXPECT(runner, "ObjectCreate decodes a device Frame's region and a MemoryObject's cache policy",
                   ResponseIs(createdDevice, SystemCallError::None, 0x0808U, 0xF1F1U),
                   deviceRequest.Type == ObjectType::Frame, deviceRequest.Pages.GetValue() == 1U,
                   deviceRequest.FrameRegion.GetSelector() == 0x0909U,
                   deviceRequest.FrameRegion.GetAuthenticator() == 0xA1A1U, deviceRequest.FrameRegionOffsetPages == 3U,
                   ResponseIs(deviceAsMemory, SystemCallError::InvalidArguments),
                   ResponseIs(noRegion, SystemCallError::InvalidArguments),
                   ResponseIs(alignedDevice, SystemCallError::InvalidArguments), management.Creates == (devices + 1U),
                   ResponseIs(uncachedMemory, SystemCallError::None, 0x0808U, 0xF1F1U),
                   uncachedRequest.MemoryCachePolicy == Asteria::Kernel::Memory::MemoryCachePolicy::Uncached,
                   ResponseIs(undefinedPolicy, SystemCallError::InvalidArguments));

    management.Result = {.Error = ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = {},
                         .Value = 0U};
    const auto destroyed = destroy(EndpointType);
    SystemCallRequest destroyReserved = MakeIpcRequest(SystemCallNumber::ObjectDestroy, domain, EndpointType, 0U);
    destroyReserved.Arguments[3U] = SendAddress;
    ASTERIA_EXPECT(
        runner, "ObjectDestroy passes the handle and type and rejects reserved arguments",
        ResponseIs(destroyed, SystemCallError::None), management.Destroys == 1U,
        management.LastDestroyed.GetSelector() == 0x0505U, management.LastDestroyed.GetAuthenticator() == 0xE0E0U,
        management.LastDestroyedType == ObjectType::Endpoint,
        ResponseIs(destroy(static_cast<UInt64>(ObjectType::Notification)), SystemCallError::InvalidArguments),
        ResponseIs(destroy(Asteria::Abi::ObjectTypeFrame), SystemCallError::None),
        management.LastDestroyedType == ObjectType::Frame,
        ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), destroyReserved), SystemCallError::InvalidArguments),
        management.Destroys == 2U, harness.AllScratchWasCleared());
}

// A Thread's 64-byte ObjectCreate block and its reclaiming ObjectDestroy.
// DeviceControl names a function, an operation from
// DeviceAssign to DeviceQuiesced and, for DeviceAssign only, a ProtectionDomain, for DeviceQuiesced
// an assignment sequence; it reaches the object management with the caller's ProtectionDomain.
void TestDeviceControl(TestContext& runner) noexcept
{
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "device control fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x3333U}};
    FakeObjectManagement management;
    harness.SetObjectManagement(&management);
    const CapabilityHandle function{0x0606U, 0xF0F0U};
    const auto control = [&](const UInt64 operation, const UInt64 selector, const UInt64 authenticator) noexcept
    {
        SystemCallRequest request = MakeIpcRequest(SystemCallNumber::DeviceControl, function, operation, 0U);
        request.Arguments[3U] = selector;
        request.Arguments[4U] = authenticator;
        return harness.Dispatch(fixture.GetSenderSpace(), request);
    };
    const auto assigned = control(Asteria::Abi::DeviceAssign, 0x0707U, 0xE1E1U);
    const auto assignRequest = management.LastDeviceDomain;
    const UInt64 assignOperation = management.LastOperation;
    const auto enabled = control(Asteria::Abi::DeviceEnableBusMaster, 0U, 0U);
    const Size calls = management.DeviceControls;
    const auto named = control(Asteria::Abi::DeviceEnableBusMaster, 0x0707U, 0U);
    const auto unknown = control(0U, 0U, 0U);
    // : Quiesced names the drained assignment's sequence.
    const auto quiesced = control(Asteria::Abi::DeviceQuiesced, 0x5E0U, 0U);
    const UInt64 quiescedSequence = management.LastDeviceDomain.GetSelector();
    const auto noSequence = control(Asteria::Abi::DeviceQuiesced, 0U, 0U);
    const auto widened = control(Asteria::Abi::DeviceQuiesced, 0x5E0U, 1U);
    const auto beyond = control(Asteria::Abi::DeviceReboot + 1U, 0U, 0U);
    ASTERIA_EXPECT(runner, "DeviceControl passes the function, operation and domain, and refuses other shapes",
                   ResponseIs(assigned, SystemCallError::None), assignOperation == Asteria::Abi::DeviceAssign,
                   assignRequest.GetSelector() == 0x0707U, assignRequest.GetAuthenticator() == 0xE1E1U,
                   management.LastFunction.GetSelector() == 0x0606U, ResponseIs(enabled, SystemCallError::None),
                   calls == 2U, ResponseIs(named, SystemCallError::InvalidArguments),
                   ResponseIs(unknown, SystemCallError::InvalidArguments), ResponseIs(quiesced, SystemCallError::None),
                   quiescedSequence == 0x5E0U, management.LastOperation == Asteria::Abi::DeviceQuiesced,
                   ResponseIs(noSequence, SystemCallError::InvalidArguments),
                   ResponseIs(widened, SystemCallError::InvalidArguments),
                   ResponseIs(beyond, SystemCallError::InvalidArguments), management.DeviceControls == 3U,
                   harness.AllScratchWasCleared());
    management.Result.Error = Asteria::Kernel::System::ObjectManagementError::ReservedMemory;
    const auto firmware = control(Asteria::Abi::DeviceAssign, 0x0707U, 0xE1E1U);
    management.Result.Error = Asteria::Kernel::System::ObjectManagementError::DisplayReserved;
    const auto display = control(Asteria::Abi::DeviceAssign, 0x0707U, 0xE1E1U);
    management.Result.Error = Asteria::Kernel::System::ObjectManagementError::BridgeReserved;
    const auto bridge = control(Asteria::Abi::DeviceAssign, 0x0707U, 0xE1E1U);
    ASTERIA_EXPECT(runner, "firmware DMA, bridge and display reservations have distinct system-call errors",
                   ResponseIs(firmware, SystemCallError::ReservedMemory),
                   ResponseIs(display, SystemCallError::DisplayReserved),
                   ResponseIs(bridge, SystemCallError::BridgeReserved));
    // The USB legacy-support access (UsbHostDriver.md): a dword offset below
    // 0x100 and, for a write, a 32-bit value; the kernel judges the rest.
    management.Result.Error = Asteria::Kernel::System::ObjectManagementError::None;
    const Size before = management.DeviceControls;
    const auto read = control(Asteria::Abi::DeviceReadConfiguration, 0x68U, 0U);
    const UInt64 readOffset = management.LastDeviceDomain.GetSelector();
    const auto written = control(Asteria::Abi::DeviceWriteLegacySupport, 0x6CU, 0xE000'0000U);
    const UInt64 writtenValue = management.LastDeviceDomain.GetAuthenticator();
    const auto readValue = control(Asteria::Abi::DeviceReadConfiguration, 0x68U, 1U);
    const auto unaligned = control(Asteria::Abi::DeviceWriteLegacySupport, 0x6AU, 0U);
    const auto outside = control(Asteria::Abi::DeviceReadConfiguration, 0x100U, 0U);
    const auto wide = control(Asteria::Abi::DeviceWriteLegacySupport, 0x6CU, 0x1'0000'0000ULL);
    ASTERIA_EXPECT(runner, "legacy-support access passes an aligned offset and a dword, and refuses other shapes",
                   ResponseIs(read, SystemCallError::None), readOffset == 0x68U,
                   ResponseIs(written, SystemCallError::None), writtenValue == 0xE000'0000U,
                   management.LastOperation == Asteria::Abi::DeviceWriteLegacySupport,
                   ResponseIs(readValue, SystemCallError::InvalidArguments),
                   ResponseIs(unaligned, SystemCallError::InvalidArguments),
                   ResponseIs(outside, SystemCallError::InvalidArguments),
                   ResponseIs(wide, SystemCallError::InvalidArguments), management.DeviceControls == before + 2U);
    const Size controlsBefore = management.DeviceControls;
    ASTERIA_EXPECT(runner, "undefined device operation 8 is rejected before object management",
                   ResponseIs(control(8U, 0U, 0U), SystemCallError::InvalidArguments),
                   ResponseIs(control(8U, 0xFFU, 2U), SystemCallError::InvalidArguments),
                   management.DeviceControls == controlsBefore);
    ASTERIA_EXPECT(runner, "power operations keep their ABI numbers and reach object management",
                   Asteria::Abi::DevicePowerQuery == 9U, Asteria::Abi::DevicePowerOff == 10U,
                   Asteria::Abi::DeviceReboot == 11U,
                   ResponseIs(control(Asteria::Abi::DevicePowerQuery, 0U, 0U), SystemCallError::None),
                   ResponseIs(control(Asteria::Abi::DevicePowerOff, 0U, 0U), SystemCallError::None),
                   ResponseIs(control(Asteria::Abi::DeviceReboot, 0U, 0U), SystemCallError::None),
                   management.DeviceControls == controlsBefore + 3U, harness.AllScratchWasCleared());
}

// IommuControl passes the domain, the operation and its operands; IommuMap's block is read from
// user memory, and a nonzero reserved byte or an undefined permission refuses it.
void TestIommuControl(TestContext& runner) noexcept
{
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "IOMMU control fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x3333U}};
    FakeObjectManagement management;
    harness.SetObjectManagement(&management);
    const CapabilityHandle domain{0x0808U, 0xF1F1U};
    const auto control =
        [&](const UInt64 operation, const UInt64 third, const UInt64 fourth, const UInt64 fifth = 0U) noexcept
    {
        SystemCallRequest request = MakeIpcRequest(SystemCallNumber::IommuControl, domain, operation, 0U);
        request.Arguments[3U] = third;
        request.Arguments[4U] = fourth;
        request.Arguments[5U] = fifth;
        return harness.Dispatch(fixture.GetSenderSpace(), request);
    };
    const auto attached = control(Asteria::Abi::IommuAttach, 0x0606U, 0xF0F0U);
    const auto attach = management.LastIommu;
    UInt8 block[Asteria::Abi::IommuMapRequestSizeBytes]{};
    StoreWord(block, Asteria::Abi::IommuMapFrameOffset, 0x0A0AU);
    StoreWord(block, Asteria::Abi::IommuMapFrameOffset + 8U, 0xB0B0U);
    StoreWord(block, Asteria::Abi::IommuMapFramePageOffset, 1U);
    StoreWord(block, Asteria::Abi::IommuMapPagesOffset, 2U);
    StoreWord(block, Asteria::Abi::IommuMapDeviceAddressOffset, 0x0010'0000U);
    block[Asteria::Abi::IommuMapPermissionsOffset] = Asteria::Abi::IommuMapDeviceWrite;
    harness.GetUserMemory().Store(SendAddress, block, sizeof(block));
    const auto mapped = control(Asteria::Abi::IommuMap, SendAddress, sizeof(block));
    const auto map = management.LastIommu;
    const auto unmapped = control(Asteria::Abi::IommuUnmap, 0x0010'0000U, 0U);
    const auto unmap = management.LastIommu;
    const Size calls = management.IommuControls;
    block[Asteria::Abi::IommuMapPermissionsOffset] = 4U;
    harness.GetUserMemory().Store(SendAddress, block, sizeof(block));
    const auto undefined = control(Asteria::Abi::IommuMap, SendAddress, sizeof(block));
    block[Asteria::Abi::IommuMapPermissionsOffset] = Asteria::Abi::IommuMapDeviceRead;
    block[Asteria::Abi::IommuMapPermissionsOffset + 1U] = 1U;
    harness.GetUserMemory().Store(SendAddress, block, sizeof(block));
    const auto reserved = control(Asteria::Abi::IommuMap, SendAddress, sizeof(block));
    const auto shortBlock = control(Asteria::Abi::IommuMap, SendAddress, sizeof(block) - 8U);
    const auto faultsWithOperand = control(Asteria::Abi::IommuFaults, 1U, 0U);
    const auto unknown = control(Asteria::Abi::IommuFaults + 1U, 0U, 0U);
    const auto attachFifth = control(Asteria::Abi::IommuAttach, 0x0606U, 0xF0F0U, 1U);
    ASTERIA_EXPECT(runner, "IommuControl passes each operation's operands and refuses other shapes",
                   ResponseIs(attached, SystemCallError::None), attach.Operation == Asteria::Abi::IommuAttach,
                   attach.Domain.GetSelector() == 0x0808U, attach.Function.GetSelector() == 0x0606U,
                   attach.Function.GetAuthenticator() == 0xF0F0U, ResponseIs(mapped, SystemCallError::None),
                   map.Frame.GetSelector() == 0x0A0AU, map.Frame.GetAuthenticator() == 0xB0B0U,
                   map.FramePageOffset == 1U, map.Pages == 2U, map.DeviceAddress == 0x0010'0000U,
                   map.Permissions == Asteria::Abi::IommuMapDeviceWrite, ResponseIs(unmapped, SystemCallError::None),
                   unmap.DeviceAddress == 0x0010'0000U, calls == 3U,
                   ResponseIs(undefined, SystemCallError::InvalidArguments),
                   ResponseIs(reserved, SystemCallError::InvalidArguments),
                   ResponseIs(shortBlock, SystemCallError::InvalidArguments),
                   ResponseIs(faultsWithOperand, SystemCallError::InvalidArguments),
                   ResponseIs(unknown, SystemCallError::InvalidArguments),
                   ResponseIs(attachFifth, SystemCallError::InvalidArguments), management.IommuControls == 3U,
                   harness.AllScratchWasCleared());

    UInt8 request[Asteria::Abi::ObjectCreateIommuDomainRequestSizeBytes]{};
    request[Asteria::Abi::ObjectCreateIommuAddressBitsOffset] = 32U;
    const auto create = [&]() noexcept
    {
        harness.GetUserMemory().Store(SendAddress, request, sizeof(request));
        SystemCallRequest call =
            MakeIpcRequest(SystemCallNumber::ObjectCreate, domain, Asteria::Abi::ObjectTypeIommuDomain, 0U);
        call.Arguments[3U] = SendAddress;
        call.Arguments[4U] = sizeof(request);
        return harness.Dispatch(fixture.GetSenderSpace(), call);
    };
    const auto created = create();
    const auto last = management.LastCreate;
    request[1U] = 1U;
    const auto reservedCreate = create();
    ASTERIA_EXPECT(runner, "ObjectCreate type 16 passes the device address width; other bytes are zero",
                   ResponseIs(created, SystemCallError::None),
                   last.Type == Asteria::Kernel::Object::ObjectType::IommuDomain, last.IommuInputAddressBits == 32U,
                   ResponseIs(reservedCreate, SystemCallError::InvalidArguments));
}

// InterruptControl passes the Interrupt and its operation; ObjectCreate type 14 passes the
// function, the source and the caller's ProtectionDomain.
void TestInterruptControl(TestContext& runner) noexcept
{
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "interrupt control fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x3333U}};
    FakeObjectManagement management;
    harness.SetObjectManagement(&management);
    const CapabilityHandle interrupt{0x0E0EU, 0xF3F3U};
    const auto control = [&](const UInt64 operation, const UInt64 third) noexcept
    {
        SystemCallRequest request = MakeIpcRequest(SystemCallNumber::InterruptControl, interrupt, operation, 0U);
        request.Arguments[3U] = third;
        return harness.Dispatch(fixture.GetSenderSpace(), request);
    };
    management.Result = {.Error = Asteria::Kernel::System::ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = {},
                         .Value = 7U};
    const auto rearmed = control(Asteria::Abi::InterruptRearm, 0U);
    const UInt64 rearmOperation = management.LastOperation;
    const auto masked = control(Asteria::Abi::InterruptKeepMasked, 0U);
    const auto unknown = control(3U, 0U);
    const auto operand = control(Asteria::Abi::InterruptRearm, 1U);
    ASTERIA_EXPECT(runner, "InterruptControl passes the Interrupt and operation and returns the sequence",
                   ResponseIs(rearmed, SystemCallError::None, 7U), rearmOperation == Asteria::Abi::InterruptRearm,
                   management.LastInterrupt.GetSelector() == 0x0E0EU, ResponseIs(masked, SystemCallError::None, 7U),
                   management.LastOperation == Asteria::Abi::InterruptKeepMasked,
                   ResponseIs(unknown, SystemCallError::InvalidArguments),
                   ResponseIs(operand, SystemCallError::InvalidArguments), management.InterruptControls == 2U,
                   harness.AllScratchWasCleared());

    UInt8 request[Asteria::Abi::ObjectCreateInterruptRequestSizeBytes]{};
    StoreWord(request, Asteria::Abi::ObjectCreateInterruptFunctionOffset, 0x0606U);
    StoreWord(request, Asteria::Abi::ObjectCreateInterruptFunctionOffset + 8U, 0xF0F0U);
    harness.GetUserMemory().Store(SendAddress, request, sizeof(request));
    SystemCallRequest call = MakeIpcRequest(SystemCallNumber::ObjectCreate, CapabilityHandle{0x0505U, 0xE0E0U},
                                            Asteria::Abi::ObjectTypeInterrupt, 0U);
    call.Arguments[3U] = SendAddress;
    call.Arguments[4U] = sizeof(request);
    management.Result = {.Error = Asteria::Kernel::System::ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = CapabilityHandle{0x0909U, 0xF2F2U},
                         .Value = 0U};
    const auto created = harness.Dispatch(fixture.GetSenderSpace(), call);
    const auto last = management.LastCreate;
    ASTERIA_EXPECT(runner, "ObjectCreate type 14 passes the function, its source and the caller's domain",
                   ResponseIs(created, SystemCallError::None, 0x0909U, 0xF2F2U),
                   last.Type == Asteria::Kernel::Object::ObjectType::Interrupt,
                   last.InterruptFunction.GetSelector() == 0x0606U,
                   last.InterruptFunction.GetAuthenticator() == 0xF0F0U, last.InterruptSource == 0U);
}

void TestThreadManagement(TestContext& runner) noexcept
{
    using Asteria::Kernel::System::ObjectManagementError;
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "thread management fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x3333U}};
    FakeObjectManagement management;
    harness.SetObjectManagement(&management);
    const CapabilityHandle domain{0x0505U, 0xE0E0U};
    UInt8 wire[Asteria::Abi::ObjectCreateThreadRequestSizeBytes]{};
    StoreWord(wire, Asteria::Abi::ObjectCreateThreadEntryOffset, 0x4000'1000U);
    StoreWord(wire, Asteria::Abi::ObjectCreateThreadStackOffset, 0x4100'0000U);
    StoreWord(wire, Asteria::Abi::ObjectCreateThreadArgumentOffset, 0x42U);
    const auto create = [&](const UInt64 size) noexcept
    {
        harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
        SystemCallRequest request =
            MakeIpcRequest(SystemCallNumber::ObjectCreate, domain, Asteria::Abi::ObjectTypeThread, 0U);
        request.Arguments[3U] = SendAddress;
        request.Arguments[4U] = size;
        return harness.Dispatch(fixture.GetSenderSpace(), request);
    };
    management.Result = {.Error = ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = CapabilityHandle{0x0909U, 0xF2F2U},
                         .Value = 0U};
    const auto created = create(sizeof(wire));
    const auto last = management.LastCreate;
    const auto wrongSize = create(sizeof(wire) - 8U);
    wire[Asteria::Abi::ObjectCreateThreadDomainOffset] = 0x31U;
    wire[Asteria::Abi::ObjectCreateThreadDomainOffset + 8U] = 0x32U;
    const auto otherDomain = create(sizeof(wire));
    const auto otherRequest = management.LastCreate;
    wire[Asteria::Abi::ObjectCreateThreadDomainOffset] = 0U;
    wire[Asteria::Abi::ObjectCreateThreadDomainOffset + 8U] = 0U;
    wire[Asteria::Abi::ObjectCreateThreadBudgetOffset] = 1U;
    const auto budget = create(sizeof(wire));
    wire[Asteria::Abi::ObjectCreateThreadBudgetOffset] = 0U;
    wire[Asteria::Abi::ObjectCreateThreadPeriodOffset + 7U] = 1U;
    const auto period = create(sizeof(wire));
    wire[Asteria::Abi::ObjectCreateThreadPeriodOffset + 7U] = 0U;
    StoreWord(wire, Asteria::Abi::ObjectCreateThreadPointerOffset, 0x4200'0000U);
    const auto threadPointer = create(sizeof(wire));
    StoreWord(wire, Asteria::Abi::ObjectCreateThreadPointerOffset, 0U);
    StoreWord(wire, Asteria::Abi::ObjectCreateThreadStackOffset, 0x4100'0008U);
    const auto misaligned = create(sizeof(wire));
    ASTERIA_EXPECT(
        runner, "ObjectCreate decodes a Thread's start and domain and refuses a schedule",
        ResponseIs(created, SystemCallError::None, 0x0909U, 0xF2F2U), last.Type == ObjectType::Thread,
        last.Thread.Entry == 0x4000'1000U, last.Thread.UserStackTop == 0x4100'0000U, last.Thread.Argument == 0x42U,
        !last.ThreadDomain.IsValid(), last.Thread.Domain == nullptr,
        ResponseIs(wrongSize, SystemCallError::InvalidArguments),
        ResponseIs(otherDomain, SystemCallError::None, 0x0909U, 0xF2F2U),
        otherRequest.ThreadDomain.GetSelector() == 0x31U, otherRequest.ThreadDomain.GetAuthenticator() == 0x32U,
        ResponseIs(budget, SystemCallError::InvalidArguments), ResponseIs(period, SystemCallError::InvalidArguments),
        ResponseIs(threadPointer, SystemCallError::InvalidArguments),
        ResponseIs(misaligned, SystemCallError::InvalidArguments), management.Creates == 2U);

    const auto destroy = [&]() noexcept
    {
        return harness.Dispatch(fixture.GetSenderSpace(), MakeIpcRequest(SystemCallNumber::ObjectDestroy, domain,
                                                                         Asteria::Abi::ObjectTypeThread, 0U));
    };
    management.Result = {.Error = ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = {},
                         .Value = 0x1234U};
    const auto reclaimed = destroy();
    management.Result.Error = ObjectManagementError::Busy;
    const auto running = destroy();
    ASTERIA_EXPECT(runner, "ObjectDestroy of a Thread returns its exit code, or WouldBlock while it runs",
                   ResponseIs(reclaimed, SystemCallError::None, 0x1234U, 0U),
                   management.LastDestroyedType == ObjectType::Thread, ResponseIs(running, SystemCallError::WouldBlock),
                   harness.AllScratchWasCleared());
}

// A CapabilitySpace's 32 zero bytes and its ObjectDestroy.
void TestCapabilitySpaceManagement(TestContext& runner) noexcept
{
    using Asteria::Kernel::System::ObjectManagementError;
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "space management fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x3333U}};
    FakeObjectManagement management;
    harness.SetObjectManagement(&management);
    const CapabilityHandle domain{0x0505U, 0xE0E0U};
    UInt8 wire[Asteria::Abi::ObjectCreateCapabilitySpaceRequestSizeBytes]{};
    const auto create = [&](const UInt64 size) noexcept
    {
        harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
        SystemCallRequest request =
            MakeIpcRequest(SystemCallNumber::ObjectCreate, domain, Asteria::Abi::ObjectTypeCapabilitySpace, 0U);
        request.Arguments[3U] = SendAddress;
        request.Arguments[4U] = size;
        return harness.Dispatch(fixture.GetSenderSpace(), request);
    };
    management.Result = {.Error = ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = CapabilityHandle{0x0A0AU, 0xF3F3U},
                         .Value = 0U};
    const auto created = create(sizeof(wire));
    const auto last = management.LastCreate;
    const auto wrongSize = create(sizeof(wire) + 8U);
    wire[sizeof(wire) - 1U] = 1U;
    const auto reserved = create(sizeof(wire));
    management.Result = {.Error = ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = {},
                         .Value = 0U};
    const auto destroyed =
        harness.Dispatch(fixture.GetSenderSpace(), MakeIpcRequest(SystemCallNumber::ObjectDestroy, domain,
                                                                  Asteria::Abi::ObjectTypeCapabilitySpace, 0U));
    ASTERIA_EXPECT(
        runner, "ObjectCreate of a CapabilitySpace takes 32 zero bytes and its ObjectDestroy destroys it",
        ResponseIs(created, SystemCallError::None, 0x0A0AU, 0xF3F3U), management.Creates == 1U,
        last.Type == ObjectType::CapabilitySpace, last.Authority.GetSelector() == 0x0505U,
        last.Authority.GetAuthenticator() == 0xE0E0U, ResponseIs(wrongSize, SystemCallError::InvalidArguments),
        ResponseIs(reserved, SystemCallError::InvalidArguments), ResponseIs(destroyed, SystemCallError::None),
        management.LastDestroyedType == ObjectType::CapabilitySpace, harness.AllScratchWasCleared());

    // An AddressSpace's block is the same 32 zero bytes.
    wire[sizeof(wire) - 1U] = 0U;
    harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
    SystemCallRequest addressSpace =
        MakeIpcRequest(SystemCallNumber::ObjectCreate, domain, Asteria::Abi::ObjectTypeAddressSpace, 0U);
    addressSpace.Arguments[3U] = SendAddress;
    addressSpace.Arguments[4U] = Asteria::Abi::ObjectCreateAddressSpaceRequestSizeBytes;
    management.Result = {.Error = ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = CapabilityHandle{0x0B0BU, 0xF4F4U},
                         .Value = 0U};
    const auto createdSpace = harness.Dispatch(fixture.GetSenderSpace(), addressSpace);
    const auto spaceRequest = management.LastCreate;
    management.Result = {.Error = ObjectManagementError::Busy,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = {},
                         .Value = 0U};
    const auto destroyedSpace =
        harness.Dispatch(fixture.GetSenderSpace(), MakeIpcRequest(SystemCallNumber::ObjectDestroy, domain,
                                                                  Asteria::Abi::ObjectTypeAddressSpace, 0U));
    ASTERIA_EXPECT(runner, "ObjectCreate of an AddressSpace takes 32 zero bytes and its ObjectDestroy waits",
                   ResponseIs(createdSpace, SystemCallError::None, 0x0B0BU, 0xF4F4U),
                   spaceRequest.Type == ObjectType::AddressSpace,
                   ResponseIs(destroyedSpace, SystemCallError::WouldBlock),
                   management.LastDestroyedType == ObjectType::AddressSpace, harness.AllScratchWasCleared());

    // A ProtectionDomain's 64 bytes: both spaces, a nonzero tag, a zero
    // billing domain.
    UInt8 domainBlock[Asteria::Abi::ObjectCreateProtectionDomainRequestSizeBytes]{};
    StoreWord(domainBlock, Asteria::Abi::ObjectCreateDomainAddressSpaceOffset, 0x0B0BU);
    StoreWord(domainBlock, Asteria::Abi::ObjectCreateDomainAddressSpaceOffset + 8U, 0xF4F4U);
    StoreWord(domainBlock, Asteria::Abi::ObjectCreateDomainCapabilitySpaceOffset, 0x0A0AU);
    StoreWord(domainBlock, Asteria::Abi::ObjectCreateDomainCapabilitySpaceOffset + 8U, 0xF3F3U);
    StoreWord(domainBlock, Asteria::Abi::ObjectCreateDomainSupervisionTagOffset, 0x7A67U);
    const auto createDomain = [&]() noexcept
    {
        harness.GetUserMemory().Store(SendAddress, domainBlock, sizeof(domainBlock));
        SystemCallRequest request =
            MakeIpcRequest(SystemCallNumber::ObjectCreate, domain, Asteria::Abi::ObjectTypeProtectionDomain, 0U);
        request.Arguments[3U] = SendAddress;
        request.Arguments[4U] = sizeof(domainBlock);
        return harness.Dispatch(fixture.GetSenderSpace(), request);
    };
    management.Result = {.Error = ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = CapabilityHandle{0x0C0CU, 0xF5F5U},
                         .Value = 0U};
    const UInt64 creates = management.Creates;
    const auto createdDomain = createDomain();
    const auto domainRequest = management.LastCreate;
    StoreWord(domainBlock, Asteria::Abi::ObjectCreateDomainSupervisionTagOffset, 0U);
    const auto untagged = createDomain();
    StoreWord(domainBlock, Asteria::Abi::ObjectCreateDomainSupervisionTagOffset, 0x7A67U);
    domainBlock[Asteria::Abi::ObjectCreateDomainBillingOffset] = 1U;
    const auto billed = createDomain();
    domainBlock[Asteria::Abi::ObjectCreateDomainBillingOffset] = 0U;
    domainBlock[sizeof(domainBlock) - 1U] = 1U;
    const auto reservedDomain = createDomain();
    ASTERIA_EXPECT(
        runner, "ObjectCreate of a ProtectionDomain decodes both spaces and the tag and refuses the rest",
        ResponseIs(createdDomain, SystemCallError::None, 0x0C0CU, 0xF5F5U),
        domainRequest.Type == ObjectType::ProtectionDomain, domainRequest.DomainAddressSpace.GetSelector() == 0x0B0BU,
        domainRequest.DomainAddressSpace.GetAuthenticator() == 0xF4F4U,
        domainRequest.DomainCapabilities.GetSelector() == 0x0A0AU,
        domainRequest.DomainCapabilities.GetAuthenticator() == 0xF3F3U, domainRequest.SupervisionTag == 0x7A67U,
        ResponseIs(untagged, SystemCallError::InvalidArguments), ResponseIs(billed, SystemCallError::InvalidArguments),
        ResponseIs(reservedDomain, SystemCallError::InvalidArguments), management.Creates == creates + 1U,
        harness.AllScratchWasCleared());
}

// MemoryMap takes FaultResolve's block in arguments 3 and 4.
void TestMemoryMap(TestContext& runner) noexcept
{
    using Asteria::Kernel::Memory::PagePermission;
    using Asteria::Kernel::System::ObjectManagementError;
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "MemoryMap fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x3333U}};
    FakeObjectManagement management;
    UInt8 wire[Asteria::Abi::FaultResolveRequestSizeBytes]{};
    StoreWord(wire, Asteria::Abi::FaultResolveAddressSpaceOffset, 0x0202U);
    StoreWord(wire, Asteria::Abi::FaultResolveAddressSpaceOffset + 8U, 0xB0B0U);
    StoreWord(wire, Asteria::Abi::FaultResolveMemoryObjectOffset, 0x0303U);
    StoreWord(wire, Asteria::Abi::FaultResolveMemoryObjectOffset + 8U, 0xC0C0U);
    StoreWord(wire, Asteria::Abi::FaultResolveFrameOffset, 0x0404U);
    StoreWord(wire, Asteria::Abi::FaultResolveFrameOffset + 8U, 0xD0D0U);
    StoreWord(wire, Asteria::Abi::FaultResolveVirtualStartOffset, 0x4C00'0000U);
    StoreWord(wire, Asteria::Abi::FaultResolvePageCountOffset, 2U);
    StoreWord(wire, Asteria::Abi::FaultResolveMemoryObjectPageOffset, 1U);
    StoreWord(wire, Asteria::Abi::FaultResolveFramePageOffset, 3U);
    wire[Asteria::Abi::FaultResolvePermissionsOffset] = 0x0BU;
    harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
    const auto map = [&](const UInt64 address, const UInt64 size) noexcept
    {
        SystemCallRequest request = MakeIpcRequest(SystemCallNumber::MemoryMap, CapabilityHandle{}, 0U, 0U);
        request.Arguments[3U] = address;
        request.Arguments[4U] = size;
        return harness.Dispatch(fixture.GetSenderSpace(), request);
    };

    const auto unconfigured = map(SendAddress, sizeof(wire));
    harness.SetObjectManagement(&management);
    management.Result = {.Error = ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = {},
                         .Value = 0U};
    const auto mapped = map(SendAddress, sizeof(wire));
    const auto& last = management.LastMap;
    ASTERIA_EXPECT(runner, "MemoryMap decodes the block and passes the caller's space",
                   ResponseIs(unconfigured, SystemCallError::InvalidConfiguration),
                   ResponseIs(mapped, SystemCallError::None, 0U, 0U), management.Maps == 1U,
                   management.Caller == &fixture.GetSenderSpace(), last.AddressSpace.GetSelector() == 0x0202U,
                   last.AddressSpace.GetAuthenticator() == 0xB0B0U, last.MemoryObject.GetSelector() == 0x0303U,
                   last.MemoryObject.GetAuthenticator() == 0xC0C0U, last.Frame.GetSelector() == 0x0404U,
                   last.Frame.GetAuthenticator() == 0xD0D0U, last.Mapping.VirtualStart.GetValue() == 0x4C00'0000U,
                   last.Mapping.Pages.GetValue() == 2U, last.Mapping.MemoryObjectOffsetPages == 1U,
                   last.Mapping.FrameOffsetPages == 3U, last.Mapping.Permissions.Contains(PagePermission::Read),
                   last.Mapping.Permissions.Contains(PagePermission::Write),
                   !last.Mapping.Permissions.Contains(PagePermission::Execute),
                   last.Mapping.Permissions.Contains(PagePermission::User));

    bool reservedRejected{true};
    constexpr Size ReservedArguments[]{0U, 1U, 2U, 5U};
    for (const Size argument : ReservedArguments)
    {
        SystemCallRequest reserved = MakeIpcRequest(SystemCallNumber::MemoryMap, CapabilityHandle{}, 0U, 0U);
        reserved.Arguments[3U] = SendAddress;
        reserved.Arguments[4U] = sizeof(wire);
        reserved.Arguments[argument] = 1U;
        reservedRejected = reservedRejected && ResponseIs(harness.Dispatch(fixture.GetSenderSpace(), reserved),
                                                          SystemCallError::InvalidArguments);
    }
    wire[Asteria::Abi::FaultResolvePermissionsOffset] = 0x13U;
    harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
    const auto undefinedPermission = map(SendAddress, sizeof(wire));
    wire[Asteria::Abi::FaultResolvePermissionsOffset] = 0x0BU;
    wire[Asteria::Abi::FaultResolveRequestSizeBytes - 1U] = 1U;
    harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
    const auto reservedByte = map(SendAddress, sizeof(wire));
    wire[Asteria::Abi::FaultResolveRequestSizeBytes - 1U] = 0U;
    harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
    ASTERIA_EXPECT(runner, "MemoryMap rejects a reserved argument, another size, a bad block or address",
                   reservedRejected, ResponseIs(map(SendAddress, sizeof(wire) - 1U), SystemCallError::InvalidArguments),
                   ResponseIs(map(0x1000U, sizeof(wire)), SystemCallError::UserMemoryFault),
                   ResponseIs(undefinedPermission, SystemCallError::InvalidArguments),
                   ResponseIs(reservedByte, SystemCallError::InvalidArguments), management.Maps == 1U);

    const auto answer = [&](const ObjectManagementError error, const CapabilityError capability) noexcept
    {
        management.Result = {.Error = error, .Capability = capability, .Reserved = {}, .Created = {}, .Value = 0U};
        return map(SendAddress, sizeof(wire));
    };
    ASTERIA_EXPECT(runner, "MemoryMap maps each management result onto the system-call errors",
                   ResponseIs(answer(ObjectManagementError::InvalidRequest, CapabilityError::Ok),
                              SystemCallError::InvalidArguments),
                   ResponseIs(answer(ObjectManagementError::CapabilityRejected, CapabilityError::RightsDenied),
                              SystemCallError::RightsDenied),
                   ResponseIs(answer(ObjectManagementError::ResourceExhausted, CapabilityError::Ok),
                              SystemCallError::ResourceExhausted),
                   ResponseIs(answer(ObjectManagementError::Busy, CapabilityError::Ok), SystemCallError::WouldBlock),
                   ResponseIs(answer(ObjectManagementError::NotConfigured, CapabilityError::Ok),
                              SystemCallError::InvalidConfiguration));
    const auto faulted = answer(ObjectManagementError::Faulted, CapabilityError::Ok);
    ASTERIA_EXPECT(
        runner, "MemoryMap reports a mapping the kernel could not complete as a violation", !faulted.HasValue(),
        !faulted.HasValue() && (faulted.GetError().Error == IpcSystemCallDispatchError::ObjectManagementViolation),
        harness.AllScratchWasCleared());
}

// MemoryUnmap names the space in arguments 0 and 1, the range in 2 and 3.
void TestMemoryUnmap(TestContext& runner) noexcept
{
    using Asteria::Kernel::System::ObjectManagementError;
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "MemoryUnmap fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x3333U}};
    FakeObjectManagement management;
    const CapabilityHandle space{0x0202U, 0xB0B0U};
    const auto unmap = [&](const UInt64 argument4, const UInt64 argument5) noexcept
    {
        SystemCallRequest request = MakeIpcRequest(SystemCallNumber::MemoryUnmap, space, 0x4C00'0000U, 2U);
        request.Arguments[3U] = 2U;
        request.Arguments[4U] = argument4;
        request.Arguments[5U] = argument5;
        return harness.Dispatch(fixture.GetSenderSpace(), request);
    };
    const auto unconfigured = unmap(0U, 0U);
    harness.SetObjectManagement(&management);
    management.Result = {.Error = ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = {},
                         .Value = 0U};
    const auto unmapped = unmap(0U, 0U);
    const auto reserved4 = unmap(1U, 0U);
    const auto reserved5 = unmap(0U, 1U);
    management.Result.Error = ObjectManagementError::InvalidRequest;
    const auto absent = unmap(0U, 0U);
    management.Result.Error = ObjectManagementError::Faulted;
    const auto faulted = unmap(0U, 0U);
    ASTERIA_EXPECT(runner, "MemoryUnmap passes the space and range and rejects reserved arguments",
                   ResponseIs(unconfigured, SystemCallError::InvalidConfiguration),
                   ResponseIs(unmapped, SystemCallError::None, 0U, 0U), management.Caller == &fixture.GetSenderSpace(),
                   management.LastUnmapSpace.GetSelector() == 0x0202U,
                   management.LastUnmapSpace.GetAuthenticator() == 0xB0B0U, management.LastUnmapStart == 0x4C00'0000U,
                   management.LastUnmapPages == 2U, ResponseIs(reserved4, SystemCallError::InvalidArguments),
                   ResponseIs(reserved5, SystemCallError::InvalidArguments),
                   ResponseIs(absent, SystemCallError::InvalidArguments), management.Unmaps == 3U, !faulted.HasValue(),
                   !faulted.HasValue() &&
                       (faulted.GetError().Error == IpcSystemCallDispatchError::ObjectManagementViolation),
                   harness.AllScratchWasCleared());
}

// ThreadTerminate names its thread in arguments 0 and 1; the rest are zero.
void TestThreadTerminate(TestContext& runner) noexcept
{
    using Asteria::Kernel::System::ObjectManagementError;
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "ThreadTerminate fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x3333U}};
    FakeObjectManagement management;
    const CapabilityHandle thread{0x0303U, 0xC0C0U};
    const auto terminate = [&](const Size reserved) noexcept
    {
        SystemCallRequest request = MakeIpcRequest(SystemCallNumber::ThreadTerminate, thread, 0U, 0U);
        if (reserved != 0U)
        {
            request.Arguments[reserved] = 1U;
        }
        return harness.Dispatch(fixture.GetSenderSpace(), request);
    };
    const auto unconfigured = terminate(0U);
    harness.SetObjectManagement(&management);
    management.Result = {.Error = ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = {},
                         .Value = 0U};
    const auto terminated = terminate(0U);
    const auto reserved2 = terminate(2U);
    const auto reserved5 = terminate(5U);
    management.Result.Error = ObjectManagementError::Busy;
    const auto blocked = terminate(0U);
    management.Result.Error = ObjectManagementError::InvalidRequest;
    const auto self = terminate(0U);
    management.Result.Error = ObjectManagementError::NotConfigured;
    const auto unsupported = terminate(0U);
    management.Result.Error = ObjectManagementError::Faulted;
    const auto faulted = terminate(0U);
    ASTERIA_EXPECT(
        runner, "ThreadTerminate passes its thread and maps the kill's outcomes",
        ResponseIs(unconfigured, SystemCallError::InvalidConfiguration),
        ResponseIs(terminated, SystemCallError::None, 0U, 0U), management.Caller == &fixture.GetSenderSpace(),
        management.LastTerminated.GetSelector() == 0x0303U, management.LastTerminated.GetAuthenticator() == 0xC0C0U,
        ResponseIs(reserved2, SystemCallError::InvalidArguments),
        ResponseIs(reserved5, SystemCallError::InvalidArguments), ResponseIs(blocked, SystemCallError::WouldBlock),
        ResponseIs(self, SystemCallError::InvalidArguments),
        ResponseIs(unsupported, SystemCallError::InvalidConfiguration), management.Terminations == 5U,
        !faulted.HasValue(),
        !faulted.HasValue() && (faulted.GetError().Error == IpcSystemCallDispatchError::ObjectManagementViolation),
        harness.AllScratchWasCleared());
}

// Resource accounting: a ResourceDomain's 56-byte block carries the
// Child's limits as seven words, decoded as they are.
void TestResourceDomainManagement(TestContext& runner) noexcept
{
    using Asteria::Kernel::System::ObjectManagementError;
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "domain management fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x3333U}};
    FakeObjectManagement management;
    harness.SetObjectManagement(&management);
    const CapabilityHandle domain{0x0505U, 0xE0E0U};
    UInt8 wire[Asteria::Abi::ObjectCreateResourceDomainRequestSizeBytes]{};
    StoreWord(wire, Asteria::Abi::ObjectCreateDomainCommittedPagesOffset, 16U);
    StoreWord(wire, Asteria::Abi::ObjectCreateDomainResidentPagesOffset, 12U);
    StoreWord(wire, Asteria::Abi::ObjectCreateDomainQueueBytesOffset, 4096U);
    StoreWord(wire, Asteria::Abi::ObjectCreateDomainThreadsOffset, 2U);
    StoreWord(wire, Asteria::Abi::ObjectCreateDomainChildrenOffset, 1U);
    const auto create = [&](const UInt64 size) noexcept
    {
        harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
        SystemCallRequest request =
            MakeIpcRequest(SystemCallNumber::ObjectCreate, domain, Asteria::Abi::ObjectTypeResourceDomain, 0U);
        request.Arguments[3U] = SendAddress;
        request.Arguments[4U] = size;
        return harness.Dispatch(fixture.GetSenderSpace(), request);
    };
    management.Result = {.Error = ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = CapabilityHandle{0x0B0BU, 0xF4F4U},
                         .Value = 0U};
    const auto created = create(sizeof(wire));
    const auto last = management.LastCreate;
    const auto wrongSize = create(sizeof(wire) - 8U);
    ASTERIA_EXPECT(runner, "ObjectCreate of a ResourceDomain decodes the child's seven limits",
                   ResponseIs(created, SystemCallError::None, 0x0B0BU, 0xF4F4U),
                   last.Type == ObjectType::ResourceDomain, last.Authority.GetSelector() == 0x0505U,
                   last.DomainLimits.CommittedMemoryPages == 16U, last.DomainLimits.ResidentMemoryPages == 12U,
                   last.DomainLimits.KernelMetadataBytes == 0U, last.DomainLimits.CapabilityHandles == 0U,
                   last.DomainLimits.IpcQueueBytes == 4096U, last.DomainLimits.Threads == 2U,
                   last.DomainLimits.ChildDomains == 1U, ResponseIs(wrongSize, SystemCallError::InvalidArguments),
                   management.Creates == 1U, harness.AllScratchWasCleared());
}

// Exact extended request lengths, reserved bytes and
// little-endian per-type limits.
void TestExtendedDomainRequests(TestContext& runner) noexcept
{
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "extended request fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x3333U}};
    FakeObjectManagement management;
    harness.SetObjectManagement(&management);
    management.Result = {.Error = Kernel::System::ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = CapabilityHandle{0x0B0BU, 0xF4F4U},
                         .Value = 0U};
    UInt8 wire[Abi::ObjectCreateResourceDomainArenaRequestSizeBytes]{};
    const auto create = [&](const UInt64 type, const Size size) noexcept
    {
        harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
        auto request = MakeIpcRequest(SystemCallNumber::ObjectCreate, CapabilityHandle{5U, 6U}, type, 0U);
        request.Arguments[3U] = SendAddress;
        request.Arguments[4U] = size;
        return harness.Dispatch(fixture.GetSenderSpace(), request);
    };
    for (Size size{0U}; size <= sizeof(wire) + 1U; ++size)
    {
        const auto result = create(Abi::ObjectTypeResourceDomain, size);
        const bool valid = size == Abi::ObjectCreateResourceDomainRequestSizeBytes ||
                           size == Abi::ObjectCreateResourceDomainExtendedRequestSizeBytes || size == sizeof(wire);
        ASTERIA_EXPECT(runner, "ResourceDomain accepts exactly 56, 80 and 88 bytes",
                       ResponseIs(result, valid ? SystemCallError::None : SystemCallError::InvalidArguments,
                                  valid ? 0x0B0BU : 0U, valid ? 0xF4F4U : 0U));
    }
    for (Size byte{82U}; byte < sizeof(wire); ++byte)
    {
        wire[byte] = 1U;
        ASTERIA_EXPECT(
            runner, "every trailing arena byte is reserved",
            ResponseIs(create(Abi::ObjectTypeResourceDomain, sizeof(wire)), SystemCallError::InvalidArguments));
        wire[byte] = 0U;
    }
    wire[0U] = Abi::ObjectCreateAddressSpaceFlagLargeArena;
    const auto flagged = create(Abi::ObjectTypeAddressSpace, Abi::ObjectCreateAddressSpaceRequestSizeBytes);
    ASTERIA_EXPECT(runner, "AddressSpace bit zero decodes the large arena request",
                   ResponseIs(flagged, SystemCallError::None, 0x0B0BU, 0xF4F4U),
                   management.LastCreate.LargePageTableArena);
    for (Size bits{2U}; bits < 256U; ++bits)
    {
        wire[0U] = static_cast<UInt8>(bits);
        ASTERIA_EXPECT(runner, "every other AddressSpace flag bit is invalid",
                       ResponseIs(create(Abi::ObjectTypeAddressSpace, Abi::ObjectCreateAddressSpaceRequestSizeBytes),
                                  SystemCallError::InvalidArguments));
    }
    wire[0U] = 1U;
    for (Size byte{1U}; byte < Abi::ObjectCreateAddressSpaceRequestSizeBytes; ++byte)
    {
        wire[byte] = 1U;
        ASTERIA_EXPECT(runner, "every other AddressSpace byte is reserved",
                       ResponseIs(create(Abi::ObjectTypeAddressSpace, Abi::ObjectCreateAddressSpaceRequestSizeBytes),
                                  SystemCallError::InvalidArguments));
        wire[byte] = 0U;
    }
    wire[0U] = 0U;
    StoreWord(wire, Abi::ObjectCreateDomainFlagsOffset, Abi::ObjectCreateDomainFlagCode);
    for (Size type{0U}; type < Abi::LimitedObjectCount; ++type)
    {
        wire[Abi::ObjectCreateDomainObjectLimitsOffset + 2U * type] = static_cast<UInt8>(type + 1U);
        wire[Abi::ObjectCreateDomainObjectLimitsOffset + 2U * type + 1U] = 1U;
    }
    const auto extended =
        create(Abi::ObjectTypeResourceDomain, Abi::ObjectCreateResourceDomainExtendedRequestSizeBytes);
    const auto last = management.LastCreate;
    bool limits{true};
    for (Size type{0U}; type < Abi::LimitedObjectCount; ++type)
    {
        limits = limits && last.DomainLimits.Objects.Counts[type] == 257U + type;
    }
    wire[Abi::ObjectCreateDomainLargeArenaLimitOffset] = 2U;
    const auto arena = create(Abi::ObjectTypeResourceDomain, sizeof(wire));
    const auto arenas = management.LastCreate.DomainLimits.Objects.LargePageTableArenas;
    wire[sizeof(wire) - 1U] = 1U;
    const auto reserved = create(Abi::ObjectTypeResourceDomain, sizeof(wire));
    wire[sizeof(wire) - 1U] = 0U;
    StoreWord(wire, Abi::ObjectCreateDomainFlagsOffset, 2U);
    const auto flags = create(Abi::ObjectTypeResourceDomain, Abi::ObjectCreateResourceDomainExtendedRequestSizeBytes);
    ASTERIA_EXPECT(
        runner, "80-byte limits and Code decode; only 88 bytes carry arenas and every reserved bit is refused",
        ResponseIs(extended, SystemCallError::None, 0x0B0BU, 0xF4F4U), limits, last.ExplicitObjectLimits,
        last.DomainFlags == Abi::ObjectCreateDomainFlagCode, last.DomainLimits.Objects.LargePageTableArenas == 0U,
        ResponseIs(arena, SystemCallError::None, 0x0B0BU, 0xF4F4U), arenas == 2U,
        ResponseIs(reserved, SystemCallError::InvalidArguments), ResponseIs(flags, SystemCallError::InvalidArguments),
        ResponseIs(create(Abi::ObjectTypeResourceDomain, 87U), SystemCallError::InvalidArguments));
    for (auto& byte : wire)
    {
        byte = 0U;
    }
    StoreWord(wire, Abi::ObjectCreateDomainAddressSpaceOffset, 7U);
    StoreWord(wire, Abi::ObjectCreateDomainAddressSpaceOffset + 8U, 8U);
    StoreWord(wire, Abi::ObjectCreateDomainCapabilitySpaceOffset, 9U);
    StoreWord(wire, Abi::ObjectCreateDomainCapabilitySpaceOffset + 8U, 10U);
    StoreWord(wire, Abi::ObjectCreateDomainSupervisionTagOffset, 11U);
    StoreWord(wire, Abi::ObjectCreateDomainFaultChannelOffset, 12U);
    StoreWord(wire, Abi::ObjectCreateDomainFaultChannelOffset + 8U, 13U);
    StoreWord(wire, Abi::ObjectCreateDomainChannelComponentKindOffset, Abi::ComponentBehaviorFaultSupervisor);
    const auto domain =
        create(Abi::ObjectTypeProtectionDomain, Abi::ObjectCreateProtectionDomainChannelRequestSizeBytes);
    const auto channel = management.LastCreate.DomainChannel;
    const auto diagnostic = management.LastCreate.DiagnosticComponentKind;
    StoreWord(wire, Abi::ObjectCreateDomainChannelComponentKindOffset, Abi::ComponentBehaviorMaximum + 1U);
    const auto tail = create(Abi::ObjectTypeProtectionDomain, Abi::ObjectCreateProtectionDomainChannelRequestSizeBytes);
    StoreWord(wire, Abi::ObjectCreateDomainChannelComponentKindOffset, 0U);
    wire[Abi::ObjectCreateDomainBillingOffset] = 1U;
    const auto billing =
        create(Abi::ObjectTypeProtectionDomain, Abi::ObjectCreateProtectionDomainChannelRequestSizeBytes);
    wire[Abi::ObjectCreateDomainBillingOffset] = 0U;
    StoreWord(wire, Abi::ObjectCreateDomainFaultChannelOffset, 0U);
    StoreWord(wire, Abi::ObjectCreateDomainFaultChannelOffset + 8U, 0U);
    const auto missing =
        create(Abi::ObjectTypeProtectionDomain, Abi::ObjectCreateProtectionDomainChannelRequestSizeBytes);
    ASTERIA_EXPECT(
        runner, "80-byte ProtectionDomain preserves its channel beside the bounded diagnostic kind and refuses billing",
        ResponseIs(domain, SystemCallError::None, 0x0B0BU, 0xF4F4U), channel.GetSelector() == 12U,
        channel.GetAuthenticator() == 13U, diagnostic == Abi::ComponentBehaviorFaultSupervisor,
        ResponseIs(tail, SystemCallError::InvalidArguments), ResponseIs(billing, SystemCallError::InvalidArguments),
        ResponseIs(missing, SystemCallError::InvalidArguments));
    StoreWord(wire, Abi::ObjectCreateDomainComponentKindOffset, Abi::ComponentBehaviorDisplay);
    const auto legacy = create(Abi::ObjectTypeProtectionDomain, Abi::ObjectCreateProtectionDomainRequestSizeBytes);
    ASTERIA_EXPECT(runner, "64-byte boot form retains its diagnostic word without naming a fault channel",
                   ResponseIs(legacy, SystemCallError::None, 0x0B0BU, 0xF4F4U),
                   management.LastCreate.DiagnosticComponentKind == Abi::ComponentBehaviorDisplay,
                   !management.LastCreate.DomainChannel.IsValid());
    for (auto& byte : wire)
    {
        byte = 0U;
    }
    const auto created = create(Abi::ObjectTypeFaultChannel, Abi::ObjectCreateFaultChannelRequestSizeBytes);
    const auto type = management.LastCreate.Type;
    wire[7U] = 1U;
    const auto bad = create(Abi::ObjectTypeFaultChannel, Abi::ObjectCreateFaultChannelRequestSizeBytes);
    ASTERIA_EXPECT(runner, "FaultChannel creation takes exactly eight zero bytes",
                   ResponseIs(created, SystemCallError::None, 0x0B0BU, 0xF4F4U), type == ObjectType::FaultChannel,
                   ResponseIs(bad, SystemCallError::InvalidArguments),
                   ResponseIs(create(Abi::ObjectTypeFaultChannel, 16U), SystemCallError::InvalidArguments),
                   harness.AllScratchWasCleared());
}

// WaitSetBind passes its WaitSet, Endpoint and tag, refuses a zero tag and a nonzero argument 5,
// and maps the outcomes.
void TestWaitSetBind(TestContext& runner) noexcept
{
    using Asteria::Kernel::System::ObjectManagementError;
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "WaitSetBind fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x3333U}};
    FakeObjectManagement management;
    const CapabilityHandle waitSet{0x0404U, 0xD0D0U};
    const auto bind = [&](const UInt64 tag, const UInt64 last) noexcept
    {
        SystemCallRequest request = MakeIpcRequest(SystemCallNumber::WaitSetBind, waitSet, 0x0505U, 0xE0E0U);
        request.Arguments[4U] = tag;
        request.Arguments[5U] = last;
        return harness.Dispatch(fixture.GetSenderSpace(), request);
    };
    const auto unconfigured = bind(7U, 0U);
    harness.SetObjectManagement(&management);
    management.Result = {.Error = ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = {},
                         .Value = 0U};
    const auto bound = bind(7U, 0U);
    const auto zeroTag = bind(0U, 0U);
    const auto reserved = bind(7U, 1U);
    management.Result.Error = ObjectManagementError::ResourceExhausted;
    const auto full = bind(7U, 0U);
    management.Result.Error = ObjectManagementError::InvalidRequest;
    const auto twice = bind(7U, 0U);
    ASTERIA_EXPECT(
        runner, "WaitSetBind passes its handles and tag and maps the binding's outcomes",
        ResponseIs(unconfigured, SystemCallError::InvalidConfiguration),
        ResponseIs(bound, SystemCallError::None, 0U, 0U), management.Caller == &fixture.GetSenderSpace(),
        management.LastWaitSet.GetSelector() == 0x0404U, management.LastWaitSet.GetAuthenticator() == 0xD0D0U,
        management.LastEndpoint.GetSelector() == 0x0505U, management.LastEndpoint.GetAuthenticator() == 0xE0E0U,
        management.LastTag == 7U, ResponseIs(zeroTag, SystemCallError::InvalidArguments),
        ResponseIs(reserved, SystemCallError::InvalidArguments), ResponseIs(full, SystemCallError::ResourceExhausted),
        ResponseIs(twice, SystemCallError::InvalidArguments), management.Binds == 3U, harness.AllScratchWasCleared());
}
// Supports 512-page session pools.
void TestRamFrameRequests(TestContext& runner) noexcept
{
    DispatcherFixture fixture;
    ASTERIA_EXPECT(runner, "RAM request fixture initializes", fixture.Initialize());
    DispatchHarness harness{fixture.GetAddressSpaceId(), ResourceDomainId{0x3333U}};
    FakeObjectManagement management;
    harness.SetObjectManagement(&management);
    management.Result = {.Error = Kernel::System::ObjectManagementError::None,
                         .Capability = CapabilityError::Ok,
                         .Reserved = {},
                         .Created = CapabilityHandle{0x0B0BU, 0xF4F4U},
                         .Value = 0U};
    UInt8 wire[Abi::ObjectCreateRamFrameRequestSizeBytes]{};
    StoreWord(wire, Abi::ObjectCreatePagesOffset, 512U);
    const auto create = [&](const Size size) noexcept
    {
        harness.GetUserMemory().Store(SendAddress, wire, sizeof(wire));
        auto request =
            MakeIpcRequest(SystemCallNumber::ObjectCreate, CapabilityHandle{5U, 6U}, Abi::ObjectTypeFrame, 0U);
        request.Arguments[3U] = SendAddress;
        request.Arguments[4U] = size;
        return harness.Dispatch(fixture.GetSenderSpace(), request);
    };
    for (UInt8 alignment{0U}; alignment <= 9U; ++alignment)
    {
        wire[8U] = alignment;
        constexpr UInt8 limits[]{0U, 32U};
        for (const UInt8 limit : limits)
        {
            wire[9U] = limit;
            ASTERIA_EXPECT(runner, "24-byte RAM form decodes every alignment and both limits",
                           ResponseIs(create(sizeof(wire)), SystemCallError::None, 0x0B0BU, 0xF4F4U),
                           management.LastCreate.Pages.GetValue() == 512U,
                           management.LastCreate.FrameAlignmentPages == (1U << alignment),
                           management.LastCreate.FrameAddressLimitBits == limit);
        }
    }
    wire[8U] = 0U;
    wire[9U] = 32U;
    for (Size byte{10U}; byte < sizeof(wire); ++byte)
    {
        wire[byte] = 1U;
        ASTERIA_EXPECT(runner, "each trailing RAM byte is reserved",
                       ResponseIs(create(sizeof(wire)), SystemCallError::InvalidArguments));
        wire[byte] = 0U;
    }
    wire[8U] = 10U;
    ASTERIA_EXPECT(runner, "RAM alignment is bounded",
                   ResponseIs(create(sizeof(wire)), SystemCallError::InvalidArguments));
    wire[8U] = 0U;
    wire[9U] = 31U;
    ASTERIA_EXPECT(runner, "RAM limit is zero or 32",
                   ResponseIs(create(sizeof(wire)), SystemCallError::InvalidArguments));
    wire[9U] = 32U;
    StoreWord(wire, 0U, 513U);
    ASTERIA_EXPECT(runner, "RAM pages are bounded",
                   ResponseIs(create(sizeof(wire)), SystemCallError::InvalidArguments));
    StoreWord(wire, 0U, 0U);
    ASTERIA_EXPECT(runner, "RAM pages are nonzero",
                   ResponseIs(create(sizeof(wire)), SystemCallError::InvalidArguments));
    StoreWord(wire, 0U, 129U);
    wire[9U] = 0U;
    ASTERIA_EXPECT(runner, "legacy RAM keeps its page bound",
                   ResponseIs(create(16U), SystemCallError::InvalidArguments));
    StoreWord(wire, 0U, 128U);
    wire[8U] = 32U;
    ASTERIA_EXPECT(runner, "legacy RAM keeps its alignment bound",
                   ResponseIs(create(16U), SystemCallError::InvalidArguments));
    wire[8U] = 16U;
    ASTERIA_EXPECT(runner, "legacy RAM keeps limit 32 and page-based alignment",
                   ResponseIs(create(16U), SystemCallError::None, 0x0B0BU, 0xF4F4U),
                   management.LastCreate.FrameAddressLimitBits == 32U, management.LastCreate.FrameAlignmentPages == 16U,
                   harness.AllScratchWasCleared());
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"IpcSystemCallDispatcherTests"};
    Asteria::Test::TestCanonicalRoundTripAndAttachment(runner);
    Asteria::Test::TestHandleAuthorizationFailures(runner);
    Asteria::Test::TestAuthorityHoldsWhereIpcTakesEffect(runner);
    Asteria::Test::TestMalformedInputAndProtocolFailure(runner);
    Asteria::Test::TestQueuePressureAndReservedControlBoundary(runner);
    Asteria::Test::TestReceiveRetryAndForeignSpace(runner);
    Asteria::Test::TestInvariantFailuresAfterQueuePreparation(runner);
    Asteria::Test::TestDebugWrite(runner);
    Asteria::Test::TestFaultResolve(runner);
    Asteria::Test::TestObjectManagement(runner);
    Asteria::Test::TestMemoryMap(runner);
    Asteria::Test::TestMemoryUnmap(runner);
    Asteria::Test::TestThreadTerminate(runner);
    Asteria::Test::TestWaitSetBind(runner);
    Asteria::Test::TestResourceDomainManagement(runner);
    Asteria::Test::TestExtendedDomainRequests(runner);
    Asteria::Test::TestRamFrameRequests(runner);
    Asteria::Test::TestThreadManagement(runner);
    Asteria::Test::TestInterruptControl(runner);
    Asteria::Test::TestIommuControl(runner);
    Asteria::Test::TestDeviceControl(runner);
    Asteria::Test::TestCapabilitySpaceManagement(runner);
    return runner.Finish();
}
