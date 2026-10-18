<!-- SPDX-License-Identifier: Apache-2.0 -->

# Resource Domain

`ResourceDomain` is the fixed-depth hierarchical quota container used by the
kernel. A domain has one boot-unique logical charge identifier and, once it is
registered, one generation-bearing `ResourceDomainObjectId`. References to a
parent use the latter and retain the exact registry object with an
`ObjectPin`.

The domain accounts resources that can be reserved and returned
locally: committed pages, resident pages, kernel metadata bytes, capability
handles, IPC queue bytes, threads, and descendant domains. CPU service,
bandwidth, IOPS, GPU memory, and network policy remain outside this accounting model.

All domains in one hierarchy share a `ResourceDomainLedger`. The ledger owns a
single bounded spin lock and outlives the tree. A reservation is first checked
against the selected domain and every ancestor, including overflow and the
resident-within-committed invariant. Counters change only after all checks
succeed. Release performs the inverse underflow checks before changing any
counter.

The hierarchy is limited to eight parent edges. Child initialization pins the
exact Ready parent generation and reserves one descendant slot in every
ancestor. Parent limits bound child limits, but siblings still compete for the
ancestor's aggregate remaining quota. This makes a successful descendant
reservation visible exactly once at every level.

`BeginDrain` marks a domain Draining once a drain of its subtree begins:
from then on a reservation, or a
child's initialization, refuses (`Draining`) when the domain charged or any
ancestor is Draining, while Release and Quiesce accept a Draining domain and
Draining ancestors. A Draining domain keeps its identifier but is not Ready.

`Quiesce` succeeds only when all direct reservations and descendants are gone.
A child then removes its descendant charge from each ancestor and releases its
parent pin. Quiesced storage is terminal. The registry and shared ledger must
outlive the domain and all retained identities.

`ProtectionDomain`, `MemoryObject`, `DeviceRegion`, and `PciFunction` are current
consumers.
Each initialization pins an exact `ResourceDomain` and reserves its fixed
kernel-metadata charge; terminal `Quiesce` releases that reservation and pin.
MemoryObject additionally accounts its declared committed pages. DeviceRegion
keeps its metadata charge until all retained MMIO mappings and direct register
operations finish. PciFunction keeps its metadata charge through registry
retirement until the exact collector record is reclaimed.

Each reservation and domain tracks large-page-table-arena usage for desktop
sessions. The 88-byte request places its UInt16 limit at byte 80,
with six trailing zero bytes; 56/80-byte children get zero for this limit.
The eight ordinary object limits still inherit in the 56-byte form. A flagged
AddressSpace reserves one unit through every ancestor, and collection releases
it. AMD64 also charges its selected 256-page arena as kernel metadata; IA-64
selects no additional storage and keeps its existing metadata charge. The boot
root's limit is two; the root-created compositor domain receives one.
