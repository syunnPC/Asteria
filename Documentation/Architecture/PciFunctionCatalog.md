<!-- SPDX-License-Identifier: Apache-2.0 -->

# PCI Function Catalog

## Scope

`PciFunctionCatalog` is the architecture-neutral, read-only boundary between PCI configuration access and later
device management. A platform adapter supplies already-authoritative segment and inclusive bus ranges together with
an `IPciConfigurationSpace` implementation. The catalog has no dependency on ACPI, UEFI, SAL, or any other firmware
table format.

The catalog does not write PCI configuration space, size BARs, allocate MMIO or I/O windows, renumber
buses, configure bridges, bind drivers, or handle hotplug. Those operations require separate policy and resource
ownership contracts.

Each published descriptor is the immutable input to a `PciFunction` authority. The platform's unique
`DeviceManager` permanently binds one completed catalog and materializes authorities by address, preventing callers
from substituting descriptor fields or creating duplicate live/quiescing BDFs. The catalog itself still owns no
registry object, driver pin, or IOMMU relationship.

## Address and read contract

`PciFunctionAddress` stores segment, bus, device, and function separately. Device and function are bounded to 31 and
7. For a valid address, `GetRequesterId()` returns only the conventional 16-bit bus/device/function value; the segment
remains separate.

`IPciConfigurationSpace::Read32()` accepts naturally aligned DWORD offsets from `0x000` through `0xFFC`. A successful
read returns the little-endian configuration DWORD as a host integer. A nonexistent function is represented by a
successful `0xFFFF'FFFF` read, while an access failure leaves the caller's output unchanged.

## Discovery

Discovery validates and copies at most eight nonoverlapping ranges. Each inclusive range is bounded by the native PCI
bus number, but there is no additional aggregate bus cap across independent segment groups. The ranges are sorted by
segment and bus and then scanned in deterministic order. Function zero is always probed first. Functions one through
seven are
probed only when function zero reports the multifunction bit. For each present function the catalog records the
vendor, device, revision, programming-interface, subclass, class, and masked header-layout fields from the common
header.

The catalog stores at most 256 functions and performs no allocation or callback. Discovery is a one-shot transaction:
success publishes the complete immutable catalog; any validation, capacity, or configuration-read failure erases all
partial state and permits a retry. The caller keeps the range storage and PCI topology stable until discovery returns.
Hotplug and concurrent configuration changes are not supported by this bootstrap contract.

Queries and discovery are serialized by a spin lock. Operations are bounded but may spin, so they are not permitted
from interrupt context or through same-CPU reentry. A bound manager may hold its own lock while querying the immutable
catalog; the fixed order is `DeviceManager` before `PciFunctionCatalog`, and catalog code never calls the manager.

## References

- [UEFI 2.11 PCI bus support](https://uefi.org/specs/UEFI/2.11/14_Protocols_PCI_Bus_Support.html)
- [PCI Local Bus Specification 3.0](https://pcisig.com/PCIConventional/Specs/Base/LocalBus_3.0),
  section 6.1: configuration-space organization, common header fields,
  nonexistent-function reads, and the multifunction bit.
