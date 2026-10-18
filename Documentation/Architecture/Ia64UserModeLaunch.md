<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 User-Mode Launch Boundary

## Scope

`Ia64UserModeLaunch` is the allocation-free native boundary from an active
IA-64 supervisor call frame to one PL3 payload. `Launch` exposes the
one-way interface, while `LaunchResumable` also recognizes a later raw
restoration of the retained supervisor chain. It does not define a portable user
context, an AMD64 implementation, or the system-call ABI. The initial successful
target transition does not return normally. Hosted builds can validate and
inspect the plan but report native transition unavailable.

The caller retains one 544-byte `Ia64UserModeSupervisorContinuation` and the
supervisor memory-stack and RSE ranges named by it. The launch leaf captures the
complete synchronous P64 state represented by `Ia64KernelRegisterState`, then
publishes `Retained` before detaching either live stack. The current runtime IVT
implements the `break` vector used by the system-call transport. Query, send,
and receive restore the interrupted PL3 frame directly. Wait instead retains
the syscall bridge, raw-restores this supervisor chain, and publishes
`ResumePending`; `LaunchResumable` returns `SupervisorResumed` without
interpreting the adapter return register. After the exact worker is blocked,
woken, and readmitted, the reverse continuation restores the retained syscall
bridge and returns its response to PL3. Other unimplemented vectors remain
fail-stop.

The continuation's final word is an explicit owner discriminator. It is
`None` at initial launch and while ordinary PL3 code runs. A resumable syscall
claims `SystemCallWait` before changing any supervisor state and releases it
only after the reverse switch completes. The page-fault entry path has a
distinct reserved owner value, so the two native transports cannot silently
share one supervisor record. A transport may clear or fault the shared
register image only while its own owner value is present; detecting a foreign
or unowned record faults local transaction state without changing that record.

## Validated contract

Validation is pure and changes no caller byte. The request must provide:

- a PAL-derived implemented virtual-address MSB from 50 through 60 (SDM rev. 2.3
  Vol. 2 Part I 4.3.2: every processor implements at least 51 bits);
- page-aligned, page-sized code, downward memory-stack, and RSE backing-store
  ranges in Regions 1 through 7;
- three pairwise-disjoint user virtual ranges, with each stack from 16 KiB
  through 64 KiB;
- a 16-byte-aligned entry bundle wholly contained in the code range;
- an initial P64 global pointer that is either zero or an eight-byte-aligned,
  implemented address in Region 1 through 7;
- a fresh, 16-byte-aligned continuation object in Region 0, containing a
  byte-zero register image and the `None` owner;
- page-aligned, pairwise-disjoint 16--64 KiB active supervisor memory-stack and
  RSE ranges in Region 0, neither of which aliases the continuation object; and
- the exact processor-state requirements below.

The integration owner, rather than this integer-only validator, must prove that
all named ranges are mapped with their required PL3 or PL0 permissions, remain
stable for the transition, and have distinct backing storage where aliasing
would violate ownership. A nonzero global pointer must name readable mapped
user memory; the P64 image decoder establishes that property for ELF input.
Guard pages are the mapping owner's responsibility.

The initial user stack pointer is exactly `MemoryStackEnd - 16`, preserving the
P64 scratch area. The execution probe writes its stack sentinel at `[r12]`,
which is `MemoryStackEnd - 16`. The initial `ar.bspstore` is exactly the first
byte of the user RSE range. `ar.rnat`, `ar.unat`, `ar.pfs`, `ar.lc`, `ar.ec`,
`ar.ccv`, and `cr.ifs` start at zero. `ar.rsc` is `0xC`: lazy mode, PL3,
little-endian RSE, and zero `loadrs`.

`InitialArguments` is an opaque four-word launch input. Native launch transports
`Register4` through `Register7` into user `r4` through `r7` without
interpreting them. The BSP runtime passes capability handles in register pairs
(selector, then authenticator): the initial image receives its bootstrap
Endpoint in `r4`/`r5` and its DiagnosticChannel in `r6`/`r7`, while the
synthetic probe receives its ProtectionDomain handle in `r4`/`r5` and zero in
`r6`/`r7`. All four words are loaded before the commit cut into bank-one
carriers: `r23`/`r24` for `r4`/`r5` and `r21`/`r22` for `r6`/`r7`. After the
cut, the no-fault tail copies the carriers to `r4` through `r7` in one
instruction group and clears the carriers in the next, as part of
static-register canonicalization.

`InitialGlobalPointer` is the P64 global-pointer value delivered to user `r1`.
The native leaf loads it from plan offset 96 into bank-one carrier `r31` before
the commit cut. The no-fault tail copies that carrier to `r1`, then clears
`r31`. Zero is an intentional compatibility value only for the fixed,
relocation-free synthetic execution probe, which does not use GP-relative
addressing. The default initial-image launch supplies the nonzero GP decoded
from its P64 entry descriptor.

## Processor-status split

The native leaf can compare only fields exposed by `mov-from-PSR`. Its exact
observable mask and value are:

```text
mask = 0x0000_0010_080E_E002
value = 0x0000_0010_0802_2000
```

This requires little-endian execution, translated instruction/data/RSE access,
both floating-point halves enabled for the continuation spills, interruption
collection enabled on entry, external interruption delivery and protection
keys disabled, and native IA-64 translation state compatible with the launch.
`mov-from-PSR` does not expose the high `bn` field or `is`, and it does not
establish CPL or VM. The request therefore carries four trusted integration
invariants which validation requires exactly: CPL0, VM0, native instruction set
(`is=0`), and bank one (`bn=1`). They are declarative preconditions, not claims
of native observation.

The fixed user `cr.ipsr` is:

```text
0x0000_1013_080E_2000
```

It selects PL3, `bn=1`, `is=0`, translated instruction/data/RSE access, and
interruption collection, while leaving external interruption delivery off.
Both `dfl` and `dfh` are set. The probe is therefore integer-only: any low- or
high-floating-register use takes a disabled-floating-point fault, which the
present IVT handles by fail-stop. `ar.fpsr` is nevertheless sanitized to the
nucleus default `0x0009_804C_0270_033F`; preserved supervisor floating state is
captured before that write and cannot be observed at PL3 while both halves are
disabled.

User execution uses bank one. Heavyweight privilege entry selects bank zero,
whose banked static scratch registers remain untouched by user execution. Starting the
user probe in bank zero would defeat that entry invariant.

The resumable probe does not provide a general isolation boundary. The
runtime refill handler requires its Region-0 context address to remain in
`ar.k3`, which IA-64 applications can read, and the IA-32 compatibility
application registers are not normalized by this launch leaf. The fixed
payload neither reads nor depends on those registers. Running untrusted
programs requires an opaque refill-context lookup and a fully defined
application-register image.

## Native commit ordering

Before mutation, the target-only leaf checks the observable PSR fields, requires
`ar.rsc == 0`, rechecks that the continuation is fresh, and verifies the live
`r12`, `ar.bspstore`, and `ar.bsp` against the retained supervisor ranges. Either
check failure returns without changing processor or continuation state.

The leaf then captures the P64 continuation, executes `cover`, `flushrs`, and
sets `ar.rsc` to lazy mode before recording `ar.rnat` and the preserved general
NaT collection. Every user launch-plan load, including the global pointer and
all four initial-argument words, completes before continuation publication. After
the final continuation store and memory fence it executes:

```asm
rsm 0x6000 // psr.i | psr.ic
;;
srlz.d
;;
```

There are no memory accesses, calls, or fallible validation steps after this
cut. The no-fault tail executes `invala` in its own instruction group, then
scrubs the physical stacked-register file: every physical stacked register was
last written by the supervisor, and PL3 must never find one of those values in
a register it allocates. `alloc` grows the zero-sized cover frame to all 96
stacked registers, which PAL_RSE_INFO proved is the whole physical file
([capture](Ia64FirmwareHandoff.md)), each of `r32`-`r127` is cleared with its
NaT bit, and a second `alloc` restores the zero-sized frame. `flushrs` left no
dirty register and the RSE is in lazy mode, so neither `alloc` moves memory.
The tail then installs the new `ar.bspstore`, zeroes RNAT/UNAT/PFS/LC/EC/CCV,
installs the sanitized FPSR and PL3 RSC, writes `cr.iip`, `cr.ipsr`, and
`cr.ifs`, installs `r12`, delivers the retained GP in `r1` and launch words in
`r4` through `r7`, clears every carrier and the remaining static general,
predicate, and branch registers used by the probe, and executes `rfi`. Current
interruption collection must be off for the
control-register writes and `rfi`; the returned IPSR independently re-enables
collection while keeping delivery disabled.

The native rejection statuses distinguish incompatible observable processor
state from an active stack/RSE mismatch. The hosted adapter reports native
transition unavailable. `LaunchResumable` first recognizes the exact
`ResumePending` continuation state produced by a raw supervisor restoration;
without that state, status zero or any unknown adapter return is
`UnexpectedAdapterReturn`. The compatibility `Launch` wrapper maps even a
recognized supervisor restoration to `UnexpectedAdapterReturn`, preserving its
one-way API.

## Verification boundary and uncertainty

The contract is covered by strict hosted tests for fixed layouts, address and
range rejection, exact GP/PSR/RSC/SP/BSPSTORE plans, zero-GP compatibility,
continuation atomicity, and all adapter-return mappings. The native source is
compiled at IA-64 O0 and O2, closed with the project-owned memory runtime
without unresolved symbols, checked against the 512-byte per-function stack
ceiling, and inspected for pre-cut GP loading, post-cut `r1` installation,
carrier clearing, and the serialized `rsm`/`srlz.d`, `invala`, RSE,
control-register, and `rfi` sequence.

This establishes source, object, and emulator consistency, not physical Itanium
proof. Montecito QEMU tests exercise the initial PL3 launch,
direct Query/send/receive returns, raw restoration of the retained supervisor on
Wait, and the reverse restoration of the retained syscall bridge after
block/wake/readmission. The empty-frame `cr.ifs.v=0` launch and RNAT behavior
after replacing `ar.bspstore` still require confirmation on physical Itanium.
