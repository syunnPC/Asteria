<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 User System-Call Runtime

## Scope

This runtime provides the reusable IA-64 user-side system-call boundary and
the separately linked initial-user image that contains it. The user runtime is
deliberately separate from `BootApplication`, the fixed user-execution probe,
private-root ownership, and scheduling. The boot path acquires the retained boot-file
ELF, decodes its P64 load plan, allocates and initializes one staging arena, and
publishes its executable slices. After BSP runtime-memory preparation it
materializes that arena as registered Frame and pinned MemoryObject metadata,
then maps every load, memory-stack, and RSE range in the private initial-
component root. The translated BSP runtime revalidates those mappings,
retains the decoded P64 entry and GP in a native launch request, and executes
the ELF as the default IA-64 terminal boot path.

The target-neutral `User::System::SystemCallClient` accepts a borrowed
`SystemCallTransport`. It constructs the 56-byte logical request from a typed
two-word user capability handle, zeroes every unused argument, and returns the
24-byte logical response unchanged. The client has wrappers for capability
Close, Query, Derive, Duplicate, and Revoke, Endpoint send and receive, and
WaitSet wait. It allocates no memory and owns neither the context nor the
transport function.

Both client and kernel consume `Source/Abi/SystemCallAbi.hpp`. The shared
header contains only stable integer constants, fixed-underlying-type enums, and
the logical register images. It exposes no kernel objects, capabilities,
locks, scheduler state, or target register representation. The request and
response structures describe values copied through registers; they are not a
shared-memory C++ protocol.

## Raw IA-64 transport

`AsteriaIa64UserSystemCallTransport` is a P64 callable leaf. Its C boundary
receives an unused transport context plus pointers to one request and one
response. Before replacing its stacked-register view, it retains the two
pointers in static registers. It then installs a six-output frame, copies the
system-call number to `r15` and arguments zero through five to `r32` through
`r37`, and executes one stopped slot-two `break.i 0x100000`. After the native
kernel return, it copies `r8`, `r9`, and `r10` to the response, restores
`ar.pfs`, and returns to its caller.

The transport object is a P64 function pointer. Its descriptor, code, and
image global pointer must therefore remain mapped for every call. The boundary
does not invoke firmware, manipulate translation registers, select authority,
or provide an AMD64 entry.

## Kernel entry and return

The kernel's `break` entry (`Ia64SystemCallEntry.S`) covers the interrupted
frame, carries the dirty partition onto its dedicated kernel backing store and
runs the C++ bridge. On return it selects enforced-lazy mode with the exact
dirty-partition distance and then scrubs the physical stacked-register file
before `loadrs`: the bridge's frames leave kernel values (pointers,
capability and lookup state) in physical stacked registers, and a later user
`alloc` would expose any register outside the user's partition. It grows the
two-output frame it called the bridge from to all 96 stacked registers, the
whole physical file that PAL_RSE_INFO reported on every CPU
([capture](Ia64FirmwareHandoff.md)); the RSE first spills any still-resident
part of the user's dirty partition to the kernel backing store. It clears
`r32`-`r127` with their NaT bits and drops the frame; `loadrs` then reloads
exactly the user's dirty partition and invalidates every other stacked
register, which therefore holds zero. The guest test program checks this
through the ABI: its `abi.stacked-registers` point grows a frame after a Query
and requires every new register to be zero.

Only `break` with immediate `0x100000` is a system call. The break vector's
slot branches to `AsteriaIa64UserBreakEntry`, which compares `cr.iim` in
bank-zero registers and preserves PR: the system-call immediate continues to
`AsteriaIa64SystemCallEntry`; any other immediate enters the complete
[user-fault entry](Ia64UserFaultEntry.md) as a Break fault, which reaches the
thread's supervisor with the immediate and the break's slot in its syndrome
([IA-64 fault delivery](Ia64UserFaultEntry.md)). The system-call entry keeps
its own immediate check and the reporter that writes
`ASTERIA:IA64-USER-BREAK:FAIL:IMMEDIATE=0x<16 hex>:IIP=0x<16 hex>` and stops,
as a defensive stop that only a route other than the demultiplexer could
reach. A break at PL0 stops in either entry's admission.

