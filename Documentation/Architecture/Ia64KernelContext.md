<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 kernel context

## Scope

`Ia64KernelContext` is the cooperative CPL0 execution primitive below the
architecture-neutral processor scheduler. It switches at a C++ call boundary,
after scheduling, time accounting, deadline selection, and translation
selection have completed. It allocates no memory and does not call firmware.

Its synchronous register image is not an interruption frame or a user-return
frame. Each context also owns one separate passive
`Ia64ExternalPreemptionFrame`; region and translation registers, user-return
state, debug state, and performance state remain separate architecture-owned
state.

The common run loop makes this separation executable at its policy
boundary. `CompleteExternallyStoppedRunAsReady` accounts a separately retained
frame and returns the logical state and address-space root to Idle without
calling `Ia64KernelContext::SwitchTo`. The retained marker stays with its exact
binding while other work runs. If normal policy later selects that owner, the
run loop installs and validates its new HardwareDeadline admission and root,
then returns an `ExternalFrame` action without testing or switching to the
cooperative context image. Multiple bindings may retain independent frames;
only one selected resume action exists for the processor admission at a time.

Neither operation captures registers, changes stacks, or resumes a frame. The
IA-64 continuation must first save and publish a fresh scheduler/idle memory-
stack and RSE image whose restore enters normal idle dispatch rather than
returning into the old save/commit path. The run loop asks the exact idle-
context object to perform that save and later queries the same object for the
action generation. A successful `CommitExternalFrameResume` consumes common
ownership, so the next control transfer must restore the exact retained frame
and execute `rfi`; inability to do that is fail-stop. Stale receipt, context,
binding, or admission evidence faults while ownership remains retained. The
IA-64 hook publishes a fresh `AsteriaIa64StartKernelContext` image only
after its dedicated idle landing and reusable stack/RBS corridor have been
configured; an unconfigured context still rejects the operation.

## High floating-point partition

