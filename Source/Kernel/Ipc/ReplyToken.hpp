// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_IPC_REPLY_TOKEN_HPP_INCLUDED
#define ASTERIA_KERNEL_IPC_REPLY_TOKEN_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Object/IKernelObject.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Scheduler/SchedulingContext.hpp"
#include "Kernel/Scheduler/Thread.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Ipc
{
enum class ReplyTokenState : Foundation::UInt8
{
    Uninitialized,
    Available,
    Pending,
    Accepted,
    Replied,
    Cancelled,
};

enum class ReplyTokenError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    InvalidTransaction,
    InvalidCaller,
    CallerRegistryMismatch,
    CallerNotFound,
    CallerTypeMismatch,
    CallerPinUnavailable,
    InvalidSchedulingContext,
    SchedulingContextRegistryMismatch,
    SchedulingContextNotFound,
    SchedulingContextTypeMismatch,
    SchedulingContextPinUnavailable,
    CallerNotBlocked,
    InvalidBlockToken,
    SchedulingContextNotReady,
    SchedulingContextCallerMismatch,
    AlreadyInitialized,
    NotInitialized,
    InvalidServer,
    ServerRegistryMismatch,
    TransactionMismatch,
    AlreadyAccepted,
    NotAccepted,
    WrongServer,
    AlreadyCompleted,
    DonationRejected,
    ReturnRejected,
};

struct ReplyTokenFailure
{
    ReplyTokenError Error;
    Object::ObjectPinError PinError;
    Scheduler::SchedulingContextError SchedulingError;
    Foundation::UInt8 Reserved[5];
};

struct ReplyTokenCompletion
{
    Scheduler::ThreadId Caller;
    Scheduler::ThreadBlockToken BlockToken;
    Foundation::UInt64 TransactionIdentifier;
    ReplyTokenState State;
    Foundation::UInt8 Reserved[7];
};

struct ReplyTokenSnapshot
{
    Scheduler::ThreadId Caller;
    Scheduler::SchedulingContextId SchedulingContext;
    Scheduler::ThreadId Server;
    Scheduler::ThreadBlockToken BlockToken;
    Foundation::UInt64 TransactionIdentifier;
    ReplyTokenState State;
    Foundation::UInt8 Reserved[7];
};

/// Retains one blocked caller and its CPU budget until one server accepts the synchronous call and
/// either replies or the call is cancelled. This object performs only the one-shot authorization
/// and donation state machine. It does not copy a reply, enqueue an Endpoint message, wake a
/// Thread, or implement a deadline. SynchronousReplyDispatcher owns the first three operations,
/// checks and consumes the server's capability to this token at acceptance, and serializes them
/// with cancellation; later syscall integration owns the remaining authorization and deadlines.
/// Caller-side setup may publish an Available token before blocking, then Arm it with the exact
/// block token. The dispatcher prepares an otherwise no-fail reply delivery before CompleteReply
/// and wakes only the exact identity returned by successful completion. Cancellation publishes no
/// reply and returns the same wake identity directly. Lifetime and concurrency: - The originating
/// ObjectRegistry outlives this token and every copied identity or completion that refers to it. -
/// Operations serialize internally, may spin, and are not callable from an ISR. Callers prevent
/// same-CPU preemption or reentry that could reacquire this token or the retained
/// SchedulingContext. - While Accepted, this token and its dispatcher exclusively own the newest
/// donation edge. No other caller may ReturnDonation for that edge. - Caller and SchedulingContext
/// pins remain retained after terminal success. The dispatcher keeps this token alive through the
/// returned completion's Wake/enqueue use, then releases its pin. The storage owner separately
/// destroys and collects the terminal one-shot token. Any snapshot BlockToken is likewise usable
/// only while this token remains alive and retains the originating Thread. - From successful Arm or
/// Initialize through that Wake/enqueue, the dispatcher excludes direct Wake, reblock, Terminate,
/// Unbind, and Destroy operations for the retained caller/context. It already owns their exact
/// stable references and does not attempt a new registry lookup after commitment.
class ReplyToken final : public Object::IKernelObject
{
public:
    constexpr ReplyToken() noexcept = default;

    /// Preconditions:
    /// - State is Uninitialized, Available, Replied, or Cancelled. A pending
    /// call was cancelled and an accepted donation was returned explicitly.
    /// - Any terminal completion has already been used to wake/enqueue its
    /// exact caller; destruction releases the retained caller/context pins.
    /// - No concurrent operation accesses this object during destruction.
    ~ReplyToken() noexcept = default;

    ReplyToken(const ReplyToken&) = delete;
    ReplyToken& operator=(const ReplyToken&) = delete;
    ReplyToken(ReplyToken&&) = delete;
    ReplyToken& operator=(ReplyToken&&) = delete;

