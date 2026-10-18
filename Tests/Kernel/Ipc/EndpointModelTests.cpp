// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

// Model-based test of Endpoint with its real collaborators against a reference
// model of EndpointQueue.md, IpcCapabilityTransfer.md, and IpcWireFormat.md.
// Three endpoints with boundary-biased configurations share a registry domain
// with four modelled capability spaces, a space without validation keys, and
// one from a foreign lock domain. The endpoints are registered objects, so
// sends and receives through an invoking capability are authorized where they
// take effect, also against revocations of the three domains. Each seeded step
// applies one weighted operation to both sides and compares every
// EndpointError field (in the documented precedence), sequence, reservation,
// delivered byte and handle, trusted metadata, clock sample, caller buffer,
// queue state, capability count, held and restored move sources, and residual
// frames and payloads in endpoint storage. ASTERIA_TEST_SEED replays a
// reported seed.
namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using Kernel::Ipc::CancellationState;
using Kernel::Ipc::CapabilityAttachment;
using Kernel::Ipc::CapabilityTransferMode;
using Kernel::Ipc::Endpoint;
using Kernel::Ipc::EndpointConfiguration;
using Kernel::Ipc::EndpointError;
using Kernel::Ipc::EndpointErrorCode;
using Kernel::Ipc::EndpointReceiveMetadata;
using Kernel::Ipc::EndpointReceiveReservation;
using Kernel::Ipc::EndpointSendClass;
using Kernel::Ipc::EndpointSendReservation;
using Kernel::Ipc::InterfaceIdentifier;
using Kernel::Ipc::MessageCodecError;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Ipc::MessageKind;
using Kernel::Ipc::VariablePayloadDescriptor;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationDomainId;
using TestSupport::ManualClock;
using TestSupport::Random;

