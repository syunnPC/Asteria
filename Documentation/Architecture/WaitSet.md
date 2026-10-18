<!-- SPDX-License-Identifier: Apache-2.0 -->

# Notification Wait Set

## Scope

`WaitSet` is the architecture-independent readiness aggregator behind
`ObjectType::WaitSet`. It retains up to 32 exact `Notification` generations and
returns caller-defined 64-bit tags in bounded round-robin order. The separate
`WaitSetThreadDispatcher` composes that claim boundary with an exact running
Thread, SchedulingContext, scheduler-owned ReadyQueue, and syscall 8. It does
not allocate storage or aggregate Endpoint and Timer readiness directly.

## Registration and lifetime

Each successful registration verifies `NotificationId` registry provenance,
pins that exact generation, checks the supplied object address, and then claims
exclusive WaitSet membership inside the Notification. Competing WaitSets cannot
both attach. Direct Notification claim operations reject while attached, while
ISR-safe Signal remains available.

Registration returns a pointer-and-sequence token. Unregister detaches first and
then releases the pin; stale, foreign, default, or replayed tokens change
nothing. The global nonwrapping sequence prevents slot-reuse ABA. Destruction
detaches every remaining entry and requires completed collection for a
registered WaitSet, no concurrent operation, and no outstanding token use.

## Readiness transaction

```text
Ready + pending source --ClaimReady--> Claimed
Claimed --CommitClaim--> Ready
Claimed --CancelClaim--> Ready + source pending
```

Only one WaitSet claim may be active. ClaimReady scans at most 32 entries from a
rotating cursor. It claims the source Notification before publishing the
WaitSet token, and no later step in that success path can fail. Commit consumes
the source claim. Cancel atomically restores it to the source pending bit.
Signals arriving during either transaction stay pending for a later scan.

All WaitSet operations serialize on its spin lock and use the fixed lock order
`WaitSet -> Notification`; registration temporarily calls ObjectRegistry before
entering the Notification lock. Signal takes neither lock. Polling may report
WouldBlock when a concurrent signal falls after its source was scanned; that
signal remains pending for the next call.

## Thread wait transaction

`WaitSetThreadDispatcher::Prepare` pins the exact WaitSet, running Thread, and
running SchedulingContext generations, captures their run correlation, and
allocates a nonwrapping transaction generation. Immediate readiness completes
without blocking. Otherwise the dispatcher retains a `Prepared` receipt while
the owning run loop finishes that exact run as `Blocked`.

`CommitBlockedFromFinishedRun` accepts only the scheduler-created block token
for the captured run. It rechecks readiness after the block commit, closing the
signal-before-block interval. A wake reserves scheduler queue capacity before
changing Thread state, commits the exact wake generation and queue reservation,
then consumes the WaitSet claim. Queue pressure cancels the claim back to the
pending source and retains the blocked transaction for bounded `Poll` retry.
Any contradiction after Thread wake is terminal because rollback could no
longer restore the old lifecycle.

Prepared and completed receipts carry both the stable dispatcher instance and
one nonzero transaction generation. Exact consume and rollback operations
reject stale receipts from an earlier transaction or an identically configured
dispatcher. Generations never wrap; exhaustion is explicit. Completion remains
retained until the first exact resumed Thread/SchedulingContext correlation
consumes it, returning the selected 64-bit tag and registration sequence.

Syscall 8 accepts only a capability handle authorizing `Wait` on that WaitSet;
Thread and SchedulingContext identities always come from the owning processor's
active run-loop binding. A prepared suspension releases the capability lease
while the dispatcher-owned pins retain lifetimes. The resumed path accepts no
user handle and repeats no authority lookup.

## Verification

Hosted tests cover initialization, registry provenance, pointer mismatch,
quiescing objects, pin lifetime, single global membership, registration and
claim ABA, round-robin ordering, signal-during-claim persistence, cancellation,
the 32-entry boundary, teardown, concurrent registration/claim races, exact
block-token correlation, queue-pressure retry, stale and cross-dispatcher
receipts, nonwrapping transaction exhaustion, syscall authorization, and exact
resumed completion. Debug uses ASan/UBSan and Thread uses TSan. IA-64 and AMD64
O0/O2 builds check the public value ABI, bounded frames, and helper-free linkage.
