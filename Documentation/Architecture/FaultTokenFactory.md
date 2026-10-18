<!-- SPDX-License-Identifier: Apache-2.0 -->

# Fault Token Factory

`FaultTokenFactory` is the allocation-free ownership boundary for transient
page-fault tokens. It owns sixteen immovable `FaultToken` objects, registers one
free slot in the shared object registry, and mints exactly `Invoke|Transfer`
authority into the trusted kernel source capability space. It does not decode
exceptions, block a Thread, send pager IPC, or edit page tables.

Creation validates and initializes the token before registry publication. If
registration fails, the slot is reset immediately. If capability minting fails,
the newly registered object is moved to the registry's quiescing state; no
capability is published, and the slot remains unavailable until the central
collector returns its exact `ObjectDestructionRecord` to `Reclaim`.

The higher fault owner calls `Retire` only after excluding claims, dispatcher
operations, and direct token lifecycle changes. `Retire` accepts Pending tokens
for abandoned delivery as well as Resolved tokens, prevents new registry
lookups, and waits indirectly for all retained leases and pins through normal
registry collection. Capability slots may still contain the stale object
generation, but subsequent lookup cannot resolve it and the capability-space
owner remains responsible for closing those handles.

Reclaim resets token metadata and releases its retained Thread and AddressSpace
pins only after collection proves that no external object reference remains.
The per-storage claim sequence is deliberately not reset. A later use therefore
gets both a new registry object generation and a new resolver-claim generation;
an old claim cannot become valid when the same physical slot is reused. A slot
whose claim sequence reaches its all-ones terminal value is retired permanently.

The factory holds its own bounded lock while it calls the shared registry and
capability-space operations. No registry path calls back into the factory, so
the lock order is factory, then the token it arms, then shared object
synchronization ([lock order](LockOrder.md)). Calls may spin and
are forbidden from interrupt context and same-CPU reentry.
