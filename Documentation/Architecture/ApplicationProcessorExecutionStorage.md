<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 Application-Processor Execution Storage

## Scope

`ApplicationProcessorExecutionStoragePreparer` initializes one standalone per-AP execution-storage unit without
granting ownership to the AP. In production, the BSP-owned `ApplicationProcessorExecutionStorageOwner` calls it once
for each enabled non-BSP processor after the Local SAPIC snapshot and wake-vector validation, but before rendezvous
handler initialization, SAL registration, `Arm`, or any wake IPI. The preparer allocates nothing and owns no state.
Its caller supplies one allocation already returned by `BootstrapPageAllocator` and keeps that allocation for the
boot lifetime regardless of preparation success because the bootstrap allocator has no rollback operation.

The preparer initializes storage only. It creates no mapping, guard page, PAL record, IVA/RR/TR state, translation state,
scheduler context, runnable state, wake authority, or AP handoff. It does not modify or seal the BSP
`InitialAddressSpacePreparation`.

## Fixed geometry

Each prepared descriptor names one physically contiguous 16-page allocation aligned to eight pages. Pages `[0, 8)`
are an exact 32 KiB downward-growing memory stack. Pages `[8, 16)` are a distinct exact 32 KiB RSE backing store. The
initial stack pointer is the memory-stack end minus the 16-byte IA-64 scratch area, and the initial BSPSTORE is the
first byte of the RSE range. There is no gap or guard range between the two halves.

The allocation must be nonzero Conventional memory, carry write-back capability, omit the firmware runtime
attribute, have zero reserved state, remain wholly representable without arithmetic wrap, and lie wholly in IA-64
virtual region zero for the bootstrap physical-identity contract. The two existing writable IA-64 view types must
name the exact halves, be page-aligned, be pointer-representable without wrap, and form one contiguous writable host
interval. On IA-64 the writable addresses must equal their physical starts exactly.

## Identity and prior descriptors

The input identity retains the logical `CpuId` and exact Local SAPIC hardware ID/EID pair already accepted by the
immutable BSP-first processor map. `CpuId` zero is the BSP and is rejected; invalid logical IDs and nonzero reserved
identity state are also rejected.

The caller supplies one bounded, read-only view of up to three descriptors returned by earlier successful AP
preparations. Each prior descriptor is revalidated as a complete 16-page storage descriptor. The new allocation must
not overlap a prior allocation. The current identity must not duplicate a prior logical `CpuId` or ID/EID pair, and
the prior set must itself contain neither duplicate identities, overlapping allocations, nor malformed descriptors.
The descriptor array must be correctly aligned, bounded, nonwrapping, and disjoint from the destination writable
interval so zeroing cannot destroy validation input.

Physical separation from the loaded image, BSP reservation, SAL handler and descriptor storage, and every earlier
allocator result follows from the required `BootstrapPageAllocator` origin: it imports only Conventional ranges and
removes each successful allocation from its free-range set. The prior-descriptor checks retain the independent
identity and pairwise descriptor postconditions needed before descriptor publication.
Production therefore supplies no arbitrary exclusion or provenance list.

## Commit behavior

All geometry, attributes, identity, pointer, alias, duplicate, and overlap checks finish before the first
write. Any such failure leaves all destination bytes unchanged and returns no descriptor. After validation, the
preparer zeroes exactly the 32 KiB memory stack and exact 32 KiB RSE range, executes the existing IA-64 `mf`
publication fence, and returns a 64-byte inert descriptor containing the CPU and ID/EID identity, allocation range,
stack start/size/initial SP, and RSE start/size.

The returned descriptor is value-only metadata. The production owner retains each successful descriptor privately
and permits descriptor access only after preparation of the complete AP set commits `Ready`. An allocation or
preparation failure publishes no descriptor set; pages already allocated remain consumed for the boot lifetime, and
the boot path reports one `ASTERIA:AP-RENDEZVOUS-ERROR:EXECUTION-STORAGE` diagnostic before any handler
initialization, SAL registration, `Arm`, or wake IPI. Preparation success alone still grants no mapping, guard, AP
ownership, translation, scheduler readiness, or runnable state. In production the owner subsequently exposes the
complete descriptor set to a single-use takeover coordinator, which copies one immutable 24-byte record for each AP.
The AP claims that record and the native bridge activates the named physical 32 KiB stack and 32 KiB RSE backing
store, purges firmware ITR(0), installs the shared physical fail-stop IVA without rescanning its 32 KiB bytes, captures
and publishes its local PAL capabilities, performs the required local translation-cache purge, installs and reads
back the sealed eight RRs plus the capacity-checked DTR/ITR initial plan, then publishes terminal success or the exact
typed failure and parks.

Runtime-IVT/root admission, cooperative AP execution and timer delivery belong
to the [translated runtime](Ia64BspKernelRuntime.md#shared-root-ap-runtime).
The preparer supplies storage only and grants no scheduling or IPI authority.
