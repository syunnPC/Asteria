<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 Firmware Handoff

This contract covers SAL discovery, physical PAL calls, and the transition out of
firmware-owned execution. The [boot contract](BootContract.md) owns the common
UEFI exit transaction; later translated execution belongs to the
[BSP runtime](Ia64BspKernelRuntime.md).

## SAL discovery boundary

UEFI publishes the SAL System Table GUID as a physical address, unchanged by
`SetVirtualAddressMap`. `SalSystemTableLocator` requires one GUID instance and
binds the complete header to a nonoverlapping readable memory-map extent.
Accepted types are boot-services data, runtime-services code/data, ACPI reclaim,
and ACPI NVS. Runtime code is accepted for combined firmware code/data images.
Runtime descriptors require `EFI_MEMORY_RUNTIME`; runtime/ACPI extents obey
IA-64's 8 KiB geometry. Other protection attributes describe capabilities, not
current access permissions.

Location runs before `SetVirtualAddressMap`. `SalSystemTableCapture` moves at
most 64 KiB into caller-owned storage and parses only that copy; overlap has
move semantics, and consumed inputs are not reread. The configuration array,
memory map, and source bytes remain stable until consumed. Success retains
inert values, not firmware pointers. The UEFI adapter scrubs its temporary
snapshot before exit.

The allocation-free parser validates the 96-byte header, `SST_` signature,
checksum, BCD revision, declared length/count, reserved fields, exact descriptor
framing/order, singleton rules, and complete stream consumption. It accepts SAL
2.8 through 3.x, including compatible later 3.x minor revisions; unsupported
major revisions are rejected. Repeated type-one memory descriptors stay opaque
because UEFI supplies the authoritative memory map.

Retained fields are:

- bundle-aligned physical PAL/SAL entrypoints and the nonzero SAL GP;
- optional platform features and purge-coherence metadata;
- firmware ITR(0) range/page size and the optional AP wake vector.

Malformed addresses, ITR encodings, wake vectors, duplicate singleton entries,
and reserved descriptor types are typed failures. Contained addresses are never
dereferenced by parsing; auxiliary lists still need bounded validation before use.

## Physical PAL call boundary

After exit, `SalEntrypointBinder` revalidates the normalized `BootInfo` map and
retained addresses. PAL must belong to `EfiPalCode`, SAL code to runtime code,
and the SAL GP to runtime code or data carrying `EFI_MEMORY_RUNTIME`. Complete
accesses must fit one descriptor and SAL's 63-bit physical range. The binding
copies values and containing regions; it contains no callable pointer.

`PalStaticCallAdapter` exposes only `PAL_VM_SUMMARY`, `PAL_VM_PAGE_SIZE`,
`PAL_MEM_ATTRIB`, `PAL_PTCE_INFO`, and `PAL_RSE_INFO`, with zero reserved
arguments. It snapshots and validates the PAL entry bundle, then uses static
PAL registers, manual `b0`, and a non-call branch to that same physical address.
It never converts the entry to a C++ function pointer or invents a
virtual-region alias.

The caller must retain the inherited physical UEFI/SAL environment at CPL0,
provide at least 8 KiB of free stack, exclude preemption and concurrent PAL entry,
and keep the binding stable. The adapter rejects incompatible PSR state, selects
bank one, disables interrupts and interruption collection, serializes, and enters
enforced-lazy RSE mode. After copying `r8`-`r11`, it restores caller PSR/RSE, GP,
return pointer, and preserved branch state.

