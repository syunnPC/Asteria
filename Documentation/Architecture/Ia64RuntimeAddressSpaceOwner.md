<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 runtime AddressSpace owner

`Ia64RuntimeAddressSpaceOwner` is a fixed boot-lifetime owner for the one
private initial-component AddressSpace. It is embedded inside
`Ia64BspRuntimeMemory`, before the runtime IVT admission boundary, so its
page-table backend, private ledger, region set, AddressSpace, pin, and retained
diagnostics are all inside the verified shared-Global Region-0 runtime-object
corridor.

The owner does not initialize an allocator, create a processor profile, write
region registers, activate a processor, or map through the backend directly.
Its `Prepare` operation receives the BSP owner's authoritative common profile,
single monotonic RID allocator, active-binding table, sealed initial
preparation, registry, issuer, and resource charge owner. It consumes the next
complete eight-RID receipt, initializes the backend with the same canonical
Region-0 seed as the common root, initializes the ordinary `AddressSpace`,
registers and pins it, and binds its typed registry identity.

Success requires the exact sequential receipt, allocator count increment and
next RID, seed mapping count, Ready identity, live registry binding, and zero
residency or pending transition. Regions 1 through 7 and the backend ledger
remain private; Region 0 remains the canonical shared-Global seed. The owner
never admits an AP. The BSP is its only permitted initial resident when the
shared `ProcessorAddressSpaceController` later selects it.

Preflight rejection leaves `Fresh` retryable. Once a RID receipt is issued,
all later failures are terminal `Failed`; possibly committed registry,
backend, RID, or identity state remains retained. There is no migration,
factory, demand paging, teardown, RID reuse, or AMD64 runtime wiring in this
owner. The AddressSpaces ObjectCreate creates and ObjectDestroy reclaims are
not this owner's: they are the [run-time slots](Ia64RuntimeAddressSpaceSlots.md).
