<!-- SPDX-License-Identifier: Apache-2.0 -->

# ELF64 Image Load Plan

## Scope

`Elf64Image` is the architecture-independent boundary between an immutable byte image and a later loader or platform
policy. It decodes a little-endian ELF64 `ET_EXEC` file field by field, without overlaying an external structure,
allocating storage, retaining a pointer, copying segment bytes, zeroing BSS, creating mappings, applying relocations,
or transferring control. The caller supplies a bounded byte view, the mapping page size, the largest segment alignment
it supports, a load-segment capacity, and caller-owned fixed plan storage.

The implementation has a compile-time ceiling of 32 `PT_LOAD` entries. A caller may select a smaller capacity for a
particular boot profile. A failed call marks the supplied plan incomplete; accessors expose neither partially decoded
segments nor a plan retained by an earlier successful call. The input and output must not overlap, and the input must
remain byte-stable for the call.

## Functional validation

The decoder requires ELF magic, the ELF64 class, little-endian encoding, current identification and header versions,
`ET_EXEC`, and the exact ELF64 header and program-header entry sizes used by the byte decoder. Extended program-header
numbering is rejected because resolving it would make this layer depend on the section table. Program-header offset,
count, multiplication, addition, host-size representation, and complete byte-view containment are checked before a
program header is read.

Each `PT_LOAD` must satisfy the following loading requirements:

- `p_filesz` does not exceed `p_memsz`;
- the complete file range is representable and contained in the supplied view;
- the virtual memory extent is representable;
- alignment is zero, one, or a power of two no larger than the caller's supported ceiling;
- `p_offset` and `p_vaddr` are congruent modulo every alignment greater than one;
- only `PF_R`, `PF_W`, and `PF_X` are present, and writable-executable segments are rejected;
- rounding a nonempty memory extent to the caller's page geometry is representable; and
- normalized nonempty page extents do not overlap, even when the original byte extents are disjoint.

Zero-length load segments and zero or one alignment are valid ELF cases and remain representable in the plan. The
decoder does not additionally require file offsets and virtual addresses to have equal page offsets: the plan describes
copy-and-zero loading and does not promise that the input file can be mapped directly as segment pages.

`PT_INTERP` and `PT_DYNAMIC` are rejected because the generic freestanding loader supplies neither an interpreter nor
runtime dynamic relocation. Other program-header kinds are ignored. Entry validation requires only that `e_entry` lie
in nonempty file-backed `PT_LOAD` bytes. It deliberately does not impose a permission rule: a direct entry may be in
execute-only bytes, while an IA-64 P64 image may point `e_entry` at a read-only function descriptor whose `{code, gp}`
fields require later machine-specific decoding and validation.

## Published plan

Successful segments are sorted by normalized page address. Each entry publishes the original file offset and size,
virtual start and memory size, derived BSS start and size, normalized page start and count, original alignment, and
normalized read/write/execute permissions. The plan also retains the selected page size and image information containing
the machine, flags, entry address, and program-header count. A loader can therefore copy `FileSizeBytes` bytes from
`FileOffset` to `VirtualStart`, zero `BssSizeBytes` bytes at `BssStart`, and create only the page mappings described by
the normalized extent and permissions.

## Policy boundary

This layer does not validate machine-specific address shape, machine number, processor flags, instruction entry,
IA-64 function-descriptor layout, IA-64 global-pointer reachability, or an architecture's native page-table encoding.
Those checks consume the published values in an AMD64, AArch64, or IA-64 policy layer. In particular, an IA-64 policy
must decode the P64 descriptor, prove the descriptor bytes readable, prove its code address executable and file-backed,
and validate the global pointer before launch.

The decoder also does not inspect or constrain section-table layout, exact segment counts or offsets, expected file
size, hashes, fingerprints, signatures, provenance, or build-specific geometry. It does not use those properties as a
tamper oracle. Section relocation records are outside this slice; the generic layer rejects runtime dependence only
where `ET_EXEC`, `PT_INTERP`, or `PT_DYNAMIC` establishes it from the ELF and program headers it owns.
