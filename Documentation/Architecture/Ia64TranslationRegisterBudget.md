<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 translation-register budget

The [boot protocol](BootProtocol.md) defines the independent loader/kernel boundary.
The kernel owns preparation and retained state in `Kernel/Boot/KernelEntry.cpp`;
the loader constructs and seals each receipt before calling preparation inside
the ExitBootServices transaction. No kernel link unit imports a UEFI
boot-application symbol. The kernel ELF reservation note supplies its three
4 MiB permission groups and the 16 MiB placement alignment; the loader reads
it from the CRC-checked boot-volume file. The temporary ELF source allocation
is released before the final-map transaction.
IA-64's runtime reservation is selected without firmware calls during
preparation from the architected minimum page-size set, retained across retries,
and checked against PAL after exit. The final-image residue proof still covers
the existing three permission groups and the 4 DTR / 2 ITR plan.

IA-64 guarantees eight instruction and eight data translation registers (ITRs and DTRs); a processor may
report more through `PAL_VM_SUMMARY`. The kernel's **mandatory set**, the translation registers correctness
relies on, fits 8 DTRs and 8 ITRs, including the reserved firmware ITR(0). Montecito reports 32 + 32;
those extra registers do not raise the mandatory budget. Further TR use must
preserve the bounds and mapping rules below.

## Why some translations must be registers

The kernel runs with the VHPT walker off. A TLB miss is refilled by the runtime IVT's alternate instruction and
data TLB handlers (`AsteriaIa64RuntimeRefillHandler`, Source/Platform/Ia64/Memory/Ia64RuntimeInterruptVectorTable.S:93).
They read the processor's `ar.k3` context, the active-binding table and the active root's
[ledger](Ia64RuntimeTranslationLedger.md), all in the run-time memory object. They insert a translation-cache
(TC) entry for the ledger record that covers `cr.ifa` (`itc.i`/`itc.d`, lines 211 and 215). A PL0 miss that no
record covers stops the processor (line 234). A TC entry can be evicted at any time, so:

1. Code that runs with `PSR.ic` clear (the IVT, each entry stub up to the point where collection is back on, and
   each exit path from `rsm psr.ic` to `rfi`) must lie in pages an ITR maps. An instruction miss with
   `PSR.ic` clear cannot be handled.
2. Data that code touches with `PSR.ic` clear must lie in pages a DTR maps: the refill handler's context,
   bindings and ledgers, and each entry's stack, RSE store and frame. A data miss with `PSR.ic` clear raises
   the Data Nested TLB vector, which the runtime IVT treats as a fail-stop.
3. The RSE follows rule 2: an entry switches `ar.bspstore` to a pinned store before it can spill.
4. A refill handler may touch only pinned memory, so accessing its lookup data cannot cause a nested miss.
5. Physical execution needs no register. The BSP runs physically until `InitialTranslationActivator`
   enables translation, after `Ia64BspRuntimeMemory::PrepareAndAdmit` has installed the runtime IVT and
   bound the boot root's ledger. An AP installs the same sealed plan before it parks and is admitted to the
   runtime IVT and shared root while still physical (`AdmitApplicationProcessorPhysicalRoot`), before its
   pivot enables translation.

## Inventory

Only `InitialTranslationInstaller` inserts translation registers (`itr.d`/`itr.i`,
Source/Platform/Ia64/Memory/InitialTranslationInstaller.S:132 and 146). It installs the one sealed plan, the
BSP's and each AP's from its own PAL capture (`Source/Kernel/Boot/KernelEntry.cpp`). It
purges the local translation cache first (`ptc.e` loop, InitialTranslationInstaller.S:49) and removes the
firmware's ITR(0) (`ptr.i`, line 73). Each installer checks the plan's counts against its own processor's
counts (InitialTranslationInstaller.cpp:468). The plan is the mapping plan of
`BootstrapMappingPlanBuilder` (Source/Platform/Ia64/Memory/BootstrapMappingPlan.cpp). The resource planner
covers each mapping with the largest naturally aligned PAL-insertable page at each address. It gives every
page a DTR and every executable page an ITR too (Source/Platform/Ia64/Memory/InitialTranslationResourcePlan.cpp:419).

| Translation | Registers | Size | Source | Why a register |
| --- | --- | --- | --- | --- |
| Firmware handoff | ITR(0), counted but purged | firmware's | InitialTranslationResourcePlan / InitialTranslationInstaller | reserved and never reassigned |
| Image `.text` | 1 DTR, 1 ITR | 4 MiB | BootstrapMappingPlan | runtime IVT, static bootstrap fail-stop IVT and every collection-disabled stub; data mirror retained by the current planner |
| Image note/read-only body/tail | 1 DTR | 4 MiB | BootstrapMappingPlan | read-only pin retained by the current planner |
| Image `.data`-`.bss` | 1 DTR | 4 MiB | BootstrapMappingPlan | native-preemption worker stack/RSE/frame and kernel globals |
| Runtime page | 1 DTR | 16 MiB preferred; one architected page on fallback | BootstrapMappingPlan / KernelEntry | bootstrap stack, translation storage, RSE, and `Ia64BspRuntimeMemory`, including refill state, per-CPU interrupt transports and the metadata slabs |

The boot has no separately allocated bootstrap reservation. Its bootstrap data view is the first
128 KiB of the runtime page: stack at +0..32 KiB, translation storage at +32..36 KiB, and reserved
padding at +36..128 KiB. The RSE occupies the final 32 KiB of the page, so its upward growth reaches
the upper guard without crossing the owner. The runtime owner begins at +128 KiB and retains
its exact object-derived corridor. The kernel places the registry and
Frame/MemoryObject slabs after the owner using the offsets in
[`Ia64RuntimeMetadataLayout.hpp`](../../Source/Platform/Ia64/Memory/Ia64RuntimeMetadataLayout.hpp).
Slab initialization runs in normal collection-enabled context. The layout constants budget the 3,416,064-byte owner, 2,686,976-byte
slabs, 10,506,240-byte bitmap area, a 4 KiB RSE separation page and the final
32 KiB RSE inside 16 MiB. The required extent is 6,270,976 bytes; 4 MiB
fallbacks are refused. These slabs add no DTR or ITR and never enter a
`PSR.ic`-clear corridor. All these data bytes share one RW/NX Global mapping with owner
`NucleusBootstrapRuntimeData`; sealing recognizes its combined stack/page-table ownership and
the platform proves exact subrange coverage before publication. The original disjoint-reservation
builder shape remains a supported physical bootstrap interface and a hosted characterization; the
native boot selects only containment at the runtime page's start. It emits four seed mappings.

The allocator retains alignment slack below the runtime page and one 4 KiB page above it. The last
4 KiB of that lower slack and the upper page are unmapped, untouched guards, outside the large DTR
and absent from all inherited root ledgers. The allocation is `(2 * pageBytes) + 4096`, with the
runtime page at `allocation + pageBytes`; retaining the lower alignment slack costs one additional
page-sized extent (16 MiB with the preferred geometry). Interior holes cannot be hardware guards
inside a pinned large page. The allocator does not reclaim that alignment slack. No guard is available for later allocation.

`InitialInterruptVectorTableInstaller.S` defines the 32 KiB static fail-stop image in executable,
non-writable `.text.ia64_bootstrap_ivt`, and an IMM64 address leaf. Its 2048 literal self-loop bundles
need no preparation writes. [Intel SDM Vol. 2, §5.5](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf)
preserves PSR.it/dt/rt on IVA delivery. Thus the table executes physically before activation and
shares the image ITR if ever used translated; it needs no dedicated register. Both BSP and AP admit
the runtime IVT while still physical before their activation/pivot. A translated IVA outside a pinned
instruction page would still be unsafe with collection disabled.

These are not registers:

- AP idle and worker storage is mapped through fixed Region-1 aliases recorded in the shared root's ledger
  (Source/Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimeLayout.hpp) and refilled as TC entries.
  Each AP's interrupt transport lies in the run-time memory object.
- User pages are TC entries refilled from their root's ledger, the boot pager's and the initial image's
  staged segments included, so their staging bounds (`Ia64InitialUserImageOwner.hpp`) change no count.
- MMIO (I/O SAPIC, PCI ECAM, AHCI) goes through `Ia64PhysicalMemoryAccess`, which clears `PSR.dt` around each
  access through the uncacheable alias (Source/Platform/Ia64/Device/Ia64PhysicalMemoryAccess.S:24).
- PAL and SAL are called physically during the boot. The boot application inserts no translation before or
  after `ExitBootServices`; firmware owns translation until the physical takeover.

DTRs = 1 (runtime page) + 3 (image) = **4**, and ITRs = 1 (reserved firmware)
+ 1 (image) = **2**, on the BSP and every AP, independently of the firmware load base. The native loader reads files with bounded boot/console scratch and places
the independent kernel ELF at a 16 MiB boundary before calling kernel entries.
Each PT_LOAD permission group has one aligned 4 MiB envelope. The loader PE
copy is not mapped by the kernel. QEMU reports
`ASTERIA:IA64-TRANSLATION-REGISTERS:DTR=04/20:ITR=02/20` (hexadecimal).

`Ia64FinalImageTranslationResidues` runs the production planners with an **8 DTR + 8 ITR PAL report**
for all 256 source residues, for production and test firmware. It requires exactly four DTRs, two
ITRs and one runtime-page DTR. Its negative capacity input is three DTRs: that must be rejected as
below the architectural minimum, rather than as exhaustion of an otherwise valid PAL report.
The hosted planner test also builds the consolidated shape with 8+8. `Ia64BootstrapIvtStaticImage`
checks table alignment, RX flags and every literal bundle in O0/O2 objects and both linked images,
rejecting six mutations per artifact. The address leaf has a reviewed disassembly golden.
`Ia64ImagePlacementBeforeGlobals` and relocation tests check copying and fixups.

### Growth

- The runtime-memory owner and its metadata slabs share one reservation page
  and one DTR. Their sizes and offsets are defined by
  [`Ia64RuntimeMetadataLayout.hpp`](../../Source/Platform/Ia64/Memory/Ia64RuntimeMetadataLayout.hpp);
  changing sizes within that page changes no TR count.
- Each image permission group has a 4 MiB envelope. Padding is part of its mapping and preserves W^X.
  Growth inside those envelopes changes no TR count. A group that outgrows its envelope fails the
  aligned-placement validation and residue proof; enlarging its page requires a deliberate layout change.
- `.data.rel.ro` and the GOT are in the read-only group. They are patched only while the copy is being
  built, and remain read-only after takeover. GP is absolute at `.rodata + 0x80000`; the BSP and AP
  translated landing addresses use `IMM64` `movl`, so they do not depend on GP's distance from text.

## Register roles

The mandatory set pins only what runs or is touched with `PSR.ic` clear, plus kernel text and data in large
naturally aligned pages that the boot places at that alignment. Everything else is a TC entry. The other
registers belong to the opportunistic allocator.

| Register | Mandatory use | Page |
| --- | --- | --- |
| ITR0 | firmware handoff until the installer purges it, then reserved | firmware's |
| ITR1 | kernel `.text`: the runtime IVT, every `PSR.ic`-clear path and the static fail-stop table | 4 MiB (16 MiB with growth) |
| ITR2-7 | spare; unused by the mandatory plan | |
| DTR0 | run-time memory page: the object, the boot stack, RSE store and translation storage, per-CPU interrupt stacks | 16 MiB |
| DTR1 | kernel `.data`/`.bss` | 4 MiB |
| DTR2 | kernel note/read-only body/tail view | 4 MiB |
| DTR3 | `.text` data mirror | 4 MiB |
| DTR4-7 | spare; unused by the mandatory plan | |

These slot names describe policy roles; the current planner assigns actual DTR indices in address
order (QEMU currently has runtime/text/read-only/writable at indices 0/1/2/3).

The text DTR mirror remains part of the mandatory plan. The alternate DTLB
handler scans the admitted ledger and supplies no identity-granule refill
window. A 16 MiB TC entry covering the image would overlap its live 4 MiB
writable DTR. Collection-disabled data references remain within locked runtime
or image mappings.

## Spare translation registers

`CheckIa64TranslationRegisterBudget` reports registers beyond the mandatory set
as spare. No opportunistic allocator is implemented. The native installer uses
only the sealed initial plan; TR and TC entries must not overlap within one RID.

## Boot check

After the plan is built, the boot calls `CheckIa64TranslationRegisterBudget`
(Source/Platform/Ia64/Memory/Ia64TranslationRegisterBudget.hpp; Kernel/Boot/KernelEntry.cpp). It checks the
plan's DTR and ITR counts against the processor's PAL counts and the mandatory budget
(`Ia64MandatoryDataTranslationRegisterBudget`, `Ia64MandatoryInstructionTranslationRegisterBudget`, 8 each). It also requires the architected eight of each kind. It writes
`ASTERIA:IA64-TRANSLATION-REGISTERS:DTR=<plan>/<PAL>:ITR=<plan>/<PAL>` in hex, then
`ASTERIA:STAGE:IA64-TRANSLATION-REGISTER-BUDGET`, or `ASTERIA:IA64-TRANSLATION-REGISTER-BUDGET:FAIL` and
stops. A processor that reports more registers does not raise the budget. Each AP's boot installation wrapper checks the same plan against its own PAL counts and
the mandatory budget before the generic installer checks every operand. Failure prints the same
visible budget failure line and stops the affected boot path.

## Independent kernel ELF boundary

[Kernel ELF](KernelElf.md) defines the reservation note and relocation contract.
The loader allocates the note's runtime body, lower alignment slack and upper
guard before ExitBootServices. The kernel imports the receipt and validates
its LoaderData provenance; it initializes no runtime byte before takeover.
The 128 KiB bootstrap prefix remains a subview, so there is still one runtime
DTR. PAL verifies the preferred 16 MiB page after exit. Legacy hosted physical
geometry retains its Conventional allocator and fallback-size tests.

The residue proof and VM observations use note ownership ranges validated
against the actual three PT_LOAD groups. They cover text, note/read-only
body/tail and data/BSS; `.opd` is inside the read-only body. SAL rendezvous
registers the kernel's relocated entry address and GP. The loader copies and
fixes both descriptor words, FPTR64/IPLTLSB sites and IMM64 fields before cache
publication. Nothing in the loader PE contributes to the kernel TR budget.
