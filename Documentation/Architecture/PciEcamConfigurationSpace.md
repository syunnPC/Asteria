<!-- SPDX-License-Identifier: Apache-2.0 -->

# PCI ECAM Configuration Space

## Target-neutral access boundary

`AcpiPciEcamTopologyAdapter` consumes only the copied `BootInfo` topology. ACPI
MCFG records describe a bus-zero base, so the adapter checks and adds
`FirstBus << 20` before publishing each generic `PciEcamRegion`. Absence
publishes an empty topology. A malformed optional topology is a typed adapter
failure that the platform boot owner treats as the same non-fatal unavailable
fallback. The adapter does not inspect EFI memory descriptors or retain ACPI
pointers, and access does not depend on the inert source-table address or
revision metadata.

`PciEcamConfigurationSpace` copies at most eight normalized regions, checks
their aperture arithmetic and bus/physical overlap, and selects by exact
segment and inclusive bus range. It validates the BDF and aligned DWORD offset
before calling `PciEcamAddressCalculator` and the injected
`IMemoryMappedIo`. A failed read never changes the caller's output. The generic interface exposes
only `IPciConfigurationSpace::Read32`; configuration writes, BAR policy and
driver authority belong to separate platform interfaces.

The injected `IMemoryMappedIo` reader is the least-authority boundary for the
generic PCI reader: one exact physical address, one width, and one output
value. The architecture backend separately implements the checked
`IMemoryMappedIoWriter` primitive, but the ECAM owner does not inject or expose
that writer capability. Configuration writes require an explicit platform
policy grant; the retained superset `DeviceRegion` policy
does not become available through the current read-only PCI interface.

## IA-64 runtime access

`PciEcamRuntime`, which both ISAs share and IA-64 constructs with its
`Ia64PhysicalMemoryAccess`, creates one registered, boot-lifetime `DeviceRegion` for
each normalized aperture through `DeviceManager`. Each region is uncached and
permits read/write byte, word, and DWORD configuration cycles. Creation capabilities are closed immediately; the embedded manager
retains the objects and the runtime retains their exact storage for every MMIO
operation. The runtime is placement-constructed in immovable boot-lifetime
storage and has a deleted destructor: even a failed preparation can retain an
already registered region. Its exact `ObjectRegistry`, registry lock,
`RootCapabilityIssuer`, and every charged `ResourceDomain`, ledger, identity,
and registration therefore outlive the runtime. Bootstrap construction and
preparation are externally serialized, non-ISR operations with same-CPU
preemption and re-entry excluded. Reads begin only after preparation and are
serialized by the embedded checked readers.

The IA-64 address-space backend has bounded mapping records and a
2 MiB maximum per mapping. Eagerly mapping as many as eight 256 MiB ECAM
apertures would therefore impose an invalid aggregate capacity restriction.
The IA-64 access instead performs one authorized DWORD read through a leaf
physical-access corridor: it masks interruption delivery, forces RSE lazy
mode, clears `PSR.dt`, forms the bit-63 uncached alias, executes
`mf; ld4.acq; mf.a`, and restores the exact entry `PSR.dt`, `PSR.i`, and
`ar.rsc` state. Native callers are PL0, native-mode (`PSR.vm=0`), and
little-endian (`PSR.be=0`); the read is not ISR-callable, and the caller
prevents same-CPU preemption or re-entry through the backend while its lock is
held. The production translated BSP continuation establishes these processor
conditions. Target disassembly checks reject calls, stack traffic, extra
loads/stores, the wrong alias bit, or reordered state restoration.

## IA-64 function discovery and publication

After construction in the translated BSP continuation, the runtime gives all
normalized MCFG segment/bus ranges to its retained, architecture-neutral
`PciFunctionCatalog`. The catalog scans in deterministic segment/BDF order,
always probes function zero, and probes functions one through seven only when
function zero reports the multifunction bit. It reads the identifier, class,
revision, programming-interface, subclass, and masked common-header layout for
every present function. A valid topology with no present function remains
usable and reports `NoFunctionPresent`.

The catalog's fixed storage admits 256 functions. The platform manager has a
separate, explicit 256-function object capacity. The IA-64 runtime checks the
complete discovered count against that manager capacity before binding the
catalog or creating any `PciFunction`; an excess produces
`FunctionCapacityExceeded` with zero PCI publication. There is no aggregate
bus cap. An unexpected registry, resource-domain, or capability failure during
the subsequent per-function publication is terminal and never reports Ready.
Already-created objects and capabilities remain retained for boot lifetime,
and the failure snapshot reports the exact completed publication count.

