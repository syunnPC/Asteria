<!-- SPDX-License-Identifier: Apache-2.0 -->

# Source provenance

Implementation follows specifications, datasheets and vendor documentation.
External firmware tables are optional test inputs
and remain outside the repository.

## Third-party data

| Material | Source and terms |
| --- | --- |
| NetBSD bold8x16 bitmap | [Font attribution](../Source/User/Graphics/Bold8x16Font.hpp), [public-domain notice](../LICENSES/LicenseRef-Public-Domain.txt) |
| Vegur Light, Regular and Bold | [Designer, source and hashes](../Data/Fonts/Vegur/NOTICE.md), [published terms](../LICENSES/LicenseRef-Vegur-No-Rights-Reserved.txt) |
| Cryptographic test vectors | [Vector inventory](../Tests/User/Crypto/Readme.md), [IETF terms](../NOTICE) |

The [default cursor](../Data/Cursors/Default/README.md) is project artwork
built from geometric primitives.

## Specification references

| Area | Primary basis | Project contract |
| --- | --- | --- |
| Keyboard and USB | Microsoft Keyboard Scan Code Specification; USB HID Usage Tables; USB/xHCI specifications | [Input](Architecture/InputBroker.md), [USB libraries](../Source/User/Usb/Readme.md) |
| DMA isolation | Intel VT-d; AMD IOMMU specification 48882; virtio specification | [Driver isolation](Architecture/IommuDomain.md), [AMD-Vi](Architecture/AmdVi.md), [virtio-IOMMU](Architecture/VirtioIommu.md) |
| Legacy USB and serial | Intel UHCI/PIIX4; PC16550D datasheet | [USB libraries](../Source/User/Usb/Readme.md), [boot protocol](Architecture/BootProtocol.md) |
| CPU and clocks | Intel/Itanium architecture manuals; Microsoft Hypervisor TLFS | [Target profiles](Readme.md#target-profiles), [processor scheduling](Architecture/ProcessorScheduler.md) |
| Firmware and PCI | UEFI/PI specifications; ACPI; PCI Local Bus specification | [Boot contract](Architecture/BootContract.md), [ACPI bootstrap](Architecture/AcpiBootstrap.md) |
| Cryptography | FIPS 180-4/197; NIST SP 800-38D/90A; IETF crypto RFCs | [Crypto](Architecture/Crypto.md), [vector inventory](../Tests/User/Crypto/Readme.md) |
| Images and cursors | RFC 1950/1951; W3C PNG; Microsoft bitmap/icon documentation | [Imaging](Architecture/Imaging.md) |

Specification URLs:

- [Microsoft Keyboard Scan Code Specification 1.3a (vendor PDF mirror)](https://christophervickery.com/babbage/courses/cs345/Manuals/ms_scancode.pdf)
- [USB HID Usage Tables 1.7](https://www.usb.org/sites/default/files/hut1_7.pdf)
- [Intel VT-d revision 5.0](https://cdrdv2-public.intel.com/831418/vt-directed-io-spec.pdf), [revision 4.0](https://cdrdv2-public.intel.com/671081/vt-directed-io-spec.pdf), [revision 3.4 mirror](https://kib.kiev.ua/x86docs/Intel/VT-d/D51397-014.pdf)
- [Microsoft Hypervisor Top-Level Functional Specification 6.0b](https://github.com/MicrosoftDocs/Virtualization-Documentation/raw/main/tlfs/Hypervisor%20Top%20Level%20Functional%20Specification%20v6.0b.pdf), [VMware KB 1009458](https://kb.vmware.com/s/article/1009458), [VMware timekeeping guide](https://www.vmware.com/pdf/vmware_timekeeping.pdf)
- [AMD 48882 revision 3.09 mirror](https://kib.kiev.ua/x86docs/AMD/IOMMU/48882-3.09.pdf)
- [Intel UHCI guide mirror](https://ftp.netbsd.org/pub/NetBSD/misc/blymn/uhci11d.pdf), [Intel PIIX4](https://www.intel.com/Assets/PDF/datasheet/290562.pdf)
- [National PC16550D datasheet mirror](https://www.scs.stanford.edu/10wi-cs140/pintos/specs/pc16550d.pdf)
- [Intel SDM volumes 3A–3D](https://cdrdv2-public.intel.com/671447/325384-sdm-vol-3abcd.pdf)
- [Intel 82574 datasheet mirror](https://www.farnell.com/datasheets/1920783.pdf)
- [UEFI PI 1.7](https://uefi.org/sites/default/files/resources/PI_Spec_1_7_final_Jan_2019.pdf), [PCI Local Bus 3.0](https://pcisig.com/PCIConventional/Specs/Base/LocalBus_3.0)
- [Synaptics guide](https://www.synaptics.com/sites/default/files/ACF126.pdf?q=decaf/utilities/ACF126.pdf)

