<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 User Execution Probe Ownership

## Scope

`Ia64UserExecutionProbe` owns the fixed metadata and boot-lifetime physical
storage for the explicit `ASTERIA_IA64_SYNTHETIC_USER_PROBE=1` diagnostic. The
option defaults to zero; normal firmware launches
`MinimalUserImage` from `USER.ELF` instead. This resumable CPL3 probe
does not load a process, select policy, schedule a thread, enter user mode, or
provide a reusable address-space factory. Its only successful product is one
validated `Ia64UserModeLaunch` record backed by three mappings in the fixed
private initial-component runtime root.

The probe uses exactly these private Region-1 virtual ranges:

| Purpose | Guard before | Mapped range | Guard after |
| --- | --- | --- | --- |
| Code | `0x2000000001000000` | `0x2000000001001000`–`0x2000000001001fff` | `0x2000000001002000` |
| Memory stack | `0x2000000002000000` | `0x2000000002001000`–`0x2000000002004fff` | `0x2000000002005000` |
| RSE backing store | `0x2000000003000000` | `0x2000000003001000`–`0x2000000003004fff` | `0x2000000003005000` |

Code is one 4-KiB read/execute user page. The downward memory stack and the
RSE backing store are distinct 16-KiB read/write user ranges. Every guard is
one unmapped 4-KiB page. All three mapped ranges and all six guards are
pairwise disjoint, outside Region 0, contained within Region 1, and valid for
the target profile's minimum implemented virtual-address width.
The numerical layout resides in the shared
`Ia64UserVirtualLayout.hpp` so the initial-image Executable layer can reject
these guarded corridors without depending on the probe's mapping, object, or
scheduler types. Scheduler-namespace aliases expose the probe's public layout constants.

## Two-phase lifetime

The first phase, `StageStorage`, runs on the non-preemptible BSP before native
translation takeover. The caller supplies three exact
`PhysicalPageRange` values returned by `BootstrapPageAllocator`, three exact
writable aliases, and a non-overlapping code-image view. The allocation
receipts must describe Conventional, write-back-capable, non-runtime firmware
memory. They must contain exactly one, four, and four pages respectively and
must not overlap. Each writable view must be page aligned, match the receipt's
physical start and complete byte size, and be disjoint from every other view
and the code-image input. On IA-64 the pre-takeover alias is additionally
required to be the exact identity address of its receipt.

All geometry and alias checks finish before the first destination write.
Validation failure therefore leaves both the owner and every destination byte
unchanged and may be retried. Success zeroes all nine pages, copies the code
image into the beginning of the code page, publishes its instruction bytes,
and retains only pointer-free allocation metadata. The writable aliases are
not retained and must not be used again after translation takeover. A staged
allocation has boot lifetime even if the second phase is never attempted.

The code input is returned by
`GetIa64UserExecutionProbePayload()`. Its relocation-free image keeps the
steady loop at offset `0x100` and the local child at offset `0x110`. The
launch record deliberately supplies zero as its initial global pointer; the
payload has no GP-relative dependency, so the reusable P64 launch transport
does not add a relocation to this fixed probe. The parent writes
`Ia64UserExecutionProbeSentinel` to the initial ABI scratch slot
at `[r12]`, allocates four local stacked registers containing the sentinel
through sentinel plus three, and calls the child. The child preserves the
parent's static `r14`/`r15`, executes one stopped `flushrs`, and reuses one
six-output, no-local stacked frame for four stopped slot-two
`break.i 0x100000` calls: `CapabilityQuery` (`2`), IPC send (`6`), IPC
receive (`7`), and WaitSet wait (`8`).

The launch-time ProtectionDomain capability remains in static `r4`/`r5` and is
used only by Query. At `0x2000000002004100` the runtime stages a 120-byte
record containing Endpoint selector `0x101`, an opaque authenticator at `+8`,
one canonical 88-byte asynchronous `AIPC` frame at `+16`
(`0x2000000002004110`), and the WaitSet selector/authenticator at `+104` and
`+112`. Send reads the Endpoint handle and frame, receive uses that same handle
to publish one exact 136-byte `AIPD` delivery at
`0x2000000002004300`. Both calls operate entirely inside the already mapped
final memory-stack page; they add no mapping or translation-cache entry. Their
six live response words and a completion sentinel conditional on selector
`0x101` and exact `{0, 1, 0}` / `{0, 1, 136}` responses are retained in the
56-byte evidence area at `0x2000000002004200`. Wait then uses the separate
handle, suspends until the exact Notification wakes this Thread, and records
`{0, 0x5741495453455401, 1}` at `0x20000000020044D0` before the completion
sentinel becomes observable.

