<!-- SPDX-License-Identifier: Apache-2.0 -->

# Pager Fault Protocol

## Scope

`PagerProtocol` is the typed IPC boundary between the kernel page-fault path
and an external pager. It builds and parses one canonical asynchronous
`MessageEnvelope`; it does not create a `FaultToken`, block or resume a Thread,
send through an Endpoint, or edit a page table.

The stable interface identifier is
`50414745522d0001:4153544552494101`, version 1.2, method 1. Every message has a
nonzero transaction identifier and may carry either a finite or infinite
deadline. Trace identifiers are optional, but a trace identifier and span must
be present together.

## Canonical body

The fixed payload is exactly 48 bytes and uses explicit little-endian integers.
Version 1.2 includes the kernel-assigned ProtectionDomain identity alongside
the record and supervision tag ([component launch](BootPager.md)).
A parser accepts only 1.2. The identity includes the registry slot and generation;
policy identifies the principal by identity before using its creator's local tag.
It never uses the C++ `FaultRecord` object representation.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | fault virtual address |
| 8 | 8 | interrupted instruction pointer |
| 16 | 8 | normalized architecture syndrome |
| 24 | 1 | read, write, or execute access |
| 25 | 1 | missing, protection, invalid-encoding or exception cause (4: a user exception other than a page fault, whose addresses are the faulting instruction's) |
| 26 | 1 | user or supervisor origin |
| 27 | 5 | zero |
| 32 | 8 | supervision tag of the faulting domain (zero for a boot domain) |
| 40 | 8 | kernel-assigned ProtectionDomain identity |

There are no variable payloads. All unused payload, descriptor, attachment,
header-reserved, and alignment bytes are zero. Parsers reject rather than
ignore noncanonical extensions, so a future version must use a new negotiated
version or method instead of assigning hidden meaning to padding.

## Resolution authority

Exactly one capability attachment is required. It is a moved `FaultToken`
capability attenuated to `Invoke` only; its requested lease deadline equals the
message deadline. The payload is sufficient for pager policy and routing, while
the capability is the sole authority to claim and resolve the retained fault.
The pager cannot turn the descriptive payload into mapping authority.

Build validates completely before its first output write. Parse publishes its
local candidate only after complete validation. Every failure therefore leaves
caller output unchanged.
`PagerFaultDispatcher` binds an already registered token and already
blocked exact Thread to reserved Endpoint delivery and terminal wake or
termination. Token-pool creation and the exception-to-block scheduler hook
remain separate and are not hidden in this codec.
