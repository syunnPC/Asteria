<!-- SPDX-License-Identifier: Apache-2.0 -->

# Virtio PCI and IOMMU backend

Implements [DMA isolation](IommuDomain.md). The wire contract is
[VIRTIO 1.3](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html),
sections 2.7 (split queues), 4.1 (modern PCI) and 5.13 (IOMMU). QEMU's reserved-region translation and detach-first ATTACH behavior affect
platform enforcement and recovery below.
Both ISAs compile the portable translation units. IA-64 links them;
AMD64 retains them in `ASTERIA_AMD64_UNLINKED_SOURCES`. IA-64 uses this backend only when DMAR names no remapping unit;
otherwise it uses the shared VT-d implementation
([DMA isolation](IommuDomain.md)).

## Transport and memory

`VirtioPciTransport` borrows PCI configuration access, an MMIO reader and writer,
and six platform-validated BAR extents. Discovery checks the PCI function,
walks at most 48 conventional capabilities, detects cycles/misalignment, checks
capability sizes and BAR kinds (including 64-bit BAR high halves), and validates
window arithmetic against the retained BAR authority. The first supported
window of each type is selected. Reserved BAR selectors (6 or greater) and
capabilities of an already-selected type are ignored before validating their
lengths or extents, as required by section 4.1.4.1. This accepts QEMU's second
notify capability in an I/O BAR (`modern-pio-notify`). Common, notify and ISR
windows are required;
the IOMMU additionally requires its device configuration window. Selected
windows cannot overlap. Discovery performs no MMIO writes and can be retried
following failure.

The platform retains/maps BAR authority before discovery: `IMemoryMappedIo`
addresses physical registers and has no mapping operation. It must support
byte, word and DWORD accesses as required by modern virtio. The current
`PhysicalMemoryMappedIo` adapter supports DWORDs only; native integration must
supply an adapter for the smaller widths. IA-64 supplies its BAR-bounded
uncached physical adapter, without read-modify-write on side-effectful registers.

A successful bounded reset must precede negotiation. VERSION_1 is mandatory;
caller-selected device feature bits 0..23 are supported. Optional unoffered
bits are omitted. Unsupported transport features are refused; packed rings,
indirect descriptors, event-index, ACCESS_PLATFORM and notification-data are
not implemented. FEATURES_OK is checked before configuring queues, DRIVER_OK
after queue setup. MSI-X vectors are disabled and available rings suppress
interrupts. The platform disables PCI INTx and controls bus mastering.

There are two queues, each at most 128 descriptors. The device size must be a
nonzero power of two no larger than 32768. The selected size is the largest
power of two within the device size and caller limit. Each queue uses three
distinct caller-owned coherent WB pages: descriptors, available ring, used
ring. Virtual and physical addresses are page aligned. Queue pages cannot
alias other queues or the request/event pages. Queue RAM and request/event RAM
remain pinned through failures; a successful reset is required before freeing
exposed buffers. DMA for the IOMMU itself is untranslated. The platform owns
RAM object lifetime, coherence and cache policy as it does VT-d table storage.

DMA fields are encoded little endian without packed structures. Aligned
16-bit ring indices use single volatile accesses to prevent byte tearing.
`Platform::Synchronization::DeviceMemoryBarrier` provides full ISA ordering
through a dedicated platform compiler adapter (GCC/Clang full barrier
intrinsic: AMD64 locked fence, IA-64 `mf`). Intrinsics stay in that adapter,
not in the portable transport. VT-d and the user-driver `OrderDmaStorage` helper use the same platform fence
for coherent DMA publication and consumption.
The transport uses the platform primitive to publish descriptors before the
available index/doorbell and
consume the used index before completion contents. UInt16 counters wrap
modulo 65536. Used index advances, descriptor ownership, IDs and writable
lengths are checked before returning a completion. `TryPoll` performs one
check and returns Empty; `Poll` accepts 1..100000 checks and poisons the
transport with TimedOut on exhaustion. Neither function measures wall time.

## Domain ownership and requests

`IIommuDomainBackend` is a single-owner interface: Map/Unmap/Attach/Detach have
no owner or domain argument. Consequently, **one shared `VirtioIommuDevice`
serves all domains**, and each domain has a small `VirtioIommuDomainBackend`
adapter. No domain owns page tables or a virtqueue.

