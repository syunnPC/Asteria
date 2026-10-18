<!-- SPDX-License-Identifier: Apache-2.0 -->

# Endpoint Pool

`EndpointPool` owns the Endpoints that user code creates and destroys through the management ABI
([object management ABI](SystemCallAbi.md)). It holds forty-eight immovable slots of aligned storage in static
kernel memory; it never allocates. Each occupied slot records its Endpoint and the Endpoint's readiness Notification
([service IPC](IpcWireFormat.md)), both registered identities, a pin on the charged
`ResourceDomain`, and the reservation charged to it.

## Create

`Create` takes the caller's capability space, a capability in it, and an `EndpointConfiguration`:

1. The capability must authorize Manage on a ResourceDomain (`Lookup`); otherwise `CapabilityRejected` carries the
   capability error. A full pool is `CapacityExceeded`.
2. The pool pins that ResourceDomain and constructs the Endpoint in a free slot with the caller's space as receiver
   and the domain's identifier as charge owner, and beside it a Notification initialized with the same charge owner.
   A configuration the Endpoint rejects is `InvalidRequest`.
3. The domain is charged `MetadataBytes` (`sizeof(Endpoint) + sizeof(Notification)`) kernel-metadata bytes and the
   configuration's `MaximumQueuedBytes` IPC-queue bytes (`GetReservation`); a refusal is `ChargeRejected` with the
   domain's error.
4. The Endpoint and then the Notification are registered (`RegisteredObjectsPerSlot` is two, which the registry
   budget checks count), the Notification becomes the Endpoint's readiness (`AttachReadiness`), and
   `RootCapabilityIssuer::MintUnderAuthority` mints `CreatedRights` (Inspect,
   Invoke, Read, Transfer, Duplicate, Derive, Manage; never Revoke) into the caller's space, re-authorizing the
   ResourceDomain capability in the same critical section. The new capability lives in that capability's revocation
   domain and expires with it, and carries the request's sealed selector, zero for none
   ([service IPC](IpcWireFormat.md)).

Every failure undoes the earlier steps. A failed mint destroys and collects both registrations at once, since no
capability names them, then releases the charge and the pin (`MintRejected`). A rollback step that fails faults the pool
(`InvariantViolation`), and a faulted pool refuses every later operation.

## Destroy and collection

`Destroy` needs a capability with Manage on an Endpoint the pool owns (`NotOwned` otherwise). It makes the Endpoint and
its Notification quiescing: every capability to the Endpoint, in any space, now fails with `ObjectNotFound` and stays
closeable, and no WaitSet can bind it any more. The lease
`Destroy` takes ends when it returns, so a following `Collect` reclaims the slot unless another holder references the
Endpoint.

`Collect` asks the registry for each quiescing slot's exact identities (`CollectDestroyedObject(ObjectId)`): first the
Endpoint's, which waits for its last lease, then the Notification's, which waits until no WaitSet registration pins it
(destroying the WaitSet drops its registrations, and `ObjectManager` collects WaitSets before Endpoints). The
registry finalizes each identity once, so the slot records which it has collected and skips the slot while either is
still referenced. Once both are collected it discards unread messages, which closes the capabilities they
carry in the receiver space, releases the charge, ends both lifetimes, resets the pins, and frees the slot.
Its owner decides when to collect; `ObjectManager` collects before every creation and destruction and again after
each destruction.

The receiver space of every created Endpoint must outlive it until collection; teardown of a domain must
destroy and collect its Endpoints first.

## Serialization

The pool holds its own lock while it calls the shared registry, capability-space and Endpoint operations and the
ResourceDomain ledger. None of them calls back into the pool, so the order is pool, then shared object
synchronization or the ledger ([lock order](LockOrder.md)). Calls may spin and are forbidden from interrupt context
and same-CPU reentry.

## Readiness

The Endpoint signals its readiness Notification, a lock-free exchange, whenever it publishes a message for the receiver
and whenever a receive leaves messages queued. A WaitSet that claims the signal and receives one message is therefore
signalled again while messages remain: readiness is level-triggered and no wakeup is lost. A receiver that wakes may
still find the queue empty (two messages received after one claim leave one stale signal), so it receives until
`WouldBlock` and then waits again. Boot Endpoints are not pool slots and have no readiness Notification; they signal
nothing.
