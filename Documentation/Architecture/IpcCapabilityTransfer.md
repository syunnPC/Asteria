<!-- SPDX-License-Identifier: Apache-2.0 -->

# Atomic IPC Capability Transfer

## Scope

The capability layer provides one fixed-capacity transaction for the at-most-four attachments carried by the typed
IPC envelope. It does not enqueue a message by itself. `Endpoint` calls its locked form only after proving queue
capacity, so capability and message publication share one lock linearization interval.

Each request contains a source handle, requested rights, requested absolute monotonic deadline, expected object type,
and either `Move` or `Copy`. The request view is non-owning, stable for the call, and limited to four elements. A
zero-count view is a canonical successful no-op: it inspects neither space, their registries, lock domain, nor the
clock, so it succeeds whatever the two spaces are.

The locked form takes one clock sample from its caller. Endpoint samples once per send, checks the message deadline
against that sample, and passes it on, so one sample governs message expiry, every attachment lease check, and the
invoking capability of an invocation. The public batch API samples once itself.

## Preflight

The source and destination spaces must share the same object registry, revocation registry, monotonic clock, and
spin lock. One critical section then validates every request before changing a slot:

- source handle authenticator, generation, and slot occupancy;
- current revocation lineage and unexpired source lease;
- `Transfer` for move or `Duplicate` for copy;
- exact expected object type and live object-registry metadata;
- nonempty known requested rights that are a subset of source rights, and for a copy without `Revoke`, since a copy
  shares its source's revocation domain and could otherwise revoke the sender's own handle;
- requested deadline that is live and does not extend the source lease;
- zero reserved field and a known disposition;
- unique source slot within the batch;
- one usable destination slot per request.

Destination slots are chosen deterministically in ascending order by one bounded slot-table pass. For a same-space
move, its source slot may be reserved as a destination after generation invalidation, unless the move is held (see
below). A same-space copy still requires a distinct free slot.
Slots whose generation would retire on close are never reserved for reuse.

## Commit and failure behavior

After successful preflight, all moved source entries are invalidated and generation-advanced, then all destination
entries are installed with the captured revocation epoch and attenuated descriptor. No fallible operation remains in
this phase. Returned handles use the destination space's identifier, validation keys, target slot, and current
generation; unused handle and reserved storage is zero.

A prepared Endpoint send commits its moves in a held form instead. Each moved source keeps its slot, generation,
revocation epoch, and descriptor and stays counted as active, but no handle resolves to it, so every operation on it,
including `Close`, reports `InvalidHandle`, and no transfer chooses it as a destination. The Endpoint later either
releases the held sources, exactly like the invalidation above, when it publishes the message, or restores them under
their unchanged handles when the preparation is cancelled. A move thus removes its source only on a successful send.

Any validation or capacity failure occurs before the commit phase. It therefore leaves every source and destination
slot unchanged: no source disappears, no destination handle becomes visible, and no partial batch can be observed.
The shared monotonic-clock watermarks may advance during a failed nonempty call; they never move backward.
Concurrent batches in the same registry domain serialize on the shared lock. Callers must enforce the documented
non-ISR, non-reentrant, same-CPU preemption contract of that lock.
