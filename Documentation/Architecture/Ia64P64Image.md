<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 P64 ELF Image Policy

## Scope

`Ia64P64Image` is the target-policy layer between the portable ELF64 decoder
and the IA-64 initial-user staging and mapping pipeline. One call decodes the
immutable byte view into private caller-owned plan storage and then validates
the complete P64 entry contract. Keeping both steps behind one interface
prevents a load plan produced from one image from being paired with descriptor
bytes from a different image.

The decoder allocates no memory and retains no source pointer. It does not copy
segments, zero BSS, allocate frames, create mappings, relocate code, publish an
instruction cache update, or transfer control. Input bytes and output storage
must not overlap, and the complete input must remain stable for the call.

## Machine and address profile

The current static application profile requires ELF machine 50 (`EM_IA_64`)
and flags exactly `0x10` (`EF_IA_64_ABI64`). Other architecture revisions,
OS-specific flags, interpreters, and dynamic relocation are unsupported. The
last two are already rejected by the portable decoder.

The caller supplies the PAL-derived implemented virtual-address most
significant bit from 50 through 60 and one user virtual region from 1 through
7. Every nonempty segment byte and its complete normalized page range must be
contained in one canonical lobe for that width and remain in the selected
region. Merely placing both endpoints in different canonical lobes is not
sufficient: a range spanning the intervening noncanonical hole is rejected. A
zero-length load header creates no mapping, but its address must still be a
canonical address in that user region. This keeps every published load-plan
address within the exact region-register identity required by the staging and
later mapping consumers.

## P64 entry descriptor

ELF `e_entry` names a 16-byte P64 function descriptor rather than an
instruction address. It must be 16-byte aligned, canonical, wholly within the
selected user region, readable, and completely file-backed by one load
segment. The decoder derives the corresponding file offset from that segment
and reads the two little-endian words byte by byte:

```text
word 0 = entry bundle address
word 1 = global pointer
```

The entry address must name one complete 16-byte, file-backed executable IA-64
bundle. Execute-only code is accepted; instruction fetch does not require the
data-read permission used for the descriptor. The global pointer must be
eight-byte aligned, canonical, remain in the selected region, and name eight
readable mapped bytes. Those bytes may be file-backed or zero-fill memory. The
current staging owner initializes the complete normalized physical segment
storage; the retained mapping owner makes the corresponding readable user
address valid for the later translated launch preparation.

## Published plan and failure behavior

Success publishes the descriptor address, entry bundle address, global
pointer, machine, flags, program-header count, page size, and the sorted
portable load segments. `Ia64InitialUserImageOwner` already consumes this plan
to allocate one bounded arena, zero the whole arena including BSS and padding,
copy and verify every file extent, verify all initialized slices, and publish
executable bytes through the required cache-maintenance sequence.
`Ia64InitialUserImageMemoryOwner` then creates and registers the aggregate
`Frame` and per-range pinned `MemoryObject` instances after BSP runtime-memory
preparation. The Frame records the root domain's numeric charge owner; the
pinned MemoryObjects reserve all aggregate pages plus their object metadata
against that domain. `Ia64InitialUserImageMappingOwner` maps only normalized
pages with the decoded permissions into the private root.
The [launch owner](Ia64InitialUserImageLaunchOwner.md) revalidates the image,
objects, mappings, BSP runtime and bootstrap grants, then prepares the request
with the decoded GP in user `r1`. The BSP runtime launches through its existing
Thread and SchedulingContext.

Failure leaves the outer plan incomplete. Its accessors expose zero counts and
no image information or segment, even if portable ELF decoding succeeded
before an IA-64 policy rejection. A generic ELF rejection is reported together
with its precise portable error; target-policy failures carry no unrelated ELF
error.

The policy does not constrain file size, section-table geometry, symbol names,
build-specific addresses, hashes, fingerprints, signatures, or provenance.
Machine, ABI, permission, address, descriptor, code, and GP checks are required
only because they determine whether the image can be mapped and entered by the
supported IA-64 runtime.

## Verification boundary

Hosted tests cover the canonical profile, output invalidation, exact generic
error propagation, machine and ABI flags, PAL address widths, virtual regions,
canonical-hole crossings and through-hole ranges, zero-length segments,
descriptor alignment and readability, complete file-backed entry bundles,
execute-only code, GP alignment and readability, and a GP in zero-fill memory.
Target builds compile the implementation for IA-64 and AMD64 at O0 and O2,
enforce the per-function stack ceiling, and close its portable ELF and
foundation dependencies.

`Ia64InitialUserImageOwner` consumes the decoded values for staging. Separate
[memory](Ia64InitialUserImageMemoryOwner.md),
[mapping](Ia64InitialUserImageMappingOwner.md), and
[launch](Ia64InitialUserImageLaunchOwner.md) owners materialize authority, install
private-root mappings, and enter the image. AMD64 uses its own
[ELF/runtime path](Amd64BootRuntime.md) without P64 descriptors.
