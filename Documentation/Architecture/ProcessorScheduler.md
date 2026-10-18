<!-- SPDX-License-Identifier: Apache-2.0 -->

# Per-CPU Processor Scheduler

## Scope

`ProcessorScheduler` is the architecture-neutral policy owner for one logical CPU. It joins a fixed-capacity
`ReadyQueue` to one `DispatchCoordinator`, serializes placement and run transactions, and keeps the selected claim
alive until accounting completes. It allocates no memory and examines at most 32 candidates per dispatch attempt.

This layer chooses and accounts runnable work. It does not directly program a hardware timer, save registers, enter
an address space, or switch stacks. `ProcessorRunController` consumes its admission, installs the common processor
deadline, and only then exposes it to architecture code. The eventual Ready, Blocked, or Exited disposition returns
through that controller to `Finish`.

## Placement ownership

Enqueue acquires queue membership in both the exact `SchedulingContext` and `Thread`. Those memberships are shared
across all per-CPU queues, so the same context or thread generation cannot be placed on two CPUs, including under
concurrent enqueue attempts. A failed second acquisition rolls the first one back. Membership and both registry
pins remain held while an entry is Ready, temporarily claimed, or actively running. They are released only by
explicit removal or a successful Blocked/Exited completion; a Ready completion retains them while taking a new FIFO
position.

The registry, clock, scheduler, context, and thread lifetime rules remain explicit. Operations may spin and are not
interrupt-safe or same-CPU reentrant. This object does not implement migration.

## Dispatch policy

`DispatchNext` samples its clock once and ranks every candidate of that selection at the sample (deadline entries by
their effective deadlines, see [ReadyQueue](ReadyQueue.md)); an empty queue is reported without a sample. It claims
the best candidate and attempts one `DispatchCoordinator::Start` transaction, which takes its own admission sample.
A context whose budget is unavailable or whose current deadline has expired remains claimed temporarily while the
scheduler checks the next ranked candidate. So does a candidate whose Thread or SchedulingContext rejects admission
for another reason, such as a refused dispatch pin, a thread that cannot begin, or a context that rejects the sample
as reversed; the coordinator has rolled it back completely. All such claims are restored to their original FIFO
positions before the operation returns, so one unadmittable entry never hides the others. Failures of the
processor's own coordinator or clock (for example `Faulted`, `InvariantViolation`, or `ClockExhausted`) stop the
attempt, restore every reversible claim, and report `DispatchRejected`; a deferred context whose next replenishment
is unrepresentable (only near the end of the 64-bit tick range) stops it with `RetryCalculationFailed`.

If no candidate is admitted, the result is `NoRunnableThread`. Its `RetryAtTicks` is the earliest absolute
replenishment, strictly after the clock sample that rejected it, of the deferred candidates, and its `Dispatch` field
records the first candidate rejection, if any. A rejected candidate is examined again at the next dispatch. The
context computes this boundary without publishing its provisional lazy refresh, so a deadline rollback cannot return
a stale time in the past. A successful admission reports both the exact start sample and `StopAtTicks`, the absolute
minimum of its budget, deadline, and replenishment limits. Architecture code must not enter the selected context at
or after that boundary. The reserved infinite timestamp is rejected before admission publication rather than being
treated as an armable timer deadline.

When selection skips a higher-ranked budget-ineligible candidate before it
admits a lower-ranked peer, the admission also carries that candidate set's
earliest absolute replenishment. The processor deadline owner arms the minimum
of this retry, the active run stop, and timer-queue work so the replenished peer
can trigger prompt rescheduling instead of waiting for the lower-ranked run to
consume its full budget. Where the fired retry cannot preempt the running
thread, the owner's refresh drops it and re-arms the run stop and timer-queue
work, and the peer is selected at the next dispatch.

## Completion and faults

`Finish` first commits budget accounting and the Thread disposition. Ready then requeues the retained claim with
fresh configuration, deadline, and FIFO sequence. Blocked and Exited consume the claim and release its memberships
and pins. Only after that queue transition succeeds does the scheduler publish Idle.

An externally retained kernel frame uses the separate
`FinishExternallyStoppedAsReady` path. It accepts only Ready and supplies the
architectural counter value captured by interruption entry, so scheduler
accounting does not charge the later entry or continuation work and performs no
second clock read. The normal-context continuation still takes the scheduler
locks; it is not an ISR API. Its caller must first prove the exact active
Thread, SchedulingContext, CPU, and run sequence.

A clock or accounting failure that leaves the coordinator Active is retryable and retains the active claim. A queue
failure after accounting is not reversible: the scheduler becomes Faulted and deliberately retains the claim and
diagnostic state for CPU fail-stop handling. It never cancels a claim after the Thread or budget transition has
committed.

## Verification

Hosted tests cover Ready/Blocked/Exited lifecycles, Ready FIFO requeue, empty and active failures, clock exhaustion,
single and multiple budget deferrals, deadline rollback, earliest retry selection, absolute stop boundaries,
deadline ranking at the selection sample, skipped pin and time-reversal rejections, cross-CPU placement races, and
concurrent snapshots. Debug uses ASan/UBSan, the Thread build uses TSan, and both
target builds compile and link the scheduler at O0 and O2 for IA-64 and AMD64. Deadline and native-entry composition are covered by
[ProcessorRunController](ProcessorRunController.md) and
[ProcessorKernelRunLoop](ProcessorKernelRunLoop.md).