Capability capture invokes the five calls in the order above and stops on the
first adapter, PAL-status, or decoder failure. Only complete success publishes
the owned CPU-local capability and purge record. Every AP performs its own
capture; a BSP record cannot substitute for it. `PAL_PTCE_INFO` admits at most
65,536 purge operations before privileged installation begins. `PAL_RSE_INFO`
must report exactly 96 physical stacked registers, the count of every IA-64
implementation and the architectural maximum frame
(`Ia64PhysicalStackedRegisterCount`), with no reserved hint or result bit;
anything else fails the capture in phase `RseInformationDecoding`. The PL3
entry paths rely on this count: one frame of all stacked registers spans the
complete physical file, which they scrub before user code can allocate a
stacked register ([launch](Ia64UserModeLaunch.md#native-commit-ordering),
[system-call return](Ia64UserSystemCallRuntime.md#kernel-entry-and-return)).

The physical adapter is valid only before native translation takeover. A virtual
PAL adapter would require an explicit ITR and matching DTR/DTC mappings. Firmware
ITR(0), its cache attributes, and SAL handoff RIDs remain reserved until the
validated native environment replaces them.

## Physical takeover order

1. Capture SAL before exit, then bind its entries and capture BSP PAL capabilities
   while the inherited physical context remains active.
2. Import conventional memory and prepare the sealed
   [initial mapping/translation plans](InitialAddressSpace.md). The BSP owns
   distinct 32 KiB stack, IVT, and RSE ranges. Publish IVT instructions and zero
   RSE storage only after complete validation.
3. Through one indivisible validation/commit boundary, clear the owned stack,
   drain the firmware RSE, install BSPSTORE with zero RNAT/PFS, set
   `r12 = stackEnd - 16`, and enter the fixed no-return continuation. Addressing
   remains physical. No public validation token authorizes a separate switch.
4. Install and read back the BSP fail-stop IVA before using the AP wake vector.
   Build the immutable BSP-first Local SAPIC map from ACPI and `cr.lid`.
5. For SMP, prepare all AP execution storage and immutable records before SAL
   registration or wake. Register and arm `OS_BOOT_RENDEZ` once, then send bounded
   Local SAPIC wake stores. SMP1 skips registration and wake.
6. Each AP claims its record, purges firmware ITR(0), abandons SAL's return context,
   adopts its dedicated 32 KiB stack/RSE pair, installs the shared fail-stop IVA,
   and publishes its PAL record. It then purges its local translation cache,
   installs/reads back all eight RRs, inserts the capacity-checked DTR/ITR plan,
   and publishes `InitialTranslationsInstalled` before parking.
7. The BSP requires every target's terminal success, freezes the complete CPU
   capability profile, and performs its own purge/RR/TR installation. Translation
   remains disabled until runtime memory admission completes.

The storage and rendezvous contracts are defined by
[AP execution ownership](ApplicationProcessorExecutionStorageOwner.md),
[SAL registration](Ia64SalBootRendezvousRegistration.md), and the
[rendezvous handler](Ia64SalBootRendezvousHandler.md). Failure retains allocated
or possibly live storage; partially initialized owners are not restarted.

## Runtime handoff and evidence

The BSP constructs the root ResourceDomain and
[runtime memory owner](Ia64BspRuntimeMemory.md), admits the shared root/runtime
IVT, and materializes and maps the initial user image in its nonresident private
root. Each step reports its stage event when it completes
(`BSP-RUNTIME-MEMORY`, `INITIAL-USER-IMAGE-OBJECTS`,
`INITIAL-USER-IMAGE-MAPPINGS`); only complete success permits
`InitialTranslationActivator` to enable translation through `rfi`, and the
translated continuation reports `INITIAL-TRANSLATION-ACTIVATION`.

Translated continuation maps AP runtime storage, prepares the BSP runtime and
initial-user request, and admits APs serially before PL3 launch. AP cooperative
service, timer delivery, shared-root shootdown, and optional BSP probes are
specified by the [BSP runtime](Ia64BspKernelRuntime.md), rather than by the sealed
initial seed.

Every completed boot step reports a neutral `ASTERIA:STAGE:<NAME>` event, and
a failure stops with its FAIL/ERROR diagnostic before any dependent stage. The
kernel's diagnostic scenarios (PCI and AHCI, AP runtime, shared-root shootdown,
the optional probes) record what they observed in the IA-64 evidence record
(`Source/Platform/Ia64/Diagnostics/Ia64EvidenceRecord.hpp`), whose address the
kernel reports once as `ASTERIA:EVIDENCE:0x<address>`. The VM runner
(`Tools/Vm/RunVmTests.py`) checks the stage order, reads and judges the record
through QMP, and owns the stopped-state checks. The `AP-RENDEZVOUS` stage covers
AP-side installation checks, but a BSP monitor snapshot does not directly
inspect every AP's RR/TR state. See the
[boot validation matrix](BootContract.md#vm-validation) and
[validation status](../ImplementationStatus.md#validation).