The shared object reserves at most 16 IDs starting at domain_range.start,
clamped to its inclusive end without wrapping. Missing INPUT_RANGE and
DOMAIN_RANGE features imply the specification's full ranges. Claim checks
12..64 input bits, requires input_range to include the entire zero-based
address space, reserves an ID, and sends no device request. Failed Claim
changes neither ownership nor native translation state. Claim is unavailable
until the explicit ClearBypass operation has verified bypass = 0. The platform
calls ClearBypass after its storage diagnostic and bus-master sweep, with bus
mastering off everywhere. Every (re)initialization clears the local bypass
verification flag, so the platform repeats this step after recovery; clearing
bypass is not an initialization side effect.

A virtio device creates a domain on its first ATTACH and destroys it, including
mappings, on the last DETACH. Map/Unmap while no endpoint is attached update
only the retained metadata and succeed without queue writes, matching VT-d.
The first Attach replays the mappings into the newly created device domain.
Both Map-before-Attach and Attach/Map/last-Detach/Unmap are supported; the latter
must not fault the kernel domain merely because its device domain is gone.

The platform supplies an immutable, borrowed list of inclusive reserved input
ranges at `VirtioIommuDevice` construction; it must outlive the device. At most
64 ranges are accepted, and malformed views/extents fail initialization before
hardware writes. **Map refuses any intersection with a reserved range as
UnsupportedConfiguration before writing a request**, even without endpoints.
PROBE is not negotiated, so this platform list is the only source; the platform
must include every reserved region, including MSI regions.

For IA-64 this includes **0xfee00000–0xfeefffff**. The platform must also declare
that window to QEMU as a faulting **RESERVED-type** region until IA-64 has an MSI
user. The configured virtual platform must enforce the reserved region before any
domain mapping. MSI-type regions are unsuitable here because IA-64 has no MSI
authority contract; the runner exercises the faulting RESERVED-type region. The software
refusal alone does not prevent an endpoint issuing an address in that window;
the platform's faulting region is required too.

Each MAP/UNMAP is one request at 4 KiB granularity, with inclusive end addresses
and read/write flags. Zero lengths, misalignment, address overflow and invalid
permissions are rejected. page_size_mask must include 4096. Up to 64 successful
MAP extents are retained as metadata, so first Attach can replay them after a
last Detach destroyed the native domain. UNMAP must cover whole MAP extents;
partial removal is refused without writes, as required by virtio's semantics.
Local metadata/attachment exhaustion returns CapacityExceeded before writing.
ATTACH/DETACH use the low 16-bit requester identifier, reject other segments
or buses under the platform's segment-0/bus-0 convention, and reject the IOMMU's own
function. A domain supports at most 16 endpoints.

Every request uses a readable descriptor followed by a separate writable
four-byte tail descriptor in one borrowed request page. The status starts at
0xff; only used length = 4 and status = OK succeed. NOENT, INVAL and RANGE
surface as InvalidRequest. IOERR, UNSUPP, DEVERR, FAULT and NOMEM surface as
HardwareFailure. NOMEM/UNSUPP are not represented as the interface's ordinary
no-write errors because virtio gives no general failure-atomicity guarantee.
Unknown/unwritten status or an invalid completion is CorruptState. Request
timeout is CommittedStateUnknown; buffers, owner and ID remain retained.
Every device error from ATTACH retains the endpoint as possibly attached,
including NOENT, INVAL and RANGE: QEMU can detach it from its previous domain
before returning failure. Release retries ATTACH then DETACH for every retained
endpoint. QEMU re-ATTACH detaches first even when the domain ID is unchanged,
and destroys that domain and its mappings if it was the sole endpoint; the
cleanup still succeeds because Release discards all of this owner's mappings.

Release re-establishes then detaches each retained endpoint, including an
ambiguously attached endpoint, so a completed partial Release can be retried.
It discards metadata/ownership only after every required completion succeeds.
Other domains' state and resources are retained on failure.

## Faults, ordering and recovery

The event queue has up to eight 24-byte writable buffers posted at initialization.
GetFaults drains at most eight completions per call, attributes each endpoint
to its current domain, updates cumulative count/source/reason, and immediately
reposts and notifies for every consumed buffer. Reports with nonzero reserved
bytes are ignored, reserved1/undefined flags are ignored, and unknown reasons
become UNKNOWN. A requester wider than the interface's UInt16 source is ignored.
Counts saturate. Short reports are dropped and reposted. Transport corruption
or MMIO failure makes IsReady false; the fault-return interface has no error
field. The device can drop events when no posted buffer remains, as the
specification allows; the backend cannot promise lossless fault reporting.

The owner serializes its operations, and the platform serializes callers
across the shared device and its lifecycle. A shared rank-4 lock additionally
serializes competing Claim/Release, queue requests and fault attribution.
Requester IDs are read before this lock, matching VT-d's endpoint lock order.
There is no allocation, ISR entry, same-CPU reentry, interrupt wait or unbounded
hardware poll.

