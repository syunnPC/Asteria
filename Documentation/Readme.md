<!-- SPDX-License-Identifier: Apache-2.0 -->

# Documentation

[Implementation status](ImplementationStatus.md) describes supported behavior and
unsupported functionality. See the [README](../README.md) for build and test commands and
[provenance](Provenance.md) for specification sources.

## Target profiles

| Target | Profile | Runtime |
| --- | --- | --- |
| Host tests | [Host Simulator](TargetProfiles/HostSimulator.md) | [Verification](../README.md#verification) |
| IA-64 | [IA-64](TargetProfiles/Ia64.md) | [Firmware handoff](Architecture/Ia64FirmwareHandoff.md), [BSP runtime](Architecture/Ia64BspKernelRuntime.md) |
| AMD64 | [AMD64](TargetProfiles/Amd64.md) | [Boot and user runtime](Architecture/Amd64BootRuntime.md) |

## Architecture contracts

| Subsystem | Entry points |
| --- | --- |
| Boot | [UEFI contract](Architecture/BootContract.md), [handoff protocol](Architecture/BootProtocol.md), [kernel image](Architecture/KernelElf.md), [ACPI](Architecture/AcpiBootstrap.md) |
| Authority | [Capabilities](Architecture/CapabilityModel.md), [system call ABI](Architecture/SystemCallAbi.md), [protection domains](Architecture/ProtectionDomain.md), [resource domains](Architecture/ResourceDomain.md) |
| IPC | [Wire format](Architecture/IpcWireFormat.md), [capability transfer](Architecture/IpcCapabilityTransfer.md), [Endpoint](Architecture/EndpointQueue.md), [Call](Architecture/SynchronousCallDispatcher.md), [Reply](Architecture/SynchronousReplyDispatcher.md) |
| Waiting | [Notification](Architecture/Notification.md), [WaitSet](Architecture/WaitSet.md), [Interrupt](Architecture/Interrupt.md) |
| Scheduling | [Processor scheduler](Architecture/ProcessorScheduler.md), [thread lifecycle](Architecture/ThreadLifecycle.md), [scheduling contexts](Architecture/SchedulingContext.md), [kernel run loop](Architecture/ProcessorKernelRunLoop.md) |
| Synchronization | [Lock order](Architecture/LockOrder.md), [processor stops](Architecture/ProcessorStops.md) |
| Time | [Timer](Architecture/Timer.md), [timer queue](Architecture/TimerQueue.md), [deadlines](Architecture/ProcessorDeadlineController.md) |
| Memory | [Memory bootstrap](Architecture/PhysicalMemoryBootstrap.md), [AddressSpace](Architecture/AddressSpace.md), [Frame](Architecture/Frame.md), [MemoryObject](Architecture/MemoryObject.md), [root activation](Architecture/ProcessorAddressSpaceController.md) |
| Faults | [Pager channel](Architecture/PagerChannel.md), [boot pager](Architecture/BootPager.md), [fault ingress](Architecture/PageFaultIngress.md), [FaultToken](Architecture/FaultToken.md), [pager protocol](Architecture/PagerProtocol.md) |
| Devices | [DeviceRegion](Architecture/DeviceRegion.md), [PCI ECAM](Architecture/PciEcamConfigurationSpace.md), [PCI functions](Architecture/PciFunction.md), [IOMMU](Architecture/IommuDomain.md), [USB drivers](Architecture/UsbHostDriver.md) |
| Storage | [AHCI](../Source/User/System/AhciPort.hpp), [filesystem protocol](Architecture/FilesystemProtocol.md), [FAT](../Source/User/Filesystem/Fat/FatVolume.hpp), [exFAT](../Source/User/Filesystem/Exfat/ExfatVolume.hpp), [AsteriaFS](Architecture/AsteriaFsVolume.md) |
| User services | [ELF64](Architecture/Elf64Image.md), [Object Directory](Architecture/ObjectDirectory.md), [network stack](../Source/User/Network/NetworkStack.hpp), [identity](Architecture/Identity.md) |
| Desktop | [Display](Architecture/Display.md), [input broker](Architecture/InputBroker.md), [session libraries](Architecture/DesktopSessionLibraries.md), [toolkit](Architecture/Toolkit.md), [fonts](Architecture/FontEngine.md) |

The [Architecture directory](Architecture/) details state machines, resource
ownership, synchronization and failure recovery.
