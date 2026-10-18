<!-- SPDX-License-Identifier: Apache-2.0 -->

# Initial Address Space

The [boot protocol](BootProtocol.md) defines loader/kernel ownership. Initial
mappings consume placed ELF ranges described in the [kernel image contract](KernelElf.md).

## Scope

`InitialMappingPlan` is the architecture-independent contract between physical-memory bootstrap and the first native
translation tables. It records what must be mapped, how it may be accessed, how it is cached, and why the mapping
exists. It does not allocate page-table pages, write an architecture entry, enable translation, flush a TLB, or
dereference a physical allocation.

The plan is constant-initializable, uses no heap, and retains at most 256 mappings in an 8,208-byte object. Calls are
single-CPU bootstrap operations. A failed addition does not change the plan, and sealing makes it immutable.

## Common validation

Every mapping uses 4 KiB pages and must have nonzero, page-aligned, non-wrapping virtual and physical extents. Only
known permission, cache-policy, and purpose values are admitted; read permission is mandatory. Virtual ranges may not
overlap. Writable-executable mappings are rejected, as is any physical overlap where one alias is writable and another
is executable. Every alias of one physical byte must use exactly the same permission bits and cache policy; a read-only
purpose therefore cannot be bypassed through a writable alias.

The purpose field constrains protection and cache policy:

- nucleus code and interrupt-vector tables are read/execute and write-back;
- nucleus read-only data and the boot handoff are read-only and write-back;
- nucleus writable data, memory stacks, register backing stores, and page tables are read/write, non-executable, and
  write-back;
- firmware runtime code is read/execute and firmware runtime data is read/write;
- device memory is read/write, non-executable, and either uncached or write-combining.

The purpose describes a mapping's use; it is not a `Frame` ownership transfer. An equal-protection, equal-cache alias is
permitted so an architecture bootstrap can temporarily retain an identity mapping while entering a higher-half
mapping. The architecture bootstrap must account for both aliases, remove transitional aliases before admitting an
untrusted protection domain, and perform the required translation and instruction-cache synchronization.

Mappings are retained in strict virtual-address order. `Seal()` requires at least one nucleus-code mapping, one
nucleus-stack mapping, and one page-table mapping, or a combined `NucleusBootstrapRuntimeData` mapping supplying both data roles. These are presence checks, not proof that the selected physical
pages were allocated to the nucleus; the architecture builder must bind every mapping to explicit physical ownership.

## Architecture boundary

A native builder consumes only a sealed plan and may reject additional target constraints. It must never add write or
execute permission, change cache policy, silently truncate an address, or omit a mapping while reporting success.

### AMD64 tables and activation

The loader validates its own canonical seven-section PE32+ application
before accepting firmware placement. That parser checks geometry, W^X,
imports and relocations; it is not the kernel's mapping producer. The separate
kernel ELF parser validates its note and PT_LOAD geometry, then the loader
copies seven placed ownership ranges into BootInfo. Their roles remain code,
read-only data and writable data; `.note.asteria`, `.got` and the metadata
page replace PE-specific ranges. The kernel owns no loader-image mapping.

The pure AMD64 bootstrap mapping builder binds that placed kernel layout to the final `BootInfo` and an exact 23-page
`LoaderData` allocation receipt. Page zero, a noncanonical or wrapping extent,
overlap with the image, missing write-back
capability, runtime attribution, a freeable image page, or incomplete Loader/LoaderData provenance is rejected
before either output changes. The reservation consists of one unmapped lower guard page, four identity-mapped
read/write non-executable stack pages, sixteen identity-mapped read/write non-executable page-table pages, and two
identity-mapped read/write non-executable descriptor-table pages. The
seven note-derived ownership ranges add exact identity mappings with purpose-derived W^X permissions; the header page must
retain Loader provenance but is not needed after parsing and is not mapped. Success seals exactly ten mappings and
publishes a fixed summary last. The builder writes no reserved byte and performs no privileged operation. Across the
image and reservation bounding envelopes, at most thirteen four-level table
pages are needed even when both cross independent
512 GiB, 1 GiB, and 2 MiB prefix boundaries, so the sixteen-page table reservation has an explicit bound.

