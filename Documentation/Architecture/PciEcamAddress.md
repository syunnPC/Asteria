<!-- SPDX-License-Identifier: Apache-2.0 -->

# PCI ECAM Address Contract

## Scope

`PciEcamAddressCalculator` is a pure bridge from one generic `PciEcamRegion` and one `PciFunctionAddress` to a
physical DWORD address. It has no dependency on ACPI and performs no mapping, volatile access, endian conversion,
serialization, or device discovery. The configuration-space adapter must translate its firmware record, establish
mapping ownership and a safe cache policy, and provide the required MMIO ordering before dereferencing the result.

## Calculation

`PciEcamRegion.PhysicalStart` corresponds to `FirstBus`, allowing callers to translate ACPI MCFG, device-tree, or
platform-owned windows before entering this generic contract. For an admitted segment, inclusive bus range, valid
device/function, and naturally aligned register offset through `0xFFC`, the calculator returns:

`PhysicalStart + ((Bus - FirstBus) << 20) + (Device << 15) + (Function << 12) + RegisterOffset`

The fields occupy disjoint parts of the ECAM offset. The calculator rejects malformed or unaligned region records,
segment or bus mismatches, invalid PCI function coordinates, invalid DWORD offsets, and any aperture or final-address
addition that would exceed 64 bits. It does not mask invalid input into a different address.