The BSP's timer evidence stays out of this page. Before the resumable launch
the BSP records its unchanged 56-byte local-timer interrupt record and an
80-byte TimerQueue-to-Notification record, and during the Wait the 64-byte
second-timer record, in the `UserProbe` part of the kernel evidence record.
The VM runner reads them through QMP as supervisor-produced proof that both BSP
timer interrupts returned through the bounded polling leaf and that normal
context completed direct and deferred Timer-to-Notification delivery. The Wait
response at `0x20000000020044D0` follows the 136-byte delivery area inside the
same final mapped stack page.

The syscall entry moves the user dirty partition to its dedicated kernel RSE
transport. Restoring the user BSPSTORE discards the clean parent partition, so
the child return necessarily refills the four parent locals. The parent accepts
only the exact sentinel sequence and publishes the result sentinel at
`Ia64UserExecutionProbeRseSentinelAddress` after that check. This is a mandatory
four-word RSE spill/refill proof, not merely an ordinary write into the RSE
range. The child saves the live Query response immediately in the fixed area
below the initial stack pointer because the two later calls overwrite
`r8`-`r10`, then reloads it before returning. The successful expected Query
response is error `0`, packed information `0x0000000000010001`
(ProtectionDomain type `1`, plus Inspect right `1` shifted by 16), and lease
deadline `UINT64_MAX` (`0xffffffffffffffff`). At the terminal loop `r14`
through `r31` are zero; the launch handle, restored Query response, stack
pointer, and four parent locals retain their public contracts.

The second phase, `PrepareLaunch`, runs after `Ia64BspRuntimeMemory` admits the
common root and the caller selects its private initial-component root. It
borrows that exact private root, verifies that the supplied
registry, resource-domain generation, mapping Thread generation, and
processor-local TLB dispatcher agree with it, and checks every existing root
mapping against the three physical receipts and complete guarded virtual
envelopes. It also requires the BSP to be the root's sole resident processor;
this probe does not publish asynchronous remote shootdown work.

After preflight, the transaction is single-shot. The owner initializes three
embedded RAM `Frame` objects and three pinned `MemoryObject` objects, registers
them through the supplied root issuer, and constructs typed identities only
from the returned registry object IDs. It then installs each mapping through
the ordinary sequence:

1. initialize and claim an embedded `FaultToken` for the supplied registered
   Thread and the borrowed BSP `AddressSpace`;
2. prepare the exact `MemoryObject`/`Frame` slice with
   `FaultFrameMappingPreparation`;
3. call `AddressSpace::BeginFaultMapping`;
4. dispatch the returned shootdown through the BSP's
   `TlbShootdownDispatcher`, which performs the real local invalidation and
   acknowledgement;
5. call `AddressSpace::CompleteFaultMapping`; and
6. commit the exact fault claim as `Map`.

The IA-64 page-table backend is never called directly. No bootstrap seed is
widened, no registry identity is constructed from an untrusted integer, and no
capability authenticator or entropy value is invented.

## Failure and ownership

Every second-phase rejection before object initialization leaves the owner
staged and retryable. Immediately before the first irreversible object or
mapping operation, the owner enters `Failed`. Any later error is terminal:
initialized objects, registry generations, ResourceDomain reservations,
mapping-policy references, fault claims, and any natively committed
translations remain owned for the complete boot lifetime. There is no unsafe
rollback across an append-only IA-64 runtime ledger or an uncertain TLB
shootdown state.

Only after all three private-root mapping transactions and all launch-record
postconditions succeed does the owner publish `Ready`. The launch operation
itself and its no-return `rfi` remain the responsibility of
`Ia64UserModeLaunch`; this owner performs neither.

The probe owner does not mint authority. After this mapping and launch-record
readiness is established, `Ia64BspKernelRuntime` mints one infinite,
Inspect-only ProtectionDomain capability and supplies its 128-bit handle through
the launch record's `Register4` and `Register5` initial arguments;
`Register6` and `Register7` stay zero because the probe receives no
DiagnosticChannel. It then
mints one infinite `Invoke|Read` Endpoint capability in the same capability
space and stages that second handle plus the canonical frame in the memory
stack for the focused IPC round trip. Finally it mints one infinite `Wait`
capability for the runtime-owned WaitSet and stages that third handle for the
resumable syscall.

The owner constructs processor-state requirements exclusively from the
exported `Ia64UserModeLaunch` constants and states CPL 0, VM 0, instruction set
0, and register bank 1 as the current native-boundary preconditions. It does
not duplicate a numeric PSR value or claim that mov-from-PSR observes fields
which the launch boundary must receive explicitly. It also preserves the
probe's explicit zero-GP launch value when the BSP runtime later installs the
capability arguments.

## Concurrency and allocation

Both phases are externally BSP-serialized and are not interrupt-safe. The
caller prevents migration, preemption, same-CPU re-entry, root mutation, and
registry-lock re-entry for each complete call. The owner and every supplied
registry, issuer, root, resource domain, Thread, dispatcher, and physical page
remain alive and immovable for the boot lifetime. No operation dynamically
allocates, frees, waits, or performs unbounded work.