Virtio specifies translation removal but does **not explicitly guarantee a physical DMA drain**.
Initialize accepts `TranslationOnly` and `DrainsDma`; the adapter advertises the chosen
policy through `IIommuDomainBackend::DrainsDma`. IA-64 uses **TranslationOnly** for all
requesters, so normal Unmap cannot release a Frame pin or alias policy. The core retires
that translation and reuses an exact retained mapping without dropping its storage reference.
On either ISA, AHCI also requires this device-quiet lifetime even behind VT-d
([AHCI block I/O](../../Source/User/System/AhciPort.hpp)).

Ordinary domain destruction and subtree drain hand active and retired storage to the
trusted device owner's parker, keyed by every assignment which could have used it.
**Frames and alias policies stay retained until DeviceQuiesced for each assignment**.
A failed release retains requester associations for retry. UNMAP/DETACH/reset of the
IOMMU establish translation removal, not controller quiet. Only a platform providing an
independent physical drain may choose DrainsDma; the hosted synchronous protocol model
uses that policy, while the translation-only core test checks delayed storage release.

A timed-out transport never reuses exposed request memory, including after a
late completion. A backend cannot reset a shared device unilaterally.
`QuiesceForRecovery` is an explicit platform-wide operation: bus mastering is
off on every endpoint and all owners are faulted or tearing down. It resets
the device, negotiates the required features, clears and reads back bypass,
and leaves queues stopped. It is allowed even after Initialize failed with
`m_initialized` false. Every failed hardware initialization attempts a best-effort
transport reset; if that reset also fails, exposed pages remain pinned until a
successful recovery. Only then may each retained owner Release without
sending requests. A failed reset/bypass verification retains ownership. After
all owners Release, Initialize can restart the device; ClearBypass remains
an explicit lifecycle step. Healthy peer domains cannot stay operational
through global recovery.

## Verification

The hosted model uses independent field encoding behind PCI/MMIO interfaces,
interprets actual descriptors and both rings, and implements endpoint/domain
creation and destruction, mapping extents, statuses and event starvation.
Tests exercise ignored reserved BARs and duplicate notify capabilities,
reserved-range intersection/no-write boundaries, metadata-only mapping in both
orders (also through IommuDomain), late initialization/reset failure recovery,
every ATTACH error status, QEMU detach-first re-ATTACH cleanup,
malformed capabilities/BARs, mandatory/rejected features,
clamped queue sizes, malformed used elements, configuration and address
boundaries, shared-domain isolation, mapping replay, all nine status codes,
unwritten/zero-length completion, exact bounded timeout, recovery, capacity,
bypass readback, fault attribution/drop/repost, and 16-bit ring wraparound.
Seeded mapping sequences compare state after every operation; ASTERIA_TEST_SEED
replays a seed. Capability fuzz replay runs 20000 mutated inputs under each
debug, release and thread preset harnesses. Target verification inspects each
of the four optimized transport ordering sites for a full hardware fence on
both ISAs and rejects a mutant deleting each one.

## IA-64 native integration

`Platform/Ia64/Device/Ia64IommuRuntime` is boot-lifetime BSP storage constructed
in the loaded image's reserved writable section. It discovers exactly one modern
`1af4:1057` type-zero function on segment 0/bus 0 through `PciEcamRuntime`;
absence leaves ObjectCreate 16 unconfigured. Duplicate or unusable IOMMUs fail
closed. The kernel never grants this function and the transport/backend rejects
its requester ID on Attach. Its INTx stays disabled and no MSI is enabled.

With decode and bus mastering disabled, the native MMIO adapter probes/restores
both halves of each 64-bit BAR (or one 32-bit BAR), bounds transactions to those
apertures and validates the processor's physical address width. Byte, word and
DWORD accesses use `Ia64PhysicalMemoryAccess`'s bit-63 uncached physical corridor,
with the RSE lazy, PSR.dt off, interrupts masked and exact state restored. No
BAR mapping or queue adds a translation register. The physical-access leaves are checked for corridor, barrier and state-restore
properties.

Eight aligned 4 KiB WB pages in that image reservation hold two split rings
(descriptor/available/used pages each), requests and fault buffers. Their CPU
addresses are kernel identity addresses; their DMA addresses are physical.
They never enter the Frame allocator and remain reserved on failure or timeout.
The reservation and mappings therefore last as long as the shared device.

