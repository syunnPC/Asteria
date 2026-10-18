<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 local TLB invalidation

`Ia64LocalTlbInvalidator` is the processor-local implementation of
`ILocalTlbInvalidator`. Initialization retains the implemented virtual-address
most-significant bit from a successfully decoded `PAL_VM_SUMMARY` result and
requires the architected 4 KiB purge size reported by `PAL_VM_PAGE_SIZE`.
Invalid and failed initialization is side-effect free and retryable; a ready
instance cannot be rebound to a different processor profile.

Each call validates a page-aligned, nonempty, nonwrapping range of at most 512
4 KiB pages before executing a privileged operation. Both ends must be valid
under the retained IA-64 sign-extension rule for VA bits 60 through the first
unimplemented bit. A rejected call executes neither purge nor serialization.

For an accepted range the adapter issues `ptc.l` once per page with page-size
encoding `12 << 2`. One final `srlz.i` makes every preceding purge visible to
both instruction and data references; IA-64 instruction serialization includes
data serialization. `ptc.l` affects only the calling processor's dynamic
instruction and data translation caches. It does not broadcast to another CPU
and does not purge translation-register entries, so SMP delivery remains the
architecture-neutral dispatcher's responsibility and runtime mappings never
replace a locked TR.

Each production processor owner must construct and initialize one instance from
that processor's retained PAL report. Calls execute on the bound processor at
CPL0, with `PSR.vm` clear, migration disabled, and processor-local re-entry
excluded. The current BSP runtime owns one such instance and composes it with
`TlbShootdownDispatcher` for the three BSP-local synthetic-user-probe mapping
transactions when that diagnostic is selected. The default initial-image
mappings are installed while the private root is nonresident and need no such
runtime invalidation. No remote targets participate in those transactions. AP invalidator ownership,
TLB-shootdown mailbox draining, and IA-64 IPI notification remain unconnected.