The AMD64 initial builder implements four-level translation with 4 KiB leaves. Before writing output it validates
48-bit canonical ranges, the reported 32-through-52-bit physical width, exact table-page demand, and a physically
contiguous storage extent wholly covered by one `PageTable`-purpose mapping. Any non-`PageTable` physical alias of
that storage is rejected. The complete writable extent must also be non-wrapping and disjoint from the mapping plan,
capability record, storage descriptor, and explicit build-result object, so clearing cannot invalidate retained state.
The result is assigned only on success and otherwise remains byte-identical; its backing physical storage is also a
caller-proved non-alias. The builder accepts at most 4,096 table pages, so clearing and leaf emission remain bounded;
unused owned pages are zeroed. Public validation failures precede the first write. A defensive internal-invariant
failure after commit begins zeroizes the owned extent rather than restoring its prior bytes.

A constant-initializable 8,288-byte `InitialPageTablePreparation` provides the AMD64 ownership boundary above that
pure encoder. Its single attempt copies every mapping into an internal sealed `InitialMappingPlan`, invokes the builder
only on that retained copy, checks the returned root and counts, stores a const view of the generated table extent, and
publishes `Prepared` last. The source plan therefore has no post-return lifetime obligation. A terminal failure exposes
neither the copied plan nor a native summary. A table range overlapping the preparation owner or any caller-owned input
is rejected before the attempt begins and leaves the owner fresh; a valid retry is then permitted. After success the
borrowed physical pages must remain reserved, mapped, and byte-stable until a later activation boundary replaces or
retires the root. Preparation performs no control-register, PAT, TLB, interrupt-state, or stack transition.

The AMD64 firmware path retains the parsed loaded-image layout, obtains one exact 23-page `LoaderData` allocation, and
keeps it owned before the final boot-services transition. After each final map is normalized, a no-firmware callback
binds the complete allocation to write-back, non-runtime `LoaderData` provenance. On the first candidate it binds the
boot-services identity alias, captures CPUID support for MSRs, PAT, PGE, long mode, NX, and the implemented physical
width, invokes the address-space owner, and completes every page-table, stack, GDT, IDT, and descriptor-register-image
write and readback before the corresponding `ExitBootServices`
call. A stale-key retry performs only validation and requires the exact same receipt, summary, retained plan, and
prepared state; it never regenerates any byte. Successful exit therefore leaves a retained sealed ten-entry plan,
an exact root/count summary, a zeroed 16 KiB stack, exact GDT/IDT pages, and fully encoded immutable table storage
without relying on post-exit write permission. The lower guard remains untouched and unmapped.