Each successful publication copies the catalog descriptor into a checked
`PciFunction`, registers it through `DeviceManager`, and retains the returned
`PciFunctionId` plus its exact `Inspect|Manage|Transfer` capability. Those
handles are authenticated only for the runtime's private boot capability space;
an immutable query includes that space identity but does not make the handle
usable in a driver's distinct space; driver assignment requires controlled
capability transfer. Failed summary and publication
queries leave caller output unchanged, and no raw manager storage is exposed.

The summary retains catalog range/bus/read counts, discovered and published
counts, the first present BDF and the unique
type-0 function with PCI class/subclass/programming-interface `01/06/01` as
AHCI evidence. If that exact match is absent or ambiguous, the summary clears
the AHCI descriptor and its presence flag. The IA-64 boot path records both the
identifier read (first BDF and vendor/device identifier) and the
discovery summary (published function count, AHCI BDF, vendor/device
identifier, and class triple) in the `Pci` part of its evidence record and
reports the `PCI-ECAM` and `PCI-DISCOVERY` stages. The VM runner compares the
record with QEMU's own `query-pci` view: the first function and its
identifier, one published function per QEMU function, and the AHCI function's
address, identifier, and `01/06/01` class triple; it pins no vendor or device
fingerprint of its own.

A machine without ECAM (no MCFG; QEMU's `hp-i2000`, whose 460GX configures PCI
through ports `0xCF8`/`0xCFC`) reports `ASTERIA:PCI-ECAM:UNAVAILABLE`. Its boot
binds no PCI function, so it skips the AHCI identify/read diagnostic and the
AHCI device publication instead of stopping (`ASTERIA:IA64-AHCI:SKIPPED:NO-ECAM`,
beside the IOMMU handoff's own `IOMMU-HANDOFF-REFUSED` skip), publishes no
assignable device or scanout, and the IOMMU reports absent; the boot continues
to the initial image.

## IA-64 AHCI resource authority

AHCI availability is an immutable result separate from ECAM state. A catalog
with no exact `01/06/01` type-zero endpoint reports AHCI absence even when the
optional firmware record is empty. Exactly one catalog endpoint must match the
captured segment and BDF. The runtime then accepts only physical-address widths
32 through 63, validates the complete captured range, requires PCI Command
Memory Space Enable and Bus Master Enable, and preflights BAR5 against the
authoritative extent. BAR5 must be one non-prefetchable 32-bit memory register
with the captured base and extent; no configuration write or BAR sizing cycle
occurs.

Only after preflight does the same private `DeviceManager`, catalog,
revocation registry, and capability space create one exact boot-lifetime ABAR
`DeviceRegion`. It is read/write, DWORD-only, and uncached. Its creation
capability is closed, and a separate `PhysicalMemoryMappedIo` backend is
initialized with only that region. The ECAM backend never receives ABAR
authority. The portable AHCI binding revalidates the catalog and BAR5, reads
the fixed global HBA snapshot through the ABAR backend, and publishes only
after the BDF, base, and extent match the firmware record again. A later
terminal failure retains any registered private region; it is not rolled back
or reused. Failed result queries leave output unchanged, while an AHCI failure
does not revoke otherwise useful ECAM readiness.

The runtime serializes two independent one-shot commands through one private
bootstrap lock. IDENTIFY derives only the ABAR from that immutable binding and
selects exactly one active ATA disk. READ DMA EXT is unavailable until
IDENTIFY has published its receipt; it derives the exact ABAR, selected port,
device sector capacity, logical-sector size, and LBA48 support exclusively
from that binding and receipt. The READ caller supplies only a clock, exact
command and external-data DMA spans, LBA/count, and bounded polling policy.
Calls before successful IDENTIFY, after no-disk or terminal IDENTIFY failure,
after authority mismatch, with rejected spans, or after the first READ attempt
fail without replacing the first READ outcome. Receipt queries publish only a
complete success and otherwise leave their output unchanged. Stored executor
failures retain the command-page and data-buffer lifetime flags without
reinterpretation.

The IA-64 boot owner uses one page-aligned 4 KiB command/FIS/table page for
IDENTIFY and then READ, plus a distinct page-aligned 4 KiB data page. It proves
both complete pages are inside writable loaded-image sections, checks all
address arithmetic and the structural bit-63-clear physical form, and supplies
an exact 512-byte destination span for the LBA-8 one-sector probe. This relies
only on the current bootstrap's virtual-equals-physical, no-IOMMU, coherent
QEMU model. The pages are ordinary memory, not volatile objects; MMIO and DMA
ordering remains the responsibility of `Ia64PhysicalMemoryAccess`. No
support for non-coherent production hardware is claimed.
