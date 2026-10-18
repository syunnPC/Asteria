<!-- SPDX-License-Identifier: Apache-2.0 -->

# AMD64 kernel context

## Scope

`Amd64KernelContext` is the target execution primitive below the
architecture-neutral processor scheduler. It switches cooperative CPL0 kernel
call frames after scheduling policy, time accounting, deadline selection, and
address-space selection have already completed. It allocates no memory and has
no dependency on firmware services.

User return and complete interrupt frames belong to the
[native entry/runtime owners](Amd64BootRuntime.md). This primitive preserves the
cooperative ABI register subset and saves x87, MMX and SSE state when an
extended-state area is attached.

## Prepared context

A fresh context accepts one page-aligned writable stack between 4 KiB and
64 KiB, a non-null canonical entry point, and one opaque argument. Preparation
is failure-atomic and retains only the virtual stack range and diagnostic entry
metadata. The initial register image enters a local trampoline with a
16-byte-aligned stack, calls `entry(argument)`, and halts if the entry violates
its non-returning contract.

The context object must not overlap either the writable stack alias or its
runtime virtual range. The caller owns the mapping and keeps the context,
stack, entry, and argument alive until retirement.

## Switch boundary

`SwitchTo` saves RBX, RBP, R12-R15, RSP, and a local resume RIP. These are the
general registers preserved by the AMD64 System V function-call ABI; caller-
saved registers are already dead at this C++ call boundary. It then restores
the selected image and jumps to its RIP. A successful invocation returns only
when a later switch resumes the original image.

The owning CPU disables local interrupts and excludes re-entry for the entire
operation. `Amd64ProcessorRunLoop` selects a root mapping the destination code
and stack before calling this primitive. Native entry handles CPL3 return and
the optional preemption frame separately.

A context may hold a thread's extended-state area (`AttachExtendedState`):
a 16-byte-aligned 512-byte FXSAVE image that attaching fills
with a new thread's state (x87 control word 0x037F, MXCSR 0x1F80, empty tag word, every register zero). Before it
switches, `SwitchTo` saves the current context's x87, MMX and SSE state into its area (`FXSAVE64`) and loads next's
from its area (`FXRSTOR64`), so the registers hold next's state when it resumes; nothing in between touches them. A
context without an area (idle, kernel workers) saves and loads nothing, and the registers keep the last thread's state,
since the kernel never uses them (Verify `Amd64KernelGeneralRegistersOnly`). The initial image's worker context and
the pager's and the spinner's contexts have areas; a native host test checks that two contexts keep their own `xmm`
registers and MXCSR across switches and that a new one starts from the initial state.

`Amd64ProcessorRunLoop` is the target-static alias that joins this primitive to
the common processor admission. It retains only stable Thread/context bindings;
selection, accounting, and deadline policy remain architecture-neutral.

## External-frame idle continuation

`ConfigureExternalFrameIdleContinuation` prepares a dedicated idle landing before
capture is armed. The caller owns its mapped stack and keeps it disjoint from
the worker and coordinator stacks. Configuration rejects overlap with the context
or its current stack and cannot be replaced once accepted.

`SaveExternalFrameIdleContinuation` requires a prepared context and a nonzero,
strictly increasing generation. It replaces the abandoned idle continuation with
the configured register image and publishes that generation. The common run loop
checks it before committing a retained-frame resume. The optional BSP preemption
diagnostic exercises this path before normal user launch.

## Verification

Hosted tests check input geometry, canonical ranges, failure atomicity,
single-use preparation, context/stack alias rejection, and adapter arguments.
They also check external-landing alias rejection and generation replay rejection.
A separate unsanitized AMD64 executable performs two real round trips between
independent stacks at O0 and O2. It gives each context distinct RBX, RBP, and
R12-R15 sentinels, checks their restoration, and captures the initial RDI and
RSP before a compiler prologue can alter them. IA-64 and AMD64 target gates
also compile the public contract, while the AMD64 implementation is compiled
and linked at O0 and O2. ASan and TSan are used for the non-switching contract
tests because their runtimes cannot track an assembly replacement of RSP.
The native processor-run-loop executable separately proves scheduler-selected
Block, Wake/resume, Exit, and idle handoff on those real stacks.