A system-call `break` requires no locals or rotating registers, zero rename
bases, at least six outputs and `ar.rsc == 0xC`. A violation is delivered as that thread's Exception. The entry
checks RSC before accessing entry storage and samples CFM with a local
`br.call b6`/`br.ret b6` pair before reading `r32`–`r37`; the pair preserves
the register frame without allocating or accessing stacked arguments
(*IA-64 Architecture Software Developer's Manual*, revision 1.1, July 2000,
Vol. 3, "br", Operation). It restores PFS and b6 before either continuation;
a rejected frame also restores UNAT and releases the syscall frame.

Rejected calls enter the existing complete-state user exception entry with
the Break vector and original immediate through its normalized landing.
Both entries retain the original RSC in privileged scratch `ar.k6` and
install enforced-lazy, PL3, little-endian `0xC` before the stop poll,
stacked state or backing store (Vol. 1 §3.1.8.2;
Vol. 2 §§6.5.1, 6.5.3–6.5.4). A mode or byte-order violation at any PL3
interruption likewise overrides its normal cause with Exception; an
external interrupt is serviced first, so it is never lost.
PL3-writable PFS can also restore an invalid CFM through `br.ret`; invalid
size/rotation/rename combinations become Exception on any user interruption
(Vol. 1 §3.1.8.10; Vol. 3, "mov ar" and "br", Operation). The covered IFS
header remains hardware-owned and checked as a kernel invariant: only the
entry's own `cover` writes it, copying CFM into `ifm` and setting `v`
(Vol. 3, "cover", Operation), and `mov cr.ifs` is privileged. The thread's
supervisor terminates it; busy delivery yields and retries
in the retained kernel continuation. If safe delivery cannot be established,
only the thread may be terminated, with its supervisor notified; neither
entry needs that fallback, since safe RSC precedes every access and capture
never reads the user backing store. PL0, nested interruptions and corrupted
kernel entry invariants still stop. The boot root pager has no supervisor, so its
rejected state still stops.
The other interrupted PSR admission bits are privileged or hardware-owned,
except `PSR.is`, which `br.ia` could set. Every launch sets privileged
`PSR.di`, so the transition instead raises a supervised native Exception
(Vol. 2 §3.1; Vol. 3, "br", Operation). PL3 cannot clear it. The writable
user-mask bits are already admitted.
An overflowing next bundle at the end of Region 7 cannot be reached from
mapped user code: `AddressSpace::TryGetRangeEnd` rejects the last page because
its exclusive end would overflow (`Source/Kernel/Memory/AddressSpace.cpp`).
Arbitrary user BSPSTORE is captured as a value; rebinding preserves the
hardware-bounded dirty partition (Vol. 2 §6.5.3), and only the new pinned
kernel backing-store distance is checked.

User images launch with f2-f127 enabled, so compiled user
code may use floating point and integer multiplication and division, which
IA-64 GCC computes in f6-f15. A system call is a call: f6-f15 are scratch
across it (the entry returns restore carriers in f6-f13), f2-f5 and f16-f31
are preserved, and the kernel computes under its own fpsr
([target profile](../TargetProfiles/Ia64.md)).

## Minimal image artifact

The target build links `MinimalUserImage.elf` separately; it never adds the
user implementation objects to the nucleus or firmware link. The production ESP
installs its stripped bytes as `EFI/ASTERIA/USER.ELF`. The boot application is a
separate PE image; the kernel is independently linked as `KERNEL.ELF`
([kernel image contract](KernelElf.md)). The boot application checks and retains the
whole user file in its LoaderData region before reservations, and staging consumes
it before translation takeover ([boot file contract](BootContract.md)).
The test ESP (`FirmwareTest/`) installs stripped `GuestTests.elf` at the same path: the
same user objects without `Ia64MinimalUserImage.cpp`, linked with the guest
test program of `Tests/Guest`, which defines its own
`AsteriaIa64MinimalUserImageMain` and routes every system call through a
transport that checks the returned `r1`. The entry stub, the transport, the
linker script and hence the layout below are shared by both images.
The ELF's fixed virtual layout is:

