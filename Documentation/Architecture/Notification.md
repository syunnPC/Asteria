<!-- SPDX-License-Identifier: Apache-2.0 -->

# Coalescing Notification

## Scope

`Notification` is the architecture-independent kernel object behind
`ObjectType::Notification`. It represents one coalescing event bit. It does not
queue a count or payload, choose a waiting thread, allocate memory, or perform a
syscall authorization check. `WaitSet` supplies the separate fixed-capacity
registration and readiness-claim layer. `TimerNotificationDelivery` supplies
the normal-context transaction that signals one exact Notification generation
from a due TimerQueue claim.

## State and claim transaction

```text
Uninitialized --Initialize--> Ready
Ready + Signal --------------------------> Ready(pending)
Ready(pending) --Claim--> Claimed
Claimed --CommitClaim--> Ready
Claimed --CancelClaim--> Ready(pending)
```

Repeated signals coalesce. `Claim` atomically removes the current pending bit
and returns a nonzero generation token bound to the exact immovable object.
Exactly one matching commit or cancel resolves that claim. A signal arriving
while a claim is active remains pending for the next claim. Cancellation merges
the claimed event back into that pending bit. Foreign, stale, replayed, and
default tokens cannot alter state, and generation exhaustion never wraps.

## Synchronization and lifetime

`Signal` is the sole interrupt-safe operation. It performs one always-lock-free
64-bit acquire-release exchange and never allocates, blocks, loops, or invokes a
callback. Acquire-release chaining across coalesced producers ensures that a
successful `Claim` observes the writes published before every signal in that
coalesced batch.

Claim, commit, cancel, initialization, and snapshot inspection serialize on one
spin lock and are not ISR operations. The pending bit is separate from the
claimed generation, so producers never acquire that lock. The immutable nonzero
resource-domain owner is release-published last during initialization and makes
the metadata accessors safe before registry publication.

A Notification may belong to at most one WaitSet. Registration holds an exact
registry-generation pin and transfers claim/resolve ownership to that WaitSet;
direct Claim, CommitClaim, and CancelClaim reject while attached. Signal remains
available and lock-free throughout membership.

Timer delivery pins the exact Notification generation and verifies its storage
identity before inspecting state or calling `Signal`. Each retained delivery
transaction is externally serialized across CPUs; the Notification's lock-free
producer path does not make the transaction object's pins and diagnostics
shareable.

The Notification remains alive and immovable until registry collection returns
it and outlives every claim-token copy. Destruction excludes concurrent
operations and active claims. Producers must first be stopped; only then may a
remaining coalesced pending event be deliberately abandoned.

## Verification

Hosted tests cover unpublished metadata, registry lifetime, coalescing, signals
during a claim, commit/cancel replay, foreign tokens, generation arithmetic,
concurrent producers, and competing consumers. Debug uses ASan/UBSan and the
Thread configuration uses TSan. IA-64 and AMD64 O0/O2 builds pin the public
value ABI and require helper-free target linkage.