Initialization follows the boot AHCI diagnostic. Every published function's
bus mastering is disabled, including the IOMMU's, before queue initialization
and explicit ClearBypass. Bypass is read back clear before only the IOMMU's own
untranslated queue DMA resumes. Each initialization resets the backend's local
bypass verification. Recovery requires repeating the sweep, quiescing all
owners, resetting the device, releasing retained domains and clearing bypass again.

The native platform advertises TranslationOnly even for its test endpoint. IOMMU invalidation alone
is not evidence that a device has released an already translated pointer. The shared ObjectManager drain
also masks interrupts, stops bus mastering and quiesces/detaches domains before
Frames; mapping pins are parked with the quarantined function until the trusted
Device Manager observes the device quiet. This is not an AHCI drain guarantee.
AHCI and NIC authority must wait for the controller-specific drain/reset and
parked-pin confirmation paths when those drivers are enabled.

The immutable reserved list includes `0xfee00000-0xfeefffff`, matching the virtual platform's
faulting RESERVED region. Four small per-domain adapters feed IommuDomainPool;
ObjectCreate 16 and IommuControl 20 use the common ABI and drain. Faults retain
count/source/reason and the address when ADDRESS is set. GetFaults returns the
existing packed Value0 and the full input address in Value1; device-level
cumulative evidence survives domain release. The edu stray probe writes one
byte: a larger unmapped QEMU transfer fragments into several fault reports and
can starve the eight posted buffers. Fault reporting remains bounded/lossy.

The runner pins edu to slot 6 (GSI 22) and the IOMMU to slot 7 (GSI 23), enables
`pci-intx-lines=8`, and uses slot 0x10 (GSI 16) for the NIC. Existing
AHCI, UHCI, LSI and VGA keep their built-in slots. The versioned IOMMU evidence
subrecord records translation state, requester, sweep, queue reservation,
domain lifetime and the stray fault. The boot reports `ASTERIA:STAGE:IOMMU-TRANSLATION`; absence reports
`ASTERIA:IOMMU:ABSENT`. User DMA publication and consumption use the common
full hardware fence on both ISAs.

The native integration preserves these contracts: PIC components retain sealed
read-only image segments once, charged to the pager grant. The linked-image
`CheckPagerGrant.py` model includes input, the edu task/service's two DMA pages
and pairs plus its registers pair, and the parked-pin interval after drain.
The model derives grant peaks and retained residue from the current images.
Input and DMA do not overlap; DMA pages are released before the input phase.
Static-PIE, input/PS/2, runtime-status and disk-write checks remain enabled.
The consolidated bootstrap requires **4 DTRs / 2 ITRs**, including the queue
reservation and the independent 16 MiB runtime-memory DTR.
IOMMU initialization failure enters the visible BootStage/DeviceBinding stop.
The boot entry copies the image into a 16 MiB-aligned LoaderCode allocation
before publishing globals; the IOMMU owner is constructed only in the copied
continuation, after the boot AHCI diagnostic. Its queue addresses are therefore
identity physical addresses in the active copy's reserved `.bss`, not in the
firmware source allocation. The runner checks all eight queue pages lie in
that relocated writable section. The linked-image model checks the ports/WaitSet/Interrupt grants on AMD64 and
the DMA and parked-pin grants on IA-64.

## The AHCI controller behind the virtio-IOMMU

[DMA isolation](IommuDomain.md) grants the AHCI controller its DMA authority through this backend,
on the condition the paragraphs above set: its drain is the controller-specific one. The AHCI driver's IommuDomain is
one of the four adapters; QEMU's AHCI resolves a command's buffers through the IOMMU when it issues the I/O, so a
dead driver's UNMAP and DETACH do not stop it, and its Frame pins stay parked with the quarantined function until
the Device Manager's port reset (COMRESET, which drains the in-flight I/O) proves the controller quiet
([AHCI block I/O](../../Source/User/System/AhciPort.hpp)). The block demonstration crashes the
driver with a read in flight into a Frame its client destroys; the IA-64 evidence's `Block` record shows the late
write in that still-pinned Frame's page as the frame scrubber zeroes it after the confirmation, never in reused memory.
The platform advertises TranslationOnly; normal AHCI Unmap retains storage until the same parked-pin confirmation.

## The e1000e behind the virtio-IOMMU

The NIC grant ([network service contract](../../Source/User/Network/NetworkStack.hpp)) gives the e1000e's driver an IommuDomain
of its own on this backend, with its descriptor page and its receive and transmit runs mapped at the driver's fixed
device addresses. The device reads and writes memory only while it processes a descriptor, gated by bus mastering, so
a dead driver's drain (bus mastering off) ends its DMA; its three Frame pins still stay parked with the quarantined
function until the Device Manager's reset of the device confirms it quiet, as for every IA-64 driver.
