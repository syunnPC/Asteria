<!-- SPDX-License-Identifier: Apache-2.0 -->

# AMD64 System Call Register Codec

`Amd64SystemCallRegisterCodec` performs fixed-width copies between native register
snapshots and the common syscall request/response. It has no hardware access,
allocation, or failure path. `Amd64SystemCallRegisterImage` is an internal snapshot,
not a user-shared ABI structure.

## Register mapping

The request convention maps the syscall number and six arguments to the common `SystemCallRequest` in this order:

| AMD64 register | Request meaning | Common field |
|---|---|---|
| RAX | Request number | `Number` |
| RDI | Argument 0 | `Arguments[0]` |
| RSI | Argument 1 | `Arguments[1]` |
| RDX | Argument 2 | `Arguments[2]` |
| R10 | Argument 3 | `Arguments[3]` |
| R8 | Argument 4 | `Arguments[4]` |
| R9 | Argument 5 | `Arguments[5]` |

The response convention writes `SystemCallResponse::Error` to RAX, `Value0` to RDX, and `Value1` to R8. Applying a
response leaves RDI, RSI, R10, and R9 bit-exact. Canonical error responses therefore place the numeric error value in
RAX and zero RDX and R8.

The register image is 56 bytes with eight-byte alignment. Its field order is RAX, RDI, RSI, RDX, R10, R8, and R9.
Compile-time size, alignment, type-property, and literal offset checks protect this internal adapter contract.

## Native integration

The [native entry](Amd64BootRuntime.md#native-entry-contract) owns GDT/TSS/MSR setup,
the supervisor stack, complete integer frame, active ProtectionDomain, and
validated IRETQ return. The codec neither establishes nor verifies those entry
preconditions. Hosted tests and both optimized/unoptimized target builds check
its field mapping and layout.
