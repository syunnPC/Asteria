<!-- SPDX-License-Identifier: Apache-2.0 -->

# Timer Queue

## Scope

`TimerQueue` is the allocation-free architecture-neutral ordering layer between `Timer` objects and a
per-processor hardware-timer policy. It holds at most 32 entries, retains each exact `ObjectRegistry` generation, and
reports the smallest scheduled finite deadline. It does not program registers, send IPC, acknowledge a userspace
notification, or run in an interrupt handler.

Every admitted Timer uses the queue's exact `IMonotonicClock` instance. This makes deadlines directly comparable and
lets an early scan conclude that no later entry is due after sampling only the earliest Timer.

## Entry and delivery states

```text
Empty --Arm--> Scheduled --ClaimDue--> Claimed --CommitClaim--> Empty
                                  \--CancelClaim--> Pending --ClaimDue--> Claimed
Claimed --RetireClaim--> Empty (the Timer consumes the expiration and is Disarmed)
Scheduled --Arm--> Scheduled with a new Timer generation
Scheduled --Cancel--> Empty
```

Arm reserves a free slot and acquires a generation-safe registry pin before changing the Timer. A failed pin, clock,
deadline, or Timer transition leaves the queue and Timer unchanged. Re-arming an existing Scheduled entry replaces
its token and deadline in the same locked transaction. The entry's retained pin keeps a destroyed generation alive
but does not authorize new work, so a re-arm pins the generation again: a Timer whose registry generation is being
destroyed is rejected with `TimerPinRejected` exactly like a new arm, and its entry is unchanged. Timer-side
membership rejects admission by a second queue. `Cancel` removes a Scheduled generation; the Timer rejects the
Expired generation of a Pending entry, and the queue's Scheduled and Pending counts each follow the entry's own state.

`ClaimDue` gives pending deliveries priority, otherwise selects the smallest scheduled deadline. Equal deadlines are
ordered by slot index, lowest first, not by arrival: a new entry takes the lowest free slot, which an earlier
cancellation or delivery may have freed, and a re-armed entry keeps its slot. A newly due Timer becomes `Expired`
while its queue pin remains held by the claim. A retryable delivery failure returns the entry to `Pending`
(`CancelClaim`); that makes every copy of the claim stale, and the next `ClaimDue` of the entry issues a fresh claim
sequence, so an older copy cannot commit a later attempt. Successful delivery commits the claim, drops queue
membership, and releases the pin. `TimerNotificationDelivery` therefore takes an independent exact Timer pin before
committing and retains it through Timer acknowledgement.

An expiration that can never be delivered, because its Timer generation or the Notification generation its delivery
is bound to is being destroyed, is retired instead (`RetireClaim`). While the entry's pin still proves the Timer's
lifetime, the Timer consumes the expiration and returns to `Disarmed`, as a committed delivery followed by
acknowledgement would leave it; the entry and its pin are then released. The caller establishes that delivery is
impossible; `TimerNotificationDelivery` and `DeferredTimerWorker` retire exactly these cases and restore every other
failure. A Pending entry then always names an expiration that a later attempt can still deliver, so an undeliverable
one cannot block the queue, the processor's dispatch, or the collection of a destroyed Timer.

## Hardware boundary and synchronization

`NextDeadlineTicks` is zero when no Scheduled entry needs hardware service and otherwise names the exact earliest
finite deadline. Arm, Cancel, ClaimDue, and claim completion all return or expose the updated value.
`ProcessorRunController::RefreshTimerQueueDeadline` can reconcile that value through its embedded
`ProcessorDeadlineController` without inspecting queue storage or bypassing the common deadline policy.

All operations use bounded scans of 32 slots and one spin lock. They may spin and are forbidden from interrupt context
and same-CPU reentry. The hardware ISR only records or signals the interrupt; normal context performs `ClaimDue` and
notification delivery. The IA-64 BSP bootstrap proof performs that work immediately after its stackless polling leaf
returns. The narrower AP proof instead has the cooperatively polling `IdleServing`
context use a boot-lifetime `DeferredTimerWorker` after exact `0xe8` sequence two;
this is bounded normal-context delivery, not interrupt work, scheduler-worker
execution, or preemption. Queue destruction requires exclusive access, cancels
Scheduled entries, and releases Pending or Claimed entries without acknowledging them.

## Verification

Hosted tests cover registry, clock, type and instance rejection; atomic arm replacement and the rejected re-arm of a
destroyed generation; direct-transition exclusion; deadline ordering and ties; early and exhausted clocks; pending
retry, retirement of undeliverable expirations, and stale claims; exact cancellation receipts; quiescing-object pin
lifetime; cross-queue exclusion; all 32 slots and overflow; destructor cleanup; and concurrent mutation/snapshot
inspection. Debug uses ASan/UBSan and the Thread configuration uses TSan. IA-64 and AMD64 O0/O2
builds pin the value ABI, bound stack frames, and link the implementation without target runtime helpers.
