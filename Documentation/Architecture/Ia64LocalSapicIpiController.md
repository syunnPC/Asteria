<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 local-SAPIC IPI controller

`Ia64LocalSapicIpiController` is the fixed-capacity INT-delivery primitive for
the IA-64 local SAPIC Processor Interrupt Block (PIB). It consumes the
pointer-free `AcpiTopology` retained before firmware exit and an explicitly
supplied current-BSP `cr.lid` value. The controller performs no allocation,
accepts one through four enabled boot processors, and becomes immutable after
one successful initialization.

## Topology admission and logical IDs

Every retained processor record must have exactly one of the ACPI enabled or
online-capable flags, use the Local SAPIC controller kind, carry zero reserved
state, fit its hardware ID and EID into eight bits, and have an ACPI UID and
hardware ID/EID pair distinct from every other retained record. The `cr.lid`
hardware ID occupies bits 31:24 and its EID occupies bits 23:16. Exactly one
enabled record must match those fields.

Logical `CpuId` 0 is always that matching BSP, independent of MADT order. Other
enabled processors retain firmware order after the BSP is moved to slot zero.
An online-capable-only record is validated but omitted: the boot path has
no CPU-hotplug admission or removal protocol and must not target a processor
that firmware did not enable for boot. Consequently the published logical mask
is contiguous and contains only enabled boot processors.

## PIB address contract

The retained physical PIB base must be nonzero, 2 MiB aligned, wholly below
IA-64's structural bit-63 physical-address boundary, and cover a complete
nonwrapping 2 MiB extent. This is an architectural requirement, not a QEMU
assumption: Intel defines the entire relocatable PIB as a 2 MiB block aligned
on a 2 MiB boundary, with the IPI delivery region in its lower 1 MiB.
Initialization also receives a typed access base:

- `BootstrapPhysical` is valid only in IA-64 physical-address mode and must
  equal `topology.LocalControllerPhysicalAddress | (1 << 63)`. In physical
  mode, effective-address bit 63 selects the UC namespace and is discarded
  when the processor forms the physical address. The retained topology base
  remains bit-63-clear structural physical data. Its raw bit-63-clear access
  alias selects WBL and is rejected because PIB delivery requires UC.
- `PinnedUncachedVirtual` is a caller assertion that a boot-lifetime, uncached
  virtual mapping covers exactly the same physical 2 MiB PIB. The controller
  checks its numeric extent but cannot create or prove the translation.

The current boot path has not connected that runtime virtual mapping. Naming
the second mode is an integration precondition, not a claim that ordinary
translated execution may already send an IPI.

For each admitted logical processor, initialization computes the eight-byte
INT-delivery address as
`accessBase + (hardwareId << 12) + (hardwareEid << 4)`. Both additions, the
complete eight-byte range, PIB containment, and eight-byte alignment are
checked before the logical map is published.

## Sending and ordering

`SendExternalInterrupt` accepts the SAL-width integer representation but admits
only IA-64 external vectors 16 through 255, plus a nonzero target mask wholly
contained in the enabled logical mask. State, vector, and the complete mask are
rejected before any native write. Accepted targets are visited in ascending
logical-ID order and therefore require at most four eight-byte stores. The INT
delivery-mode field and redirect bit are both zero, so the written value is
exactly the vector and the address selects one physical hardware ID/EID.

The native adapter executes `mf` before the uncached `st8`, ordering all
preceding coherent ordinary-memory/mailbox publication before interrupt
assertion. Intel defines `mf` as waiting until cache-coherent agents have
observed preceding memory operations. The adapter then executes `mf.a`, which
waits until the preceding UC store has been accepted by the external platform,
before returning. These are intentionally distinct guarantees: `mf.a` alone
does not publish preceding coherent mailbox stores. The host test replacement
records the exact address/value sequence at the same C ABI symbol.

This primitive delivers the already-retained SAL AP wake vector in the current
production path. The surrounding boot code owns the coordinator and handler;
each AP completes physical stack/RSE takeover, fail-stop IVA installation, and
PAL-capability publication before locally purging its translation cache,
installing/reading back the sealed eight-RR and capacity-checked DTR/ITR initial
plan, and parking. Runtime IPI-driven shootdown is unsupported: it requires a pinned uncached
PIB mapping, AP interrupt service, acknowledgement, mailbox, and online-state
ownership. The shared
runtime root/IVT admission, one-shot local-timer proof, boot-lifetime Region-1
cooperative scheduler service, and separate idle-context TimerQueue-to-
Notification delivery are implemented elsewhere. The scheduler
service polls a separate reusable acquire/release BSP-to-AP mailbox, resumes one
retained worker in the same native context for run sequence two, and leaves the
AP `IdleServing`; it does not use a runtime IPI or make this controller a
shootdown notifier. The separate timer-delivery request also leaves that worker
blocked and does not use an IPI.

## Architectural basis

Intel Itanium Architecture Software Developer's Manual, Volume 2, Revision
2.3 defines the relevant contracts in Part 1 sections 4.4.2 (physical-address
memory attributes), 5.8.3.1 (CR.LID), 5.8.4 (the complete PIB extent and
alignment), and 5.8.4.1 (IPI address/data encoding and UC access), plus Part 2
section 11.1 (`mf.a`). In particular, CR.LID places `id` in bits 31:24 and
`eid` in bits 23:16; the IPI address places those fields in bits 19:12 and
11:4 respectively.
