<!-- SPDX-License-Identifier: Apache-2.0 -->

# Device-region authority

`DeviceRegion` is the architecture-neutral authority for one bounded hardware
register range. A region is either memory-mapped I/O or port I/O and publishes
an immutable base, length, read/write policy, supported access widths, and
cache policy. The common object validates and retains an operation; target HAL
code performs the actual volatile or port instruction.

The platform-discovery path supplies candidates to the fixed-capacity
`DeviceManager`. The current manager owns 32 immovable region slots plus 256
separate PCI-function slots and is the sole issuer for these discovered MMIO,
port-I/O, and PCI authorities. It rejects
overlapping live or quiescing ranges within the same address-space kind;
MMIO and port-I/O remain separate address spaces. `DeviceRegion` does not probe
hardware and does not infer ownership from an address supplied by a driver.
Its registry generation, concrete instance, and originating registry are
checked independently whenever a relationship is installed.

Creation registers the exact slot generation and mints one initial capability
with only `Inspect`, `Transfer`, the descriptor's read/write rights, and `Map`
for MMIO. A failed registration restores the unpublished slot. A failed mint
starts normal registry destruction, so the range remains reserved until the
collector returns the exact destruction record. Physical removal follows
`Retire -> CollectDestroyedObject -> Reclaim`; only then can the slot and range
be reused with a new registry generation. Driver restart normally revokes or
replaces capabilities without retiring the persistent hardware region.

Memory-mapped regions accept only uncached or write-combining policy. A WC
publication permits UC fallback; an ordinary UC publication cannot be upgraded.
All retained mappings of a region must have the same policy. The atomic last
retained policy survives the final unmap so terminal AMD64 drawing can match
the broker without a stop-time lock. A terminal single
CAS latches that policy against future changes; a racing selection makes the
stopper skip drawing. Same-policy mappings remain allowed. A mapping
must be page aligned, remain wholly inside the region, be readable, never be
executable, and request no write authority beyond the region descriptor. Port
regions are limited to the 16-bit port-address space, cannot advertise 64-bit
accesses, and are never page-mapped. A retained direct register access also
checks operation, width, range, and natural alignment before the HAL operation
can run.

Device-backed page mapping requires four independent authorities:

- a pager-authorized `MemoryObject` for the logical range and protection;
- the exact `Frame` generation for the physical pages;
- the exact `DeviceRegion` generation for that MMIO range and cache policy;
- the target `AddressSpace` transaction.

`FaultFrameMappingPreparation` pins and validates all three resource objects
before publishing its inert plan. `AddressSpace` repeats the generation and
range checks, retains a DeviceRegion mapping reference before the native page
table is changed, and releases it only after the native root is quiescent. A
failed native transaction releases provisional references; an ambiguous
commit retains them until terminal root retirement.

Initialization charges `sizeof(DeviceRegion)` kernel-metadata bytes to the
exact pinned `ResourceDomain`. `Quiesce` succeeds only after every mapping and
direct register operation has released its reference; it then returns that
charge and releases the domain pin. Quiesced storage is terminal to public
`DeviceRegion` users. Only its owning `DeviceManager`, after receiving the
registry's exact destruction record, can reset that private slot for a new
discovered region. The originating registry, shared ResourceDomain ledger,
manager, and concrete object storage outlive every retained pin and
`DeviceRegionId` use.