The same single-use owner performs the post-exit activation without accepting a caller-supplied root, stack,
descriptor image, callback, or token. It first revalidates the complete retained plan, all table bytes including
unused storage, every stack and descriptor byte, the processor snapshot, and the current CR3/control/MSR state. The
boot establishes four control bits itself instead of requiring them of the firmware (older firmware leaves them
otherwise): the processor snapshot records CR0, CR4 and EFER as `EstablishInitialControlState` makes them (CR0.WP and
EFER.NXE set, CR4.PGE and CR4.PCIDE clear), the comparison with the current state applies the same idempotent
function to it, and the activation writes them after ExitBootServices, once validation has passed and before the
assembly corridor, so no firmware code runs under them. EFER.NXE is set only on a processor whose paging capability
capture found execute-disable; without it the boot stops before ExitBootServices with a console report. Clearing
PGE invalidates every TLB entry, global ones included, before the corridor loads the owned root. A PCID left in
CR3[11:0] is still rejected rather than cleared. The
profile rejects every `Global` leaf and requires CR4.PGE, LA57, PCIDE, CET, PKS, LASS, supervisor LAM, and every high
CR4 bit including FRED to be clear; it also rejects RFLAGS.TF/DF/NT/VM. CR0.WP, EFER.LME/LMA/NXE, CR4.PAE and PAT
index zero as write-back are mandatory; the assembly corridor asserts them again. Every page the initial address space
maps must have the MTRR type write-back or uncached, which keep it coherent and strongly ordered: uncached wins an
overlap, and disabled MTRRs (IA32_MTRR_DEF_TYPE.E clear, as Hyper-V presents them) make every page uncached, which is
slow on hardware and, under nested paging, not the guest's to decide. Write-combining, write-through and
write-protect ranges over such a page are refused, with the MTRRs on the console. Intel SDM volume 3A section
12.11.2.3, figure 12-7, ends the PhysBase and PhysMask address fields at MAXPHYADDR-1 and reserves the bits above.
The decoder ignores PhysMask bits at and above the CPUID physical-address width;
they do not participate in address matching. Set low reserved bits (PhysBase 11:8, PhysMask 10:0) are still refused. The
application processors' startup code sets its own CR0 (PE, WP, PG), CR4 (PAE from the reset value) and EFER (LME,
NXE), and every processor's entry install then writes CR4 whole. The assembly corridor executes `cli`, installs
the owned IDT, clears DF, installs the owned GDT, switches to the owned stack, performs the cache-disable/`WBINVD`
sequence, clears PGE, reloads the inherited root, writes IA32_PAT=`0x0007040100070406`, loads the owned root, flushes
again, and restores CR0. It then reads back CR3, CR0, CR4, EFER, PAT, flags, and the stack sentinel before reporting
the stage event `ASTERIA:STAGE:AMD64-INITIAL-ADDRESS-SPACE` and entering the native runtime. MTRRs are validated
but not rewritten; EFER is rewritten only to set NXE.

Pre-EBS preparation and post-EBS revalidation run under the same captured CR3, and no firmware callback or project
writer is permitted between successful exit and activation. This is the explicit old-root identity-continuity TCB;
the activation path does not walk firmware-owned page tables. The IDTR, GDTR, and RSP become
owned, but `lgdt` does not reload the cached CS, SS, TR, or LDTR state. All initial gates use IST zero and assume the
already selected CPL0 stack. NMI or machine-check delivery before `lidt`, a fault in the new stack/code mapping, and
double-fault recovery are outside this initial corridor's guarantee. The subsequent
[AMD64 runtime](Amd64BootRuntime.md) installs owned segment/TSS state, separate
IST stacks, and per-CPU entry tables before user execution and AP service.

### IA-64 translation resources

IA-64 capability discovery has a separate pure decoder for the four return registers from `PAL_VM_SUMMARY`,
`PAL_VM_PAGE_SIZE`, and `PAL_MEM_ATTRIB`. It requires successful call status, zero reserved results, architected
address/RID/key relationships, at least 8 ITRs and DTRs, at least 16 PKRs, a coherent translation-cache topology,
every mandatory insertable and purgeable page size, and the write-back/uncached attributes needed by this profile.
It also rejects page-size bits below the architectural 4 KiB minimum and requires every insertable size to be
purgeable. It widens the 8-bit maximum-register indices before adding one, so a reported index of 255 becomes 256
rather than wrapping. The physical static-PAL coordinator executes and serializes those calls on the BSP; the decoder
accepts only their captured register values. Each AP repeats capture on that processor.

