<!-- SPDX-License-Identifier: Apache-2.0 -->

# Per-CPU Ready Queue

## Scope

`ReadyQueue` is the architecture-neutral, fixed-capacity placement boundary between a runnable `Thread` plus its
`SchedulingContext` and `DispatchCoordinator`. Each instance belongs to one CPU and holds at most 32 entries. It
allocates no memory, embeds no ISA register state, and does not arm a timer or perform a context switch.

The queue retains the configured weight but does not implement dynamic weighted-fair accounting.

## Identity and lifetime

Enqueue accepts registry-origin-bound, generation-bearing identities for both objects and matching typed
references. It pins both exact generations, checks that the pinned instances are the supplied instances, and then
requires a `Ready` Thread and a bound, donated, or budget-exhausted context whose current Thread is that same
generation. A quiescing object, reused slot, foreign registry, wrong type, mismatched instance, or mismatched
context/thread pair is rejected without changing the queue.

Enqueue atomically acquires membership in both the concrete context and thread. Each generation can therefore belong
to at most one ready queue globally, even when two per-CPU schedulers race to place it. A failed second membership
acquisition rolls the first one back. The scheduler excludes direct lifecycle/context transitions while an entry is
queued or claimed, and the objects enforce the transitions they can refuse: `Thread::Terminate`,
`SchedulingContext::Unbind`, and `SchedulingContext::Donate` reject an object that holds a membership (ready,
claimed, or reserved) with `InReadyQueue` and change nothing. The dispatcher's own transitions (`BeginRun`,
`StartRun`) and the reply protocol's `ReturnDonation` cannot tell a ready entry from a claimed or reserved one; a
misuse of them on a ready entry is caught by `ClaimNext` revalidation. The scheduler also removes or completes the
entry before requesting registry destruction of either generation; a quiescing object is never newly enqueued. The
queue's registry and object storage outlive the queue and all claim copies. Queue operations may spin and are not
ISR-safe or same-CPU reentrant.

A synchronous reply may reserve an entry while its caller is still `Blocked`. The reservation pins the same exact
context/thread generations, verifies the block token, requires the caller to belong to the context's donation chain
(`ContextThreadMismatch` otherwise; it need not be the current thread yet), acquires both global queue memberships,
and allocates the FIFO sequence, but is invisible to `ClaimNext`. After the reply transaction returns any donation
and performs the exact `Thread::Wake`, `CommitReservedEnqueue` revalidates the runnable pair and exposes the entry
without another pin, capacity decision, or sequence allocation. Cancellation releases both memberships and pins. A
reserved entry cannot be removed through the ordinary ready-entry API.

## Placement and selection

The CPU affinity mask must include the queue CPU. `Isolated` entries additionally require an affinity mask naming
only that CPU. The queue has no clock: `ClaimNext(now)` ranks at the caller's sample of the processor clock.
Selection is deterministic:

1. `EmergencyRecovery`, `Isolated`, `DeadlineSporadic`, `WeightedFair`, then `Background`;
2. higher criticality;
3. higher static priority;
4. earlier effective deadline within `DeadlineSporadic`;
5. older enqueue sequence.

The effective deadline is `SchedulingContext::GetEffectiveDeadline(now)`: the deadline of the budget window
containing `now` while that window can still admit the context, otherwise the deadline of its next window. It
applies the lazy refresh that `StartRun` would perform without publishing it, so a context that waited past a period
boundary ranks by its current window rather than by the stale deadline of the window in which it was placed, and a
context whose budget or deadline in the current window is spent ranks by the window in which it can run again. The
queue caches each entry's effective deadline together with the sample that produced it; the value stays valid until
the sample reaches it, and every publication of the entry discards it.

Selection is bounded and deterministic. The queue does not implement dynamic weighted-fair virtual runtime,
load balancing, admission control across CPUs, or timer-driven preemption.
Budget-exhausted contexts may remain present so `StartRun` can perform its authoritative lazy time refresh. If the
replenishment time has not arrived, the caller cancels the claim and must delay reselection;
the queue does not busy-retry that entry.

## Claim transaction

`ClaimNext` revalidates candidates in rank order: the Thread must still be `Ready`, the context runnable, and that
Thread its current thread. A ready entry that fails this check was changed behind the queue; it is skipped for this
selection, stays `Ready` with its memberships and pins, and can be removed with `Remove`, so it never blocks the
other entries. `Empty` means that no ready entry revalidated. Structural damage to the queue's own entry, pins, or
memberships still reports `QueueCorrupt`. `ClaimNext` changes the selected entry from ready to claimed but retains
both pins. It returns a copyable view, which includes the effective deadline at the selection sample, plus a
queue-bound, monotonically generated claim token. The dispatcher can therefore acquire its own run pin before
`CommitClaim` removes the entry. If admission fails, `CancelClaim` restores the entry with its original FIFO rank.
Every new claim gets a distinct token, so a copied token from an earlier canceled claim cannot consume a later claim
of the same entry. `Remove` applies only to unclaimed entries.

`RequeueClaim` is the completion path for a Thread that remains Ready. It retains both memberships, pins, and the
fixed-capacity slot, refreshes cached scheduling inputs, assigns a new FIFO sequence, and consumes the old claim only
after every validation succeeds. Blocked and Exited completions instead use `CommitClaim` and release ownership.

## Verification

Hosted tests cover identity, registry, type, instance, lifecycle, context/thread, affinity, duplicate, full-queue,
and empty-queue failures; deterministic class/criticality/priority/deadline/FIFO selection, with effective deadlines
at selection samples on exact window edges; stale, foreign, canceled, and committed claims; blocked-entry reservation
(including callers outside the chain), cancellation, exact Wake publication, concurrent exact-once claiming; rejected
lifecycle changes of queued objects and skipped entries changed behind the queue; pin release at queue destruction;
and handoff through the production `DispatchCoordinator`. Debug uses ASan/UBSan, the Thread build uses TSan, and both
target builds compile and link the implementation at O0 and O2.
