<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 active runtime-translation bindings

`Ia64ActiveRuntimeTranslationBindings` is a constant-initializable,
allocation-free table with one atomic word for each of four CPUs. Each slot has
one exact state: `0` means its refill path and shared corridor are not admitted,
`1` means admitted with no active ledger, and any other valid value is a
naturally aligned Region-0 pointer to an active immutable
`Ia64RuntimeTranslationLedger`.

The six-method operational surface is deliberately narrow:
`ValidateAdmission`, `AdmitProcessor`, `ActivateAndBind`,
`AcquireActiveLedger`, `ClearActiveLedger`, and `IsBound`. The private,
side-effect-free `ValidateAdmission` preflight checks the CPU, numerical
Region-0 table and runtime-object corridor, any already-fixed admission owner
and corridor, and the selected slot's unadmitted encoding. It performs no
publication or native operation. `AdmitProcessor` repeats that validation,
fixes the immutable owner and corridor on first admission, and performs the
atomic `0`-to-`1` transition. Its integration precondition is that the CPU's
alternate refill IVT and complete pinned shared-Global RR0 corridor have
already been installed and serialized. `Ia64BspRuntimeMemory` owns the table,
derives the exact corridor from its page-aligned extent, and invokes both
private admission operations through its owned IVT. `ActivateAndBind` validates
the CPU, prior encoding, and complete table, region-set, and ledger ranges
before invoking `Ia64RuntimeRegionActivator`. Numerical Region-0 placement is
necessary but does not prove locked, pinned, or Global coverage; the
boot-lifetime BSP owner retains that proof obligation.

Invalid input and `RejectedBeforeCommit` leave the slot unchanged. Exact native
success publishes the target ledger pointer with a release store. A committed
uncertainty, including any impossible unknown activation result, release-stores
sentinel `1` because an old pointer is no longer truthful after partially
changed region registers. The common `AddressSpace` and controller apply
that conservative fail-stop/residency contract for the BSP bootstrap/idle root.
The boot-lifetime owner retains every possibly relevant native object and does
not attempt rollback or teardown after an uncertain commit.

`AcquireActiveLedger` performs one acquire load and returns a borrowed const
pointer or a typed invalid-CPU, unadmitted, no-ledger, or malformed-encoding
failure. `ClearActiveLedger` uses an exact-pointer acquire-release CAS to return
one slot to sentinel `1`; every same-CPU refill reader must first be quiescent.
`IsBound` performs a bounded four-slot acquire scan only as a post-quiescence
consistency guard. It is neither reader reclamation nor lifetime proof.

Boot-only admission precedes runtime use. Activation/replacement and clear of
one CPU slot are externally serialized single-writer operations with software
preemption, migration, and processor-local re-entry excluded. Cross-CPU slots
are independent. The table and every published ledger outlive all readers.

During its initial transaction, the BSP owner admits only the BSP slot, claims and registers one common
bootstrap/idle `AddressSpace`, activates it through
`ProcessorAddressSpaceController`, and verifies that the slot contains that
backend's exact private ledger. The post-`ExitBootServices` continuation
executes this transaction before `InitialTranslationActivator` enables
translation, and the QEMU state reaches the translated landing with that
binding and runtime IVA live. The later translated AP sequence uses the owning
runtime IVT and physical-bootstrap activation path to admit CPUs 1 through 3
and bind these slots to the same root ledger before each Region-1 context pivot.
The binding table installs neither the IVT nor the page-table backend.
Cooperative AP execution and timer delivery use these admitted bindings through
the [translated runtime](Ia64BspKernelRuntime.md#shared-root-ap-runtime),
which owns their scheduling and mailbox transactions.
