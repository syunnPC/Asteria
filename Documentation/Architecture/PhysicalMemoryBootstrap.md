<!-- SPDX-License-Identifier: Apache-2.0 -->

# Physical Memory Bootstrap

`BootstrapPageAllocator` imports the final `BootInfo` into fixed metadata and
issues contiguous physical page ranges. It has no heap, mapping, clearing,
firmware calls, or page-return path. The namespace-scope `constinit` owner is
constructed before entry and initialized once after `ExitBootServices`.

Only conventional memory becomes available. AMD64's pre-exit `LoaderData`
reservations therefore cannot be reissued. The
[initial address-space owners](InitialAddressSpace.md) validate image provenance,
allocation receipts, mapping permissions, and storage before native activation.
They retain all storage the processor may still use.

## Input boundary

Initialization revalidates the complete handoff instead of relying on its producer. It requires the `ASTRINFO`
signature, exact structure version and extent, zero reserved fields, descriptor version 1, a descriptor stride of at
least 40 bytes, exact raw-map-size provenance, and one through 2,048 regions. Every region must have a nonzero,
page-aligned, non-wrapping physical extent; a nonzero virtual extent must obey the same alignment and overflow rules.
Inclusive endpoints admit the final physical page and the special full-width descriptor without overflow. The list
must be ordered and non-overlapping. The normalized kind must agree with the original UEFI type; standard types and
the defined OEM/OS namespaces are accepted, while the undefined numeric gap is rejected and OEM/OS values remain
reserved. A region cannot be both `Conventional` and marked `EFI_MEMORY_RUNTIME`; that contradiction is rejected
because every runtime-attributed range must remain owned and mapped after boot-services exit.

Only `Conventional` regions enter the free table. Loader, boot-services, runtime-services, ACPI, persistent,
unaccepted, PAL, MMIO, unusable, and reserved memory remain unavailable. Physical page zero is removed even if
firmware describes it as conventional. Adjacent ranges coalesce only when their firmware type and memory attributes
are identical, preserving cache and protection provenance.

## Allocation policy

- Page size is 4 KiB.
- The free table holds at most 2,048 ranges and is stored inside the allocator object.
- Requests specify a nonzero page count and a nonzero power-of-two alignment measured in pages.
- A request must fit within one source range; the allocator does not synthesize physical contiguity across gaps.
- The lowest suitably aligned physical address wins, making bootstrap output deterministic.
- Prefix, suffix, exact, and middle consumption update the ordered table in place.
- A middle allocation may create one extra metadata range. If the table is full, the request fails with
  `RangeCapacityExceeded` before changing any range or counter.
- Exhaustion and malformed requests are typed failures. No path unwinds, allocates metadata, or silently selects a
  less-preferred address.

Initialization is single-use so an accidental reset cannot make live pages appear free. All loops are bounded by the
2,048-range limit. Calls are not synchronized; bootstrap code must keep the allocator on one CPU and prevent reentry.

## Ownership boundary

This allocator does not charge ResourceDomains, create Frame capabilities, zero
pages, install tables, reclaim firmware memory, or return pages. Higher-level
owners perform those transactions separately. Native integration is documented in the
[boot contract](BootContract.md).
