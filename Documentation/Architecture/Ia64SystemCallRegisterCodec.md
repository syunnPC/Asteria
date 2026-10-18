<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 System Call Register Codec

## Scope

`Ia64SystemCallRegisterCodec` is the pure conversion boundary used by the
trusted IA-64 system-call entry. It reads or writes only caller-owned
C++ value objects, performs no allocation, accesses no global state, and issues
no native instruction. Equal inputs therefore produce equal outputs.

`Ia64SystemCallEntry`, rather than this pure codec, implements break-immediate
validation, IVT entry state, the RSE switch, and the `rfi` return.
In particular, this component neither establishes that a call came from user
mode nor selects the current protection domain. Those responsibilities belong
to that trusted native entry and the active processor run loop.

## Register image

The fixed image contains eleven `UInt64` words in this exact order:

| Offset | Field | Meaning |
|---:|---|---|
| 0 | `R15` | Request number |
| 8 | `R32` | Argument 0 |
| 16 | `R33` | Argument 1 |
| 24 | `R34` | Argument 2 |
| 32 | `R35` | Argument 3 |
| 40 | `R36` | Argument 4 |
| 48 | `R37` | Argument 5 |
| 56 | `InputNatBits` | NaT metadata for the seven request registers |
| 64 | `R8` | Response error |
| 72 | `R9` | Response value 0 |
| 80 | `R10` | Response value 1 |

The image is 88 bytes with eight-byte alignment. Literal size, alignment, and
member-offset assertions protect this layout. It is a trusted internal adapter
contract, not a memory structure shared directly with user mode.

## NaT metadata and request decoding

Only bits 0 through 6 of `InputNatBits` are defined:

| Bit | Register |
|---:|---|
| 0 | `R15` |
| 1 | `R32` |
| 2 | `R33` |
| 3 | `R34` |
| 4 | `R35` |
| 5 | `R36` |
| 6 | `R37` |

`DecodeRequest` validates the full metadata word before changing the supplied
`SystemCallRequest`. Its outcomes are:

| Result | Meaning |
|---|---|
| `None` | No input is NaT; the request is ready for dispatch. |
| `InputContainsNat` | At least one known input register is NaT. |
| `InvalidNatMetadata` | At least one bit outside bits 0 through 6 is set. |

Unknown metadata has priority over known NaT bits when both are present. Both
failures leave the request unchanged. The native bridge translates
`InputContainsNat` to the canonical user response
`{SystemCallError::InvalidArguments, 0, 0}` and skips dispatcher mutation.
`InvalidNatMetadata` instead identifies a kernel capture invariant failure; it
must follow the kernel failure path and must never be exported as a user error.

## Response application

`ApplyResponse` writes the numeric `SystemCallError` to `R8`, `Value0` to `R9`,
and `Value1` to `R10`. It preserves `R15`, `R32` through `R37`, and
`InputNatBits` bit-exact. Canonicalization remains the producer's contract: the
architecture-neutral dispatcher and the native NaT-rejection bridge supply zero
value words for error results, while the codec maps their values exactly.

## Native integration

The implemented IA-64 assembly boundary performs the architectural work that
this codec deliberately cannot perform:

- the runtime IVT break slot branches directly to the native entry for
  `break.i 0x100000`;
- the entry validates the PL3/bank-one interrupted state and fixed nonrotating
  output frame, captures GR15 and stacked GR32 through GR37 with their NaT
  metadata, and normalizes them into the image above;
- `cover` and the dedicated 32-KiB kernel RSE store move the interrupted dirty
  partition away from the user backing store while the bridge dispatches
  through the exact active processor run loop;
- restore reloads the interrupted partition before restoring user BSPSTORE and
  RNAT, makes GR8 through GR10 non-NaT, advances the resume coordinate past the
  slot-two break, and returns with `rfi`.

The explicit synthetic-user-probe diagnostic exercises this path three times through one no-local
six-output child frame. `CapabilityQuery` uses the launch handle in GR32/GR33
and returns `0`, `0x0000000000010001`, and `UINT64_MAX` in GR8-GR10. The
payload saves those live values before issuing IPC send and receive with a
separately staged Endpoint handle, stores each later live response as public
evidence, and reloads the Query result before returning to its parent. Every
call rewrites all six arguments, keeps unused arguments zero, and uses a
stopped slot-two `break.i 0x100000`.
