<!-- SPDX-License-Identifier: Apache-2.0 -->

# Protection Domain

`ProtectionDomain` is the kernel execution boundary that associates one live
`AddressSpace` generation with one live `ResourceDomain` generation. A
`UserCapable` domain additionally associates one immovable `CapabilitySpace`;
an explicitly `KernelOnly` domain has no capability space. It is a registry
object with its own generation-bearing `ProtectionDomainId`; the association
itself does not grant authority.

Initialization is two-stage. The normal `Initialize` path validates that all
collaborators use the same `ObjectRegistry` and records the live
capability-space pointer plus its boot-unique identifier. The separate
`InitializeKernelOnly` path accepts no capability-space argument and records
the canonical null pointer and invalid identifier pair. Both paths require an
already registered ResourceDomain and an already registered AddressSpace,
retain those exact object generations with `ObjectPin`s, and
reserve `sizeof(ProtectionDomain)` kernel-metadata bytes from the real resource
hierarchy. The object is then registered and `BindIdentity` confirms that the
returned registry entry refers to the same storage.

Before publishing a binding, the domain verifies that the typed AddressSpace
pointer still denotes the exact pinned object generation and validates the
stored `ProtectionDomainKind`. `UserCapable` accepts only a ready,
registry-matching capability space whose live identifier still equals the
recorded identifier. `KernelOnly` accepts only the canonical null/invalid pair;
a mixed pair or unknown kind is `NotReady`. No pin, quota, or partial binding
remains after a failed operation.

For `UserCapable`, the capability-space owner outlives the domain. The resource
hierarchy and registry outlive every domain. `Quiesce` is a terminal transition
performed only after scheduler and syscall consumers have stopped using the
binding; it returns the metadata reservation, clears the typed AddressSpace
storage and domain kind, releases both object pins, and clears the publishable
charge owner before the domain is collected.

A `PciFunction` assignment retains one internal device-use reference while it
pins the domain generation. Ready validation and reference acquisition share
the domain lock, so `Quiesce` cannot race past an active or newly published
device assignment. Releasing the assignment drops that reference before its
generation pin. The lock is nested below the owning `PciFunction` lock, and the
domain never calls back into that function.

`ProcessorKernelRunLoop` binds every Thread/native-context pair to a Ready
ProtectionDomain. The scheduler owner keeps that immutable binding Ready until
Unbind, and the active run-loop snapshot exposes both the domain object and its
identity. Multiple Threads may share one domain. An entered native context
cannot migrate to another run loop or ProtectionDomain through this binding.

`SystemCallIngress` consumes the active binding and reacquires this domain's
live binding. A kernel-only binding reaches the existing fail-closed
`CapabilitySpaceMissing` result and never calls `SystemCallDispatcher`. A
user-capable binding must still match its recorded and live capability-space
identifiers before dispatch. Native entry calls that common boundary after trusted register capture. The binding exposes both the generation-bearing
AddressSpace identity and the exact typed storage retained by the domain pin;
fault correlation uses the identity, while `ProcessorKernelRunLoop` passes the
typed storage to the processor-local activation controller before every
different-root native context switch. Native privilege entry and user-mode
return remain separate target-specific transactions.