The kernel is compiled to leave f32-f127 alone (`-mfixed-range=f32-f127`, Verify `Ia64KernelLowFloatingPartition`), so
those registers keep the last user thread's values while kernel code runs.
A user thread's context owns an `Ia64HighFloatingArea`
of 96 spill slots, attached once while the context is Fresh (`AttachHighFloatingArea`). `SwitchTo` spills the high
partition into the current context's area (`AsteriaIa64SaveHighFloatingPartition`) and fills it from the next
context's (`AsteriaIa64RestoreHighFloatingPartition`) before it switches; a context without an area, such as idle,
does neither. The area is separate from the page-fault frame, whose first capture requires the frame pristine,
floating registers included. The boot pager's context and the initial image's worker context have areas. Ordinary user images launch with PSR.dfh clear and may use the partition; the
synthetic execution probe launches with PSR.dfh set
([launch policy](../TargetProfiles/Ia64.md#runtime-and-permitted-extensions)).

## Synchronous page-fault frame

Each context also selects one `Ia64PageFaultFrame`, embedded or permanently
bound before preparation. `SwitchTo` refuses a source or destination whose
frame is `Capturing` or `ResumeCommitted`, the states the native entries own
until their `rfi`, or holds an undefined state. A `Retained` frame does not
block: a thread whose fault is retained by the
[user-fault bridge](Ia64UserFaultEntry.md) blocks through the run loop and is
resumed on its own entry storage.

## Persistent external-preemption frame

The 2,688-byte, 64-byte-aligned external frame is inline in the context object.
It retains r1-r15 and interrupted-bank r16-r31 with separate spill-UNAT images,
b0-b7, predicates, PFS/RSC/BSP/BSPSTORE/RNAT/UNAT/LC/EC/CSD/CCV/FPSR,
interruption coordinates, the covered IFS and dirty-partition `loadrs`
configuration, and f2-f127. Constant r0, f0, and f1 require no storage. CSD is
the implicit second half of native `ld16`, `st16`, and `cmp8xchg16`, so a
preemption between those operations must preserve it. IA-32-only application
registers, including SSD, are outside this native-only proof and are neither
captured nor restored. Timer metadata records the fixed `0xe8` claim, selected
run-stop deadline, non-reserved observed tick, exact masked-ITV readback, and
exactly one EOI.

Capture reserves a per-context, lifetime-monotonic nonzero token but leaves its
publication word zero. Ordered stages distinguish interruption, integer,
application, floating-point, RSE-transition, and timer-acknowledgement work.
Only the complete final stage may release-publish the token. A retained token
survives common scheduling until exact resume commit. Token exhaustion is
terminal rather than wrapping, and any Capturing state remains owned even
though its token is not yet visible. `SwitchTo` rejects either a source or
destination that owns such a frame, so asynchronous state cannot fall through
the cooperative restore corridor.

The hosted release helper models the final state transition only. Native code
cannot make a normal C++ call after restoring the architectural registers. The
raw restore tail therefore retains `PublishedToken` through every frame access,
release-clears it once, installs only its remaining non-faulting carriers, and
executes `rfi` without another call. Failure before that release is fail-stop.
The resumed code receives the complete image: after their fills, the tail
writes no static r1-r15 and no bank-one r16-r31 register (its final stores and
carriers use bank-zero r16-r31, which are not part of the interrupted BN=1
image), so every retained general register, with its NaT bit, reaches `rfi`
unchanged.

No private coordinator stack or RSE backing store is attached to a context.
The BSP runtime owns one bounded, resettable per-CPU coordinator stack/RBS. The
raw interrupt bridge returns normally before the restore tail; the fresh idle
landing is no-return. The IVT dispatcher selects this native path only for the
explicitly armed one-shot BSP sentinel proof and otherwise preserves the legacy
interrupt and poll/evidence path. AP, user, nested, and general kernel-context
preemption remain unsupported.

## Owned storage

Each prepared context retains two distinct, page-aligned writable ranges:

- a 16–64 KiB memory stack; and
- a 16–64 KiB RSE backing store.

Preparation validates both data aliases and runtime virtual extents against
the PAL-reported 50–60 implemented-address-bit profile. The context object may
not overlap either range. All validation completes before the retained register
image or public metadata changes.

A fresh register image starts with `r12` at the memory-stack end minus the
P64 16-byte scratch area, `ar.bspstore` at the RSE range start, enforced-lazy
RSE mode, the target ABI floating-point status value, and a local trampoline.
The trampoline resolves the supplied IA-64 function descriptor, installs its
`gp`, calls `entry(argument)`, and fail-stops if it returns.

## Synchronous switch

The native adapter preserves the P64 synchronous-context state: r4–r7 and
their NaT collection, `gp`, `sp`, `tp`, return IP, PFS, RSC, BSP/BSPSTORE,
RNAT, UNAT, LC, FPSR, preserved predicates and branch registers, and
f2–f5/f16–f31. It flushes the current RSE, enters enforced-lazy mode, publishes
the saved image, invalidates stacked-register state, installs the destination
RSE state, restores the retained registers, and returns through the saved
branch target so CFM is restored from PFS.

The owning CPU disables interruption collection and excludes re-entry. The
caller guarantees that the source is the executing context, the destination is
suspended on every CPU, both storage ranges remain alive, and the selected
translation regime maps the destination code and storage.

`Ia64ProcessorRunLoop` statically instantiates the common run-loop with this
native type, so scheduling, deadline, binding, address-space ordering,
rollback, and completion logic are shared with AMD64 while the register/RSE
switch remains IA-64-only. The common run loop will not call `SwitchTo` until
its processor address-space controller accepts the bound root. IA-64 has a
runtime translation backend and the BSP bootstrap/idle root is claimed and
activated during the post-exit handoff. Production dispatches the BSP
worker through that common run loop for the resumable PL3 probe. Each admitted
AP also borrows its exact admitted shared-root controller after the one-shot
timer proof and enters that portable run loop with one retained scheduler-
selected kernel worker. The worker blocks after run sequence one, resumes at the
same saved native continuation at sequence two when the owning AP polls one
request from the reusable BSP-to-AP service mailbox, and blocks again. The AP
remains in its boot-lifetime `IdleServing` loop with `PSR.ic=1` and `PSR.i=0`.
After stable acknowledgement of the first timer sequence, that idle context
handles the separate one-shot TimerQueue-to-Notification request at exact
`0xe8` sequence two while the saved worker remains blocked.

## Verification

Hosted tests cover address geometry, distinct storage, object alias rejection,
failure atomicity, single-use preparation, initial ABI state, the adapter
boundary, exact external-frame ABI geometry, ordered and failure-atomic frame
transitions, timer acknowledgement, publication/commit/release, nonwrapping
tokens, and cooperative-switch exclusion. The cooperative IA-64 implementation
and both public layouts compile and link
at O0 and O2 with the target toolchain, including the 512-byte per-function
memory-stack limit and unresolved-symbol closure. The production QEMU path
executes the BSP common-run-loop dispatch and the persistent cooperative
block/wake/readmission service plus focused idle-context timer delivery on each
admitted AP.

Hosted common tests additionally model the external-frame policy transaction
for a known PL0 worker with no held kernel lock. General kernel preemption
remains out of scope until a preempt-disable/lock discipline and the full IA-64
interruption-frame/RSE transport are present. No QEMU preemption evidence is
claimed by this common prerequisite.