| Interval start | Purpose | Permission |
|---:|---|---|
| `0x2000000004000000` | code | read/execute |
| `0x2000000004010000` | constants | read-only |
| `0x2000000004011000` | two-word entry descriptor | read-only |
| `0x2000000004012000` | compiler P64 descriptors | read-only |
| `0x2000000004020000` | mutable data | read/write |
| `0x2000000004021000` | GOT | read/write |
| `0x2000000004022000` | zero-initialized data | read/write |

The ELF entry is `AsteriaIa64MinimalUserImageEntryDescriptor`, not a code
address. Its first word is `AsteriaIa64MinimalUserImageEntry`; its second word
is the fixed `__gp` value `0x2000000004020800`. The launch boundary must install
that descriptor word in `r1`; startup deliberately leaves `r1` untouched,
establishes a four-output P64 frame, calls the minimal C++ body, and then idles.
The launch boundary supplies two capabilities as static-register pairs: the
bootstrap Endpoint in `r4`/`r5` and an `Inspect|Write` DiagnosticChannel in
`r6`/`r7` (see [DebugWrite](SystemCallAbi.md#debugwrite-semantics)). The startup
entry copies `r4` through `r7` into the C++ call's four arguments
(selector, authenticator, selector, authenticator). The production entry body
ignores the DiagnosticChannel; a test image reports through it.

The C++ entry obtains the IA-64 transport and delegates to the noinline,
transport-injected `RunIa64MinimalUserImage` production routine. That routine
first calls the noinline BSS observation helper. The helper performs one
observable read of the uninitialized `constinit volatile`
`Asteria::User::Platform::Ia64::Runtime::g_bssZeroInitializationProbe` before
replacing the same probe with a diagnostic sentinel. A zero observation writes
`0x4253535041535301` (`BSSPASS`); any nonzero observation writes
`0x4253534641494c01` (`BSSFAIL`) and terminates the runtime sequence before its
first system call.

After a BSS pass, the routine queries the exact bootstrap handle and requires
one infinite `Inspect|Invoke|Read` Endpoint. It sends one fixed, canonical
88-byte attachment-free asynchronous `AIPC` frame through that Endpoint, then
receives the same sequence into a global 136-byte buffer. Success requires the
exact `{None, 1, 0}` Send response, `{None, 1, 136}` Receive response, and the
exact 48-byte `AIPD` header plus the original `AIPC` frame. The header identifies
normal delivery from root ResourceDomain 1 with effective rights 7, zero sealed
selector, no attachments, and zero reserved bytes.

The three responses remain independently observable in
`g_querySystemCallResponse`, `g_sendSystemCallResponse`, and
`g_receiveSystemCallResponse`; `g_lastSystemCallResponse` remains a compatibility
copy of Query. `g_ipcFrame`, `g_ipcDelivery`, and
`g_ipcCompletionSentinel` retain the wire evidence. The nonzero completion
sentinel is written only after every response and byte check succeeds. A
system-call error or local contract mismatch returns immediately and leaves later
responses reset and completion zero.

Hosted tests invoke that same production routine through a scripted transport
with selector `0x100` and a nonzero authenticator. They cover BSS rejection,
invalid transport, each system-call error cut, malformed Query metadata, wrong
Send and Receive results, corrupted delivery-header and embedded-frame bytes,
exact request addresses and sizes, retained evidence, and completion-sentinel
timing during all three callbacks.

The build validates only execution-relevant properties: IA-64 executable type,
the entry descriptor's exact code and GP words, and exactly three nonoverlapping
nonempty load segments with RX/R/RW permissions. Every segment must be readable
and have `p_memsz >= p_filesz`; no virtual, physical, or file extent may wrap.
The one nonempty writable `NOBITS` `.bss` and the exact probe-symbol extent must
both lie inside the unique RW segment's
`[p_vaddr + p_filesz, p_vaddr + p_memsz)` zero-fill interval, and the probe must
lie inside `.bss`. The build also checks absence of constructors, unwind and
dynamic-link state, complete symbol and relocation closure, and one raw
system-call break.

The narrow target call-structure oracle confirms one real unpredicated branch
call from the C++ entry to the hosted-tested production routine and, in that
routine, one real unpredicated branch call each to the observation helper and
the exact Query, Send, and Receive implementations in that order. It
deliberately ignores non-call
address materialization and does not claim to prove arbitrary control-flow
reachability; the hosted recording-transport tests own the behavior claim. The
separate startup oracle requires the ordered unpredicated
sequence that preserves loader-provided `r1`, establishes the four-output P64
frame, forwards `r4` through `r7` into `r32` through `r35`, observes the
required CFM and argument dependency stops, and makes exactly one call to the
C++ entry body; `Ia64CallArguments` independently checks that the frame and the
written output registers cover every declared parameter of the C++ entry.
Mutation fixtures cover missing read permission, `p_memsz < p_filesz`, wrapped
extents, writable executable permissions, file-backed or read-only `.bss`,
missing, reordered, duplicate, or predicated direct calls, explicit `r1`
reload, implicit `r1` post-increment, and the existing startup ordering cases.
The gates do not record or constrain whole-file size, hashes, fingerprints,
signatures, provenance, or tamper evidence.

`Ia64InitialUserImageSource` exposes the retained bytes as an immutable
`Elf64ByteView`. `Get` validates a file index and its extent in the retained
LoaderData region; `CreateView` checks a supplied view against that region. The
provider owns no storage and performs no allocation, copy, ELF decoding,
mapping, or launch. Before translation takeover,
`Ia64InitialUserImageOwner` calls this source, invokes the IA-64 `Ia64P64Image`
decoder over the retained file bytes, and thereby consumes the portable `Elf64Image`
decoder. It allocates one bounded Conventional write-back arena, zeroes it,
copies and verifies every normalized file and BSS slice, verifies the separate
memory-stack and RSE storage, publishes every executable slice, and retains only
pointer-free staging metadata. This staging creates or registers no `Frame` or
`MemoryObject`, charges no `ResourceDomain`, mints no capability, and creates no
scheduler state or launch record. Later in the same physical owned-context
continuation, after BSP runtime-memory preparation,
`Ia64InitialUserImageMemoryOwner` creates and registers one aggregate Frame and
one pinned MemoryObject per nonempty logical range. The Frame records the root
domain's numeric charge owner; only the pinned MemoryObjects reserve all arena
pages and their metadata against that ResourceDomain. That object transaction
still creates no mapping, capability, scheduler state, or launch record. The
following retained mapping owner installs the load, memory-stack, and RSE
ranges in the private initial-component root while preserving both guard
holes; it creates no capability, scheduler state, or launch record.

The fixed `Ia64UserExecutionProbe` remains a separate diagnostic path with its
own payload and storage. Setting `ASTERIA_IA64_SYNTHETIC_USER_PROBE=1` selects
that synthetic Query/send/receive/wait-resume path instead of the default
boot-file image launch; the option defaults to zero.

## Launch integration and limits

`Ia64InitialUserImageLaunchOwner` consumes the ready memory and mapping owners,
reuses the BSP runtime's existing private AddressSpace, ProtectionDomain,
capability space, Thread, and SchedulingContext, and validates an explicit
bootstrap grant. The default BSP runtime captures the pinned image mappings,
reserves the fixed Endpoint and DiagnosticChannel resource bundle, constructs
and registers a self-receiving boot Endpoint and a DiagnosticChannel, publishes
the mappings to the bounded user-copy backend, and supplies two infinite grants:
`Inspect|Invoke|Read` for the Endpoint and `Inspect|Write` for the
DiagnosticChannel. The launch owner mints exactly those two capabilities, each
in its own revocation root, places them in `r4`/`r5` and `r6`/`r7`, and builds
the `Ia64UserModeLaunch` request
from the decoder's entry bundle and GP, the mapped user stack/RSE ranges, and
the runtime's retained supervisor ranges. The common run loop selects the
existing worker and `DispatchInitialUserImage` performs the one-way native PL3
launch.

The user runtime and launch owner provide the initial image's system-call and
PL3 entry boundaries. Production boot separately attaches the
[boot pager](BootPager.md) and created-thread table; initial-image dispatch admits
the pager first and continues scheduling the pager, initial image and created
threads. The synthetic probe remains the separate Wait/resume and timer
diagnostic. See [AMD64 runtime](Amd64BootRuntime.md) for
the corresponding CPL3 launch and repeated WaitSet-resume path.
