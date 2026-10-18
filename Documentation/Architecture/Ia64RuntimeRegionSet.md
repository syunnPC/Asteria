<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 runtime region-register construction

`Ia64RuntimeRegionSet` is a bounded, allocation-free value builder for the
eight IA-64 region-register words used by the initial runtime identity policy.
It performs no privileged instruction and does not read live processor state.
A default object is unbuilt, successful construction is one-shot, and any
failure leaves the output unchanged.

## Exact bootstrap adoption

Bootstrap adoption is a separate operation for the idle/bootstrap root. It
accepts only the exact eight-word profile already selected by initial
translation planning:

```text
RR[i] = ((0x2000 + i) << 8) | 0x30, for i = 0 through 7
```

Thus every bootstrap word uses its corresponding RID from `0x2000` through
`0x2007`, preferred page size 12 (4 KiB), and a disabled VHPT walker. Adoption
checks the consumed RID-width capability, retained reserved capability fields,
RR reserved fields, representability, and this exact profile. It does not
repeat unrelated PAL or firmware validation. The bootstrap policy designates
RR0 alone as shared at RID `0x2000`; RR1 through RR7 are not shared and have no
current seed mapping.

## Initial runtime policy

Runtime construction consumes one valid monotonic eight-RID allocator receipt.
RR0 remains the fixed shared bootstrap identity:

```text
RR0 = (0x2000 << 8) | 0x30 = 0x00200030
```

Candidate zero is deliberately not used. RR1 through RR7 select receipt
candidates one through seven as private runtime identities:

```text
RR[i] = (receipt[i] << 8) | 0x30, for i = 1 through 7
```

All eight runtime words therefore use preferred page size 12 and disable the
VHPT walker. PS zero is not a neutral encoding: it denotes an unsupported
one-byte page size. The architecture specifies that writing an unimplemented
page size to a region register raises a Reserved Register/Field fault; see
[Intel Itanium Architecture Software Developer's Manual, Volume 2,
section 4.1.1.7](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf).

## Construction is not admission

The words encode RID selection, preferred page size, and walker enablement
only. An RR word alone cannot prove that mappings associated with its RID are
`Global`. Neither bootstrap adoption nor runtime construction provides mapping,
VHPT/refill, activation, register installation, serialization, or runnable-root
support.

The canonical bootstrap builder and preparation stage provide that independent
proof only for the current BSP image, initial memory stack/RSE, translation
storage, and boot-lifetime runtime-owner corridor: every identity mapping lies
wholly in region 0 and is `Global`, every mapping has locked DTR coverage, and
every executable chunk also has locked ITR coverage. The post-exit BSP owner
connects the runtime backend, active-ledger binding, alternate-IVT refill state,
and common bootstrap/idle-root activation, and QEMU reaches the translated
landing with this runtime region set installed.
AP root/IVT admission, cooperative execution and timer delivery are specified by
the [translated runtime](Ia64BspKernelRuntime.md#shared-root-ap-runtime).
They consume this region set without changing its construction contract.
