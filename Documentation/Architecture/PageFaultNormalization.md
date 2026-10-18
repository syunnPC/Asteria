<!-- SPDX-License-Identifier: Apache-2.0 -->

# Page-fault normalization

The target decoders are the narrow boundary between native exception state and
the common `FaultRecord` consumed by `FaultToken`. They do not walk a page
table, block a Thread, allocate a token, or contact a pager. Every rejected
input leaves the caller's output unchanged.

## AMD64

`Amd64PageFaultDecoder` accepts the CR2 fault address, interrupted RIP, and the
complete error code pushed for exception vector 14. It maps read/write,
user/supervisor, instruction-fetch, present, reserved-entry, protection-key,
shadow-stack, and RMP information into the common fields. Architecturally
reserved error-code bits and contradictory instruction/data access encodings
are rejected. Reserved-entry, protection-key, and RMP causes require the
present bit. The shadow-stack bit describes the access kind, so a non-present
shadow-stack access remains a missing mapping. `ArchitectureSyndrome` retains
the exact error code.

The exception entry path must copy CR2 before any nested page fault can replace
it. The fixed AMD64 paging profile enables execute-disable semantics before
this decoder is used.

## IA-64

`Ia64PageFaultDecoder` accepts IFA, IIP, ISR, IPSR, and the IVT vector offset
while interruption collection remains disabled. It handles the four faults
that have a direct external-pager meaning:

- Page Not Present (`0x5000`);
- Key Permission (`0x5100`);
- Instruction Access Rights (`0x5200`);
- Data Access Rights (`0x5300`).

TLB refill, key-miss, dirty-bit, and access-bit vectors remain on native kernel
paths because they do not by themselves mean that a user mapping is missing or
unauthorized. IA-32 interruption state is outside the native-only profile.

The low 44 bits of `ArchitectureSyndrome` retain the architected native ISR
fields. Its high 16 bits contain the IVT vector offset, which otherwise would
be lost after normalization. IIP remains the bundle pointer; ISR.ei retains the
faulting slot. IPSR.cpl values zero through two normalize to supervisor and
value three normalizes to user. ISR.ni is rejected because the interruption
registers then describe an earlier event rather than the nested access.

`PageFaultIngress` decides whether a normalized user fault may enter the
external-pager transaction and binds it to the scheduler-produced block token.
Faults in wired kernel regions remain fatal or are handled entirely within the
target port.
