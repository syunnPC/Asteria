<!-- SPDX-License-Identifier: Apache-2.0 -->

# Synchronous Call Dispatcher

## Scope

`SynchronousCallDispatcher` is the architecture-independent caller-side owner
for one bounded Call transaction. It joins a running caller, its running
`SchedulingContext`, a preconfigured and registered `ReplyToken`, the temporary
caller capability for that token, a server-facing call `Endpoint`, and the
`SynchronousReplyDispatcher` that will eventually publish a reply or cancel the
call.

The dispatcher allocates no memory and owns one fixed-size normalized message
plus one fixed-size validation frame. It accepts only infinite
deadlines and at most three caller-supplied attachments, reserving the final
attachment for its hidden ReplyToken. Deadline queues, user-copy, syscall
numbers, and architecture entry code remain outside this dispatcher.

## Prepare and commit

`Prepare` runs while the caller and context are both Running on the same CPU.
It pins and correlates their exact registry generations, run sequence, current
thread, resource owner, and the reply dispatcher's wake-queue processor. It
first performs a side-effect-free reply-dispatcher preflight, then authenticates
the exact call Endpoint capability with `Invoke` and retains its handle. It also
authenticates an infinite-lease ReplyToken capability carrying both `Invoke` and
`Transfer`, then copies and validates the Call in owned storage.

The hidden final attachment requests only `Invoke`, uses `Move`, and names
`ObjectType::ReplyToken`. A caller cannot supply its own ReplyToken attachment.
Prepare does not mutate either capability space's slots, either Endpoint, the
token, Thread, or SchedulingContext. Therefore `CancelPrepared` can restore
Fresh without a rollback protocol. Every failed Prepare clears provisional
pins, identities, run sequence, block token, staged bytes, and capability
metadata before it retains the root-cause diagnostic. A second Prepare while
Prepared returns a local `AlreadyPrepared` result without replacing the
original transaction.

After the captured run finishes as Blocked, `CommitBlocked` accepts only its
exact non-forgeable `ThreadBlockToken` and unchanged run correlation. It then:

1. enters `ArmPending` and arms the Available ReplyToken with the blocked
   caller and transaction;
2. enters `ReplyInitializationPending` and initializes
   `SynchronousReplyDispatcher` exactly once;
3. atomically sends the staged Call and its attachments through
   `Endpoint::SendFromInvocation`, which authorizes the retained Endpoint
   handle again in the critical section that publishes the Call and takes the
   server-visible rights and sealed selector from it there;
4. records the Endpoint sequence and releases preparation pins.

Authority can end while the caller is blocked: another domain may revoke the
Endpoint capability's revocation domain, or its lease may expire. Such a Call
is not published with the stale authority; the send fails with
`InvocationRejected` and the capability error, and the caller is recovered as
below.

Per-object reference exhaustion before Arm leaves the token Available and the
dispatcher in `ArmPending`. Exhaustion during reply initialization leaves the
token Pending and the dispatcher in `ReplyInitializationPending`; retry cannot
Arm it again. `Poll` advances only the retained phase. The run-loop completion
observer reports `RetryableRejected` for either state so the processor returns
to its idle coordinator, which owns the later pressure-relief and Poll policy.

Endpoint publication is the first capability-slot mutation. Its batch transfer
is failure-atomic: either the complete Call and every destination capability
are visible, or the source handles and Endpoint are unchanged. The server's
copy of the ReplyToken capability is its one-shot reply authority, which
`SynchronousReplyDispatcher::Accept` consumes.

## Failed publication

If Endpoint publication fails after the caller has blocked, the temporary
ReplyToken capability is no longer useful to the caller and is closed. The
reply dispatcher then consumes the armed token, wakes the exact block
generation, and publishes its reserved ReadyQueue entry. Successful recovery
returns the original `EndpointSendRejected` status while the snapshot records
`Cancelled`. This distinguishes a recovered blocked caller from a published
Call without losing the Endpoint diagnostic.

ReadyQueue capacity or reference pressure can temporarily prevent that wake.
The dispatcher retains `CancellationPending`; `PollCancellation` retries only
the wake transaction and preserves the original send diagnostic. Any
contradiction after token arming is fail-stop `Faulted` state rather than a
false rollback claim.

Observer re-calls in retry states return the retained retry classification
without repeating a completed phase. Re-calls after `Committed` or `Cancelled`
are accepted idempotently, and calls after a retained terminal failure do not
change its state or root-cause diagnostic.

## Architecture boundary

The production object is compiled and linked into the IA-64 firmware target so
its freestanding ABI, code generation, link closure, and 512-byte stack budget
are checked. It is not installed in `BootApplication`, assigned
a syscall number, or connected to IA-64 assembly ingress.

Call publication makes the message visible in the server Endpoint;
it does not select or wake a passive server Thread, accept the ReplyToken, or
donate the caller's SchedulingContext. Passive-server wake and donation
are not implemented by this dispatcher.

## Verification boundary

Hosted tests cover inert validation and cancellation, endpoint-capability
authentication, materially different duplicate Prepare, complete failed-
Prepare correlation rollback, a user attachment moved atomically beside the
hidden token, receiver-space rollback, unready reply delivery, deterministic
pre-Arm and post-Arm reference pressure, run-loop observer retry and idle Poll,
end-to-end Reply and exact wake with the reply authority consumed, Endpoint
authority revoked while the caller is blocked, and Endpoint/ReadyQueue
backpressure recovery.
ReplyToken tests separately cover pre-block configuration and exact post-block
arming. Debug and Release exercise the full hosted suite, Thread uses TSan, and
both target profiles compile the portable implementation under shared ABI and
stack checks.
