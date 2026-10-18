<!-- SPDX-License-Identifier: Apache-2.0 -->

# PCI Function Authority

## Scope

`PciFunction` turns one immutable `PciFunctionDescriptor` into a registry-publishable kernel authority. It is the
architecture-neutral identity used both for driver assignment and as the exact `IIommuDevice` requester endpoint.
It does not enumerate buses, size BARs, allocate address windows, enable bus mastering, reset hardware, or write any
PCI configuration register.

## Ownership and lifetime

Initialization pins the exact `ResourceDomain` generation, reserves `sizeof(PciFunction)` metadata bytes, and copies
the validated descriptor. The descriptor requires a valid segment/bus/device/function address, a present vendor ID,
and a common-header layout. The platform's unique `DeviceManager` owns 256 immovable function slots and binds one
completed immutable catalog for its lifetime. Creation accepts only an address and copies the matching catalog
descriptor; callers cannot substitute vendor, class, or requester fields. It returns a registry-qualified
`PciFunctionId` and a capability with exactly `Inspect|Manage|Transfer`, never a raw storage pointer.

The IA-64 bootstrap owner discovers the complete catalog before binding it and
preflights the catalog count against those 256 slots. It retains each successful
identifier and capability in boot-lifetime storage. The handle remains scoped
to that owner's private capability-space identity until controlled
capability transfer moves it; copying the immutable publication record does not
grant authority in another capability space.

Retirement first changes an idle function from `Available` to `Retiring` under its lock. This seal prevents a new
driver assignment before registry destruction starts. The BDF and slot remain reserved while capabilities, leases,
and pins drain. Only the exact collector record can enter manager reclamation, where the `Retiring` state is checked,
the metadata charge and resource-domain pin are released, and the storage is reset for a new registry generation.
The assignment sequence is preserved across slot reuse. A slot whose sequence reaches its maximum is permanently
retired instead of wrapping; old assignment tokens therefore cannot acquire meaning again. The manager storage
outlives every `PciFunctionId`, assignment token, retained lease, pin, and direct use of its embedded functions.

Assignment accepts one registry-qualified `ProtectionDomainId` and its exact storage. The caller must already have
authorized the operation. A successful assignment pins that exact object generation and atomically retains a Ready
device-use reference in the domain, preventing concurrent domain quiescence. It returns a private, non-authorizing
token naming the function and a nonzero monotonically increasing sequence. The sequence never wraps, so a token from
an earlier assignment cannot release a later one. Exact release drops the domain use before dropping the object pin.

## DMA ordering

Only an assigned function may be claimed by an `IommuDomain`, and only one domain may claim it at a time. The exact
claiming domain must release it before the driver assignment can be released. Trusted device-management code must
therefore revoke driver access, disable bus mastering, drain in-flight DMA, detach the IOMMU relationship, and only
then release the assignment. `PciFunction` enforces the relationship order but deliberately does not perform those
hardware operations itself.

## Synchronization

All manager operations serialize on the manager's spin lock. Its fixed nesting order is `DeviceManager` before
`PciFunctionCatalog`, `PciFunction`, `ObjectRegistry`, `ProtectionDomain`, and `ResourceDomain`; none of those
collaborators may call back into the manager. All function state transitions serialize on the function's spin lock,
which may then nest the registry, protection-domain, or resource-domain operation required by that transition.
Operations are bounded but may spin, so they are forbidden from interrupt context and same-CPU reentry.
