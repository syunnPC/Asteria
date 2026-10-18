// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstring>
#include <thread>

#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

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
using Foundation::UInt32;
using Foundation::UInt64;
using Kernel::Ipc::CancellationState;
using Kernel::Ipc::CapabilityAttachment;
using Kernel::Ipc::Endpoint;
using Kernel::Ipc::EndpointErrorCode;
using Kernel::Ipc::EndpointSendClass;
using Kernel::Ipc::InterfaceIdentifier;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Ipc::MessageKind;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationRegistry;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

class EndpointFixture final
{
public:
    EndpointFixture() noexcept
        : m_objects{m_synchronization}, m_revocations{m_synchronization},
          m_source{m_objects, m_revocations, m_clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL},
          m_receiver{m_objects, m_revocations, m_clock, 0x1357'9BDF'2468'ACE0ULL, 0x0ECA'8642'FDB9'7531ULL}
    {
    }

    [[nodiscard]] CapabilitySpace& GetSource() noexcept
    {
        return m_source;
    }

    [[nodiscard]] CapabilitySpace& GetReceiver() noexcept
    {
        return m_receiver;
    }

private:
    SpinLock m_synchronization;
    ManualClock m_clock;
    Asteria::TestSupport::TestObjectRegistry m_objects;
    RevocationRegistry m_revocations;
    CapabilitySpace m_source;
    CapabilitySpace m_receiver;
};

[[nodiscard]] constexpr bool HandlesEqual(const CapabilityHandle left, const CapabilityHandle right) noexcept
{
    return (left.GetSelector() == right.GetSelector()) && (left.GetAuthenticator() == right.GetAuthenticator());
}

