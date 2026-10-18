<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 SAL Boot-Rendezvous Handler

`Ia64SalBootRendezvousHandler` is the allocation-free dispatcher behind the physical `OS_BOOT_RENDEZ` entry. The
production multi-CPU path composes it with `Ia64ApplicationProcessorTakeoverCoordinator`: each first-time enabled AP
records arrival, claims one immutable 24-byte physical takeover record, purges firmware ITR(0), abandons SAL's return
context, switches to its dedicated physical stack/RSE storage, installs the shared physical fail-stop IVA, publishes
its AP-local PAL capabilities, installs the sealed initial RR/TR plan with those capabilities, reaches a terminal
state, and parks. An arrival word or PAL-capability publication by itself remains intermediate diagnostic state;
production success is the coordinator's terminal initial-translation-installation publication.

## One-shot shared state

The BSP initializes an immutable map of one through four enabled logical processors from an already-ready
`Ia64LocalSapicIpiControllerSnapshot`. The map is BSP-first, so logical CPU 0 is always the BSP. Initialization
revalidates the snapshot's contiguous enabled mask, Local SAPIC addresses, reserved fields, unique ACPI UIDs and
unique ID/EID pairs. CPU 0's pair must exactly match the ID and EID fields of the single `cr.lid` value captured by
the BSP. Production must capture that value once and pass it to both Local SAPIC IPI and rendezvous initialization.

The boot-lifetime handler transition is only `Uninitialized` to `Configured` to `Armed`. Configuration and arming are
BSP-serialized, each publishes with a release store, and arming succeeds once. No reset, disarm, generation, or epoch
exists because this stage admits neither CPU hotplug nor an IPI-carried generation token.

Each logical AP owns one aligned atomic arrival word. After an acquire observation of `Armed`, a known AP checks
its own slot and release-stores one. SAL serializes repeated `OS_BOOT_RENDEZ` invocations on an individual AP; calls
on different APs use different slots. Duplicate detection therefore needs no shared compare-and-swap or fetch
operation. Snapshots can still build an arrival mask with acquire loads. A not-armed, unknown, BSP, or duplicate
invocation returns a typed result and changes no arrival slot, but production does not use that arrival mask as its
completion condition.

Before handler registration, the BSP initializes the takeover coordinator from the complete `Ready` execution-storage
owner. Initialization revalidates all owner geometry and identities and copies each enabled non-BSP descriptor into an
exact 24-byte record: initial stack pointer, RSE backing-store physical start, and zero-extended logical `CpuId`.
Release publication exposes each slot as `Prepared`. `Claim` atomically changes only the matching AP slot to `Claimed`
and returns a stable pointer to that immutable record; BSP, unknown, and repeated claims fail without substitution.

## Physical entry, SAL discharge, and takeover

SAL calls a raw, 16-byte-aligned no-argument entry with the registered GP in `r1`, a private memory stack and RSE
backing store, and its return address in `b0`. Raw assembly getters expose the entry bundle address and current image
GP without retaining a C++ function pointer or creating a function-descriptor relocation.

The entry saves `ar.pfs`, `b0`, and `gp`, reads `cr.lid`, and makes one direct call to the C++ dispatcher. The
dispatcher records the arrival and returns four words: the retained SAL ITR(0) virtual address and encoded page size,
a `ReturnToSal`, `TakeOver`, or `FailStop` disposition, and, only for takeover, the claimed record's physical-identity
address. Every disposition first executes:

```asm
ptr.i r8, r9
srlz.i
```

The SAL handoff guarantees that ITR(0) is the only valid TR; every DTR and other ITR is already invalid, so no `ptr.d`
or enumeration through PAL is needed. The SAL system-table translation-register descriptor describes that mapping and
supplies the exact `ptr.i` operands. This purge both satisfies SAL 3.4 Section 3.2.5.1, Table 3-2 note 2 on a fallback
return and discharges the last firmware translation before a production takeover.

Initialization structurally revalidates those operands using the same domain as the SAL parser: descriptor
presence, zero retained reserved byte, an encoded six-bit page size of at least 4 KiB, and natural alignment within
the 61-bit virtual-region offset. A raw invocation before configuration cannot return portably because no valid
purge operands exist. The native bridge therefore enters a local fail-stop loop instead of returning with SAL's ITR
still installed. Production must initialize this singleton before registering its entry.

`ReturnToSal` remains an explicit compatibility/rejection disposition for a not-armed, unknown, BSP, duplicate, or
arrival-only uninitialized-coordinator invocation. Only that path restores GP, `b0`, and `ar.pfs` and returns through
SAL's BR0. The production path initializes the coordinator before registration, targets each enabled AP once, and
requires `TakeOver`; coordinator-initialization failure or an impossible claim produces `FailStop`.