Before those PAL calls can occur, a pure SAL locator binds the unique physical UEFI configuration-table pointer to a
non-overlapping IA-64-permitted boot-time memory-map extent before `SetVirtualAddressMap`. It enforces runtime/ACPI
8 KiB layout rules while treating UEFI protection attributes as capabilities, not current settings. A bounded capture
layer moves at most 64 KiB into caller-owned storage and gives only that copy to the parser. The parser validates the
table checksum and revision, exact descriptor stream, raw PAL/SAL/GP addresses, firmware-owned ITR(0), and optional
purge/AP metadata. The post-exit path runs a pure binder that revalidates the normalized handoff and binds the PAL
entry to `EfiPalCode`, the SAL entry to runtime code, and the SAL GP to runtime code or data.
None of these stages executes firmware. The privileged adapter must still validate every auxiliary referenced
physical extent and cross-check the firmware ITR address and page size against the decoded PAL capabilities before
using either value.

The IA-64 initial encoder is a pure, bounded power-of-two range adapter. It validates the decoded physical,
implemented virtual-address, region-identifier, protection-key, insertable-page-size, memory-attribute, and
hardware-walker capabilities. It maps common read-only, read/execute, and read/write permissions to PL0 IA-64 access
rights and derives the architected present, accessed, dirty, exception-deferral, memory-attribute, physical-page,
page-size, key, and region fields. `EncodePage` retains the 4 KiB interface, while `EncodeRange` admits a naturally
aligned PAL-supported range wholly contained in one common mapping. Write-through is rejected because IA-64 has no
matching translation memory attribute.

One region register selects one address-space identity for every mapping in that region. The encoder therefore
requires all mappings intersecting the selected 2^61-byte region to agree with the region's shared/private setting;
a common `Global` mapping cannot silently share a region identifier with private mappings. Enabling the VHPT walker
also requires a reported hardware walker. On success the encoder returns the exact translation word, ITIR, IFA, and
RR value without allocating storage, mutating the plan, or executing a privileged instruction. The activation adapter
must keep PSR.vm clear for these address rules, program the RR before inserting the translation, and perform the
architecturally required data and instruction serialization.

The fixed-capacity initial translation-resource planner consumes the sealed common plan, validated PAL capabilities,
the parsed firmware ITR(0), and one region configuration per IA-64 virtual region. It greedily selects the largest
supported naturally aligned range at each address, which deterministically minimizes register use for the dyadic
page sizes, and retains at most 256 encoded entries. The native four-mapping seed contains three image mappings
normalized by adjacency and identical policy from the validated six-section geometry, and one combined
bootstrap/runtime page. The disjoint physical bootstrap interface retains eight mappings. It reserves firmware ITR(0) separately in capacity accounting and checks the resulting DTR/ITR
counts against the PAL-reported limits. The canonical seed configures RR0 alone as shared at RID
`0x2000`; RR1 through RR7 are not shared and contain no current seed mapping. The planner proves complete locked DTR
coverage for every seed mapping and locked ITR coverage for every executable chunk. Firmware ITR(0) remains reserved,
so the reported instruction-register count includes that slot and the planned executable entries start at ITR(1).
The planner rejects insufficient PAL register counts before publishing an immutable output and leaves a failed output
byte-unchanged. It also requires the firmware ITR page size to be both
insertable and purgeable and its complete virtual range to be implemented before retaining the purge operands. The
published object seals the encoded value for all eight native region registers, including regions with no current
mapping, so the later commit never rereads mutable region-policy input. Until PKR and PTA/VHPT ownership is defined,
the bootstrap profile accepts only protection key zero with the hardware walker disabled. It also conservatively
requires every active virtual region to use a distinct RID, because hardware matches
translation registers by RID plus VA[60:0], not by virtual-region number. Active native regions also may not use the
SAL handoff RIDs `0x1000` through `0x1003`; this initial profile does not rely on immediate reuse of a firmware
address-space identity. The planner rejects every native executable extent whose validated RID and low 61-bit
interval intersect firmware ITR(0). The firmware RID is derived from SAL's specified `0x1000 + VRN` handoff identity
for virtual regions zero through three. Thus the published register set can be installed without an architectural TR
overlap even when two displayed 64-bit addresses use different VRNs.