[[nodiscard]] constexpr UInt64 ReadLittleEndian64(const UInt8* const bytes) noexcept
{
    UInt64 value{0U};
    for (UInt32 index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

constexpr void WriteLittleEndian64(UInt8* const bytes, const UInt64 value) noexcept
{
    for (UInt32 index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

[[nodiscard]] bool SetUniqueEndpointWord(Endpoint& endpoint, const UInt64 expected, const UInt64 replacement) noexcept
{
    UInt8* const bytes = reinterpret_cast<UInt8*>(&endpoint);
    Size matchOffset{0U};
    Size matchCount{0U};
    for (Size offset{0U}; (offset + sizeof(UInt64)) <= sizeof(endpoint); offset += alignof(UInt64))
    {
        if (ReadLittleEndian64(bytes + offset) == expected)
        {
            matchOffset = offset;
            ++matchCount;
        }
    }
    if (matchCount != 1U)
    {
        return false;
    }

    WriteLittleEndian64(bytes + matchOffset, replacement);
    return true;
}

[[nodiscard]] bool MessagesEqual(const MessageEnvelope& left, const MessageEnvelope& right) noexcept
{
    bool equal = (left.Interface.GetLow() == right.Interface.GetLow()) &&
                 (left.Interface.GetHigh() == right.Interface.GetHigh()) &&
                 (left.TransactionIdentifier == right.TransactionIdentifier) &&
                 (left.Deadline.GetTicks() == right.Deadline.GetTicks()) &&
                 (left.Trace.TraceIdentifierHigh == right.Trace.TraceIdentifierHigh) &&
                 (left.Trace.TraceIdentifierLow == right.Trace.TraceIdentifierLow) &&
                 (left.Trace.SpanIdentifier == right.Trace.SpanIdentifier) &&
                 (left.MethodIdentifier == right.MethodIdentifier) &&
                 (left.InterfaceVersionMajor == right.InterfaceVersionMajor) &&
                 (left.InterfaceVersionMinor == right.InterfaceVersionMinor) &&
                 (left.FixedPayloadSizeBytes == right.FixedPayloadSizeBytes) &&
                 (left.PayloadSizeBytes == right.PayloadSizeBytes) && (left.Kind == right.Kind) &&
                 (left.Cancellation == right.Cancellation) &&
                 (left.VariablePayloadCount == right.VariablePayloadCount) &&
                 (left.AttachmentCount == right.AttachmentCount) && (left.Reserved == right.Reserved) &&
                 (left.ReservedAlignment == right.ReservedAlignment);
    for (Size index{0U}; index < MessageEnvelope::MaximumCapabilityAttachments; ++index)
    {
        const CapabilityAttachment& leftAttachment = left.Attachments[index];
        const CapabilityAttachment& rightAttachment = right.Attachments[index];
        equal = equal && HandlesEqual(leftAttachment.Handle, rightAttachment.Handle) &&
                (leftAttachment.RequestedDeadline.GetTicks() == rightAttachment.RequestedDeadline.GetTicks()) &&
                leftAttachment.RequestedRights.IsEqualTo(rightAttachment.RequestedRights) &&
                (leftAttachment.ExpectedType == rightAttachment.ExpectedType) &&
                (leftAttachment.TransferMode == rightAttachment.TransferMode) &&
                (leftAttachment.Reserved == rightAttachment.Reserved);
    }
    for (Size index{0U}; index < MessageEnvelope::MaximumVariablePayloads; ++index)
    {
        equal = equal && (left.VariablePayloads[index].OffsetBytes == right.VariablePayloads[index].OffsetBytes) &&
                (left.VariablePayloads[index].SizeBytes == right.VariablePayloads[index].SizeBytes) &&
                (left.VariablePayloads[index].Reserved == right.VariablePayloads[index].Reserved);
    }
    for (Size index{0U}; index < MessageEnvelope::MaximumPayloadBytes; ++index)
    {
        equal = equal && (left.Payload[index] == right.Payload[index]);
    }
    return equal;
}

[[nodiscard]] MessageEnvelope MakeMessage(const UInt64 transaction, const Foundation::UInt16 payloadSize = 0U,
                                          const MonotonicTime deadline = MonotonicTime::Infinite()) noexcept
{
    MessageEnvelope message{};
    message.Interface = InterfaceIdentifier{0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL};
    message.TransactionIdentifier = transaction;
    message.Deadline = deadline;
    message.MethodIdentifier = 7U;
    message.InterfaceVersionMajor = 1U;
    message.InterfaceVersionMinor = 0U;
    message.FixedPayloadSizeBytes = payloadSize;
    message.PayloadSizeBytes = payloadSize;
    message.Kind = MessageKind::Asynchronous;
    message.Cancellation = CancellationState::None;
    for (Foundation::UInt16 index{0U}; index < payloadSize; ++index)
    {
        message.Payload[index] = static_cast<Foundation::UInt8>((transaction + index) & 0xFFU);
    }
    return message;
}

void TestSequenceExhaustion(TestContext& runner) noexcept
{
    EndpointFixture fixture;
    Endpoint endpoint{fixture.GetReceiver(), ResourceDomainId{90U}};
    MessageEnvelope output{};
    bool warmupSucceeded{true};
    for (UInt64 sequence{1U}; sequence <= 16U; ++sequence)
    {
        const auto send = endpoint.Send(fixture.GetSource(), ResourceDomainId{1U}, EndpointSendClass::Normal,
                                        MakeMessage(500U + sequence));
        const auto receive = endpoint.Receive(output);
        warmupSucceeded =
            warmupSucceeded && send.HasValue() && receive.HasValue() && (send.GetValue().Sequence == sequence);
    }
    const bool sequenceInjected = SetUniqueEndpointWord(endpoint, 17U, ~UInt64{0U});
    const auto finalSend =
        endpoint.Send(fixture.GetSource(), ResourceDomainId{1U}, EndpointSendClass::Normal, MakeMessage(600U));
    const auto finalReceive = endpoint.Receive(output);
    const auto exhausted =
        endpoint.Send(fixture.GetSource(), ResourceDomainId{1U}, EndpointSendClass::Normal, MakeMessage(601U));
    ASTERIA_EXPECT(runner, "the maximum sequence is issued once and permanently exhausts the endpoint", warmupSucceeded,
                   sequenceInjected, finalSend.HasValue(), finalSend.GetValue().Sequence == ~UInt64{0U},
                   finalReceive.HasValue(), !exhausted.HasValue(),
                   exhausted.GetError().Code == EndpointErrorCode::SequenceExhausted,
                   endpoint.GetQueueState().NextSequence == 0U, endpoint.GetQueueState().QueuedMessages == 0U);
}

void TestConcurrentFifo(TestContext& runner) noexcept
{
    EndpointFixture fixture;
    Endpoint endpoint{fixture.GetReceiver(), ResourceDomainId{90U}};
    constexpr Size ProducerCount{2U};
    constexpr Size MessagesPerProducer{32U};
    constexpr Size TotalMessages{ProducerCount * MessagesPerProducer};
    constexpr Size MaximumRetries{1'000'000U};
    std::atomic<bool> begin{false};
    std::atomic<Size> producersDone{0U};
    std::atomic<Size> unexpected{0U};
    bool seen[TotalMessages]{};
    Size receivedCount{0U};

    const auto producer = [&](const Size producerIndex) noexcept
    {
        while (!begin.load())
        {
            std::this_thread::yield();
        }
        for (Size index{0U}; index < MessagesPerProducer; ++index)
        {
            const UInt64 transaction = (producerIndex * MessagesPerProducer) + index + 1U;
            const MessageEnvelope message = MakeMessage(transaction, 1U);
            bool sent{false};
            for (Size retry{0U}; retry < MaximumRetries; ++retry)
            {
                const auto result = endpoint.Send(fixture.GetSource(), ResourceDomainId{producerIndex + 1U},
                                                  EndpointSendClass::Normal, message);
                if (result.HasValue())
                {
                    sent = true;
                    break;
                }
                if (result.GetError().Code != EndpointErrorCode::WouldBlock)
                {
                    break;
                }
                std::this_thread::yield();
            }
            if (!sent)
            {
                ++unexpected;
            }
        }
        ++producersDone;
    };
    const auto consumer = [&]() noexcept
    {
        UInt64 expectedSequence{1U};
        while (receivedCount < TotalMessages)
        {
            MessageEnvelope output{};
            const auto result = endpoint.Receive(output);
            if (!result.HasValue())
            {
                if (result.GetError().Code != EndpointErrorCode::WouldBlock)
                {
                    ++unexpected;
                    break;
                }
                if ((producersDone.load() == ProducerCount) && (endpoint.GetQueueState().QueuedMessages == 0U))
                {
                    break;
                }
                std::this_thread::yield();
                continue;
            }

            const UInt64 transaction = output.TransactionIdentifier;
            const Size transactionIndex = transaction == 0U ? TotalMessages : static_cast<Size>(transaction - 1U);
            const Size expectedProducer = transactionIndex / MessagesPerProducer;
            if ((result.GetValue().Sequence != expectedSequence) || (transactionIndex >= TotalMessages) ||
                seen[transactionIndex] || (result.GetValue().Sender.GetValue() != (expectedProducer + 1U)))
            {
                ++unexpected;
            }
            else
            {
                seen[transactionIndex] = true;
            }
            ++expectedSequence;
            ++receivedCount;
        }
    };

    std::thread firstProducer{producer, 0U};
    std::thread secondProducer{producer, 1U};
    std::thread receiver{consumer};
    begin.store(true);
    firstProducer.join();
    secondProducer.join();
    receiver.join();

    bool allSeen = receivedCount == TotalMessages;
    for (const bool value : seen)
    {
        allSeen = allSeen && value;
    }
    ASTERIA_EXPECT(runner, "concurrent producers and receiver observe one serialized FIFO sequence",
                   unexpected.load() == 0U, allSeen, endpoint.GetQueueState().QueuedMessages == 0U,
                   endpoint.GetQueueState().NextSequence == (TotalMessages + 1U));
    static_cast<void>(endpoint.DiscardUnread());
}

void TestPreparedSendIdentityAndConcurrency(TestContext& runner) noexcept
{
    {
        EndpointFixture fixture;
        Endpoint endpoint{fixture.GetReceiver(), ResourceDomainId{90U}};
        const MessageEnvelope message = MakeMessage(700U);
        const MessageEnvelope messageBefore = message;
        // Byte snapshot of the polymorphic Endpoint, vtable pointer included.
        UInt8 endpointBefore[sizeof(Endpoint)]{};
        static_cast<void>(std::memcpy(endpointBefore, static_cast<const void*>(&endpoint), sizeof(endpointBefore)));
        const Size sourceCountBefore = fixture.GetSource().GetActiveCount();
        const Size receiverCountBefore = fixture.GetReceiver().GetActiveCount();
        const auto invalid =
            endpoint.PrepareSend(fixture.GetSource(), ResourceDomainId{}, EndpointSendClass::Normal, message);
        ASTERIA_EXPECT(runner, "invalid preparation preserves the endpoint, message, and capability spaces",
                       !invalid.HasValue(), invalid.GetError().Code == EndpointErrorCode::InvalidArgument,
                       std::memcmp(endpointBefore, static_cast<const void*>(&endpoint), sizeof(endpointBefore)) == 0,
                       MessagesEqual(message, messageBefore), fixture.GetSource().GetActiveCount() == sourceCountBefore,
                       fixture.GetReceiver().GetActiveCount() == receiverCountBefore);
    }

    {
        EndpointFixture fixture;
        Endpoint endpoint{fixture.GetReceiver(), ResourceDomainId{90U}};
        const auto first = endpoint.PrepareSend(fixture.GetSource(), ResourceDomainId{1U}, EndpointSendClass::Normal,
                                                MakeMessage(800U));
        const auto firstCancelled =
            first.HasValue() ? endpoint.CancelPreparedSend(first.GetValue()) : Kernel::Ipc::EndpointError{};
        const auto second = endpoint.PrepareSend(fixture.GetSource(), ResourceDomainId{1U}, EndpointSendClass::Normal,
                                                 MakeMessage(801U));
        const auto oldCommit =
            first.HasValue()
                ? endpoint.CommitPreparedSend(first.GetValue())
                : Foundation::Result<Kernel::Ipc::EndpointSendReceipt, Kernel::Ipc::EndpointError>::Failure({});
        const auto oldCancel =
            first.HasValue() ? endpoint.CancelPreparedSend(first.GetValue()) : Kernel::Ipc::EndpointError{};
        const auto secondCommit =
            second.HasValue()
                ? endpoint.CommitPreparedSend(second.GetValue())
                : Foundation::Result<Kernel::Ipc::EndpointSendReceipt, Kernel::Ipc::EndpointError>::Failure({});
        MessageEnvelope output{};
        const auto received = endpoint.Receive(output);
        ASTERIA_EXPECT(runner, "an old token cannot resolve a later active reservation", first.HasValue(),
                       firstCancelled.Code == EndpointErrorCode::None, second.HasValue(),
                       second.GetValue().GetIdentifier() != first.GetValue().GetIdentifier(), !oldCommit.HasValue(),
                       oldCommit.GetError().Code == EndpointErrorCode::StaleReservation,
                       oldCancel.Code == EndpointErrorCode::StaleReservation, secondCommit.HasValue(),
                       received.HasValue(), output.TransactionIdentifier == 801U);
    }

    {
        EndpointFixture fixture;
        Endpoint endpoint{fixture.GetReceiver(), ResourceDomainId{90U}};
        const auto prepared = endpoint.PrepareSend(fixture.GetSource(), ResourceDomainId{1U}, EndpointSendClass::Normal,
                                                   MakeMessage(900U));
        if (!prepared.HasValue())
        {
            ASTERIA_EXPECT(runner, "concurrent prepared-send fixture is valid", false);
            return;
        }
        std::atomic<bool> begin{false};
        bool commitSucceeded{false};
        EndpointErrorCode commitError{EndpointErrorCode::None};
        EndpointErrorCode cancelError{EndpointErrorCode::None};
        std::thread committer{[&]() noexcept
                              {
                                  while (!begin.load())
                                  {
                                      std::this_thread::yield();
                                  }
                                  const auto result = endpoint.CommitPreparedSend(prepared.GetValue());
                                  commitSucceeded = result.HasValue();
                                  if (!result.HasValue())
                                  {
                                      commitError = result.GetError().Code;
                                  }
                              }};
        std::thread canceller{[&]() noexcept
                              {
                                  while (!begin.load())
                                  {
                                      std::this_thread::yield();
                                  }
                                  cancelError = endpoint.CancelPreparedSend(prepared.GetValue()).Code;
                              }};
        begin.store(true);
        committer.join();
        canceller.join();
        const bool cancelSucceeded = cancelError == EndpointErrorCode::None;
        ASTERIA_EXPECT(runner, "concurrent commit and cancel have exactly one linearized winner",
                       commitSucceeded != cancelSucceeded,
                       commitSucceeded ? (cancelError == EndpointErrorCode::StaleReservation)
                                       : (commitError == EndpointErrorCode::StaleReservation),
                       endpoint.GetQueueState().QueuedMessages == (commitSucceeded ? 1U : 0U));
        if (commitSucceeded)
        {
            MessageEnvelope output{};
            static_cast<void>(endpoint.Receive(output));
        }
    }

    {
        EndpointFixture fixture;
        Endpoint endpoint{fixture.GetReceiver(), ResourceDomainId{90U}};
        bool warmup{true};
        for (UInt64 identifier{1U}; identifier <= 16U; ++identifier)
        {
            const auto prepared = endpoint.PrepareSend(fixture.GetSource(), ResourceDomainId{1U},
                                                       EndpointSendClass::Normal, MakeMessage(1'000U + identifier));
            warmup = warmup && prepared.HasValue() && (prepared.GetValue().GetIdentifier() == identifier);
            if (prepared.HasValue())
            {
                warmup = warmup && (endpoint.CancelPreparedSend(prepared.GetValue()).Code == EndpointErrorCode::None);
            }
        }
        const bool injected = SetUniqueEndpointWord(endpoint, 17U, ~UInt64{0U});
        const auto maximum = endpoint.PrepareSend(fixture.GetSource(), ResourceDomainId{1U}, EndpointSendClass::Normal,
                                                  MakeMessage(1'100U));
        Kernel::Ipc::EndpointError maximumCancel{};
        if (maximum.HasValue())
        {
            maximumCancel = endpoint.CancelPreparedSend(maximum.GetValue());
        }
        const auto exhausted = endpoint.PrepareSend(fixture.GetSource(), ResourceDomainId{1U},
                                                    EndpointSendClass::Normal, MakeMessage(1'101U));
        const auto ordinarySend =
            endpoint.Send(fixture.GetSource(), ResourceDomainId{1U}, EndpointSendClass::Normal, MakeMessage(1'102U));
        ASTERIA_EXPECT(runner, "the last reservation identifier is issued once without disabling ordinary send", warmup,
                       injected, maximum.HasValue(), maximum.GetValue().GetIdentifier() == ~UInt64{0U},
                       maximumCancel.Code == EndpointErrorCode::None, !exhausted.HasValue(),
                       exhausted.GetError().Code == EndpointErrorCode::SequenceExhausted, ordinarySend.HasValue());

        MessageEnvelope output{};
        bool receiveWarmup{true};
        for (UInt64 identifier{1U}; identifier <= 16U; ++identifier)
        {
            const auto prepared = endpoint.PrepareReceive(output);
            const bool cancelled =
                prepared.HasValue() &&
                (endpoint.CancelPreparedReceive(prepared.GetValue().Reservation).Code == EndpointErrorCode::None);
            receiveWarmup =
                receiveWarmup && cancelled && (prepared.GetValue().Reservation.GetIdentifier() == identifier);
        }
        const bool receiveInjected = SetUniqueEndpointWord(endpoint, 17U, ~UInt64{0U});
        const auto maximumReceive = endpoint.PrepareReceive(output);
        const bool maximumReceiveCancelled =
            maximumReceive.HasValue() &&
            (endpoint.CancelPreparedReceive(maximumReceive.GetValue().Reservation).Code == EndpointErrorCode::None);
        const auto exhaustedReceive = endpoint.PrepareReceive(output);
        const auto ordinaryReceive = endpoint.Receive(output);
        ASTERIA_EXPECT(
            runner, "the last receive-reservation identifier is issued once without disabling ordinary receive",
            receiveWarmup, receiveInjected, maximumReceive.HasValue(),
            maximumReceive.GetValue().Reservation.GetIdentifier() == ~UInt64{0U}, maximumReceiveCancelled,
            !exhaustedReceive.HasValue(), exhaustedReceive.GetError().Code == EndpointErrorCode::SequenceExhausted,
            ordinaryReceive.HasValue(), output.TransactionIdentifier == 1'102U);
        static_cast<void>(endpoint.DiscardUnread());
    }
}

// Two Endpoints hold moved sources of one space at the same time; resolving
// one reservation restores or releases exactly its own sources. The model test
// rarely prepares two sends from one space at once.
void TestPreparedMovesResolveOnlyTheirOwnSources(TestContext& runner) noexcept
{
    SpinLock synchronization;
    ManualClock clock;
    Asteria::TestSupport::TestObjectRegistry objects{synchronization};
    RevocationRegistry revocations{synchronization};
    Kernel::Object::RootCapabilityIssuer issuer{objects};
    CapabilitySpace source{objects, revocations, clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL};
    CapabilitySpace receiver{objects, revocations, clock, 0x1357'9BDF'2468'ACE0ULL, 0x0ECA'8642'FDB9'7531ULL};
    Endpoint first{receiver, ResourceDomainId{90U}};
    Endpoint second{receiver, ResourceDomainId{91U}};
    const auto object = issuer.CreateObject(first);
    const auto domain = revocations.CreateRoot();
    const Kernel::Object::CapabilityRights rights{
        Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Transfer}.With(
            Kernel::Object::CapabilityRight::Inspect)};
    CapabilityHandle handles[2]{};
    for (CapabilityHandle& handle : handles)
    {
        const auto minted =
            (object.HasValue() && domain.HasValue())
                ? issuer.Mint(source, object.GetValue(), rights, domain.GetValue(), MonotonicTime::Infinite(), 0U)
                : Foundation::Result<CapabilityHandle, Kernel::Object::CapabilityError>::Failure(
                      Kernel::Object::CapabilityError::InvalidArgument);
        handle = minted.HasValue() ? minted.GetValue() : CapabilityHandle{};
    }
    MessageEnvelope messages[2]{MakeMessage(1'200U), MakeMessage(1'201U)};
    for (Size index{0U}; index < 2U; ++index)
    {
        messages[index].Attachments[0U] = CapabilityAttachment{
            .Handle = handles[index],
            .RequestedDeadline = MonotonicTime::Infinite(),
            .RequestedRights = Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Inspect},
            .ExpectedType = Kernel::Object::ObjectType::Endpoint,
            .TransferMode = Kernel::Ipc::CapabilityTransferMode::Move,
            .Reserved = 0U,
        };
        messages[index].AttachmentCount = 1U;
    }
    const auto firstPrepared = first.PrepareSend(source, ResourceDomainId{1U}, EndpointSendClass::Normal, messages[0U]);
    const auto secondPrepared =
        second.PrepareSend(source, ResourceDomainId{1U}, EndpointSendClass::Normal, messages[1U]);
    const bool heldWhilePrepared = !source.Query(handles[0U]).HasValue() && !source.Query(handles[1U]).HasValue();
    const auto cancelled =
        secondPrepared.HasValue() ? second.CancelPreparedSend(secondPrepared.GetValue()) : Kernel::Ipc::EndpointError{};
    const auto committed =
        firstPrepared.HasValue()
            ? first.CommitPreparedSend(firstPrepared.GetValue())
            : Foundation::Result<Kernel::Ipc::EndpointSendReceipt, Kernel::Ipc::EndpointError>::Failure({});
    const auto restored = source.Query(handles[1U]);
    const auto removed = source.Query(handles[0U]);
    ASTERIA_EXPECT(runner, "a prepared send holds its moved sources and resolves only its own", handles[0U].IsValid(),
                   handles[1U].IsValid(), firstPrepared.HasValue(), secondPrepared.HasValue(), heldWhilePrepared,
                   source.GetActiveCount() == 1U, cancelled.Code == EndpointErrorCode::None, committed.HasValue(),
                   restored.HasValue(), restored.GetValue().Rights.IsEqualTo(rights), !removed.HasValue(),
                   removed.GetError() == Kernel::Object::CapabilityError::InvalidHandle,
                   receiver.GetActiveCount() == 1U, first.GetQueueState().QueuedMessages == 1U,
                   second.GetQueueState().QueuedMessages == 0U);
    static_cast<void>(first.DiscardUnread());
    static_cast<void>(source.Close(handles[1U]));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"EndpointTests"};
    Asteria::Test::TestSequenceExhaustion(runner);
    Asteria::Test::TestConcurrentFifo(runner);
    Asteria::Test::TestPreparedSendIdentityAndConcurrency(runner);
    Asteria::Test::TestPreparedMovesResolveOnlyTheirOwnSources(runner);
    return runner.Finish();
}
