<!-- SPDX-License-Identifier: Apache-2.0 -->

# WaitSet Pool

`WaitSetPool` owns the WaitSets that user code creates and destroys through the management ABI, ObjectCreate and
ObjectDestroy type 12 ([service IPC](IpcWireFormat.md)). It holds sixteen immovable slots of
aligned storage in static kernel memory and never allocates. Each occupied slot records its WaitSet, the registered
identity, and a pin on the charged `ResourceDomain`.

## Create

The request is eight zero bytes (`Abi::ObjectCreateWaitSetRequestSizeBytes`). The authority capability must allow
Manage on a ResourceDomain (`CapabilityRejected` otherwise), and a full pool is `CapacityExceeded`. The pool pins the
domain, constructs the WaitSet in a free slot, initializes it with the domain as charge owner, and charges the domain
`sizeof(WaitSet)` kernel-metadata bytes; a refusal is `ChargeRejected`. It then registers the WaitSet and mints
`CreatedRights` into the caller's space: Inspect, Wait, Transfer, Duplicate, Derive and Manage, never Revoke. A failed
mint destroys and collects the registration at once, then releases the charge and the pin. A rollback step that fails
faults the pool, which then refuses every operation.

## Destroy and collection

`Destroy` needs Manage on a WaitSet the pool owns. A WaitSet the kernel built for itself, such as the boot pager's,
is `NotOwned`. The WaitSet becomes quiescing: every capability to it fails with `ObjectNotFound` and stays closeable.
A waiter already blocked on it keeps a pin and still completes, by readiness, expiry or abort, and its pin goes with
the completion.

`Collect` reclaims each quiescing slot once the registry finalizes its identity. Ending the WaitSet's lifetime
detaches every registered Notification and drops its pin, so the readiness Notification of an Endpoint destroyed
earlier becomes collectable. `ObjectManager` therefore collects WaitSets after threads and before Endpoints.

## Serialization

The pool holds its own lock while it calls the shared registry and capability-space operations, the WaitSet's
initialization and destructor, and the ResourceDomain ledger. None of them calls back into the pool, so the order is
pool, then the WaitSet, the shared object lock or the ledger ([lock order](LockOrder.md)).
