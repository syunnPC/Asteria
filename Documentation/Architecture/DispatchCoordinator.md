<!-- SPDX-License-Identifier: Apache-2.0 -->

# Dispatch Coordinator

## Scope

`DispatchCoordinator` is the architecture-neutral transaction joining one production `Thread` run to one
`SchedulingContext` budget run. It does not choose a runnable object, authorize a capability, arm a timer, or save
ISA registers. The per-CPU `ProcessorScheduler` retains the selected `ReadyQueue` claim for the complete run while
this coordinator holds its additional dispatch pin, keeping the transaction shared by IA-64 and AMD64.

The coordinator is constructed with one object registry and one bounded, lock-free monotonic clock. `Start` requires
the caller-authorized `ThreadId`, the matching typed `Thread` reference, the selected `SchedulingContext`, and one
valid CPU. It acquires a generation-specific `ObjectPin` before touching the Thread, verifies that the pinned object
is the supplied concrete instance, and retains that pin until completion.

## Start transaction

Start samples the clock once, admits the Thread first, and then admits the budget at that same timestamp:

```text
pin exact Thread generation
sample monotonic clock
Thread Ready -> Running
SchedulingContext Bound/Donated -> Running
publish Active coordinator and return that exact start sample
```

The Thread transition is the reversible side. If budget admission rejects affinity, identity, time, deadline, or
budget, the coordinator returns the Thread to `Ready`, releases its temporary pin, and remains `Idle`. Pin, clock,
and Thread admission failures occur before any budget state is committed. Direct transitions on either owned state
machine are excluded while a coordinator operation or active dispatch uses them.

## Finish transaction

Finish validates the requested disposition before observing time. A bad or exhausted clock and any recoverable
accounting failure leave both state machines running so the caller can retry with a valid sample. Once accounting
succeeds, the coordinator commits exactly one `Ready`, `Blocked`, or `Exited` Thread disposition and releases its
dispatch pin:

```text
sample monotonic clock
SchedulingContext Running -> stopped and charged
Thread Running -> Ready / Blocked / Exited
release dispatch pin
publish Idle coordinator
```

The irreversible `Exited` transition therefore cannot precede successful accounting. A failure after accounting is
an internal invariant violation: the coordinator retains the pinned diagnostic state and becomes `Faulted`; the
enclosing processor scheduler must fail-stop that CPU rather than attempt recovery.

## Concurrency and lifetime

Coordinator operations serialize on their own spin lock, may spin, and are not ISR-safe or same-CPU reentrant. The
object registry and monotonic clock outlive the coordinator. After `Start` succeeds, the exact caller-owned
`SchedulingContext` storage remains alive and immovable until successful `Finish` or terminal fault handling; its own
destructor contract independently forbids destruction while a run or dispatcher reference is active. The Thread is
held by a registry pin for the same interval. The coordinator must be `Idle` before destruction. The enclosing
`ProcessorScheduler` lock excludes direct state-machine calls, ready-queue mutations, and coordinator operations on
the same entry throughout each CPU-local transaction.

## Verification

Hosted tests cover successful admission and accounting, Ready/Blocked/Exited completion, identity and concrete
instance matching, wrong type and quiescing pin failures, context-rejection rollback, reversed-clock retry,
single-active serialization, and release of temporary lifetime pins. Processor-scheduler integration additionally
verifies that the queue retains both objects through `Finish`, reports the exact start sample as an absolute stop
boundary, and requeues a Ready completion without a lifetime gap. Debug uses ASan/UBSan, the Thread configuration
uses TSan, and both target builds compile and link the implementation at O0 and O2.
