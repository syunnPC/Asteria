<!-- SPDX-License-Identifier: Apache-2.0 -->

# One-shot Reply Token

## Scope

`ReplyToken` is the architecture-independent ownership boundary for one
synchronous IPC call. It retains the exact blocked caller generation, its exact
block token, the associated `SchedulingContext`, and a nonzero transaction
identifier. One server may accept the token, temporarily receive the caller's
CPU budget, and then either reply or return that budget during cancellation.

This object does not copy message bytes, enqueue an `Endpoint`
entry, wake a thread, enforce a deadline, or authorize a syscall. The
architecture-independent `SynchronousReplyDispatcher` performs the first
three operations through Endpoint and ReadyQueue. Deadline enforcement and
syscall authorization are not implemented by either object.

## State and ownership

The state machine is:

```text
Uninitialized -> Available -> Pending -> Accepted -> Replied
                                  \ \-> Cancelled
                                   \------------> Cancelled
```

`Configure` release-publishes the immutable resource owner and makes a token
Available without retaining caller state. This permits the storage owner to
register the token and mint the temporary capability needed by caller-side
Call setup. `Arm` then pins the exact caller and scheduling-context registry
generations only after proving that the caller is blocked on the supplied token
and is the current endpoint of the context's donation chain. Every Arm failure
leaves the object Available and releases provisional pins. The one-step
`Initialize` path remains for trusted owners that already have their blocked
caller and do not need pre-block registration.

`Accept` delegates to `SchedulingContext::Donate`; therefore the context owns a
generation-specific server pin for the lifetime of the accepted edge. The
reply token and its dispatcher exclusively own that newest donation edge until
`CompleteReply` or `Cancel` returns it. Terminal success preserves the retained
caller and context pins together with the diagnostic identities, so the
returned raw identities cannot lose their backing storage before Wake/enqueue.
The dispatcher then releases its own token pin. The token's storage owner marks
the one-shot object destroyed and collects it, which releases the token's
retained pins during object destruction. A terminal operation cannot be replayed.

## Dispatcher ordering

Before `Arm` or one-step `Initialize`, the dispatcher blocks the caller and
excludes direct thread/context lifecycle changes while their correlated
snapshots are checked. That exclusion continues through the terminal
Wake/enqueue: no independent Wake, reblock, terminate, unbind, or destroy
operation may invalidate the block generation or make either object quiescing.
The dispatcher already retains the exact object references established by the
call transaction and does not try to reacquire them from the registry after
commitment.
Before `CompleteReply`, it stops the server run, reserves an invisible blocked
caller queue entry, and privately prepares the correlated Endpoint reply. It
also serializes this commit against cancellation. After the token returns the
donation, it publishes the reply, performs the exact Wake, and finally exposes
the ready entry. The token remains pinned through that sequence; its storage
owner destroys it only after the dispatcher releases the terminal pin.

Cancellation never fabricates a reply. Pending cancellation consumes directly;
accepted cancellation first returns the donation, then both variants perform
the same reserved Wake/enqueue sequence. The dispatcher checks and consumes the
server's ReplyToken capability at acceptance. Deadline selection,
cancellation-message authentication, user-copy fault handling, and the remaining
syscall rights checks are not implemented by the dispatcher.

## Concurrency and lifetime

All mutable operations and snapshots serialize on the token's spin lock. The
lock order is token, then scheduling context or thread, then any registry work
performed by context donation ([lock order](LockOrder.md)). These operations may spin, are forbidden in interrupt
context, and require callers to exclude same-CPU preemption or reentry that
could reacquire either object.

The originating `ObjectRegistry` outlives an armed token and every copied
`ThreadId`, `SchedulingContextId`, snapshot, or completion. Destruction requires
an uninitialized, available, or terminal token, no active operation, and
completion of the terminal Wake/enqueue use. Pending tokens are cancelled
rather than abandoned; accepted donations are returned before destruction. As
with other registry objects, backing storage remains alive and immovable until
destruction and collection complete. A snapshot's raw block token is used only
while the reply token remains alive and retains its originating caller.

## Verification boundary

Hosted tests cover invalid and cross-registry identities, stale block
generations, wrong object types, context/caller disagreement, accepted and
pending cancellation, reply transaction and server authentication, donation
return failure, preconfigured registration and exact Arm, terminal replay,
retained-object cleanup, and a concurrent reply/cancel exactly-one-winner
boundary. Debug uses ASan/UBSan, the Thread
configuration uses TSan, and both target profiles compile and link the object at
O0 and O2 with the common ABI and stack contracts.