    /// Publishes the immutable resource owner without arming a call. An
    /// Available token can be registered and receive one temporary capability
    /// before its caller blocks.
    [[nodiscard]] ReplyTokenFailure Configure(Object::ResourceDomainId chargeOwner) noexcept;

    /// Pins the exact blocked caller and SchedulingContext generations of one
    /// previously configured token. Failure leaves the token Available and
    /// unpinned. The caller serializes the Thread and SchedulingContext
    /// lifecycle while this operation validates their correlated snapshots.
    [[nodiscard]] ReplyTokenFailure Arm(Object::ObjectRegistry& registry, Scheduler::ThreadId caller,
                                        Scheduler::SchedulingContextId schedulingContext,
                                        Scheduler::ThreadBlockToken blockToken,
                                        Foundation::UInt64 transactionIdentifier) noexcept;

    /// One-step initialization retained for trusted owners that do not need to
    /// register or mint authority before the caller blocks. Failure leaves the
    /// token Uninitialized and unpinned.
    [[nodiscard]] ReplyTokenFailure Initialize(Object::ObjectRegistry& registry, Object::ResourceDomainId chargeOwner,
                                               Scheduler::ThreadId caller,
                                               Scheduler::SchedulingContextId schedulingContext,
                                               Scheduler::ThreadBlockToken blockToken,
                                               Foundation::UInt64 transactionIdentifier) noexcept;

    /// Transfers the retained SchedulingContext from the caller to one exact
    /// server generation. A failed donation leaves the token Pending.
    [[nodiscard]] ReplyTokenFailure Accept(Scheduler::ThreadId server) noexcept;

    /// Returns the donated SchedulingContext and consumes the token at the
    /// dispatcher's reply commit boundary. Before calling, the dispatcher has
    /// prepared the exact correlated delivery so that no fallible work remains
    /// after success, has stopped the server run, and excludes cancellation.
    /// The returned caller and block token authorize only the subsequent wake
    /// and enqueue while this token remains alive. Failure leaves the token
    /// Accepted.
    [[nodiscard]] Foundation::Result<ReplyTokenCompletion, ReplyTokenFailure>
    CompleteReply(Scheduler::ThreadId server, Foundation::UInt64 transactionIdentifier) noexcept;

    /// Cancels a pending call or first returns an accepted donation. The
    /// dispatcher excludes a concurrent reply-publication commit. Success
    /// carries the exact caller wake identity and consumes the token.
    [[nodiscard]] Foundation::Result<ReplyTokenCompletion, ReplyTokenFailure> Cancel() noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept;

    /// Any returned BlockToken is usable only while this ReplyToken remains
    /// alive and its lifecycle-exclusion contract is held.
    [[nodiscard]] ReplyTokenSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

private:
    [[nodiscard]] static ReplyTokenFailure
    MakeFailure(ReplyTokenError error, Object::ObjectPinError pinError = Object::ObjectPinError::Ok,
                Scheduler::SchedulingContextError schedulingError = Scheduler::SchedulingContextError::None) noexcept;
    [[nodiscard]] ReplyTokenCompletion MakeCompletionLocked(ReplyTokenState state) const noexcept;
    [[nodiscard]] ReplyTokenFailure ArmLocked(Object::ObjectRegistry& registry, Scheduler::ThreadId caller,
                                              Scheduler::SchedulingContextId schedulingContext,
                                              Scheduler::ThreadBlockToken blockToken,
                                              Foundation::UInt64 transactionIdentifier) noexcept;
    [[nodiscard]] bool IsCompletedLocked() const noexcept;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Platform::Synchronization::AtomicUInt64 m_chargeOwnerValue;
    Object::ObjectPin m_callerPin;
    Object::ObjectPin m_schedulingContextPin;
    Object::ObjectRegistry* m_registry{nullptr};
    Scheduler::ThreadId m_caller;
    Scheduler::SchedulingContextId m_schedulingContext;
    Scheduler::ThreadId m_server;
    Scheduler::ThreadBlockToken m_blockToken;
    Foundation::UInt64 m_transactionIdentifier{0U};
    ReplyTokenState m_state{ReplyTokenState::Uninitialized};
};

static_assert(sizeof(ReplyTokenState) == 1U);
static_assert(sizeof(ReplyTokenError) == 1U);
static_assert(sizeof(ReplyTokenFailure) == 8U);
static_assert(alignof(ReplyTokenFailure) == 1U);
static_assert(sizeof(ReplyTokenCompletion) == 48U);
static_assert(alignof(ReplyTokenCompletion) == 8U);
static_assert(sizeof(ReplyTokenSnapshot) == 80U);
static_assert(alignof(ReplyTokenSnapshot) == 8U);
} // Namespace Asteria::Kernel::Ipc

#endif // ASTERIA_KERNEL_IPC_REPLY_TOKEN_HPP_INCLUDED
