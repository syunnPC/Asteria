<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 SAL Boot-Rendezvous Registration

`SalBootRendezvousRegistration` is the first executable prerequisite for IA-64 application-processor startup. It is
restricted to registering the single architected `OS_BOOT_RENDEZ` vector; SAPIC access, AP wake-up, per-processor
stacks/RSE storage, and the rendezvous handler itself remain separate stages.

## Retained-input boundary

The call copies and revalidates the consumed `SalEntrypointBinding`, `SalSystemTableInformation`, and handler values
before invoking firmware. The binding must still contain complete PAL, SAL-code, and SAL-GP accesses in the retained
UEFI regions. The parsed physical addresses must still match the binding, the SAL revision must remain valid BCD in
the supported 2.8-through-3.x range, and the handler must provide:

- a nonzero, 16-byte-aligned physical entry bundle;
- a nonzero, eight-byte-aligned physical GP;
- a nonempty, bundle-aligned 63-bit physical code extent containing that complete entry bundle.

The code extent is only a structural authorization range. Registration does not read code bytes, calculate a
checksum, prove artifact provenance, or impose a firmware-artifact size policy.

The returned disposition, rather than the diagnostic status alone, controls handler ownership:

- `NotRegistered` means this call's submitted handler is known not to have committed. It does not assert that no
  earlier vector remains installed.
- `Registered` means a well-formed SAL status zero confirmed the registration.
- `Indeterminate` means the side-effecting call may have committed, but its result cannot prove whether it did.

Both `Registered` and `Indeterminate` transfer a lifetime obligation to the handler owner. Every first-level bundle
reachable by the submitted entry must remain resident, immutable, and physically executable, and all data reached
through the submitted GP must remain resident, physically accessible, and immutable wherever the handler requires
it. Those properties must hold until a later SAL operation replaces or invalidates the vector and every AP rendezvous
use of the submitted vector is quiesced. The component validates the supplied range at registration time but does not
hash code, infer reachability, retain ownership, or authenticate later bytes.

## SAL call contract

The physical adapter passes exactly eight SAL arguments: function ID `0x01000000` (`SAL_SET_VECTORS`), vector type
two (`OS_BOOT_RENDEZ`), the handler entry, the handler GP, and four zeroes. The first of those zeroes is the handler
length field under SAL 2.8/2.9 and the `length_cs` field under SAL 3.x. Zero is the common SAL-defined no-length form;
for 3.x it also leaves the checksum-present and checksum fields clear. The remaining three zeroes are the reserved
secondary entry/GP/length triple.

The C-to-assembly boundary passes two parameters: a pointer to a fixed 80-byte request and a pointer to the native
invocation-state word. The request contains the raw SAL entry, SAL GP, and all eight SAL arguments at fixed eight-byte
offsets. The native frame therefore has two inputs and eight SAL outputs; it never treats parameter slots eight and
beyond as stacked input registers. This follows the Itanium ABI rule that only slots zero through seven use `in0`
through `in7`, while slot eight begins at `sp+16`. SAL returns its four-word result directly in `r8` through `r11`.

`mov = psr` exposes only PSR bits 31:0 and 36:35. The bridge consequently checks exactly the readable fields needed
by this physical adapter: PSR.be, PSR.dt, PSR.rt, and PSR.it must be zero. It cannot read PSR.bn or the SAL-required
PSR.cpl, PSR.is, PSR.da, PSR.dd, PSR.ss, PSR.ri, PSR.ed, and PSR.ia fields. The caller must enter at CPL0 with
PSR.vm zero, PSR.bn one, and those fixed fields in their required state; a CPL or virtualization violation can fault
on the privileged PSR read rather than return `UnsupportedProcessorState`. The bridge preserves its caller frame,
`b0`, `b5`, and `gp`.

The caller must keep the operation serialized, non-preemptible, and on one processor. After all outer caller frames,
at least 17,408 bytes must still be free in each of the memory stack and RSE backing store at `Register` entry. This
is SAL's 16 KiB-at-`SAL_PROC` requirement plus a stable 1,024-byte adapter reserve. In strict target builds, the
worst O0 memory-stack chain consumes 528 bytes: the `Register` frame is 432 bytes, `IsBindingValid` adds 32,
`IsAccessContained` adds 48, and its `TryGetRegionEnd` callee adds 16. O2 uses 288 bytes, and the native bridge does
not move `sp`. At the SAL branch, the conservative sum of the O2 `Register` SOF (30) and bridge SOF (15) is 45
stacked registers; with a worst-position RNAT word this occupies at most 368 bytes. The 1,024-byte gate therefore
covers both complete verified live chains with compiler headroom while leaving at least 16 KiB free when SAL begins.

The result distinguishes native processor-state rejection from every defined SAL status (`-1`, `-2`, `-3`, and
`-4`) and retains the exact raw firmware status. Prevalidation, native rejection, and well-formed `-1`, `-2`, or `-4`
results are `NotRegistered`; a well-formed status zero is `Registered`. A nonzero reserved result register makes even
status zero `Indeterminate`. Status `-3` and every unexpected status are also `Indeterminate`, because SAL provides no
atomicity or rollback guarantee for a side effect completed before reporting the failure. Registration success means
only that firmware accepted the vector; it does not mean that an AP has been woken or entered the handler.

The architectural behavior follows Sections 8.1, 8.2, and `SAL_SET_VECTORS` in the
[Intel Itanium System Abstraction Layer Specification, Revision 3.4][SalSpecification], Chapter 8 of the
[Intel Itanium Software Conventions and Runtime Architecture Guide][ItaniumConventions], and the PSR access rules in
[Intel Itanium Architecture Software Developer's Manual, Volume 2, Section 3.3.2][ItaniumArchitecture].

[SalSpecification]: https://www.intel.com/content/dam/www/public/us/en/documents/specification-updates/itanium-system-abstraction-layer-specification.pdf
[ItaniumConventions]: https://web.archive.org/web/20031013201915id_/http://developer.intel.com/design/itanium/downloads/24535803.pdf
[ItaniumArchitecture]: https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf
