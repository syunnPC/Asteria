<!-- SPDX-License-Identifier: Apache-2.0 -->

# IOMMU domain

`IommuDomain` is the architecture-neutral DMA-isolation object behind
`ObjectType::IommuDomain`. It owns one native translation backend, at most 64
device-visible mappings, and at most 16 attached DMA requester endpoints. The
fixed bounds keep mapping, revocation, and fault recovery allocation-free.

The current layer defines the complete kernel state machine and the HAL
ownership contract. AMD64's VT-d backend, `VtdDomainBackend`, implements
`IIommuDomainBackend` over a `VtdUnit`:

- The unit runs in legacy mode, with no pass-through, queued invalidation or
  interrupt remapping.
- Every requester's context entry is not present until it is attached, so an
  unattached requester's DMA is blocked and recorded as a fault. A detach or a
  release blocks it again.
- Each backend is one domain identifier of the unit with eight second-level
  table pages. The top table is allocated at `Claim`, and the others are
  allocated as mappings need them and kept until `Release`.
- `Map` counts the tables a range needs before writing anything, so running
  out is an ordinary `CapacityExceeded`.
- `Unmap` invalidates the domain's IOTLB, with draining where the unit offers
  it.
- Table writes are flushed from the cache when the unit's walks do not snoop.
- Faults are polled into counters per domain.

AMD64 also implements AMD-Vi through `AmdViDomainBackend` and `AmdViUnit`
([architecture](AmdVi.md)), selected from IVRS with permanent IVMD domains.
IA-64 uses the [virtio-iommu backend](VirtioIommu.md).
Unsupported machines keep bus mastering disabled.

`IommuDomainPool` holds the IommuDomains user code creates, one per platform
backend in slots the platform provides, so a kernel without an IOMMU carries
none. `Create` initializes a slot's domain on its backend, charged to the
ResourceDomain a `Manage` capability names, and mints the creator's capability
(Inspect, Map, Transfer, Duplicate, Derive, Manage) under that authority.
`Destroy` and the drain first hand the domain to an `IIommuDomainQuiescer`,
which stops the DMA of every function attached to it and quiesces it; a domain
that cannot be quiesced stays live. `Collect` ends a destroyed domain once the
registry releases it. `UnmapAt` removes a mapping by its device address, and
`GetFaults` passes on the backend's fault record for the domain.

## Firmware DMA reservations

The pool hands every domain it creates the platform's `IDmaReservations`
(`Kernel/Device/DmaReservations.hpp`), or none. A domain never maps a device
address overlapping a range firmware reserves for a requester it serves: `Map` refuses a range (the requested one, and on an identity backend
the physical one it chooses) that overlaps a reservation of any attached
requester, and `Attach` refuses a requester for which a present mapping
overlaps one of its reservations. Both answer `ReservedDmaRange` before any
pin, policy or backend change. AMD64 builds the table at boot from the DMAR's
RMRR scopes or the IVRS's IVMD blocks as the resolved topology plan assigns
them (`Platform/Device/FirmwareDmaReservations`); it does not follow a
function's assignment restriction, so a function released from firmware keeps
its ranges unmappable. The table is immutable and takes no lock; the domain
reads each attached endpoint's requester identifier under its own lock, as
`Attach` already claims endpoints. virtio-iommu's domain-wide reserved regions
stay in its backend.

## Authority and lifetime

Initialization pins the exact `ResourceDomain` generation, charges the complete
fixed object as kernel metadata, and exclusively claims an empty native backend.
The registry, backend, resource domain, every mapped `Frame`, and every attached
device endpoint outlive their retained use.

The device-assignment path supplies one unique `IIommuDevice` endpoint per
hardware requester. Endpoint `Claim` serializes assignment across all domains.
There is no separate readiness preflight: `Claim` returns `NotReady` atomically,
and every failed claim leaves endpoint ownership unchanged.
Untrusted code never supplies a C++ endpoint pointer: a syscall layer must first
resolve the caller's device and domain capabilities, while the trusted device
owner disables bus mastering before attach, detach, or terminal recovery.

## Mapping transaction

