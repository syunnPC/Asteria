<!-- SPDX-License-Identifier: Apache-2.0 -->

# Timer Notification Delivery

## Scope

`TimerNotificationDelivery` is the allocation-free normal-context transaction
that connects one due `TimerQueue` entry to one exact `Notification`
generation. It does not program a hardware deadline, acknowledge an interrupt,
allocate memory, or run from an ISR. Each transaction object retains mutable
pins and diagnostics, so the caller grants one CPU exclusive access across a
complete `Deliver` call and externally serializes direct queue-claim completion
and Timer acknowledgement with it.

## Transaction

`Deliver` performs one ordered transaction:

1. claim the next due or Pending Timer from the exact queue;
2. validate the caller's Timer identity, registry, storage address, Expired
   state, expiration token, and deadline;
3. acquire an independent exact Timer pin before the queue pin can be released;
4. validate and pin the exact initialized Notification generation;
5. publish the Notification's coalescing Pending bit;
6. commit the queue claim, removing membership and releasing its pin; and
7. acknowledge the exact Timer token while the independent Timer pin remains
   retained.

A failure before Notification publication cancels the queue claim back to
Pending for a later attempt; a failed cancellation is reported separately with
the original cause. Two failures instead prove that the expiration can never be
delivered, and the claim is retired (`TimerQueue::RetireClaim`, reported with
`ClaimRetired`): the claimed Timer generation can no longer be pinned, which
the queue pin proves means it is being destroyed, or the Notification
generation can no longer be pinned although the caller declared the destination
`Pinned`, that is, it holds its own pin on that generation for as long as it
delivers this Timer there. `Deliver` and `DeliverClaimed` name a `Transient`
destination the caller may retry with another Notification, so their
Notification pin failures stay retryable. Every other pin failure, such as a
saturated reference count, proves nothing about lifetime and is retried. A
failed retirement is reported as `ClaimRetirementRejected` with the original
cause. After publication, rollback would fabricate an unobservable undo, so
commit or Timer-acknowledgement contradictions are returned with explicit
`NotificationPublished` and `QueueCommitted` diagnostics. They require the
owning runtime's recovery or fail-stop policy.

The transaction object retains its pins, token, claimed Timer pointer, receipt,
and failure record. This keeps call frames bounded while ensuring no object can
be destroyed between validation, queue-pin release, and final acknowledgement.

## Deferred worker

`DeferredTimerWorker` retains up to the TimerQueue capacity of exact
Timer-to-Notification bindings. Registration pins both object generations,
rejects duplicate Timer ownership, and returns a nonwrapping worker-bound token.
Unregister requires that exact token and a Disarmed Timer.

`DrainDue` processes at most the fixed TimerQueue capacity. It stops
successfully on Empty, NotDue, or its work limit and delegates each due claim to
the transaction above, with the binding as a `Pinned` destination. An
expiration whose Timer, or whose binding's Notification, was destroyed while
the Timer was queued is retired and counted in the receipt's `RetiredCount`,
and the drain continues with the next claim; a Timer destroyed while queued
without a binding is retired the same way. The binding itself stays until its
owner removes it, which the now Disarmed Timer permits. A missing binding of a
live Timer, recoverable by registering one, and every other prepublication
rejection restore the queue claim to Pending before returning a retryable
failure. Failed restoration or retirement, or a contradiction after
Notification publication, faults the worker because safe rollback is no longer
provable. The retained-result entry avoids copying the full nested diagnostic
through a target's bounded stack.

## IA-64 bootstrap composition

The IA-64 BSP first one-shot proof invokes this transaction only after the
stackless external-interrupt entry has masked ITV, issued EOI, published its
56-byte record, and returned through `rfi`. The call therefore runs in normal
context with interruption delivery disabled. The BSP then claims and commits
the Notification, refreshes the empty TimerQueue through
`ProcessorRunController`, and publishes a separate 80-byte semantic record.

For the subsequent PL3 WaitSet suspension, the BSP arms the same Timer for a
second sequence and returns to idle. After the interrupt boundary,
the retained `DeferredTimerWorker` drains that exact Timer, signals the
registered Notification, and empties the queue. WaitSet polling consumes the
Notification claim only after exact Thread wake and ReadyQueue publication.
The resumed syscall returns the WaitSet tag and registration sequence to PL3.

This proves both direct and deferred TimerQueue-to-Notification composition. It
also supplies the one-shot AP-local composition used by the IA-64 `IdleServing`
timer-delivery request. That focused path drains exactly one retained binding,
then claims and commits the Notification while keeping the scheduler worker
blocked. It does not provide periodic timers, timer-driven AP scheduling,
scheduler preemption, or a general timer service.

## Verification

Hosted tests cover successful delivery, early claims, exact registry and
storage mismatches, Timer-state contradictions, Notification rejection,
pre-publication Pending retry, retirement when the claimed Timer was destroyed,
stale claim rejection, postpublication failure diagnostics, object-pin
lifetime, and repeated delivery. IA-64 and AMD64 O0/O2
target fixtures compile and link the production transaction and deferred worker
without hosted headers and enforce a 512-byte per-function stack limit. Deferred
worker tests additionally cover exact registration generations, duplicate and
capacity rejection, stale unregister, Empty/NotDue/work-limit stops, pending
claim restoration and retry after a transient pin failure, retirement of
expirations whose Timer or bound Notification was destroyed (with and without a
binding) followed by delivery of the next Timer, and terminal postpublication
failures.
