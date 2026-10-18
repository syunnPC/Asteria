<!-- SPDX-License-Identifier: Apache-2.0 -->

# AMD64 Local Timer

`Amd64LocalTimer` is the processor-local implementation of `Hal::Time::ILocalTimer` for AMD64. It requires CPUID
support for MSRs, x2APIC, the TSC-deadline timer, and invariant TSC. Initialization requires the local APIC to be
globally enabled, enters x2APIC mode when necessary, selects TSC-deadline mode in the timer LVT entry, and leaves that
entry masked with `IA32_TSC_DEADLINE` cleared.

Programming writes the new absolute TSC deadline before unmasking the configured LVT vector, verifies the LVT, and
then reads the deadline MSR. Reaching the deadline generates the event and automatically clears
`IA32_TSC_DEADLINE`; a zero readback is therefore treated as `DeadlineReached` only when a later serializing
`CPUID; RDTSC` sample proves that the boundary passed. The adapter then masks the LVT entry and restores the previous
deadline and mask state. If that previous deadline also passed while being restored, its zero readback is accepted
only after the same ordered TSC proof, because the previous event is already pending. Cancellation masks the LVT
entry before clearing the deadline MSR. An interrupt that was already latched can still arrive and must be
acknowledged as a stale event before all deadline sources are re-evaluated.

After a target entry has already masked and verified the LVT source,
`AcknowledgeExternallyMaskedDeadline` can consume the exact backend software
arm without another MSR write. Mismatch and invalid-state failures preserve
that software metadata. This keeps the HAL contract symmetric with IA-64 for
the architecture-neutral external-stop transaction; it does not connect an
AMD64 preemption vector.

The object is owned and serialized by one processor with local interrupts disabled. Invariant TSC guarantees a
constant tick rate across power-state changes, but this primitive does not by itself prove that separate processors'
TSC values are synchronized. SMP admission must establish that condition before a scheduling context migrates between
clock domains. The integration must also stop the time domain before the 64-bit TSC wraps.

This x2APIC backend is target-built and host-tested but is not selected by native
boot. The [Q35 runtime](Amd64BootRuntime.md) uses `Amd64ApicTimer`, a separate
xAPIC countdown backend connected to interrupts, deferred delivery, and scheduling.