A map request names an exact registry-bound `Frame` generation, a Frame
subrange, a page-aligned DMA address, and device-perspective Read/Write
permissions. `Map` performs all of the following before publication:

1. validate the configured 32-64 bit DMA address range without wrap;
2. reject every overlap in the domain's device-visible address space;
3. pin and address-check the exact RAM `Frame` generation;
4. validate the physical subrange;
5. reserve Frame-wide alias policy, which claims the Frame's pages;
6. ask the native backend to install and invalidate the translation;
7. publish a nonwrapping mapping token only after success.

A device-write mapping reserves the same write policy used by CPU page-table
mappings. It therefore conflicts with an executable CPU alias in either
creation order and remains reserved across every root and IOMMU domain that
uses that exact Frame generation. Device-read-only DMA does not create a write
alias. Every reservation, read-only DMA included, also keeps the Frame's
claim on its pages in the `ObjectRegistry`, so a second Frame object naming
the same RAM cannot be mapped around the reservation: `Map` refuses it with
`FramePolicyRejected` and `PhysicalOwnershipConflict` (see `Frame.md`).

`Unmap` accepts only the exact domain and active sequence token. Backend success includes
IOTLB invalidation. A backend's `DrainsDma` contract additionally attests physical DMA
completion; virtio on IA-64 advertises translation-only completion. AHCI requires trusted
device quiescence on either ISA even behind an architectural draining backend
([AHCI block I/O](../../Source/User/System/AhciPort.hpp)).
In that case Unmap retires the translation/token while retaining its Frame policy and pin.
An exact remap reuses the retired record; active and retired records together use the existing
64-entry capacity (`MappingCount` counts retained storage). Different extents at a retired
address can map, but the old physical storage remains protected. Ordinary no-write Map
failure leaves a reused retired record intact; an ambiguous Map retains it in Faulted state.
Detach answers Busy/WouldBlock while retained storage needs a requester association. Quiesce requires a
parker for this storage; the control parks every relevant assignment even outside a drained
subtree, and ordinary assignment release quarantines it too. Only the trusted owner's
DeviceQuiesced for the assignment releases the pins and alias policy. Architectural drain
backends without such a requester keep immediate Unmap release. Token reuse, cross-domain
use, and sequence wrap are rejected.
An unexposed domain with no attachment can release its storage after backend release without
a parker, including ordinary Attach refusal; ambiguous Attach retains a conservative
attachment and still requires the parker.

## Failure and restart

`UnsupportedConfiguration` and `CapacityExceeded` are the only ordinary HAL
operation failures. They perform no native write, so provisional policy and
pins are rolled back and the exact request can be retried. Every other backend
operation failure makes the domain `Faulted`. A possibly committed Map or
Attach remains represented by a conservative internal record, so Frame memory
cannot be repurposed or made executable while hardware state is uncertain: the
record keeps its Frame pin and write reservation, as does a mapping whose
`Unmap` failed, and that reservation refuses executable CPU mappings of those
pages, and mappings through any overlapping Frame, in every root until
`Quiesce`.

`Quiesce` is the driver-crash revocation boundary. After the device owner has
stopped bus mastering, it releases the complete native backend first. Every
failure retains the control's Domain/Attachment association for retry, including
an ambiguously committed Attach; a failed
bus-master-off write prevents backend release. Device-quiet storage goes to the
assignment parker instead of being released. Successful backend release proves no requester can use
an old translation. The domain then releases or parks every Frame policy and pin, releases every
device claim, and finally its ResourceDomain charge. This works for both Ready
and Faulted domains and does not depend on a crashed driver returning its
individual mapping tokens.

```text
Uninitialized --Initialize--> Ready
Ready --ambiguous native failure--> Faulted
Ready|Faulted --Quiesce--> Quiesced
```

All public operations may spin on the domain lock and are forbidden from ISRs
and same-CPU reentry. Lock nesting is domain, then registry, ResourceDomain
ledger, Frame, device, or backend; a Frame reservation or release may take the
registry lock inside the Frame lock to claim or release its pages. None of
those callees enters the domain.
HAL endpoints are bounded and callback-free. Cache maintenance for a
noncoherent device is performed by its driver or platform DMA layer before
ownership is transferred to or from the device.
