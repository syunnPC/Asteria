<!-- SPDX-License-Identifier: Apache-2.0 -->

# Monotonic Timer

## Scope

`Timer` is the architecture-neutral kernel object behind `ObjectType::Timer`. It owns one finite one-shot deadline
in the boot clock's tick domain. It does not program an ISA timer, allocate memory, deliver an IPC message, or choose
a CPU. `TimerQueue` retains armed objects, orders their deadlines, and supplies the next finite boundary to the
per-CPU deadline policy. `TimerNotificationDelivery` connects a due queue claim
to one exact Notification in normal context.

One-shot behavior is the nucleus primitive. Periodic user timers rearm after delivery, so the kernel object does not
need catch-up loops, hidden drift policy, or unbounded missed-period accounting.

## State and generation

The state machine is:

```text
Uninitialized --Initialize--> Disarmed --ArmAt--> Armed
Armed --ArmAt--> Armed with a new generation
Armed --Cancel--> Disarmed
Armed --TryExpire(deadline reached)--> Expired --Acknowledge--> Disarmed
Expired --retirement by its TimerQueue (undeliverable expiration)--> Disarmed
```

`ArmAt` accepts only a finite absolute deadline strictly after its one clock sample. Replacing an armed deadline is
one locked transaction. Every successful arm advances a nonzero 64-bit generation and returns a `TimerToken` bound
to both that generation and the exact immovable `Timer`. A delayed event carrying an older token is rejected and
cannot expire or cancel a newer arm. Generation exhaustion fails without wrapping. The pointer-bearing token is a
kernel-internal queue value and is never serialized or returned through the syscall ABI.

Expiry remains pending until acknowledged. While `Expired`, rearm and cancellation are rejected so notification
delivery cannot silently discard an already observed event. The one exception is an expiration that can never be
delivered because the Timer's own generation, or the Notification generation its delivery is bound to, is being
destroyed: its `TimerQueue` retires it, consuming the expiration exactly as acknowledgement would, so a destroyed
Timer still reaches the `Disarmed` state its destruction requires. An early `TryExpire` reports the exact observed
tick and preserves every state byte. The all-ones clock value is reserved as infinite and is never accepted as a
deadline or successful clock observation.

## Lifetime and synchronization

Initialization publishes one immutable nonzero resource-domain owner through the lock-free metadata word required by
`ObjectRegistry`. A registered Timer remains alive and immovable until registry collection returns it. The originating
clock outlives the Timer, and the Timer outlives every use of its tokens.

Once admitted to a `TimerQueue`, direct arm, cancel, expire, and acknowledge operations are rejected. The queue owns
those transitions until cancellation or committed delivery releases membership. This prevents one Timer generation
from being placed in two processor queues. Delivery takes a second exact
registry pin before committing the queue claim, then retains that pin until it
acknowledges the same expiration token.

State operations serialize on one spin lock, may spin, and are not interrupt handlers. The injected clock operation
is bounded, nonblocking, allocation-free, callback-free, lock-free, and safe while the Timer lock is held. Destruction
requires `Uninitialized` or `Disarmed` and excludes concurrent operations and token use.

## Verification

Hosted tests cover unpublished behavior, registry publication, invalid and reached deadlines, atomic replacement,
foreign and stale tokens, early and exact expiry, pending-delivery exclusion, acknowledgement, cancellation, clock
exhaustion, sequence boundaries, and concurrent mutation/snapshot inspection. Debug uses ASan/UBSan and the Thread
configuration uses TSan. IA-64 and AMD64 O0/O2 builds pin the public value ABI and compile/link the implementation
without target runtime helpers.
