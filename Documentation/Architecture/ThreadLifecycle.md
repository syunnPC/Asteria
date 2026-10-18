<!-- SPDX-License-Identifier: Apache-2.0 -->

# Thread Lifecycle

## Scope

`Thread` is the architecture-neutral lifecycle object behind `ObjectType::Thread`. It provides a bounded state
machine, immutable resource-domain ownership, exclusive CPU-run ownership, and generation-specific blocking. It does
not contain saved registers, an address-space binding, a kernel stack, queue links, or architecture switch code.
Those remain separate so the common lifecycle does not encode an AMD64 or IA-64 register layout.

## Publication and lifetime

A default object is uninitialized and cannot enter any runnable state. `Initialize` accepts one nonzero resource
domain exactly once. Invalid initialization is failure-atomic and retryable. The owner is published last with a
release store and is read through the lock-free acquire metadata accessor required by `ObjectRegistry`.

The object is immovable after registration. Destruction requires no active run, no concurrent access, no dispatcher
or wait-queue reference, and no remaining `ThreadBlockToken` use. This lifetime prevents a reused storage address
from validating a token issued by a prior object.

Lifecycle and snapshot operations serialize on the thread spin lock, may spin, and are forbidden from interrupt
context or same-CPU reentry while that lock may be held. The two immutable registry metadata accessors are lock-free,
bounded, and safe before publication.

## State machine

The state transitions are:

```text
Uninitialized --Initialize--> Dormant --MakeReady--> Ready
Ready --BeginRun(cpu)--> Running
Running --FinishRun(Ready)--> Ready
Running --FinishRun(Blocked)--> Blocked --Wake(token)--> Ready
Running --FinishRun(Exited)--> Exited
Dormant/Ready/Blocked --Terminate--> Exited
```

`Exited` is terminal. One valid CPU owns `Running`; completion on another CPU is rejected without changing state.
External termination cannot race a running dispatch, and `Terminate` rejects a thread that holds a ready-queue
membership (a ready, claimed, or reserved entry) with `InReadyQueue`: its owner removes the placement first, so an
entry never names an exited thread. Non-exit completion must not carry an exit code, and every failed transition
preserves the complete snapshot.

Each blocked completion increments a 64-bit sequence and returns a token bound to both that sequence and the exact
immovable `Thread`. A stale token cannot wake a later block instance, and a token from another thread is rejected
even when both sequences have the same number. Sequence exhaustion leaves the thread running and must be handled as
a terminal scheduler failure rather than wrapping.

## Scheduler boundary

`SchedulingContext` retains production `Thread` objects by registry origin and generation. Each context and thread
also carries one private ready-queue membership, preventing cross-CPU duplicate placement. `ProcessorScheduler`
retains the pair's claim through the active run, while `DispatchCoordinator` commits the two state machines as one
transaction, owns the clock sample, and rolls back the reversible Thread admission if budget admission fails. Direct
state-machine calls are excluded while an entry is queued, claimed, or active; `Terminate` enforces this itself, and
the ready queue skips an entry whose thread was changed behind it. `ProcessorRunController` arms the minimum run,
timer-queue, and scheduler-retry boundary before publishing a run. `ProcessorKernelRunLoop` binds that exact retained
Thread to a prepared AMD64 or IA-64 native context, performs the cooperative switch, and returns all Ready, Blocked,
or Exited accounting through the same controller.

A thread whose fault its pager resolves with `FaultResolve` Terminate
is terminated while Blocked in that fault, through
`PagerFaultDispatcher`. It is in no ready queue and cannot be enqueued, so it is never admitted again, but its
run-loop binding and SchedulingContext stay as they are (`ProcessorKernelRunLoop::Unbind` needs the loop idle, which
the pager's system call is not), and its kernel context stays suspended inside `FinishFaultAndDispatch`. Its domain's
teardown will release them.

## Verification

Hosted tests cover initialization and registry publication, all ordinary run dispositions, CPU mismatch, invalid
dispositions, terminal exit, external termination (the ready-queue model also rejects it for queued threads), stale
and cross-thread wake tokens, 4,096 sequential block generations, and concurrent snapshot inspection.
Scheduling-context tests use the production `Thread` class rather than a type-only dummy. Debug uses ASan/UBSan, the
Thread configuration uses TSan, and target checks compile and link the implementation at O0 and O2 for both IA-64 and
AMD64.
