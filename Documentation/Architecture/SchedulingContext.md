<!-- SPDX-License-Identifier: Apache-2.0 -->

# Scheduling Context

## Scope

`SchedulingContext` is the ISA-neutral CPU-budget object. It separates the right to consume processor time
from a `ThreadId`, retains the resource domain charged for that time, and allows one bounded call chain to lend the
same budget to passive servers. The object allocates no memory and supports at most four processors.

This object is an accounting and admission state machine. It binds the production architecture-neutral `Thread`
lifecycle object and is placed by the fixed-capacity per-CPU `ProcessorScheduler`, but does not provide saved register
contexts, timer interrupts, context switching, timeout faults, or the dynamic weighted-fair service ledger. The five
scheduling classes are validated policy inputs; the current object applies the same bounded periodic budget
mechanism to each class and therefore does not claim a complete sporadic-server implementation.

## Configuration and lifetime

Initialization accepts one nonzero resource-domain owner and fixes these immutable parameters:

- nonzero budget, relative deadline, and period with `budget <= deadline <= period`;
- nonzero scheduling weight plus priority, class, and criticality metadata;
- a nonempty affinity mask containing only CPU IDs 0 through 3.

The initial period begins at the supplied monotonic timestamp. Time additions must be representable. A failed
initialization publishes nothing and can be retried; a successful initialization is permanent. The charge owner is
published by one lock-free release store and is subsequently immutable. The metadata accessor uses an acquire load,
so a concurrent pre-publication reader observes either the invalid owner or the complete owner, never a torn value,
and never waits for the state lock.

Construction binds the context to one `ObjectRegistry`. That registry outlives the context, every retained pin, and
every `ThreadId` copied from a dispatch, receipt, or snapshot. This lifetime also prevents registry-address reuse
from turning a stale provenance pointer into a new registry identity. State operations acquire the context lock
before any registry lock; registry operations do not call back into the context while holding their lock.
Destruction is allowed only after the context is unreachable by dispatch and releases every remaining thread pin
through its members.

## Binding and donation

An initialized context binds to one base thread. `Donate(caller, server)` appends a server only when the caller is the
current chain tip. `ReturnDonation(server)` removes only that same tip. The chain is LIFO, rejects repeated thread IDs
as cycles, and permits at most eight donation edges. Binding, unbinding, donation, and return are forbidden while a
run is active. `Unbind` and `Donate` also reject a context that holds a ready-queue membership (a ready, claimed, or
reserved entry) with `InReadyQueue`, because the entry names the current thread. `ReturnDonation` stays permitted
there: a reply reserves its blocked caller's entry while the context is still donated, returns the donation, and
only then commits the entry, which revalidates the pair. This provides the ownership rule used by synchronous IPC.

`ThreadId` combines its originating registry with a generation-bearing `ObjectId`; it is not an authenticated
user-space handle. Every operation rejects an identity from a different registry even when both registries issued
the same raw object number. `Bind` and `Donate` require the exact generation to be alive and of
`ObjectType::Thread`, then retain it with a private `ObjectPin`. A quiescing or collected generation rejects new
edges; registry-slot reuse produces a distinct identity. `Unbind`,
`ReturnDonation`, and context destruction release the corresponding pins. The IPC dispatcher is
responsible for authorizing caller/server relationships before asking this lifetime primitive to install them.
The strong-reference graph points from a scheduling context to its thread objects. The current `Thread` does not
retain a context; a strong pin back to the same context would create an ownership cycle.

The externally visible states are uninitialized, unbound, bound, donated, running, and budget exhausted. State and
accounting methods serialize on the context spin lock; callers must prevent same-CPU reentry and must not call them
from interrupt context.

## Dispatch and accounting

`StartRun` admits only the current chain tip on an allowed CPU. It rejects decreasing time, exhausted budget, and an
expired deadline. A successful dispatch returns the maximum interval bounded by all three remaining limits: budget,
absolute deadline, and replenishment boundary. Every rejected dispatch leaves both the visible window and its
internal monotonic watermark unchanged.

`GetEffectiveDeadline(now)` is the read-only ranking input of the ready queue. It applies the same lazy refresh
without publishing it and returns the deadline of the window containing `now` if that window still has budget and
`now` precedes its deadline, which is exactly when `StartRun` would admit the budget; otherwise it returns the
deadline of the next window, in which the budget is available again. `MonotonicTime::Infinite()` means that no such
window is representable or that the context is uninitialized. While the context state is unchanged, the result is
constant for every later sample below it.

`EndRun` must name the CPU that started the run. Its receipt distinguishes:

- `ElapsedTicks`: all processor time observed between start and stop;
- `ChargedTicks`: the portion removed from the budget admitted at start;
- `OverrunTicks`: time executed beyond that admission;
- `DeadlineMissed`: comparison against the deadline fixed by that dispatch.

Resource-domain CPU charging must use `ElapsedTicks`; `ChargedTicks` is only budget-window consumption
and excludes overruns. A run that is stopped after a
replenishment boundary consumed only the time since that boundary: the current period's budget is reduced by that
overrun, so a stop exactly on the boundary leaves it whole, and a stop interrupt that arrives a few ticks late costs
those ticks rather than the period. A run that covered a whole period after the boundary was never stopped by its
budget, so the current period is unavailable until the next boundary.

Missed periods collapse directly to the current window. The freestanding implementation computes the period
remainder with a fixed 64-step unsigned algorithm, avoiding compiler division helpers on IA-64. `StartRun` and
`GetNextReplenishmentAfter` reject a reversed sample or an unrepresentable window without wrapping the clock.
`EndRun` cannot refuse time that has already elapsed: when the window after the stop is unrepresentable, it still
completes the run and leaves the context without a future window, with zero budget and the reserved `Infinite`
value as both deadline and replenishment boundary. Such a context is never admitted again, its next replenishment
reports `TimeRangeExhausted`, and its effective deadline is `Infinite`. The all-ones timestamp is reserved for
`MonotonicTime::Infinite()` and is never accepted or returned as a finite deadline or replenishment boundary.

## Verification boundary

Hosted tests cover configuration failure atomicity, the complete donation depth and cycle rules, CPU affinity,
deadline and overrun behavior, boundary and multi-period replenishment, near-`UInt64` exhaustion, effective
deadlines, rejection of unbinding and donation while queued, and lock-free metadata publication under concurrent
reads. They also verify quiescing and stale generations, wrong object types, slot reuse, donation-pin release, and
context-destruction release. A 4,096-case property test compares the target remainder algorithm with host division
across full-width timestamps. Debug uses ASan/UBSan, the Thread configuration uses TSan, and the target gate compiles
and links the implementation at O0 and O2 for IA-64 and AMD64 without runtime arithmetic helpers.

`ProcessorScheduler` retains and deterministically selects runnable context/thread pairs, couples the selected pair
through `DispatchCoordinator` with one monotonic sample and reversible start ordering, and returns absolute stop and
retry boundaries. `ReplyToken` retains one blocked caller and drives donation/return as a one-shot state machine.
`SynchronousReplyDispatcher` joins it to Endpoint preparation/publication and exact ReadyQueue Wake/enqueue;
the dispatcher does not implement syscall authorization or deadline cancellation.
