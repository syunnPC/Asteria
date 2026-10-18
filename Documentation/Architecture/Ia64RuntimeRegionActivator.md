<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 runtime region activation

`Ia64RuntimeRegionActivator` is an allocation-free commit boundary for one
immutable `Ia64RuntimeRegionSet`. Before entering its native helper, it requires
RR0 to equal the fixed shared word `0x00200030`. RR1 through RR7 must each use a
nonzero, unique private RID distinct from RR0, preferred page size 12 (4 KiB), a
disabled VHPT walker, and zero reserved fields.

CPL0, `PSR.vm=0`, software exclusion of preemption, migration, and
processor-local re-entry, and the pinned shared-Global code/data/stack/RSE/
operand corridor and lifetime are caller preconditions. The native helper
rejects visible `PSR.i` or `PSR.pk` and a bad live RR0 before mutation;
validating `PSR.i` establishes the required local interrupt-disabled state.
It saves the entry `PSR.ic` state first, resolves every RR address in registers,
then clears IC and executes `srlz.d` and `srlz.i` before the first write.
This no-memory, no-stack leaf keeps IC clear through the complete RR transaction.

The first RR1 write is the commit point. The helper writes RR1 through RR7 in
order, executes `srlz.d` and `srlz.i`, then reads back every private region
register. Exact readback reports success. Any mismatch after RR1 reports
`CommittedStateUnknown`; prior RR state is not reconstructed or restored.
Success and readback mismatch converge before return. If IC was set on entry,
the helper restores it and executes `srlz.d` and `srlz.i`; an IC-clear caller
therefore also returns IC-clear. The physical-bootstrap entry retains its
stronger preflight requiring `PSR.ic`, `PSR.i`, `PSR.pk`, `PSR.it`, `PSR.dt`,
and `PSR.rt` clear, so this preservation does not broaden physical takeover.

The post-exit BSP runtime backend invokes this component while activating the
common bootstrap/idle root. The activator itself never writes RR0, purges
translations, publishes an active ledger, installs an IVT refill path, or
admits a runtime page-table backend or runnable root; the caller owns those
surrounding transactions.
