<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 runtime translation-cache inserter

`Ia64RuntimeTranslationCacheInserter` is an allocation-free commit primitive for
one already-encoded 4 KiB data or instruction translation. Before entering its
privileged helper, it validates the insertion kind, key-zero/page-size-12 ITIR,
nonzero-region page-aligned IFA, and the complete supported translation-word
shape. A rejection invokes neither helper and changes no architectural state.

The data helper writes `cr.ifa` and `cr.itir`, executes one `itc.d`, and completes
`srlz.d`. The instruction helper performs the corresponding single `itc.i` and
`srlz.i` sequence. The inserter does not clear PSR bits, install region
registers, purge translations, allocate, lock, call firmware, or invoke a
callback.

The commit path has strict caller-owned preconditions: execution is at CPL0
with `PSR.vm=0` and `PSR.ic=0`; local interrupts, preemption, migration, and
processor-local re-entry are excluded; the correct RR/RID is active; and the
code, data, stack, RSE backing store, and operands are pinned on the shared path.
The caller must establish these conditions before calling `Insert`.

## Component boundary

The inserter does not provide an IVT refill
entry, CPU-local active-ledger binding, region-register activation, a runtime
`IAddressSpacePageTable` backend, a runnable IA-64 root, or an AP/IPI path.