For `TakeOver`, the bridge validates the record pointer and all three copied fields while SAL's stack and RSE still
exist. It requires the stack pointer to be physical and 16-byte aligned, the RSE start to be physical and 32 KiB
aligned, the initial stack pointer plus IA-64's 16-byte scratch area to equal that RSE start, and the logical `CpuId` to
name a non-BSP target. It admits only the reviewed physical and external-interrupt-disabled PSR profile, while allowing
SAL's specified interruption-collection state. RSC mode and BE must be zero; inherited PL, `loadrs`, and unused
reserved fields are accepted because `PSR.rt` is clear and the takeover drains the old RSE before replacing RSC. It
then selects bank one, clears `PSR.ic`, performs data and instruction serialization, drains and invalidates the
inherited RSE frame, installs
the AP's RSE start in `ar.bspstore` with zero RNAT/PFS, fences, switches `r12` to the claimed stack pointer, and calls
the fixed no-return AP continuation. The old SAL `b0` and PFS are never restored on this path.

The continuation validates only the retained 16-byte IVT summary and installs its 32 KiB-aligned physical start in
`cr.iva`; it deliberately does not rescan the BSP-published immutable 32 KiB fail-stop image on every AP. It serializes
and reads `cr.iva` back. A precommit rejection or committed readback mismatch is release-published as the exact IVT
failure from `Claimed`, after which the AP parks. Success publishes `OwnedPhysical`, then invokes
`PAL_VM_SUMMARY`, `PAL_VM_PAGE_SIZE`, `PAL_MEM_ATTRIB`, `PAL_PTCE_INFO`, and `PAL_RSE_INFO` on that same AP. The
validated 88-byte capability/purge record is copied before the intermediate `PalCapabilitiesPublished` state is
release-published; an adapter, PAL-status, or decoder failure instead copies its exact phase and typed payload before
publishing `Failed`.
After PAL success, the AP passes that same local record to the initial translation installer. The installer performs
the PAL-required local translation-cache purge, purges firmware ITR(0), installs and reads back all eight sealed RRs,
then inserts the sealed, capacity-checked DTR/ITR entries while translation remains disabled. A typed install failure
is published as phase `InitialTranslationInstallation`; success release-publishes terminal
`InitialTranslationsInstalled`. The AP parks after either final publication and never reports terminal success before
the installer returns.

The BSP polls the coordinator, rejects any `FailedMask`, and proceeds only when the terminal mask exactly equals the
target AP mask. Consequently the `ASTERIA:STAGE:AP-RENDEZVOUS` event is emitted only after terminal success from every
target, not after mere arrival or PAL publication; the admitted processor count and mask are recorded in the
`Processors` part of the kernel's evidence record, which the VM runner compares with the case and QEMU. The stage
attests AP-local initial RR/TR installation as well as physical takeover. This rendezvous provides no AP runtime
IVT/root activation, runtime translation,
`AddressSpace` admission, scheduler context or run-loop entry. The later
translated runtime supplies one bounded AP-local-timer proof followed by a boot-
lifetime fixed-storage cooperative scheduler service. Its retained worker
blocks at run sequence one, resumes in the same native context at sequence two
for one polled reusable-mailbox request, and blocks again while the AP remains
`IdleServing`. After stable acknowledgement of the first timer sequence, the
later runtime also handles one separate TimerQueue-to-Notification delivery in
that idle context at exact `0xe8` sequence two. None of these translated-runtime
proofs or services is part of this physical rendezvous.

## Audited resource bounds

The SAL-side dispatcher and coordinator operations allocate nothing and are bounded by the one-through-four-processor
profile. The native bridge does not move `sp` before either returning to SAL or committing the owned stack/RSE switch;
the no-return continuation executes on the AP's 32 KiB pair. Target builds retain the common 512-byte per-function
memory-stack frame gate at O0 and O2.

Native inspection must retain `cr.lid`, the direct dispatcher call, `ptr.i`/`srlz.i`, the complete fallback epilogue,
the physical record loads and validation, the bank/RSE/stack switch, the direct no-return continuation call, and the
local fail-stop. The raw entry-address materialization uses `R_IA64_IMM64`; direct calls use `R_IA64_PCREL21B`. The
object contains no `R_IA64_FPTR64LSB`, `.opd`, unwind-state, or initialization-array dependency.

The architectural behavior follows Sections 3.2.4, 3.2.5, 3.2.6.3, and 3.3.2.1 of the
[Intel Itanium System Abstraction Layer Specification, Revision 3.4][SalSpecification], the procedure-call rules in
the [Intel Itanium Software Conventions and Runtime Architecture Guide][ItaniumConventions], and the TLB purge and
page-size rules in Volume 2, Sections 4.1.1.7 and 4.1.4 of the
[Intel Itanium Architecture Software Developer's Manual][ItaniumArchitecture].

[SalSpecification]: https://www.intel.com/content/dam/www/public/us/en/documents/specification-updates/itanium-system-abstraction-layer-specification.pdf
[ItaniumConventions]: https://web.archive.org/web/20031013201915id_/http://developer.intel.com/design/itanium/downloads/24535803.pdf
[ItaniumArchitecture]: https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf
