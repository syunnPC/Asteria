<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 runtime interrupt-vector table

`Ia64RuntimeInterruptVectorTable` owns one immutable, 32 KiB-aligned
interrupt-vector table and four aligned, per-CPU contexts. The table is
read/execute storage. The architected alternate instruction-TLB slot at
`0x0c00` and alternate data-TLB slot at `0x1000` contain refill handlers. The
break slot at `0x2c00` begins with a direct one-bundle branch to
`AsteriaIa64UserBreakEntry`, which preserves PR and sends the system call's
immediate to the system-call entry and any other to the user exception entry
([system calls](Ia64UserSystemCallRuntime.md)). The external-interrupt slot at
`0x3000` likewise
begins with a direct one-bundle branch to
`AsteriaIa64ExternalInterruptDispatchEntry`. The dispatcher sends an
interrupt taken at PL3 (`IPSR.cpl` 3) to the PL3 interrupt claim
`AsteriaIa64UserInterruptClaim` ([user-fault entry](Ia64UserFaultEntry.md#pl3-interrupts)),
and otherwise uses the local-timer evidence route unless an exact BSP
one-shot control is armed; the IVT never bypasses it by branching directly to
any of these entries. Their remaining
63 bundles contain terminal stop branches. The native fault/trap slots in
[the complete inventory](Ia64UserFaultEntry.md#interruption-inventory)
carry selector 6 and their vector offset in `ar.k4`, then branch to
`AsteriaIa64UserExceptionEntry`, whose admission takes only a PL3
occurrence. This includes `0x5000`–`0x5300`; the
capture-only [page-fault entry](Ia64PageFaultEntry.md) is separate. Both alternate
refill handlers may escape a PL3 ledger miss to the complete user entry. Every
other slot (VHPT, data nested TLB, virtualization, IA-32 and reserved) is
fail-stop.
Replacement requires a separate serialized protocol and is outside this
component.

Installation is a post-firmware boundary. It must occur only after the final
SAL handoff because SAL rendezvous code may use the kernel registers as
scratch. For one validated CPU, the native leaf installs that CPU's immutable
context address in `ar.k3`, installs the alternate table address in `cr.iva`,
serializes instruction state, and verifies both readbacks. It does not enable
translation, interruption collection, or external interrupts and does not
switch stacks, RSE state, region registers, or translation registers.

Each admitted BSP or AP uses an align-16, 48-byte context containing the fixed
binding-wire base, CPU index, one system-call-entry-context address, and one
external-interrupt-entry-context address, followed by the page-fault context
address and the user-fault-entry-context address. The first two words are
immutable after installation. Each entry address starts at zero and is
release-published after its complete retained storage has been validated; the
corresponding bank-zero entry consumes it with an acquire load. The external
and page-fault words are published exactly once. The system-call word and the
user-fault word name the running user thread's contexts: each thread's
contexts are validated and admitted once (at most thirty-four per processor, one per
entry slot of the [runtime memory](Ia64BspRuntimeMemory.md)), and the
owning processor release-selects the admitted pair of the thread that is about
to enter PL3, with PSR.i clear, before its launch and whenever it returns from a
blocked system call or fault
([execution model](PagerChannel.md)). The system-call publication names its frame,
dedicated stack/RSE storage, and owning run loop. The per-processor external-
interrupt publication names one immutable timer context and one initially zero,
64-byte reusable evidence mailbox whose first 56 bytes are the captured payload.
The binding wire remains the single authority for the complete Region-0
runtime object corridor. `Ia64BspRuntimeMemory` derives that corridor from its
own page-aligned object extent and retains the one boot-lifetime IVT owner.
Before native installation, the IVT validates the prepared initial mapping and
translation-resource plans, complete instruction coverage for the table,
complete data coverage for the runtime object corridor, exact object placement,
the derived private binding slot, and the still-unadmitted state. Installation
itself does not require system-call execution storage. The later system-call
publication separately proves that its exact frame, 16-KiB memory
stack, and 32-KiB RSE range are inside the retained corridor, mutually disjoint,
and do not overlap the immutable context. External publication separately
proves the complete aligned 32-byte context and full 64-byte mailbox ranges are
inside that corridor, mutually disjoint, and zero before release. Admission is
private to this IVT object. It
publishes the slot only after native installation; any contradiction after that
commit enters the local fail-stop path.

Page-fault publication separately proves the exact 64-byte immutable context,
192-byte one-shot control, 2,688-byte frame, and dedicated coordinator stack/RSE
geometry inside the retained corridor. The interrupted user RBS is validated
separately and is never dereferenced by the capture-only entry. The default
boot path does not configure or publish this transport, so its context
word stays zero; normal boot delivers those vectors through the complete
user-fault context at offset 40 instead.

Both refill handlers use only bank-0 scratch registers. They handle the
architecturally necessary interruption state, acquire the admitted ledger
publication and perform a bounded immutable-record lookup. Instruction and
data hits insert with `itc.i` and `itc.d`, serialize and return with `rfi`.
When either scan finds no record at PL3, the handler re-reads IPSR, restores
PR and escapes to the [user-fault entry](Ia64UserFaultEntry.md): selector 4
for data, selector 6 plus `ar.k4=0x0c` for instruction. Mandatory RSE data
misses take the same path, including incomplete frames. PL0 misses and every
admission failure still stop. The refill path trusts the
immutable context, corridor, slot, and ledger invariants established by
installation, private admission, and release publication; it is not a per-refill
tamper or corruption validator. Failure has no recovery, callback, allocation,
lock, stack, or RSE path.

An instruction refill rejects an RSE reference and incomplete-register-frame
state. A data refill admits an RSE-origin reference together with its
architected incomplete-frame companion, but rejects an isolated incomplete
frame. This permits the stopped child `flushrs`, syscall transport, and mandatory
parent fills to populate user RSE translations through the same bounded
alternate-DTLB handler.

Hosted tests cover the immutable table layout, validation and admission model,
and the side-effect-free refill model. Strict IA-64 target builds compile the
real assembly table and handlers as part of the ordinary freestanding closure.
These checks do not execute privileged runtime code.

The [BSP runtime-memory owner](Ia64BspRuntimeMemory.md) installs this table,
admits processor contexts and activates the associated roots. Translation is
enabled separately by `InitialTranslationActivator`.
The [system-call entry](Ia64UserSystemCallRuntime.md) owns register capture,
RSE switching, syscall dispatch and PL3 return after the break branch.
The [external-interrupt entry](Ia64ExternalInterruptEntry.md) owns the local
timer/evidence route; PL3 interrupts use the complete user-fault transport.
The optional one-shot BSP preemption control selects its dedicated native path.
AP cooperative scheduling and timer delivery belong to the
[BSP runtime](Ia64BspKernelRuntime.md#shared-root-ap-runtime), not this IVT owner.

Bootstrap consolidation pins the exact runtime owner corridor inside the guarded runtime page at
+128 KiB. Bootstrap stack storage before it and page-top RSE storage share the same DTR but are outside the object-derived
corridor; runtime entry admission cannot substitute those bytes for retained runtime transports.
The static bootstrap fail-stop IVA lives in `.text` beside this runtime table and shares its image ITR.
IVA delivery preserves translation state; both processors' physical admission paths replace the
bootstrap IVA with this table before enabling translation. The native mandatory set is four DTRs
and two ITRs including reserved firmware ITR0, on every processor. Read-only identity-window TC refills and text data-mirror removal are
unimplemented; TC entries must not overlap live image DTRs.
