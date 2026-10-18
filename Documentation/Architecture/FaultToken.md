<!-- SPDX-License-Identifier: Apache-2.0 -->

# Fault token lifecycle

`FaultToken` is the architecture-neutral single-use object at the boundary
between exception delivery and an external pager. It does not edit a page table
and it does not grant mapping authority. It retains the exact registry
generations of the faulting `Thread` and its `AddressSpace` while a separately
authorized mapper performs the real mapping transaction.

The token stores a normalized fault record: virtual address, instruction
pointer, architecture syndrome, access kind, missing/protection/encoding cause
or, for a user exception other than a page fault, the exception cause
([domain creation ABI](SystemCallAbi.md)), and user or
supervisor origin. The AMD64 and IA-64 page-fault decoders and the AMD64
exception description perform that target-specific normalization before token
creation. Reserved bytes must
be zero.

The state machine is intentionally small:

```text
Uninitialized -> Pending -> Claimed -> Resolved
                         \-> Pending
```

`ClaimResolution` gives exactly one resolver exclusive ownership of the fault.
Each cancelled claim advances a nonwrapping generation, so a delayed resolver
cannot commit a later attempt. The mapper validates capabilities and installs a
mapping while the claim is active. It then commits `Map`, `Terminate`, or
`Redirect`; a failed mapping transaction cancels the claim instead. A resolved
token is terminal.

Initialization pins the exact `Thread` and `AddressSpace` object generations.
Equal raw object numbers from another registry are rejected, and a second pin
failure releases the first pin before returning. The pins stay alive through
resolution so the dispatcher can resume or terminate the retained thread before
the token owner destroys the token. This strong-reference edge is one-way:
Thread and AddressSpace do not retain their FaultToken.

`PagerFaultDispatcher` retains the blocked Thread and its SchedulingContext,
publishes the token through the reserved pager channel, and consumes a terminal
claim only after the authorized operation completes. `FaultFrameMappingPreparation`
binds an active claim to separately authorized, registry-pinned `MemoryObject`
and `Frame` slices and validates logical/physical range, access, cache policy,
user visibility, W^X, and device-execution policy.
The fixed-capacity `FaultTokenFactory` owns registration, minimal
`Invoke|Transfer` capability minting, retirement, and ABA-safe slot reuse. The
target exception capture remains architecture-specific.