The IA-64 executable path does not publish those two plans independently. A constant-initializable 20,696-byte
`InitialAddressSpacePreparation` owns the 8,208-byte common mapping plan, the 12,384-byte translation-resource plan,
their summaries, and the exact nested failure. Its single BSP call enters a terminal state before invoking the mapping
builder, invokes the resource planner only after the common plan is sealed, checks both child summaries against their
owned objects, and validates both storage views before publication. In the disjoint shape the unique 32 KiB
IVT mapping must be RX/write-back and the unique eight-page RSE mapping RW/NX/write-back. In the consolidated
shape the static IVT is covered by image text and the exact RSE range by the combined runtime mapping. The
disjoint commit fills 2,048 fail-stop bundles and publishes instruction coherence; the consolidated commit never
writes the static table. Both shapes zero the exact 32 KiB RSE range and execute `mf`. Only then does it write `Prepared`. A pure preflight first proves that both complete storage views are
disjoint from each other, the owner, and every retained input. An owner-alias rejection therefore leaves the owner and
both destinations unchanged and permits a corrected first attempt; once a non-aliasing transaction is admitted,
mapping, resource, IVT, or RSE rejection leaves the owner permanently failed and no retry or reset can replace the
first failure. Checked read-only accessors return `NotPrepared` for a fresh owner and `PreviousPreparationFailed` for a
failed owner, so neither child plan nor its partial summary is obtainable before atomic publication. This is a
bootstrap publication boundary, not a synchronization primitive: same-CPU preemption, interrupts, reentry, and
concurrent access remain excluded by the executable handoff contract.

The PAL capability coordinator completes all permitted static PAL calls while inherited physical addressing and the
required interrupt state are established. It publishes value-owned virtual-memory capabilities and a validated
`PAL_PTCE_INFO` result for the current processor before installation begins. The privileged installation layer then
validates every operand before its commit point, selects register bank one explicitly, clears `PSR.ic`, walks the
complete local translation-cache purge domain, and serializes completion. It next purges and serializes ITR(0), writes
all eight sealed native region registers, serializes and reads back all eight values, and fail-stops before any native
TR insertion if a value differs. Only then does it insert DTRs and ITRs with the planned indices and perform final
data/instruction serialization. It never assigns ITR(0) to a planned entry. On success `PSR.ic`, `PSR.it`, `PSR.dt`,
and `PSR.rt` remain clear, so this is an install-only transition rather than native address-space activation. VHPT
construction, miss handling, replacement, and later shootdown remain outside the encoder, planner, and installer.
AMD64 page-table encodings do not enter the IA-64 abstraction.

The pure `PAL_PTCE_INFO` decoder rejects a nonzero PAL status, either zero loop count, more than 65,536 `ptc.e`
operations, or any wrap in the addresses actually consumed by the nested purge loop. The operation ceiling is an
explicit initial-target real-time bound: hardware reporting a larger local purge domain is outside this bootstrap
profile and fails before interrupts are disabled. The decoder preserves PAL's base, counts, and strides exactly;
zero strides and an unaligned base are not rejected because PAL specifies no such restriction. For outer count
`count1`, inner count `count2`, outer stride `stride1`, and inner stride `stride2`, the last used address is validated
as `base + (count1 * count2 - 1) * stride2 + (count1 - 1) * stride1`. The privileged installer executes the specified
loop on that same processor and serializes it; decoding alone does not purge a translation.

## Boot integration

IA-64 reserves one guarded runtime page from Conventional memory in the final map, with a
128 KiB bootstrap data prefix, `Ia64BspRuntimeMemory` after it, and a 32 KiB RSE at the page top.
The page is 16 MiB preferred, with an architected insertable fallback large enough for all three. Guards are outside its ends, not
holes inside the pinned DTR. The four native seed mappings are Global: the RW/NX runtime page and
three image permission groups. The prefix retains these offsets:

| Page offset in runtime page | Use | Protection |
| --- | --- | --- |
| 0-7 | 32 KiB memory stack | RW/NX, write-back |
| 8 | Translation storage | RW/NX, write-back |
| 9-31 | Retained padding | RW/NX; untouched |
| 32 onward, below top RSE | Runtime memory owner | RW/NX, write-back |
| Final 8 pages | 32 KiB RSE backing store | RW/NX, write-back |

The bootstrap fail-stop IVT is an immutable 32 KiB self-loop table in `.text`, sharing the image ITR.
The new mapping owner names combined bootstrap/runtime data ownership; platform validation proves
exact stack/RSE containment and static IVT instruction coverage. The disjoint 32-page bootstrap
interface and its eight seed mappings remain supported for physical callers and hosted
characterizations; the native path selects consolidation by supplying a bootstrap view at the runtime
page's start. The native preparation validates the static table and never writes it; its native address
comes from an IMM64 leaf. It zeroes the RSE before publication and later clears only the runtime
owner, preserving the live bootstrap prefix and top RSE. The [budget document](Ia64TranslationRegisterBudget.md)
records the guard allocation footprint, PAL checks and nested-fault rules.

After reading boot files using its bounded file/console scratch,
and before initializing runtime image globals, the IA-64 firmware entry copies the already relocated image
into an exact 16 MiB-aligned LoaderCode allocation, applies its checked DIR64/IMM64 fixups, publishes
instruction coherence and enters the copied continuation descriptor. `EFI_LOADED_IMAGE` continues to
name the firmware source; the executing image derives its own base and end from relocated linker
symbols. The source and the copy both remain Loader memory in the final map. Temporary alignment
slack is returned before normal boot processing.

`Ia64LoadedImageLayout` has a placement discriminator (168 bytes total). `AlignedCopy` binds the
executing 16 MiB placement, and the bootstrap planner requires three naturally aligned 4 MiB groups
and LoaderCode provenance. Firmware and hosted geometry retain the original general section profile.
The GOT and relocation-time immutable objects join `.rodata`, and both translated landing addresses
are `IMM64` operands rather than short GP-relative references. W^X remains exact across all groups.
Build checks enumerate all 256 firmware-source residues of the 16 MiB placement period, model the
aligned copy, require exactly 4 DTRs and 2 ITRs at every residue and exactly one run-time memory DTR,
using an 8+8 PAL report; a below-minimum PAL report must fail. A group that grows beyond 4 MiB fails verification rather than silently
increasing the mandatory set. The boot checks the sealed plan against PAL and the mandatory budget.

The [firmware handoff](Ia64FirmwareHandoff.md#physical-takeover-order) performs
physical PAL capture and plan preparation, then abandons the firmware stack/RSE
through one indivisible validate/clear/fence/commit operation. The fixed adapter
installs the owned stack and backing store before IVA/RR/TR takeover. A public
validation summary cannot authorize a separate switch. Every AP completes its
own PAL capture, local purge, and initial translation installation before the
BSP proceeds.

The [BSP memory owner](Ia64BspRuntimeMemory.md) then admits the runtime root and
IVT, and the initial-user owners install their objects and private-root mappings.
Only success permits translation activation and the
[BSP kernel runtime](Ia64BspKernelRuntime.md). Later AP stack/RSE aliases and
shared-root mappings extend the runtime ledger; they do not widen this sealed
initial seed.

AMD64 continues from its verified activation corridor into the
[native runtime](Amd64BootRuntime.md), which owns TSS/IST state, AP startup, private
CPL3 mappings, and one polled shared-root shootdown. General runtime mapping,
higher-half transition, and teardown remain separate work. The
[boot contract](BootContract.md#vm-validation) records both ISA VM matrices;
exact live-state checks belong to the VM test runner (`Tools/Vm/RunVmTests.py`).