// Documented wire and queue bounds; the model uses these, not the code's.
constexpr Size HeaderBytes{88U};
constexpr Size DescriptorBytes{8U};
constexpr Size AttachmentBytes{32U};
constexpr Size FrameCeiling{504U};
constexpr Size PayloadCeiling{256U};
constexpr Size DescriptorCeiling{4U};
constexpr Size AttachmentCeiling{4U};
constexpr Size QueueEntryCeiling{8U};
constexpr Size QueueByteCeiling{4'032U};
constexpr Size QueueAttachmentCeiling{32U};
static_assert((HeaderBytes == Kernel::Ipc::MessageCodec::WireHeaderSizeBytes) &&
              (FrameCeiling == Kernel::Ipc::MessageCodec::MaximumWireSizeBytes) &&
              (PayloadCeiling == MessageEnvelope::MaximumPayloadBytes) &&
              (AttachmentCeiling == MessageEnvelope::MaximumCapabilityAttachments) &&
              (QueueEntryCeiling == Endpoint::MaximumQueueEntries) &&
              (QueueByteCeiling == Endpoint::MaximumQueueBytes) &&
              (QueueAttachmentCeiling == Endpoint::MaximumQueuedAttachments));

// Spaces 0 to 3 share the endpoints' registry domain and are modelled slot by
// slot (0 and 1 receive for endpoints 0 and 1). Space 4 lacks validation keys;
// space 5 belongs to a foreign lock domain.
constexpr Size ModelledSpaces{4U};
constexpr Size BrokenSpace{4U};
constexpr Size ForeignSpace{5U};
constexpr Size SpaceCount{6U};
constexpr Size EndpointCount{3U};
constexpr Size ObjectCount{5U};
// Objects ObjectCount and up are the registered Endpoints themselves.
constexpr Size InvocableObjects{ObjectCount + EndpointCount};
constexpr Size DomainCount{3U};
constexpr Size SlotCount{CapabilitySpace::MaximumSlots};
constexpr Size PoolCapacity{384U};
constexpr Size RecentCapacity{8U};
constexpr Size InvocableHistory{8U};
constexpr Size TokenHistory{16U};
constexpr Size ConfigurationProbes{128U};
constexpr Size StepsPerSeed{3'000U};
constexpr UInt64 Infinite{~UInt64{0U}};
constexpr UInt16 KnownRights{0x3FFFU};
constexpr UInt16 InspectRight{0x0001U};
constexpr UInt16 InvokeRight{0x0002U};
constexpr UInt16 ReadRight{0x0004U};
constexpr UInt16 TransferRight{0x0080U};
constexpr UInt16 DuplicateRight{0x0100U};
constexpr UInt16 RevokeRight{0x0400U};
constexpr Size NoIndex{~Size{0U}};
constexpr UInt64 SenderDomains[]{0x5E11U, 0x5E22U, 0x5E33U, 0x5E44U};

// Scalar field widths, in declaration order, of the packed records mutated.
constexpr Size ConfigurationWidths[]{2U, 2U, 2U, 1U, 1U, 1U, 1U, 1U, 1U, 4U};
constexpr Size HeaderWidths[]{16U, 8U, 8U, 8U, 8U, 8U, 4U, 2U, 2U, 2U, 2U, 1U, 1U, 1U, 1U, 4U, 4U};
constexpr Size AttachmentWidths[]{16U, 8U, 2U, 1U, 1U, 4U};
constexpr Size DescriptorWidths[]{2U, 2U, 4U};

template <Size Count> [[nodiscard]] constexpr Size FieldOffset(const Size (&widths)[Count], const Size field) noexcept
{
    Size offset{0U};
    for (Size index{0U}; index < field; ++index)
    {
        offset += widths[index];
    }
    return offset;
}

static_assert((FieldOffset(ConfigurationWidths, 10U) == sizeof(EndpointConfiguration)) &&
              (FieldOffset(HeaderWidths, 17U) == offsetof(MessageEnvelope, Attachments)) &&
              (FieldOffset(AttachmentWidths, 6U) == sizeof(CapabilityAttachment)) &&
              (FieldOffset(DescriptorWidths, 3U) == sizeof(VariablePayloadDescriptor)));

// The first object-type encoding above every known type, computed once at
// compile time from the kernel's classification so new types need no change.
[[nodiscard]] consteval ObjectType FirstUnknownType() noexcept
{
    UInt8 highest{0U};
    for (UInt16 value{1U}; value <= 0xFFU; ++value)
    {
        const bool known = Kernel::Object::IsKnownObjectType(static_cast<ObjectType>(value));
        highest = known ? static_cast<UInt8>(value) : highest;
    }
    return static_cast<ObjectType>(highest + 1U);
}

class ModelledObject final : public Kernel::Object::IKernelObject
{
public:
    constexpr ModelledObject(const ObjectType type, const UInt64 owner) noexcept : m_type{type}, m_owner{owner}
    {
    }

    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return m_type;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return m_owner;
    }

private:
    ObjectType m_type;
    ResourceDomainId m_owner;
};

// Real system under test; the last storage slot holds configuration probes.
struct System
{
    Platform::Synchronization::SpinLock Lock;
    Asteria::TestSupport::TestObjectRegistry Registry{Lock};
    Kernel::Object::RevocationRegistry Revocations{Lock};
    Kernel::Object::RootCapabilityIssuer Issuer{Registry};
    ManualClock Clock{1'000U};
    Platform::Synchronization::SpinLock ForeignLock;
    Asteria::TestSupport::TestObjectRegistry ForeignRegistry{ForeignLock};
    Kernel::Object::RevocationRegistry ForeignRevocations{ForeignLock};
    CapabilitySpace Spaces[SpaceCount]{
        {Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0x0F1E'2D3C'4B5A'6978ULL},
        {Registry, Revocations, Clock, 0x7777'1111'2222'3333ULL, 0x9999'AAAA'BBBB'CCCCULL},
        {Registry, Revocations, Clock, 0x2468'ACE0'1357'9BDFULL, 0x1122'3344'5566'7788ULL},
        {Registry, Revocations, Clock, 0x0F0F'F0F0'3C3C'C3C3ULL, 0x5A5A'A5A5'6969'9696ULL},
        {Registry, Revocations, Clock, 0U, 0U},
        {ForeignRegistry, ForeignRevocations, Clock, 0x1357'2468'ACE0'BDF1ULL, 0x0102'0304'0506'0708ULL},
    };
    ModelledObject Objects[ObjectCount]{{ObjectType::MemoryObject, 10U},
                                        {ObjectType::Endpoint, 11U},
                                        {ObjectType::Notification, 12U},
                                        {ObjectType::Thread, 10U},
                                        {ObjectType::Frame, 11U}};
    alignas(Endpoint) unsigned char Storage[EndpointCount + 1U][sizeof(Endpoint)]{};
    Endpoint* Endpoints[EndpointCount]{};

    ~System() noexcept
    {
        for (Endpoint* const endpoint : Endpoints)
        {
            if (endpoint != nullptr)
            {
                endpoint->~Endpoint();
            }
        }
    }
};

// A handle and the space that issued it; Forged marks corrupted bytes.
struct IssuedHandle
{
    Size Space;
    CapabilityHandle Handle;
    bool Forged;
};

// Held marks a source moved by an unresolved PrepareSend: still occupied and
// counted, but no handle resolves to it until commit frees or cancel restores it.
struct SlotState
{
    bool Occupied;
    UInt64 Generation;
    Size Object;
    UInt16 Rights;
    Size Domain;
    UInt64 Deadline;
    UInt64 Sealed;
    bool Held;
    UInt32 Epoch;
};

struct ObjectState
{
    Kernel::Object::ObjectId Identity;
    ObjectType Type;
    UInt64 Owner;
    bool Registered;
};

// A queued or staged message with the receiver slot/generation of each handle.
struct MessageRecord
{
    MessageEnvelope Message;
    UInt64 Sequence;
    UInt64 Sender;
    UInt64 Sealed;
    UInt16 Rights;
    UInt16 FrameBytes;
    Size Attachments;
    EndpointSendClass SendClass;
    Size Slots[AttachmentCeiling];
    UInt64 Generations[AttachmentCeiling];
    Size HeldSpace;
    Size HeldSlots[AttachmentCeiling];
    Size HeldCount;
};

struct EndpointState
{
    bool Valid;
    Size Receiver;
    EndpointConfiguration Configuration;
    MessageRecord Queue[QueueEntryCeiling];
    Size Head;
    Size Count;
    Size Bytes;
    Size Attachments;
    UInt64 NextSequence;
    UInt64 NextSendReservation;
    UInt64 ActiveSend;
    MessageRecord Prepared;
    UInt64 NextReceiveReservation;
    UInt64 ActiveReceive;
    bool ReceiveByInvocation;
    IssuedHandle ReceiveInvocation;
};

struct TransferPrediction
{
    CapabilityError Error;
    bool ClockRead;
    Size SourceSlots[AttachmentCeiling];
    Size DestinationSlots[AttachmentCeiling];
};

template <typename TError> struct Check
{
    bool Failed;
    TError Error;
};

struct Field
{
    const char* Name;
    UInt64 Expected;
    UInt64 Actual;
};

template <typename TError, Size Count>
[[nodiscard]] TError FirstFailure(const Check<TError> (&checks)[Count], const TError success) noexcept
{
    for (const Check<TError>& check : checks)
    {
        if (check.Failed)
        {
            return check.Error;
        }
    }
    return success;
}

// Bucket of roll for ascending cumulative bounds.
template <Size Count> [[nodiscard]] constexpr Size Bucket(const UInt64 roll, const UInt64 (&bounds)[Count]) noexcept
{
    Size bucket{0U};
    for (const UInt64 bound : bounds)
    {
        bucket += (roll >= bound) ? 1U : 0U;
    }
    return bucket;
}

[[nodiscard]] constexpr EndpointError Failure(const EndpointErrorCode code,
                                              const CapabilityError capability = CapabilityError::Ok) noexcept
{
    return EndpointError{code, MessageCodecError::None, capability, 0U};
}

[[nodiscard]] constexpr Size Minimum(const Size left, const Size right) noexcept
{
    return (left < right) ? left : right;
}

[[nodiscard]] Kernel::Object::CapabilityRights RightsFrom(const UInt16 bits) noexcept
{
    Kernel::Object::CapabilityRights rights{};
    for (UInt16 bit{0U}; bit < 16U; ++bit)
    {
        if ((bits & (UInt16{1U} << bit)) != 0U)
        {
            rights = rights.With(static_cast<Kernel::Object::CapabilityRight>(UInt16{1U} << bit));
        }
    }
    return rights;
}

[[nodiscard]] constexpr Size FrameBytes(const MessageEnvelope& message) noexcept
{
    return HeaderBytes + (DescriptorBytes * message.VariablePayloadCount) +
           (AttachmentBytes * message.AttachmentCount) + message.PayloadSizeBytes;
}

template <typename TValue>
[[nodiscard]] EndpointError ErrorOf(const Foundation::Result<TValue, EndpointError>& result) noexcept
{
    return result.HasValue() ? EndpointError{} : result.GetError();
}

// Sets a scalar (at most 16 bytes, little-endian) to 0, ~0, +-1, 0..7, or random.
void MutateScalar(Random& random, UInt8* const bytes, const Size width) noexcept
{
    UInt64 words[2]{};
    std::memcpy(words, bytes, width);
    const UInt64 low[]{0U, ~UInt64{0U}, words[0] + 1U, words[0] - 1U, random.Below(8U), random.Next()};
    const UInt64 high[]{0U, ~UInt64{0U}, words[1], words[1], 0U, random.Next()};
    const UInt64 mode = random.Below(6U);
    words[0] = low[mode];
    words[1] = high[mode];
    std::memcpy(bytes, words, width);
}

// Reference envelope validation (IpcWireFormat.md) in the codec's order.
[[nodiscard]] MessageCodecError ExpectedCodecError(const MessageEnvelope& message) noexcept
{
    const UInt8 kind = static_cast<UInt8>(message.Kind);
    const bool requested = message.Cancellation == CancellationState::Requested;
    const bool traced = (message.Trace.TraceIdentifierHigh != 0U) || (message.Trace.TraceIdentifierLow != 0U);
    const bool emptyBody = (message.FixedPayloadSizeBytes == 0U) && (message.PayloadSizeBytes == 0U) &&
                           (message.VariablePayloadCount == 0U) && (message.AttachmentCount == 0U);
    const Check<MessageCodecError> header[]{
        {message.PayloadSizeBytes > PayloadCeiling, MessageCodecError::PayloadTooLarge},
        {message.FixedPayloadSizeBytes > message.PayloadSizeBytes, MessageCodecError::InvalidFixedPayloadSize},
        {message.VariablePayloadCount > DescriptorCeiling, MessageCodecError::TooManyVariablePayloads},
        {message.AttachmentCount > AttachmentCeiling, MessageCodecError::TooManyCapabilityAttachments},
        {(message.Reserved != 0U) || (message.ReservedAlignment != 0U), MessageCodecError::ReservedFieldNonzero},
        {!message.Interface.IsValid(), MessageCodecError::InvalidInterfaceIdentifier},
        {message.MethodIdentifier == 0U, MessageCodecError::InvalidMethodIdentifier},
        {message.InterfaceVersionMajor == 0U, MessageCodecError::InvalidInterfaceVersion},
        {message.TransactionIdentifier == 0U, MessageCodecError::InvalidTransactionIdentifier},
        {traced != (message.Trace.SpanIdentifier != 0U), MessageCodecError::InvalidTraceContext},
        {(kind == 0U) || (kind > 4U), MessageCodecError::InvalidMessageKind},
        {static_cast<UInt8>(message.Cancellation) > 2U, MessageCodecError::InvalidCancellationState},
        {(((kind == 1U) || (kind == 3U)) && requested) ||
             ((kind == 2U) && (message.Cancellation != CancellationState::None)) ||
             ((kind == 4U) && (!requested || !emptyBody)),
         MessageCodecError::InvalidMessageContract},
    };
    MessageCodecError error = FirstFailure(header, MessageCodecError::None);
    Size offset = message.FixedPayloadSizeBytes;
    for (Size index{0U}; (error == MessageCodecError::None) && (index < DescriptorCeiling); ++index)
    {
        const VariablePayloadDescriptor& descriptor = message.VariablePayloads[index];
        const bool used = index < message.VariablePayloadCount;
        const bool misplaced =
            (descriptor.OffsetBytes != offset) || (descriptor.SizeBytes > (message.PayloadSizeBytes - offset));
        const Check<MessageCodecError> checks[]{
            {!used && ((descriptor.OffsetBytes != 0U) || (descriptor.SizeBytes != 0U) || (descriptor.Reserved != 0U)),
             MessageCodecError::InvalidVariablePayloadLayout},
            {used && (descriptor.Reserved != 0U), MessageCodecError::ReservedFieldNonzero},
            {used && misplaced, MessageCodecError::InvalidVariablePayloadLayout},
        };
        error = FirstFailure(checks, MessageCodecError::None);
        offset += used ? descriptor.SizeBytes : 0U;
    }
    if ((error == MessageCodecError::None) && (offset != message.PayloadSizeBytes))
    {
        error = MessageCodecError::InvalidVariablePayloadLayout;
    }
    for (Size index{0U}; (error == MessageCodecError::None) && (index < AttachmentCeiling); ++index)
    {
        const CapabilityAttachment& attachment = message.Attachments[index];
        const UInt16 rights = attachment.RequestedRights.GetRawValue();
        const bool used = index < message.AttachmentCount;
        bool duplicate{false};
        for (Size previous{0U}; previous < index; ++previous)
        {
            duplicate =
                duplicate || (message.Attachments[previous].Handle.GetSelector() == attachment.Handle.GetSelector());
        }
        const bool unusedSet =
            (attachment.Handle.GetSelector() != 0U) || (attachment.Handle.GetAuthenticator() != 0U) ||
            (attachment.RequestedDeadline.GetTicks() != 0U) || (rights != 0U) ||
            (attachment.ExpectedType != ObjectType::Invalid) ||
            (attachment.TransferMode != CapabilityTransferMode::Invalid) || (attachment.Reserved != 0U);
        const bool knownMode = (attachment.TransferMode == CapabilityTransferMode::Move) ||
                               (attachment.TransferMode == CapabilityTransferMode::Copy);
        const Check<MessageCodecError> checks[]{
            {!used && unusedSet, MessageCodecError::InvalidMessageContract},
            {used && (attachment.Reserved != 0U), MessageCodecError::ReservedFieldNonzero},
            {used && (attachment.Handle.GetSelector() == 0U), MessageCodecError::InvalidAttachmentHandle},
            {used && (((rights & ~KnownRights) != 0U) || (rights == 0U)), MessageCodecError::InvalidAttachmentRights},
            {used && !Kernel::Object::IsKnownObjectType(attachment.ExpectedType),
             MessageCodecError::InvalidAttachmentObjectType},
            {used && !knownMode, MessageCodecError::InvalidAttachmentTransferMode},
            {used && duplicate, MessageCodecError::DuplicateAttachment},
        };
        error = FirstFailure(checks, MessageCodecError::None);
    }
    for (Size index{message.PayloadSizeBytes}; (error == MessageCodecError::None) && (index < PayloadCeiling); ++index)
    {
        error = (message.Payload[index] != 0U) ? MessageCodecError::InvalidMessageContract : error;
    }
    return error;
}

// Reference configuration validation; EndpointQueue.md states only the intent.
[[nodiscard]] bool ExpectedConfigurationValid(const EndpointConfiguration& configuration) noexcept
{
    const Size messages = configuration.MaximumQueuedMessages;
    const Size reservedMessages = configuration.ReservedControlMessages;
    const Size senderMessages = configuration.MaximumMessagesPerSender;
    const Size normalMessages = messages - reservedMessages;
    const Size bytes = configuration.MaximumQueuedBytes;
    const Size reservedBytes = configuration.ReservedControlBytes;
    const Size senderBytes = configuration.MaximumBytesPerSender;
    const Size attachments = configuration.MaximumQueuedAttachments;
    const Size reservedAttachments = configuration.ReservedControlAttachments;
    const Size senderAttachments = configuration.MaximumAttachmentsPerSender;
    return (configuration.Reserved == 0U) && (messages != 0U) && (messages <= QueueEntryCeiling) &&
           (reservedMessages < messages) && (senderMessages != 0U) && (senderMessages <= normalMessages) &&
           (bytes <= QueueByteCeiling) && (bytes >= (messages * HeaderBytes)) && (bytes <= (messages * FrameCeiling)) &&
           (reservedBytes <= bytes) && (reservedBytes >= (reservedMessages * HeaderBytes)) &&
           (reservedBytes <= (reservedMessages * FrameCeiling)) &&
           ((bytes - reservedBytes) >= (normalMessages * HeaderBytes)) &&
           ((bytes - reservedBytes) <= (normalMessages * FrameCeiling)) &&
           (senderBytes >= (senderMessages * HeaderBytes)) && (senderBytes <= (bytes - reservedBytes)) &&
           (senderBytes <= (senderMessages * FrameCeiling)) && (attachments <= QueueAttachmentCeiling) &&
           (attachments <= (messages * AttachmentCeiling)) && (reservedAttachments <= attachments) &&
           (reservedAttachments <= (reservedMessages * AttachmentCeiling)) &&
           ((attachments - reservedAttachments) <= (normalMessages * AttachmentCeiling)) &&
           (senderAttachments <= (senderMessages * AttachmentCeiling)) &&
           (senderAttachments <= (attachments - reservedAttachments));
}

// Offset of the first differing byte, or NoIndex; the envelope has no padding.
[[nodiscard]] Size FirstDifference(const MessageEnvelope& expected, const MessageEnvelope& actual) noexcept
{
    const UInt8* const left = static_cast<const UInt8*>(static_cast<const void*>(&expected));
    const UInt8* const right = static_cast<const UInt8*>(static_cast<const void*>(&actual));
    for (Size index{0U}; index < sizeof(MessageEnvelope); ++index)
    {
        if (left[index] != right[index])
        {
            return index;
        }
    }
    return NoIndex;
}

// Counts an eight-byte pattern anywhere in an endpoint. With every staging use
// scrubbed, "AIPC" v1 headers remain only in queued and prepared frames, and a
// delivered payload only in its frame while a prepared receive retains it.
[[nodiscard]] Size Occurrences(const Endpoint& endpoint, const UInt8* const pattern) noexcept
{
    const UInt8* const bytes = static_cast<const UInt8*>(static_cast<const void*>(&endpoint));
    Size found{0U};
    for (Size offset{0U}; (offset + 8U) <= sizeof(Endpoint); ++offset)
    {
        found += ((bytes[offset] == pattern[0U]) && (std::memcmp(bytes + offset, pattern, 8U) == 0)) ? 1U : 0U;
    }
    return found;
}

// Each endpoint's latest reservation token plus a cross-endpoint history, from
// which resolve operations draw live, stale, foreign, and default tokens.
template <typename TReservation> class TokenBook final
{
public:
    struct Entry
    {
        TReservation Token{};
        Size Endpoint{NoIndex};
    };

    void Remember(const Size endpoint, const TReservation token) noexcept
    {
        m_entries[endpoint] = Entry{token, endpoint};
        m_entries[EndpointCount + (m_count % TokenHistory)] = m_entries[endpoint];
        ++m_count;
    }

    [[nodiscard]] const Entry& Latest(const Size endpoint) const noexcept
    {
        return m_entries[endpoint];
    }

    [[nodiscard]] Entry Pick(Random& random, const Size endpoint) const noexcept
    {
        const Size other = (endpoint + 1U + static_cast<Size>(random.Below(EndpointCount - 1U))) % EndpointCount;
        const Entry choices[]{m_entries[endpoint], m_entries[EndpointCount + random.Below(TokenHistory)],
                              m_entries[other], Entry{TReservation{}, NoIndex}};
        return choices[Bucket(random.Below(10U), {6U, 8U, 9U})];
    }

private:
    Entry m_entries[EndpointCount + TokenHistory]{};
    Size m_count{0U};
};

using SendTokens = TokenBook<EndpointSendReservation>;
using ReceiveTokens = TokenBook<EndpointReceiveReservation>;

class Model final
{
public:
    UInt64 Now{1'000U};
    ObjectState Objects[InvocableObjects]{};
    RevocationDomainId Domains[DomainCount]{};
    UInt32 Epochs[DomainCount]{1U, 1U, 1U};
    EndpointState Endpoints[EndpointCount]{};
    SlotState Slots[ModelledSpaces][SlotCount]{};
    Size Active[SpaceCount]{};

    [[nodiscard]] bool Expired(const UInt64 deadline) const noexcept
    {
        return (deadline != Infinite) && (Now >= deadline);
    }

    // ResolveHandleLocked: an unforged handle of space naming a live, unheld generation.
    [[nodiscard]] Size Resolve(const Size space, const IssuedHandle& handle) const noexcept
    {
        const UInt64 generation = handle.Handle.GetSelector() >> 8U;
        const Size index = static_cast<Size>(handle.Handle.GetSelector() & 0xFFU);
        const bool authentic = (space < ModelledSpaces) && (handle.Space == space) && !handle.Forged;
        const bool current = authentic && (generation != 0U) && Slots[space][index].Occupied &&
                             !Slots[space][index].Held && (Slots[space][index].Generation == generation);
        return current ? index : NoIndex;
    }

    // AuthorizeInvocationLocked: Lookup's checks, then the exact Endpoint object.
    [[nodiscard]] CapabilityError Invocation(const Size space, const IssuedHandle& handle, const UInt16 required,
                                             const Size endpoint) const noexcept
    {
        Size index{NoIndex};
        const CapabilityError usable = Usable(space, handle, required, index);
        const Size object = (index == NoIndex) ? 0U : Slots[space][index].Object;
        const Check<CapabilityError> checks[]{
            {usable != CapabilityError::Ok, usable},
            {Objects[object].Type != ObjectType::Endpoint, CapabilityError::TypeMismatch},
            {object != (ObjectCount + endpoint), CapabilityError::InvalidArgument},
        };
        return FirstFailure(checks, CapabilityError::Ok);
    }

    // GetUsableDescriptorLocked order; the root domains are revoked but never
    // destroyed, and objects always stay current.
    [[nodiscard]] CapabilityError Usable(const Size space, const IssuedHandle& handle, const UInt16 required,
                                         Size& index) const noexcept
    {
        index = Resolve(space, handle);
        const SlotState& slot = Slots[Minimum(space, ModelledSpaces - 1U)][(index == NoIndex) ? 0U : index];
        const Check<CapabilityError> checks[]{
            {index == NoIndex, CapabilityError::InvalidHandle},
            {slot.Epoch != Epochs[slot.Domain], CapabilityError::Revoked},
            {Expired(slot.Deadline), CapabilityError::Expired},
            {(slot.Rights & required) != required, CapabilityError::RightsDenied},
        };
        return FirstFailure(checks, CapabilityError::Ok);
    }

    void Occupy(const Size space, const Size index, const SlotState& value) noexcept
    {
        const UInt64 generation = (Slots[space][index].Generation == 0U) ? 1U : Slots[space][index].Generation;
        Slots[space][index] = value;
        Slots[space][index].Occupied = true;
        Slots[space][index].Generation = generation;
        ++Active[space];
    }

    void Free(const Size space, const Size index) noexcept
    {
        const UInt64 generation = Slots[space][index].Generation + 1U;
        Slots[space][index] = SlotState{};
        Slots[space][index].Generation = generation;
        --Active[space];
    }

    // EndpointQueue.md: Normal pays every total limit minus its reservation plus
    // the sender's Normal quota; ReservedControl uses the full limits only.
    [[nodiscard]] static bool QuotaExceeded(const EndpointState& endpoint, const EndpointSendClass sendClass,
                                            const UInt64 sender, const Size frame, const Size attachments) noexcept
    {
        const EndpointConfiguration& limits = endpoint.Configuration;
        const bool normal = sendClass == EndpointSendClass::Normal;
        const Size messageLimit = limits.MaximumQueuedMessages - (normal ? limits.ReservedControlMessages : 0U);
        const Size byteLimit = limits.MaximumQueuedBytes - (normal ? limits.ReservedControlBytes : 0U);
        const Size attachmentLimit =
            limits.MaximumQueuedAttachments - (normal ? limits.ReservedControlAttachments : 0U);
        if ((endpoint.Count >= messageLimit) || ((endpoint.Bytes + frame) > byteLimit) ||
            ((endpoint.Attachments + attachments) > attachmentLimit))
        {
            return true;
        }
        Size usage[3]{};
        for (Size offset{0U}; normal && (offset < endpoint.Count); ++offset)
        {
            const MessageRecord& record = endpoint.Queue[(endpoint.Head + offset) % QueueEntryCeiling];
            const bool counted = (record.SendClass == EndpointSendClass::Normal) && (record.Sender == sender);
            usage[0] += counted ? 1U : 0U;
            usage[1] += counted ? record.FrameBytes : 0U;
            usage[2] += counted ? record.Attachments : 0U;
        }
        return normal &&
               ((usage[0] >= limits.MaximumMessagesPerSender) || ((usage[1] + frame) > limits.MaximumBytesPerSender) ||
                ((usage[2] + attachments) > limits.MaximumAttachmentsPerSender));
    }

    // IpcCapabilityTransfer.md and EndpointQueue.md: an empty transfer inspects
    // no source space and reads the clock only for a finite message deadline;
    // otherwise the source must share the lock domain, one clock sample (the
    // invocation's, if any) governs the deadline and every lease, the preflight
    // runs in attachment order, and destinations ascend. A released same-space
    // move may reuse its own source slot; a held one keeps it.
    [[nodiscard]] TransferPrediction PredictTransfer(const Size source, const Size destination,
                                                     const MessageEnvelope& message, const IssuedHandle* const sources,
                                                     const bool sampled, const bool hold) const noexcept
    {
        TransferPrediction prediction{CapabilityError::InvalidConfiguration, false, {}, {}};
        const Size count = message.AttachmentCount;
        const bool finite = message.Deadline.GetTicks() != Infinite;
        if (count == 0U)
        {
            prediction.ClockRead = !sampled && finite;
            prediction.Error = Expired(message.Deadline.GetTicks()) ? CapabilityError::Expired : CapabilityError::Ok;
            return prediction;
        }
        if ((source >= ModelledSpaces) || (destination >= ModelledSpaces))
        {
            return prediction;
        }
        prediction.ClockRead = !sampled;
        prediction.Error = Expired(message.Deadline.GetTicks()) ? CapabilityError::Expired : CapabilityError::Ok;
        for (Size index{0U}; (prediction.Error == CapabilityError::Ok) && (index < count); ++index)
        {
            const CapabilityAttachment& attachment = message.Attachments[index];
            const bool move = attachment.TransferMode == CapabilityTransferMode::Move;
            Size slot{NoIndex};
            const CapabilityError usable = Usable(source, sources[index], move ? TransferRight : DuplicateRight, slot);
            const SlotState& state = Slots[source][(slot == NoIndex) ? 0U : slot];
            const UInt16 rights = attachment.RequestedRights.GetRawValue();
            const UInt64 deadline = attachment.RequestedDeadline.GetTicks();
            bool repeated{false};
            for (Size previous{0U}; previous < index; ++previous)
            {
                repeated = repeated || (prediction.SourceSlots[previous] == slot);
            }
            const Check<CapabilityError> checks[]{
                {usable != CapabilityError::Ok, usable},
                {Objects[state.Object].Type != attachment.ExpectedType, CapabilityError::TypeMismatch},
                // A copy shares its source's revocation domain, so it may not
                // carry Revoke.
                {((rights & state.Rights) != rights) || (!move && ((rights & RevokeRight) != 0U)),
                 CapabilityError::RightsAmplification},
                {Expired(deadline), CapabilityError::Expired},
                {(state.Deadline != Infinite) && ((deadline == Infinite) || (deadline > state.Deadline)),
                 CapabilityError::LeaseExtension},
                {repeated, CapabilityError::InvalidArgument},
            };
            prediction.Error = FirstFailure(checks, CapabilityError::Ok);
            prediction.SourceSlots[index] = slot;
        }
        Size selected{0U};
        const bool placing = prediction.Error == CapabilityError::Ok;
        for (Size slot{0U}; placing && (slot < SlotCount) && (selected < count); ++slot)
        {
            bool available = !Slots[destination][slot].Occupied;
            for (Size request{0U}; (source == destination) && !hold && (request < count); ++request)
            {
                available = available || ((message.Attachments[request].TransferMode == CapabilityTransferMode::Move) &&
                                          (prediction.SourceSlots[request] == slot));
            }
            prediction.DestinationSlots[selected] = slot;
            selected += available ? 1U : 0U;
        }
        if ((prediction.Error == CapabilityError::Ok) && (selected != count))
        {
            prediction.Error = CapabilityError::SpaceExhausted;
        }
        return prediction;
    }

    // Move sources (distinct slots) are freed, or held for a prepared send,
    // before destinations are installed.
    void CommitTransfer(const Size source, const Size destination, const MessageEnvelope& message,
                        const TransferPrediction& prediction, const bool hold, MessageRecord& record) noexcept
    {
        SlotState values[AttachmentCeiling]{};
        record.HeldSpace = source;
        record.HeldCount = 0U;
        for (Size index{0U}; index < message.AttachmentCount; ++index)
        {
            values[index] = Slots[source][prediction.SourceSlots[index]];
            values[index].Rights = message.Attachments[index].RequestedRights.GetRawValue();
            values[index].Deadline = message.Attachments[index].RequestedDeadline.GetTicks();
            if (message.Attachments[index].TransferMode != CapabilityTransferMode::Move)
            {
                continue;
            }
            if (hold)
            {
                Slots[source][prediction.SourceSlots[index]].Held = true;
                record.HeldSlots[record.HeldCount] = prediction.SourceSlots[index];
                ++record.HeldCount;
            }
            else
            {
                Free(source, prediction.SourceSlots[index]);
            }
        }
        for (Size index{0U}; index < message.AttachmentCount; ++index)
        {
            record.Slots[index] = prediction.DestinationSlots[index];
            Occupy(destination, record.Slots[index], values[index]);
            record.Generations[index] = Slots[destination][record.Slots[index]].Generation;
        }
    }

    [[nodiscard]] bool PendingLive(const Size space, const MessageRecord& record) const noexcept
    {
        bool live{true};
        for (Size index{0U}; index < record.Attachments; ++index)
        {
            const SlotState& slot = Slots[space][record.Slots[index]];
            live = live && slot.Occupied && (slot.Generation == record.Generations[index]);
        }
        return live;
    }

    // Releases the pending handles a receiver has not already closed.
    void ReleasePending(const Size space, const MessageRecord& record) noexcept
    {
        for (Size index{0U}; index < record.Attachments; ++index)
        {
            const SlotState& slot = Slots[space][record.Slots[index]];
            if (slot.Occupied && (slot.Generation == record.Generations[index]))
            {
                Free(space, record.Slots[index]);
            }
        }
    }

    // A committed prepared send removes its held sources; a cancelled one
    // restores them under their unchanged handles.
    void ResolveHeld(const MessageRecord& record, const bool commit) noexcept
    {
        for (Size index{0U}; index < record.HeldCount; ++index)
        {
            Slots[record.HeldSpace][record.HeldSlots[index]].Held = false;
            if (commit)
            {
                Free(record.HeldSpace, record.HeldSlots[index]);
            }
        }
    }
};

class Driver final
{
public:
    Driver(System& system, Model& model, const UInt64 seed) noexcept
        : m_system{system}, m_model{model}, m_random{seed}, m_seed{seed}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        bool registered{true};
        for (Size index{0U}; index < ObjectCount; ++index)
        {
            const ModelledObject& object = m_system.Objects[index];
            const auto created = m_system.Issuer.CreateObject(m_system.Objects[index]);
            m_model.Objects[index] = ObjectState{created.GetValue(), object.GetObjectType(),
                                                 object.GetChargeOwner().GetValue(), created.HasValue()};
            registered = registered && created.HasValue();
        }
        for (RevocationDomainId& domain : m_model.Domains)
        {
            const auto created = m_system.Revocations.CreateRoot();
            domain = created.GetValue();
            registered = registered && created.HasValue();
        }
        for (UInt8& value : m_canary.Payload)
        {
            value = static_cast<UInt8>(m_random.Next());
        }
        // Receivers start empty, partly filled, or nearly full; a nearly full one
        // hoards (never closes) handles, so destination slots run out.
        for (Size space{0U}; registered && (space < ModelledSpaces); ++space)
        {
            constexpr Size FillBase[]{0U, 32U, 200U, 244U};
            constexpr Size FillSpan[]{1U, 64U, 40U, 10U};
            const Size level = (space < 2U) ? Bucket(m_random.Below(20U), {7U, 14U, 18U}) : 0U;
            const Size fill =
                FillBase[level] + static_cast<Size>(m_random.Below(FillSpan[level])) + ((space > 0U) ? 16U : 0U);
            m_hoarding[space] = level == 3U;
            for (Size count{0U}; registered && (count < fill); ++count)
            {
                registered = Mint(space);
            }
        }
        const Field setup[]{{"object, domain, and handle setup", 1U, registered ? 1U : 0U}};
        return SameFields("initialize", setup) && CreateEndpoints() && CheckState("initialize");
    }

    [[nodiscard]] bool Step(const Size step) noexcept
    {
        struct Operation
        {
            bool (Driver::*Run)() noexcept;
            UInt64 Weight[3];
        };
        // Weights per phase: filling, draining, and balanced queues.
        static constexpr Operation Operations[]{
            {&Driver::Send, {317U, 138U, 228U}},    {&Driver::Receive, {92U, 264U, 165U}},
            {&Driver::Resolve, {179U, 166U, 167U}}, {&Driver::Discard, {12U, 32U, 20U}},
            {&Driver::MintAny, {184U, 184U, 193U}}, {&Driver::Revoke, {2U, 2U, 2U}},
            {&Driver::Close, {126U, 126U, 132U}},   {&Driver::Advance, {88U, 88U, 93U}},
        };
        m_step = step;
        m_phase = m_random.Chance(1U, 120U) ? static_cast<Size>(m_random.Below(3U)) : m_phase;
        UInt64 roll = m_random.Below(1'000U);
        for (const Operation& operation : Operations)
        {
            if (roll < operation.Weight[m_phase])
            {
                return (this->*operation.Run)();
            }
            roll -= operation.Weight[m_phase];
        }
        return true;
    }

    // Leaves each endpoint quiescent (no reservation, no unread message).
    void Drain() noexcept
    {
        for (Size index{0U}; index < EndpointCount; ++index)
        {
            Endpoint* const endpoint = m_system.Endpoints[index];
            if ((endpoint == nullptr) || !endpoint->IsReady())
            {
                continue;
            }
            static_cast<void>(endpoint->CancelPreparedSend(m_sendTokens.Latest(index).Token));
            static_cast<void>(endpoint->CancelPreparedReceive(m_receiveTokens.Latest(index).Token));
            MessageEnvelope output{};
            for (Size message{0U}; !endpoint->DiscardUnread().HasValue() && (message < QueueEntryCeiling); ++message)
            {
                static_cast<void>(endpoint->Receive(output));
            }
        }
    }

private:
    [[nodiscard]] bool Mismatch(const char* const operation, const char* const detail, const UInt64 expected,
                                const UInt64 actual) const noexcept
    {
        static_cast<void>(
            std::fprintf(stderr, "MODEL MISMATCH seed=%llu step=%zu operation=%s: %s expected=%llu actual=%llu\n",
                         static_cast<unsigned long long>(m_seed), m_step, operation, detail,
                         static_cast<unsigned long long>(expected), static_cast<unsigned long long>(actual)));
        return false;
    }

    template <Size Count>
    [[nodiscard]] bool SameFields(const char* const operation, const Field (&fields)[Count]) const noexcept
    {
        for (const Field& field : fields)
        {
            if (field.Expected != field.Actual)
            {
                return Mismatch(operation, field.Name, field.Expected, field.Actual);
            }
        }
        return true;
    }

    // Compares every EndpointError field and the clock samples of one call.
    [[nodiscard]] bool SameOutcome(const char* const operation, const EndpointError& expected, const bool succeeded,
                                   const EndpointError& actual, const bool clockRead) noexcept
    {
        const EndpointErrorCode code = succeeded ? EndpointErrorCode::None : actual.Code;
        const Field fields[]{
            {"result", static_cast<UInt64>(expected.Code), static_cast<UInt64>(code)},
            {"failure without an error code", 0U, (!succeeded && (actual.Code == EndpointErrorCode::None)) ? 1U : 0U},
            {"codec error", static_cast<UInt64>(expected.Message), static_cast<UInt64>(actual.Message)},
            {"capability error", static_cast<UInt64>(expected.Capability), static_cast<UInt64>(actual.Capability)},
            {"error reserved byte", 0U, actual.Reserved},
            {"clock samples", clockRead ? 1U : 0U, m_system.Clock.TakeReadCount()},
        };
        return SameFields(operation, fields);
    }

    [[nodiscard]] bool SameQueueState(const char* const operation, const EndpointState& expected,
                                      const Kernel::Ipc::EndpointQueueState& actual) const noexcept
    {
        const UInt8 zero[sizeof(actual.Reserved)]{};
        const Field fields[]{
            {"queued messages", expected.Count, actual.QueuedMessages},
            {"queued bytes", expected.Bytes, actual.QueuedBytes},
            {"queued attachments", expected.Attachments, actual.QueuedAttachments},
            {"next sequence", expected.NextSequence, actual.NextSequence},
            {"queue state reserved bytes", 0U, (std::memcmp(actual.Reserved, zero, sizeof(zero)) == 0) ? 0U : 1U},
        };
        return SameFields(operation, fields);
    }

    [[nodiscard]] bool CheckState(const char* const operation) noexcept
    {
        bool same{true};
        for (Size index{0U}; same && (index < EndpointCount); ++index)
        {
            same = (m_system.Endpoints[index] == nullptr) ||
                   SameQueueState(operation, m_model.Endpoints[index], m_system.Endpoints[index]->GetQueueState());
        }
        for (Size space{0U}; same && (space < SpaceCount); ++space)
        {
            const Field count[]{
                {"active capabilities", m_model.Active[space], m_system.Spaces[space].GetActiveCount()}};
            same = SameFields(operation, count);
        }
        static_cast<void>(m_system.Clock.TakeReadCount());
        return same;
    }

    // Global state plus, for one operation in four, the endpoint's frames.
    [[nodiscard]] bool CheckEndpoint(const char* const operation, const Size index) noexcept
    {
        constexpr UInt8 FrameHeader[8]{0x41U, 0x49U, 0x50U, 0x43U, 0x01U, 0x00U, 0x58U, 0x00U};
        const EndpointState& endpoint = m_model.Endpoints[index];
        const Size expected = endpoint.Count + ((endpoint.ActiveSend != 0U) ? 1U : 0U);
        const Size actual = m_random.Chance(1U, 4U) ? Occurrences(*m_system.Endpoints[index], FrameHeader) : expected;
        const Field frames[]{{"resident frames", expected, actual}};
        return SameFields(operation, frames) && CheckState(operation);
    }

    // Pools a handle (evicting a random one when full); recent = just delivered.
    void Remember(const IssuedHandle& handle, const bool recent) noexcept
    {
        Size& count = m_poolCount[handle.Space];
        const Size victim = (count < PoolCapacity) ? count : static_cast<Size>(m_random.Below(PoolCapacity));
        m_pool[handle.Space][victim] = handle;
        count = Minimum(count + 1U, PoolCapacity);
        m_recent[m_recentCount % RecentCapacity] = recent ? handle : m_recent[m_recentCount % RecentCapacity];
        m_recentCount += recent ? 1U : 0U;
    }

    [[nodiscard]] IssuedHandle PickHandle(const Size space, const bool live) noexcept
    {
        IssuedHandle handle{space, CapabilityHandle{}, false};
        for (Size attempt{0U}; (m_poolCount[space] > 0U) && (attempt < (live ? 8U : 1U)); ++attempt)
        {
            handle = m_pool[space][m_random.Below(m_poolCount[space])];
            attempt = (m_model.Resolve(space, handle) == NoIndex) ? attempt : 8U;
        }
        return handle;
    }

    // Mostly a recent capability to the target Endpoint, preferring one that
    // authorizes the operation; else any handle, sometimes from another space.
    // Broken and foreign spaces have no pool.
    [[nodiscard]] IssuedHandle PickInvocation(const Size space, const Size endpoint, const UInt16 required) noexcept
    {
        const Size home = (space < ModelledSpaces) ? space : static_cast<Size>(m_random.Below(ModelledSpaces));
        const Size from = m_random.Chance(1U, 16U) ? static_cast<Size>(m_random.Below(ModelledSpaces)) : home;
        const Size known = Minimum(m_invocableCount[from][endpoint], InvocableHistory);
        for (Size attempt{0U}; (known != 0U) && (attempt < 4U) && !m_random.Chance(1U, 8U); ++attempt)
        {
            const IssuedHandle candidate = m_invocable[from][endpoint][m_random.Below(known)];
            if ((attempt == 3U) || (m_model.Invocation(from, candidate, required, endpoint) == CapabilityError::Ok))
            {
                return candidate;
            }
        }
        IssuedHandle handle = PickHandle(from, false);
        handle.Forged = m_random.Chance(1U, 25U);
        handle.Handle =
            CapabilityHandle{handle.Handle.GetSelector(), handle.Handle.GetAuthenticator() ^ (handle.Forged ? 1U : 0U)};
        return handle;
    }

    // A usable pool handle with Transfer or Duplicate not yet attached, or NoIndex.
    [[nodiscard]] Size PickTransferable(const Size space, const MessageEnvelope& message, const Size attached,
                                        IssuedHandle& handle) noexcept
    {
        for (Size attempt{0U}; attempt < 16U; ++attempt)
        {
            handle = PickHandle(space, false);
            Size index{NoIndex};
            bool usable = (m_model.Usable(space, handle, 0U, index) == CapabilityError::Ok) &&
                          ((m_model.Slots[space][index].Rights & (TransferRight | DuplicateRight)) != 0U);
            for (Size previous{0U}; previous < attached; ++previous)
            {
                usable = usable && (message.Attachments[previous].Handle.GetSelector() != handle.Handle.GetSelector());
            }
            if (usable)
            {
                return index;
            }
        }
        return NoIndex;
    }

    // Low or high with probability 1/3 each, else uniform in [low, high].
    [[nodiscard]] Size PickBounded(const Size low, const Size high) noexcept
    {
        const Size span = (high > low) ? (high - low + 1U) : 1U;
        const Size choices[]{low, high, low + static_cast<Size>(m_random.Below(span))};
        return (high > low) ? choices[m_random.Below(3U)] : low;
    }

    // Attachment budgets favour their maximum so attachments stay common.
    [[nodiscard]] Size PickAttachmentBudget(const Size high) noexcept
    {
        const Size choices[]{0U, static_cast<Size>(m_random.Below(high + 1U)), high};
        return choices[Bucket(m_random.Below(6U), {1U, 3U})];
    }

    // A configuration inside every documented bound, drawn with boundary bias.
    [[nodiscard]] EndpointConfiguration MakeValidConfiguration() noexcept
    {
        const Size messages = PickBounded(1U, QueueEntryCeiling);
        const Size reservedMessages = PickBounded(0U, messages - 1U);
        const Size normalMessages = messages - reservedMessages;
        const Size senderMessages = PickBounded(1U, normalMessages);
        const Size reservedBytes =
            PickBounded(reservedMessages * HeaderBytes,
                        Minimum(reservedMessages * FrameCeiling, QueueByteCeiling - (normalMessages * HeaderBytes)));
        const Size normalBytes = PickBounded(normalMessages * HeaderBytes,
                                             Minimum(normalMessages * FrameCeiling, QueueByteCeiling - reservedBytes));
        const Size senderBytes =
            PickBounded(senderMessages * HeaderBytes, Minimum(normalBytes, senderMessages * FrameCeiling));
        const Size reservedAttachments =
            PickAttachmentBudget(Minimum(reservedMessages * AttachmentCeiling, QueueAttachmentCeiling));
        const Size normalAttachments = PickAttachmentBudget(
            Minimum(normalMessages * AttachmentCeiling, QueueAttachmentCeiling - reservedAttachments));
        const Size senderAttachments =
            PickAttachmentBudget(Minimum(senderMessages * AttachmentCeiling, normalAttachments));
        return EndpointConfiguration{static_cast<UInt16>(reservedBytes + normalBytes),
                                     static_cast<UInt16>(reservedBytes),
                                     static_cast<UInt16>(senderBytes),
                                     static_cast<UInt8>(messages),
                                     static_cast<UInt8>(reservedMessages),
                                     static_cast<UInt8>(senderMessages),
                                     static_cast<UInt8>(reservedAttachments + normalAttachments),
                                     static_cast<UInt8>(reservedAttachments),
                                     static_cast<UInt8>(senderAttachments),
                                     0U};
    }

    // Endpoints 0 and 1 are valid (0 sometimes default) and receive into spaces 0
    // and 1. Endpoint 2 and the transient probes get zero to two field
    // mutations, sometimes no charge owner or an unready receiver space.
    [[nodiscard]] bool CreateEndpoints() noexcept
    {
        bool matched{true};
        for (Size index{0U}; matched && (index < (EndpointCount + ConfigurationProbes)); ++index)
        {
            const bool fixed = index < 2U;
            EndpointConfiguration configuration = MakeValidConfiguration();
            const bool standard = (index == 0U) && m_random.Chance(1U, 4U);
            configuration = standard ? EndpointConfiguration::Default() : configuration;
            for (UInt64 count{fixed ? 0U : m_random.Below(3U)}; count > 0U; --count)
            {
                const Size field = static_cast<Size>(m_random.Below(10U));
                UInt8* const bytes = static_cast<UInt8*>(static_cast<void*>(&configuration));
                MutateScalar(m_random, bytes + FieldOffset(ConfigurationWidths, field), ConfigurationWidths[field]);
            }
            const UInt64 owner = (!fixed && m_random.Chance(1U, 16U)) ? 0U : 0xE0U + index;
            const Size unfixed = static_cast<Size>(m_random.Below(ModelledSpaces));
            const Size receiver = fixed ? index : (m_random.Chance(1U, 16U) ? BrokenSpace : unfixed);
            Endpoint* const endpoint = new (m_system.Storage[Minimum(index, EndpointCount)])
                Endpoint{m_system.Spaces[receiver], ResourceDomainId{owner}, configuration};
            const bool valid = ExpectedConfigurationValid(configuration) && (owner != 0U) && (receiver != BrokenSpace);
            UInt64 identity{0U};
            for (Size space{0U}; space < SpaceCount; ++space)
            {
                identity |= endpoint->HasReceiverCapabilitySpace(m_system.Spaces[space]) ? (UInt64{1U} << space) : 0U;
            }
            // The three Endpoints are registered objects so that capabilities
            // can invoke them; registration needs a valid charge owner.
            const bool registering = index < EndpointCount;
            const auto registered =
                registering
                    ? m_system.Issuer.CreateObject(*endpoint)
                    : Foundation::Result<Kernel::Object::ObjectId, Kernel::Object::ObjectRegistryError>::Failure({});
            const Field fields[]{
                {"ready", valid ? 1U : 0U, endpoint->IsReady() ? 1U : 0U},
                {"object type", static_cast<UInt64>(ObjectType::Endpoint),
                 static_cast<UInt64>(endpoint->GetObjectType())},
                {"charge owner", owner, endpoint->GetChargeOwner().GetValue()},
                {"receiver space identity", UInt64{1U} << receiver, identity},
                {"registered", (registering && (owner != 0U)) ? 1U : 0U, registered.HasValue() ? 1U : 0U},
            };
            matched = SameFields(registering ? "create endpoint" : "configuration probe", fields);
            if (!registering)
            {
                endpoint->~Endpoint();
                continue;
            }
            m_system.Endpoints[index] = endpoint;
            m_model.Objects[ObjectCount + index] =
                ObjectState{registered.HasValue() ? registered.GetValue() : Kernel::Object::ObjectId{},
                            ObjectType::Endpoint, owner, registered.HasValue()};
            EndpointState& state = m_model.Endpoints[index];
            state.Valid = valid;
            state.Receiver = receiver;
            state.Configuration = configuration;
            state.NextSequence = 1U;
            state.NextSendReservation = 1U;
            state.NextReceiveReservation = 1U;
        }
        return matched;
    }

    // Payload, descriptors tiling [fixed, payload), and attachments; one body in
    // ten is minimal (88 bytes) and one in fifty maximal (504 bytes).
    void FillBody(MessageEnvelope& message, const Size source, IssuedHandle* const sources) noexcept
    {
        constexpr Size PayloadBase[]{0U, 0U, 0U, 128U};
        constexpr Size PayloadSpan[]{1U, 17U, 129U, 129U};
        const Size tier = Bucket(m_random.Below(10U), {2U, 5U, 8U});
        const Size shape = Bucket(m_random.Below(50U), {5U, 6U});
        const Size randomPayload = PayloadBase[tier] + static_cast<Size>(m_random.Below(PayloadSpan[tier]));
        const Size payloads[]{0U, PayloadCeiling, randomPayload};
        const Size descriptorCounts[]{0U, DescriptorCeiling, static_cast<Size>(m_random.Below(DescriptorCeiling + 1U))};
        const Size attachmentCounts[]{0U, AttachmentCeiling, Bucket(m_random.Below(100U), {38U, 63U, 78U, 90U})};
        const Size payload = payloads[shape];
        const Size descriptors = descriptorCounts[shape];
        const Size fixed = (descriptors == 0U) ? payload : static_cast<Size>(m_random.Below(payload + 1U));
        Size cursor = fixed;
        for (Size index{0U}; index < descriptors; ++index)
        {
            const Size span = static_cast<Size>(m_random.Below(payload - cursor + 1U));
            const Size size = ((index + 1U) == descriptors) ? payload - cursor : span;
            message.VariablePayloads[index] =
                VariablePayloadDescriptor{static_cast<UInt16>(cursor), static_cast<UInt16>(size), 0U};
            cursor += size;
        }
        message.PayloadSizeBytes = static_cast<UInt16>(payload);
        message.FixedPayloadSizeBytes = static_cast<UInt16>(fixed);
        message.VariablePayloadCount = static_cast<UInt8>(descriptors);
        message.AttachmentCount = static_cast<UInt8>(attachmentCounts[shape]);
        for (Size index{0U}; index < payload; ++index)
        {
            message.Payload[index] = static_cast<UInt8>(m_random.Next());
        }
        for (Size index{0U}; index < message.AttachmentCount; ++index)
        {
            FillAttachment(source, message, index, sources);
        }
    }

    // Mostly an authorized attachment; else a repeated, stale, or cross-space
    // handle, or one flaw: expired or extending lease, amplified rights, wrong
    // known type, or the first unknown type encoding.
    void FillAttachment(const Size source, MessageEnvelope& message, const Size index,
                        IssuedHandle* const sources) noexcept
    {
        CapabilityAttachment& attachment = message.Attachments[index];
        if ((index > 0U) && m_random.Chance(1U, 16U))
        {
            sources[index] = sources[index - 1U];
            attachment = message.Attachments[index - 1U];
            return;
        }
        const Size home = (source < ModelledSpaces) ? source : static_cast<Size>(m_random.Below(ModelledSpaces));
        const Size from = m_random.Chance(1U, 16U) ? static_cast<Size>(m_random.Below(ModelledSpaces)) : home;
        IssuedHandle handle{};
        Size slot = PickTransferable(from, message, index, handle);
        if ((slot == NoIndex) || m_random.Chance(1U, 10U))
        {
            handle = PickHandle(from, false);
            slot = m_model.Resolve(from, handle);
        }
        const SlotState base = (slot == NoIndex) ? SlotState{false, 0U, 0U, KnownRights, 0U, Infinite, 0U, false, 1U}
                                                 : m_model.Slots[from][slot];
        const UInt16 subset = static_cast<UInt16>(base.Rights & m_random.Next());
        const UInt16 rights = (subset == 0U) ? base.Rights : subset;
        const UInt64 remaining = (base.Deadline == Infinite) ? 500U : base.Deadline - m_model.Now;
        const bool attenuate = !m_model.Expired(base.Deadline) && m_random.Chance(1U, 2U);
        const UInt64 attenuated = m_model.Now + 1U + m_random.Below(m_model.Expired(base.Deadline) ? 1U : remaining);
        const UInt64 flaw = m_random.Below(30U);
        const UInt64 deadlines[]{m_model.Now, Infinite, attenuate ? attenuated : base.Deadline};
        const ObjectType type = m_model.Objects[base.Object].Type;
        const ObjectType wrong = m_model.Objects[(base.Object + 1U) % ObjectCount].Type;
        const ObjectType types[]{type, wrong, FirstUnknownType(), type};
        const UInt16 amplified = static_cast<UInt16>(rights | (UInt16{1U} << m_random.Below(14U)));
        const bool move = ((base.Rights & TransferRight) != 0U) &&
                          (((base.Rights & DuplicateRight) == 0U) || m_random.Chance(1U, 2U));
        attachment = CapabilityAttachment{handle.Handle,
                                          MonotonicTime{deadlines[Minimum(flaw, 2U)]},
                                          RightsFrom((flaw == 2U) ? amplified : rights),
                                          types[Bucket(flaw, {3U, 4U, 5U})],
                                          move ? CapabilityTransferMode::Move : CapabilityTransferMode::Copy,
                                          0U};
        sources[index] = handle;
    }

    // Mutates one envelope field; a changed handle counts as forged.
    void MutateField(MessageEnvelope& message, IssuedHandle* const sources) noexcept
    {
        const Size header = static_cast<Size>(m_random.Below(17U));
        const Size attachment = static_cast<Size>(m_random.Below(6U));
        const Size descriptor = static_cast<Size>(m_random.Below(3U));
        const Size offsets[]{FieldOffset(HeaderWidths, header),
                             offsetof(MessageEnvelope, Attachments) +
                                 (m_random.Below(AttachmentCeiling) * sizeof(CapabilityAttachment)) +
                                 FieldOffset(AttachmentWidths, attachment),
                             offsetof(MessageEnvelope, VariablePayloads) +
                                 (m_random.Below(DescriptorCeiling) * sizeof(VariablePayloadDescriptor)) +
                                 FieldOffset(DescriptorWidths, descriptor),
                             offsetof(MessageEnvelope, Payload) + m_random.Below(PayloadCeiling)};
        const Size widths[]{HeaderWidths[header], AttachmentWidths[attachment], DescriptorWidths[descriptor], 1U};
        const Size area = Bucket(m_random.Below(20U), {10U, 15U, 19U});
        MutateScalar(m_random, static_cast<UInt8*>(static_cast<void*>(&message)) + offsets[area], widths[area]);
        for (Size index{0U}; index < AttachmentCeiling; ++index)
        {
            const CapabilityHandle now = message.Attachments[index].Handle;
            const CapabilityHandle before = sources[index].Handle;
            sources[index].Forged = sources[index].Forged || (now.GetSelector() != before.GetSelector()) ||
                                    (now.GetAuthenticator() != before.GetAuthenticator());
        }
    }

    void BuildMessage(const Size source, MessageEnvelope& message, IssuedHandle* const sources) noexcept
    {
        constexpr MessageKind Kinds[]{MessageKind::Cancellation, MessageKind::SynchronousCall, MessageKind::Reply,
                                      MessageKind::Asynchronous};
        const UInt64 deadlines[]{Infinite, m_model.Now + 1U + m_random.Below(300U), m_model.Now + 1U,
                                 m_model.Now - m_random.Below(3U)};
        ++m_transaction;
        message.Interface = InterfaceIdentifier{m_random.Next() | 1U, m_random.Next()};
        message.TransactionIdentifier = m_transaction;
        message.Deadline = MonotonicTime{deadlines[Bucket(m_random.Below(20U), {15U, 18U, 19U})]};
        message.MethodIdentifier = static_cast<UInt32>(1U + m_random.Below(4'096U));
        message.InterfaceVersionMajor = static_cast<UInt16>(1U + m_random.Below(4U));
        message.InterfaceVersionMinor = static_cast<UInt16>(m_random.Below(8U));
        message.Kind = Kinds[Bucket(m_random.Below(20U), {2U, 8U, 11U})];
        if (m_random.Chance(1U, 2U))
        {
            message.Trace = Kernel::Ipc::TraceContext{m_random.Next(), m_random.Next() | 1U, m_random.Next() | 1U};
        }
        const bool cancellable = (message.Kind != MessageKind::Reply) && m_random.Chance(2U, 5U);
        message.Cancellation = cancellable ? CancellationState::Cancellable : CancellationState::None;
        if (message.Kind == MessageKind::Cancellation)
        {
            message.Cancellation = CancellationState::Requested;
        }
        else
        {
            FillBody(message, source, sources);
        }
        if (m_random.Chance(1U, 6U))
        {
            MutateField(message, sources);
        }
    }

    // Metadata and the caller's envelope with predicted receiver handles.
    [[nodiscard]] bool CompareDelivery(const char* const operation, const MessageRecord& record,
                                       const EndpointReceiveMetadata& metadata,
                                       const MessageEnvelope* const output) const noexcept
    {
        MessageEnvelope expected = record.Message;
        for (Size index{0U}; (output != nullptr) && (index < record.Attachments); ++index)
        {
            const UInt64 selector = (record.Generations[index] << 8U) | record.Slots[index];
            expected.Attachments[index].Handle =
                CapabilityHandle{selector, output->Attachments[index].Handle.GetAuthenticator()};
        }
        const Field fields[]{
            {"delivered sequence", record.Sequence, metadata.Sequence},
            {"delivered sender", record.Sender, metadata.Sender.GetValue()},
            {"delivered sealed selector", record.Sealed, metadata.SenderEndpointSealedObjectSelector},
            {"delivered sender rights", record.Rights, metadata.SenderEndpointRights.GetRawValue()},
            {"delivered frame size", record.FrameBytes, metadata.FrameSizeBytes},
            {"delivered attachment count", record.Attachments, metadata.AttachmentCount},
            {"delivered send class", static_cast<UInt64>(record.SendClass), static_cast<UInt64>(metadata.SendClass)},
            {"first differing delivered byte", NoIndex,
             (output != nullptr) ? FirstDifference(expected, *output) : NoIndex},
        };
        return SameFields(operation, fields);
    }

    // Pools each delivered handle (a prepared head stays queued, so closing one
    // fails a later discard) and queries it: the descriptor must match.
    [[nodiscard]] bool VerifyDelivered(const char* const operation, const Size space, const MessageEnvelope& output,
                                       const Size attachments) noexcept
    {
        bool verified{true};
        for (Size attachment{0U}; verified && (attachment < attachments); ++attachment)
        {
            const IssuedHandle handle{space, output.Attachments[attachment].Handle, false};
            Remember(handle, true);
            Size index{NoIndex};
            const CapabilityError expected = m_model.Usable(space, handle, InspectRight, index);
            const auto queried = m_system.Spaces[space].Query(handle.Handle);
            const CapabilityError actual = queried.HasValue() ? CapabilityError::Ok : queried.GetError();
            const Field result[]{{"query result", static_cast<UInt64>(expected), static_cast<UInt64>(actual)}};
            verified = SameFields(operation, result);
            if (!verified || !queried.HasValue())
            {
                continue;
            }
            const SlotState& slot = m_model.Slots[space][index];
            const ObjectState& object = m_model.Objects[slot.Object];
            const Kernel::Object::CapabilityDescriptor& descriptor = queried.GetValue();
            const Field fields[]{
                {"query object", object.Identity.GetValue(), descriptor.Object.GetValue()},
                {"query type", static_cast<UInt64>(object.Type), static_cast<UInt64>(descriptor.Type)},
                {"query rights", slot.Rights, descriptor.Rights.GetRawValue()},
                {"query deadline", slot.Deadline, descriptor.LeaseDeadline.GetTicks()},
                {"query domain", m_model.Domains[slot.Domain].GetValue(), descriptor.RevocationDomain.GetValue()},
                {"query charge owner", object.Owner, descriptor.ChargeOwner.GetValue()},
                {"query sealed selector", slot.Sealed, descriptor.SealedObjectSelector},
            };
            verified = SameFields(operation, fields);
        }
        return verified;
    }

    [[nodiscard]] bool Enqueue(const char* const operation, EndpointState& endpoint, MessageRecord& record,
                               const UInt64 sequence) noexcept
    {
        const Field fields[]{
            {"sequence", endpoint.NextSequence, sequence},
            {"model queue capacity", 0U, (endpoint.Count >= QueueEntryCeiling) ? 1U : 0U},
        };
        if (!SameFields(operation, fields))
        {
            return false;
        }
        record.Sequence = sequence;
        endpoint.Queue[(endpoint.Head + endpoint.Count) % QueueEntryCeiling] = record;
        ++endpoint.Count;
        endpoint.Bytes += record.FrameBytes;
        endpoint.Attachments += record.Attachments;
        ++endpoint.NextSequence;
        return true;
    }

    static void PopHead(EndpointState& endpoint) noexcept
    {
        endpoint.Bytes -= endpoint.Queue[endpoint.Head].FrameBytes;
        endpoint.Attachments -= endpoint.Queue[endpoint.Head].Attachments;
        endpoint.Head = (endpoint.Head + 1U) % QueueEntryCeiling;
        --endpoint.Count;
    }

    // The three send APIs, in EndpointQueue.md's precedence: configuration,
    // arguments, invocation authority, active send reservation, codec, queue
    // budget, capability transaction. An invocation authorizes in a source
    // space of the lock domain with one clock sample that the transaction
    // reuses; a prepared send holds its moved sources.
    [[nodiscard]] bool Send() noexcept
    {
        constexpr const char* Names[]{"send", "send from invocation", "prepare send"};
        const Size api = Bucket(m_random.Below(69U), {36U, 56U});
        const bool invoked = api == 1U;
        const bool prepare = api == 2U;
        const Size index = Bucket(m_random.Below(100U), {50U, 85U});
        EndpointState& endpoint = m_model.Endpoints[index];
        const Size spaces[]{BrokenSpace, ForeignSpace, endpoint.Receiver, 1U + static_cast<Size>(m_random.Below(3U))};
        const Size source = spaces[Bucket(m_random.Below(100U), {3U, 5U, 20U})];
        const UInt64 sender = m_random.Chance(1U, 50U) ? 0U : SenderDomains[m_random.Below(4U)];
        const EndpointSendClass classes[]{
            EndpointSendClass::Normal, EndpointSendClass::ReservedControl, EndpointSendClass::Invalid,
            static_cast<EndpointSendClass>(static_cast<UInt8>(3U + m_random.Below(253U)))};
        const EndpointSendClass sendClass = classes[Bucket(m_random.Below(40U), {29U, 38U, 39U})];
        const IssuedHandle invocation = invoked ? PickInvocation(source, index, InvokeRight) : IssuedHandle{};
        MessageEnvelope message{};
        IssuedHandle sources[AttachmentCeiling]{};
        BuildMessage(source, message, sources);
        const MessageEnvelope original = message;
        const MessageCodecError codec = ExpectedCodecError(message);
        const bool arguments = (sender != 0U) && ((sendClass == EndpointSendClass::Normal) ||
                                                  (sendClass == EndpointSendClass::ReservedControl));
        const bool sampled = invoked && endpoint.Valid && arguments && (source < ModelledSpaces);
        const CapabilityError authority = !invoked ? CapabilityError::Ok
                                          : (source >= ModelledSpaces)
                                              ? CapabilityError::InvalidConfiguration
                                              : m_model.Invocation(source, invocation, InvokeRight, index);
        const TransferPrediction transfer =
            (codec == MessageCodecError::None)
                ? m_model.PredictTransfer(source, endpoint.Receiver, message, sources, sampled, prepare)
                : TransferPrediction{};
        const Check<EndpointErrorCode> checks[]{
            {!endpoint.Valid, EndpointErrorCode::InvalidConfiguration},
            {!arguments, EndpointErrorCode::InvalidArgument},
            {authority != CapabilityError::Ok, EndpointErrorCode::InvocationRejected},
            {endpoint.ActiveSend != 0U, EndpointErrorCode::ReservationActive},
            {codec != MessageCodecError::None, EndpointErrorCode::InvalidMessage},
            {Model::QuotaExceeded(endpoint, sendClass, sender, FrameBytes(message), message.AttachmentCount),
             EndpointErrorCode::WouldBlock},
            {transfer.Error != CapabilityError::Ok, EndpointErrorCode::CapabilityTransferFailed},
        };
        const EndpointErrorCode code = FirstFailure(checks, EndpointErrorCode::None);
        const bool reached = (code == EndpointErrorCode::None) || (code == EndpointErrorCode::CapabilityTransferFailed);
        const CapabilityError detail = reached                                           ? transfer.Error
                                       : (code == EndpointErrorCode::InvocationRejected) ? authority
                                                                                         : CapabilityError::Ok;
        const EndpointError expected{
            code, (code == EndpointErrorCode::InvalidMessage) ? codec : MessageCodecError::None, detail, 0U};
        const Size invoking = (authority == CapabilityError::Ok) ? m_model.Resolve(source, invocation) : NoIndex;
        const SlotState& metadata =
            m_model.Slots[Minimum(source, ModelledSpaces - 1U)][(invoking == NoIndex) ? 0U : invoking];
        const UInt16 rights = (invoked && (invoking != NoIndex)) ? metadata.Rights : UInt16{0U};
        const UInt64 sealed = (invoked && (invoking != NoIndex)) ? metadata.Sealed : 0U;
        Endpoint& real = *m_system.Endpoints[index];
        CapabilitySpace& space = m_system.Spaces[source];
        const ResourceDomainId from{sender};
        bool succeeded{false};
        EndpointError error{};
        UInt64 value{0U};
        EndpointSendReservation token{};
        if (prepare)
        {
            const auto result = real.PrepareSend(space, from, sendClass, message);
            succeeded = result.HasValue();
            error = ErrorOf(result);
            token = succeeded ? result.GetValue() : token;
            value = token.IsValid() ? token.GetIdentifier() : 0U;
        }
        else
        {
            const auto result = invoked ? real.SendFromInvocation(space, invocation.Handle, from, sendClass, message)
                                        : real.Send(space, from, sendClass, message);
            succeeded = result.HasValue();
            error = ErrorOf(result);
            value = succeeded ? result.GetValue().Sequence : 0U;
        }
        const Field unchanged[]{{"caller envelope changed at byte", NoIndex, FirstDifference(original, message)}};
        if (!SameOutcome(Names[api], expected, succeeded, error, sampled || (reached && transfer.ClockRead)) ||
            !SameFields(Names[api], unchanged))
        {
            return false;
        }
        if (succeeded)
        {
            MessageRecord record{};
            record.Message = message;
            record.Sender = sender;
            record.Sealed = sealed;
            record.Rights = rights;
            record.FrameBytes = static_cast<UInt16>(FrameBytes(message));
            record.Attachments = message.AttachmentCount;
            record.SendClass = sendClass;
            m_model.CommitTransfer(source, endpoint.Receiver, message, transfer, prepare, record);
            const Field reservation[]{{"send reservation identifier", endpoint.NextSendReservation, value}};
            const bool accepted =
                prepare ? SameFields(Names[api], reservation) : Enqueue(Names[api], endpoint, record, value);
            if (!accepted)
            {
                return false;
            }
            if (prepare)
            {
                endpoint.ActiveSend = endpoint.NextSendReservation;
                ++endpoint.NextSendReservation;
                endpoint.Prepared = record;
                m_sendTokens.Remember(index, token);
            }
        }
        return CheckEndpoint(Names[api], index);
    }

    // Receive consumes the head; PrepareReceive decodes and reserves it. The
    // invocation form first requires the receiver space (RightsDenied without
    // a clock read) and then Read authority, with one clock sample.
    [[nodiscard]] bool Receive() noexcept
    {
        constexpr const char* Names[]{"receive", "prepare receive", "prepare receive from invocation"};
        const Size api = Bucket(m_random.Below(22U), {14U, 18U});
        const bool prepare = api != 0U;
        const bool invoked = api == 2U;
        const char* const operation = Names[api];
        const Size index = Bucket(m_random.Below(100U), {50U, 85U});
        EndpointState& endpoint = m_model.Endpoints[index];
        const Size caller = m_random.Chance(1U, 8U) ? static_cast<Size>(m_random.Below(SpaceCount)) : endpoint.Receiver;
        const IssuedHandle invocation = invoked ? PickInvocation(caller, index, ReadRight) : IssuedHandle{};
        const bool sampled = invoked && endpoint.Valid && (caller == endpoint.Receiver);
        const CapabilityError authority = !invoked ? CapabilityError::Ok
                                          : (caller != endpoint.Receiver)
                                              ? CapabilityError::RightsDenied
                                              : m_model.Invocation(caller, invocation, ReadRight, index);
        const Check<EndpointErrorCode> checks[]{
            {!endpoint.Valid, EndpointErrorCode::InvalidConfiguration},
            {authority != CapabilityError::Ok, EndpointErrorCode::InvocationRejected},
            {endpoint.ActiveReceive != 0U, EndpointErrorCode::ReservationActive},
            {endpoint.Count == 0U, EndpointErrorCode::WouldBlock},
        };
        const EndpointErrorCode code = FirstFailure(checks, EndpointErrorCode::None);
        const EndpointError expected =
            Failure(code, (code == EndpointErrorCode::InvocationRejected) ? authority : CapabilityError::Ok);
        MessageEnvelope output = m_canary;
        Endpoint& real = *m_system.Endpoints[index];
        bool succeeded{false};
        EndpointError error{};
        EndpointReceiveMetadata metadata{};
        EndpointReceiveReservation reservation{};
        if (prepare)
        {
            const auto result =
                invoked ? real.PrepareReceiveFromInvocation(m_system.Spaces[caller], invocation.Handle, output)
                        : real.PrepareReceive(output);
            succeeded = result.HasValue();
            error = ErrorOf(result);
            metadata = succeeded ? result.GetValue().Metadata : metadata;
            reservation = succeeded ? result.GetValue().Reservation : reservation;
        }
        else
        {
            const auto result = real.Receive(output);
            succeeded = result.HasValue();
            error = ErrorOf(result);
            metadata = succeeded ? result.GetValue() : metadata;
        }
        const Field unchanged[]{{"failure changed the output at byte", NoIndex, FirstDifference(m_canary, output)}};
        if (!SameOutcome(operation, expected, succeeded, error, sampled) ||
            (!succeeded && !SameFields(operation, unchanged)))
        {
            return false;
        }
        if (!succeeded)
        {
            return CheckEndpoint(operation, index);
        }
        const MessageRecord& head = endpoint.Queue[endpoint.Head];
        const Size attachments = head.Attachments;
        const bool scan = head.Message.PayloadSizeBytes >= 8U;
        const Field fields[]{
            {"receive reservation identifier", prepare ? endpoint.NextReceiveReservation : 0U,
             reservation.IsValid() ? reservation.GetIdentifier() : 0U},
            {"payload copies left in the endpoint", (scan && prepare) ? 1U : 0U,
             scan ? Occurrences(real, head.Message.Payload) : 0U},
        };
        if (!CompareDelivery(operation, head, metadata, &output) || !SameFields(operation, fields))
        {
            return false;
        }
        if (prepare)
        {
            endpoint.ActiveReceive = endpoint.NextReceiveReservation;
            ++endpoint.NextReceiveReservation;
            endpoint.ReceiveByInvocation = invoked;
            endpoint.ReceiveInvocation = invocation;
            m_receiveTokens.Remember(index, reservation);
        }
        else
        {
            PopHead(endpoint);
        }
        return VerifyDelivered(operation, endpoint.Receiver, output, attachments) && CheckEndpoint(operation, index);
    }

    // Commit or cancel a prepared send or receive. Committing a send removes
    // its held sources; cancelling it closes its private destination handles
    // and restores them. Committing an invoked receive authorizes Read again.
    [[nodiscard]] bool Resolve() noexcept
    {
        constexpr const char* Names[]{"commit prepared send", "cancel prepared send", "commit prepared receive",
                                      "cancel prepared receive"};
        const Size kind = Bucket(m_random.Below(57U), {20U, 31U, 47U});
        const bool send = kind < 2U;
        const bool commit = (kind % 2U) == 0U;
        const Size index = Bucket(m_random.Below(100U), {50U, 85U});
        EndpointState& endpoint = m_model.Endpoints[index];
        const SendTokens::Entry sendToken = m_sendTokens.Pick(m_random, index);
        const ReceiveTokens::Entry receiveToken = m_receiveTokens.Pick(m_random, index);
        const UInt64 identifier = send ? sendToken.Token.GetIdentifier() : receiveToken.Token.GetIdentifier();
        const UInt64 active = send ? endpoint.ActiveSend : endpoint.ActiveReceive;
        const bool pendingClosed = send && !commit && !m_model.PendingLive(endpoint.Receiver, endpoint.Prepared);
        const bool invokedCommit = !send && commit && endpoint.ReceiveByInvocation;
        const CapabilityError authority =
            invokedCommit ? m_model.Invocation(endpoint.Receiver, endpoint.ReceiveInvocation, ReadRight, index)
                          : CapabilityError::Ok;
        const Check<EndpointErrorCode> checks[]{
            {!endpoint.Valid, EndpointErrorCode::InvalidConfiguration},
            {(send ? sendToken.Endpoint : receiveToken.Endpoint) != index, EndpointErrorCode::InvalidReservation},
            {(active == 0U) || (identifier != active), EndpointErrorCode::StaleReservation},
            {pendingClosed, EndpointErrorCode::QueueCorrupt},
            {authority != CapabilityError::Ok, EndpointErrorCode::InvocationRejected},
        };
        const EndpointErrorCode code = FirstFailure(checks, EndpointErrorCode::None);
        const bool sampled =
            invokedCommit && ((code == EndpointErrorCode::None) || (code == EndpointErrorCode::InvocationRejected));
        Endpoint& real = *m_system.Endpoints[index];
        EndpointError error{};
        UInt64 sequence{0U};
        EndpointReceiveMetadata metadata{};
        if (send && commit)
        {
            const auto result = real.CommitPreparedSend(sendToken.Token);
            error = ErrorOf(result);
            sequence = result.HasValue() ? result.GetValue().Sequence : 0U;
        }
        else if (commit)
        {
            const auto result = real.CommitPreparedReceive(receiveToken.Token);
            error = ErrorOf(result);
            metadata = result.HasValue() ? result.GetValue() : metadata;
        }
        else
        {
            error = send ? real.CancelPreparedSend(sendToken.Token) : real.CancelPreparedReceive(receiveToken.Token);
        }
        const bool succeeded = error.Code == EndpointErrorCode::None;
        const CapabilityError details[]{CapabilityError::Ok, CapabilityError::InvalidHandle, authority};
        const EndpointError expected = Failure(code, details[(code == EndpointErrorCode::QueueCorrupt)         ? 1U
                                                             : (code == EndpointErrorCode::InvocationRejected) ? 2U
                                                                                                               : 0U]);
        if (!SameOutcome(Names[kind], expected, succeeded, error, sampled) ||
            (succeeded && send && commit && !Enqueue(Names[kind], endpoint, endpoint.Prepared, sequence)) ||
            (succeeded && !send && commit &&
             !CompareDelivery(Names[kind], endpoint.Queue[endpoint.Head], metadata, nullptr)))
        {
            return false;
        }
        if (succeeded && send)
        {
            if (!commit)
            {
                m_model.ReleasePending(endpoint.Receiver, endpoint.Prepared);
            }
            m_model.ResolveHeld(endpoint.Prepared, commit);
        }
        if (succeeded && !send && commit)
        {
            PopHead(endpoint);
        }
        endpoint.ReceiveByInvocation = (succeeded && !send) ? false : endpoint.ReceiveByInvocation;
        endpoint.ActiveSend = (succeeded && send) ? 0U : endpoint.ActiveSend;
        endpoint.ActiveReceive = (succeeded && !send) ? 0U : endpoint.ActiveReceive;
        return CheckEndpoint(Names[kind], index);
    }

    // Pending handles a receiver already closed (learned through a cancelled
    // PrepareReceive) are skipped; the others are closed.
    [[nodiscard]] bool Discard() noexcept
    {
        const Size index = Bucket(m_random.Below(100U), {50U, 85U});
        EndpointState& endpoint = m_model.Endpoints[index];
        const Check<EndpointErrorCode> checks[]{
            {!endpoint.Valid, EndpointErrorCode::InvalidConfiguration},
            {(endpoint.ActiveSend != 0U) || (endpoint.ActiveReceive != 0U), EndpointErrorCode::ReservationActive},
        };
        const EndpointErrorCode code = FirstFailure(checks, EndpointErrorCode::None);
        const auto result = m_system.Endpoints[index]->DiscardUnread();
        const EndpointError expected = Failure(code);
        if (!SameOutcome("discard unread", expected, result.HasValue(), ErrorOf(result), false) ||
            (result.HasValue() && !SameQueueState("discard unread", endpoint, result.GetValue())))
        {
            return false;
        }
        for (Size offset{0U}; result.HasValue() && (offset < endpoint.Count); ++offset)
        {
            m_model.ReleasePending(endpoint.Receiver, endpoint.Queue[(endpoint.Head + offset) % QueueEntryCeiling]);
        }
        endpoint.Count = result.HasValue() ? 0U : endpoint.Count;
        endpoint.Bytes = result.HasValue() ? 0U : endpoint.Bytes;
        endpoint.Attachments = result.HasValue() ? 0U : endpoint.Attachments;
        return CheckEndpoint("discard unread", index);
    }

    // Mostly transferable rights, some finite leases; only a full space fails.
    // One mint in three names a registered Endpoint, usually able to invoke it.
    [[nodiscard]] bool Mint(const Size space) noexcept
    {
        const Size endpointObject = ObjectCount + static_cast<Size>(m_random.Below(EndpointCount));
        const bool invocable = m_random.Chance(1U, 3U) && m_model.Objects[endpointObject].Registered;
        const Size object = invocable ? endpointObject : static_cast<Size>(m_random.Below(ObjectCount));
        const Size domain = static_cast<Size>(m_random.Below(DomainCount));
        const UInt16 random = static_cast<UInt16>(m_random.Next() & KnownRights);
        const UInt16 rightsChoices[]{static_cast<UInt16>(random | InspectRight | TransferRight | DuplicateRight),
                                     random, m_random.Chance(1U, 2U) ? TransferRight : DuplicateRight};
        const UInt16 invocation = (invocable && m_random.Chance(3U, 4U)) ? (InvokeRight | ReadRight) : 0U;
        const UInt16 rights = static_cast<UInt16>(rightsChoices[Bucket(m_random.Below(10U), {6U, 9U})] | invocation);
        const UInt64 deadline = m_random.Chance(9U, 20U) ? m_model.Now + 1U + m_random.Below(600U) : Infinite;
        const UInt64 sealed = m_random.Next();
        Size slot{0U};
        while ((slot < SlotCount) && m_model.Slots[space][slot].Occupied)
        {
            ++slot;
        }
        const auto minted =
            m_system.Issuer.Mint(m_system.Spaces[space], m_model.Objects[object].Identity, RightsFrom(rights),
                                 m_model.Domains[domain], MonotonicTime{deadline}, sealed);
        const CapabilityError expected = (slot == SlotCount) ? CapabilityError::SpaceExhausted : CapabilityError::Ok;
        const UInt64 selector = minted.HasValue() ? minted.GetValue().GetSelector() : 0U;
        if (minted.HasValue())
        {
            m_model.Occupy(
                space, slot,
                SlotState{true, 0U, object, rights, domain, deadline, sealed, false, m_model.Epochs[domain]});
            Remember(IssuedHandle{space, minted.GetValue(), false}, false);
            if (object >= ObjectCount)
            {
                Size& count = m_invocableCount[space][object - ObjectCount];
                m_invocable[space][object - ObjectCount][count % InvocableHistory] =
                    IssuedHandle{space, minted.GetValue(), false};
                ++count;
            }
        }
        const CapabilityError actual = minted.HasValue() ? CapabilityError::Ok : minted.GetError();
        const Field fields[]{
            {"result", static_cast<UInt64>(expected), static_cast<UInt64>(actual)},
            {"issued slot", minted.HasValue() ? slot : 0U, selector & 0xFFU},
            {"issued generation", minted.HasValue() ? m_model.Slots[space][slot].Generation : 0U, selector >> 8U},
        };
        return SameFields("mint", fields) && CheckState("mint");
    }

    // Senders need handles; a hoarding receiver is kept saturated.
    [[nodiscard]] bool MintAny() noexcept
    {
        Size space = m_random.Chance(1U, 5U) ? 0U : 1U + static_cast<Size>(m_random.Below(ModelledSpaces - 1U));
        for (Size receiver{0U}; receiver < 2U; ++receiver)
        {
            space = (m_hoarding[receiver] && m_random.Chance(1U, 2U)) ? receiver : space;
        }
        return Mint(space);
    }

    // Closes mostly delivered handles, including those of a still-queued head.
    [[nodiscard]] bool Close() noexcept
    {
        const bool recent = (m_recentCount > 0U) && m_random.Chance(2U, 5U);
        const Size space = m_random.Chance(3U, 5U) ? static_cast<Size>(m_random.Below(2U))
                                                   : static_cast<Size>(m_random.Below(ModelledSpaces));
        const Size from = m_random.Chance(1U, 20U) ? static_cast<Size>(m_random.Below(ModelledSpaces)) : space;
        IssuedHandle handle = recent ? m_recent[m_random.Below(Minimum(m_recentCount, RecentCapacity))]
                                     : PickHandle(from, m_random.Chance(2U, 3U));
        handle.Forged = !recent && m_random.Chance(1U, 25U);
        const UInt64 authenticator = handle.Handle.GetAuthenticator() ^ (handle.Forged ? 1U : 0U);
        handle.Handle = CapabilityHandle{handle.Handle.GetSelector(), authenticator};
        const Size target = recent ? handle.Space : space;
        if (m_hoarding[target])
        {
            return true;
        }
        const Size index = m_model.Resolve(target, handle);
        const CapabilityError actual = m_system.Spaces[target].Close(handle.Handle);
        const Field result[]{{"closed", (index == NoIndex) ? 0U : 1U, (actual == CapabilityError::Ok) ? 1U : 0U}};
        if (index != NoIndex)
        {
            m_model.Free(target, index);
        }
        return SameFields("close", result) && CheckState("close");
    }

    // Advances one root domain's epoch: every capability minted in it before,
    // wherever it was transferred, reports Revoked; later mints are current.
    [[nodiscard]] bool Revoke() noexcept
    {
        const Size domain = static_cast<Size>(m_random.Below(DomainCount));
        const Kernel::Object::RevocationError revoked = m_system.Revocations.Revoke(m_model.Domains[domain]);
        ++m_model.Epochs[domain];
        const Field result[]{
            {"revoke", static_cast<UInt64>(Kernel::Object::RevocationError::Ok), static_cast<UInt64>(revoked)}};
        return SameFields("revoke", result) && CheckState("revoke");
    }

    [[nodiscard]] bool Advance() noexcept
    {
        const UInt64 ticks = m_random.Chance(1U, 10U) ? m_random.Below(400U) : m_random.Below(40U);
        m_system.Clock.Advance(ticks);
        m_model.Now += ticks;
        return CheckState("advance time");
    }

    System& m_system;
    Model& m_model;
    Random m_random;
    UInt64 m_seed;
    Size m_step{0U};
    Size m_phase{2U};
    UInt64 m_transaction{0U};
    bool m_hoarding[ModelledSpaces]{};
    MessageEnvelope m_canary{};
    IssuedHandle m_pool[ModelledSpaces][PoolCapacity]{};
    Size m_poolCount[ModelledSpaces]{};
    IssuedHandle m_recent[RecentCapacity]{};
    Size m_recentCount{0U};
    IssuedHandle m_invocable[ModelledSpaces][EndpointCount][InvocableHistory]{};
    Size m_invocableCount[ModelledSpaces][EndpointCount]{};
    SendTokens m_sendTokens{};
    ReceiveTokens m_receiveTokens{};
};

// The fixtures are hundreds of kilobytes; they live in static storage and are
// placement-constructed per seed because tests may not allocate.
alignas(System) unsigned char g_systemStorage[sizeof(System)];
alignas(Model) unsigned char g_modelStorage[sizeof(Model)];
alignas(Driver) unsigned char g_driverStorage[sizeof(Driver)];

void RunSeed(TestContext& context, const UInt64 seed) noexcept
{
    System* const system = new (g_systemStorage) System{};
    Model* const model = new (g_modelStorage) Model{};
    Driver* const driver = new (g_driverStorage) Driver{*system, *model, seed};
    bool matched = driver->Initialize();
    for (Size step{0U}; matched && (step < StepsPerSeed); ++step)
    {
        matched = driver->Step(step);
    }
    if (!matched)
    {
        static_cast<void>(
            std::fprintf(stderr, "  reproduce with ASTERIA_TEST_SEED=%llu\n", static_cast<unsigned long long>(seed)));
    }
    ASTERIA_EXPECT(context, "endpoint queue matches its reference model for one seed", matched);
    driver->Drain();
    driver->~Driver();
    model->~Model();
    system->~System();
}

// A valid attachment-free envelope for the example checks.
[[nodiscard]] MessageEnvelope MinimalMessage() noexcept
{
    MessageEnvelope message{};
    message.Interface = InterfaceIdentifier{1U, 0U};
    message.TransactionIdentifier = 1U;
    message.Deadline = MonotonicTime{Infinite};
    message.MethodIdentifier = 1U;
    message.InterfaceVersionMajor = 1U;
    message.Kind = MessageKind::Asynchronous;
    return message;
}

// A receiver may close a handle it learned from a PrepareReceive that it then
// cancelled; that is not queue corruption: DiscardUnread skips the released
// handle and still empties the queue.
[[nodiscard]] bool DiscardSkipsHandleClosedAfterPeek() noexcept
{
    System& system = *new (g_systemStorage) System{};
    const auto object = system.Issuer.CreateObject(system.Objects[0U]);
    const RevocationDomainId domain = system.Revocations.CreateRoot().GetValue();
    const auto source =
        system.Issuer.Mint(system.Spaces[1U], object.GetValue(), RightsFrom(InspectRight | DuplicateRight), domain,
                           MonotonicTime{Infinite}, 0U);
    system.Endpoints[0U] = new (system.Storage[0U]) Endpoint{system.Spaces[0U], ResourceDomainId{1U}};
    Endpoint& endpoint = *system.Endpoints[0U];
    MessageEnvelope message = MinimalMessage();
    message.AttachmentCount = 1U;
    message.Attachments[0U] =
        CapabilityAttachment{source.GetValue(),        MonotonicTime{Infinite},      RightsFrom(InspectRight),
                             ObjectType::MemoryObject, CapabilityTransferMode::Copy, 0U};
    MessageEnvelope output{};
    const bool sent =
        endpoint.Send(system.Spaces[1U], ResourceDomainId{2U}, EndpointSendClass::Normal, message).HasValue();
    const auto peek = endpoint.PrepareReceive(output);
    const bool closed = system.Spaces[0U].Close(output.Attachments[0U].Handle) == CapabilityError::Ok;
    const bool cancelled = peek.HasValue() && (endpoint.CancelPreparedReceive(peek.GetValue().Reservation).Code ==
                                               EndpointErrorCode::None);
    const auto discard = endpoint.DiscardUnread();
    const bool matched = sent && closed && cancelled && discard.HasValue() &&
                         (discard.GetValue().QueuedMessages == 1U) && (endpoint.GetQueueState().QueuedMessages == 0U) &&
                         (system.Spaces[0U].GetActiveCount() == 0U);
    system.~System();
    return matched;
}

// IpcCapabilityTransfer.md: an empty transfer is a no-op, so an
// attachment-free send never inspects its source space: not one without keys
// or from a foreign lock domain, and not a clock watermark the shared clock
// has fallen behind, which an attachment-bearing send from that space must
// reject. The model's clock never runs backwards.
[[nodiscard]] bool EmptySendIgnoresSourceSpace() noexcept
{
    System& system = *new (g_systemStorage) System{};
    system.Endpoints[0U] = new (system.Storage[0U]) Endpoint{system.Spaces[0U], ResourceDomainId{1U}};
    Endpoint& endpoint = *system.Endpoints[0U];
    const Size sources[]{BrokenSpace, ForeignSpace};
    bool matched{true};
    for (const Size source : sources)
    {
        matched =
            matched &&
            endpoint.Send(system.Spaces[source], ResourceDomainId{2U}, EndpointSendClass::Normal, MinimalMessage())
                .HasValue();
    }
    const auto object = system.Issuer.CreateObject(system.Objects[0U]);
    const RevocationDomainId domain = system.Revocations.CreateRoot().GetValue();
    const auto attachment =
        system.Issuer.Mint(system.Spaces[1U], object.GetValue(), RightsFrom(InspectRight | DuplicateRight), domain,
                           MonotonicTime{Infinite}, 0U);
    system.Clock.Set(2'000U);
    static_cast<void>(system.Spaces[1U].Query(CapabilityHandle{}));
    system.Clock.Set(1'500U);
    MessageEnvelope timed = MinimalMessage();
    timed.Deadline = MonotonicTime{1'600U};
    MessageEnvelope attached = timed;
    attached.AttachmentCount = 1U;
    attached.Attachments[0U] =
        CapabilityAttachment{attachment.GetValue(),    MonotonicTime{Infinite},      RightsFrom(InspectRight),
                             ObjectType::MemoryObject, CapabilityTransferMode::Copy, 0U};
    const auto empty = endpoint.Send(system.Spaces[1U], ResourceDomainId{2U}, EndpointSendClass::Normal, timed);
    const EndpointError behind =
        ErrorOf(endpoint.Send(system.Spaces[1U], ResourceDomainId{2U}, EndpointSendClass::Normal, attached));
    matched = matched && attachment.HasValue() && empty.HasValue() &&
              (behind.Code == EndpointErrorCode::CapabilityTransferFailed) &&
              (behind.Capability == CapabilityError::InvalidConfiguration) &&
              (endpoint.GetQueueState().QueuedMessages == 3U) && endpoint.DiscardUnread().HasValue();
    system.~System();
    return matched;
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"EndpointModelTests"};
    ASTERIA_EXPECT(runner, "closing a peeked handle of a queued message does not make DiscardUnread fail",
                   Asteria::Test::DiscardSkipsHandleClosedAfterPeek());
    ASTERIA_EXPECT(runner, "an attachment-free send never inspects its source space",
                   Asteria::Test::EmptySendIgnoresSourceSpace());
    const Asteria::TestSupport::SeedRange seeds = Asteria::TestSupport::GetSeedRange(1U, 64U);
    for (Asteria::Foundation::UInt64 seed = seeds.First; seed < seeds.First + seeds.Count; ++seed)
    {
        Asteria::Test::RunSeed(runner, seed);
    }
    return runner.Finish();
}
