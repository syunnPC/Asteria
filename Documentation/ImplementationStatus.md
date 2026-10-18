<!-- SPDX-License-Identifier: Apache-2.0 -->

# Implementation status

Asteria is an experimental C++23 capability microkernel with IA-64 and AMD64
UEFI boot paths.

## Supported behavior

- Both targets run user services with capability validation, synchronous IPC,
  timers, supervised faults, private address spaces, W^X protections and bounded
  cross-core TLB shootdown.
- Up to four processors are supported. AMD64 schedules user threads on application
  processors; IA-64 currently runs user threads on CPU 0. See
  [processor scheduling](Architecture/ProcessorScheduler.md).
- Usable RAM enters a bounded Frame allocator and is zeroed before reuse.
  Firmware DMA must be contained or stopped before BootServices memory is
  admitted. See [memory bootstrap](Architecture/PhysicalMemoryBootstrap.md).
- USB, AHCI and network drivers run as separate services. VT-d, AMD-Vi and
  virtio-IOMMU provide DMA isolation; explicitly trusted driver profiles also
  exist. See [IOMMU](Architecture/IommuDomain.md) and [target profiles](Readme.md#target-profiles).
- Filesystem services support bounded FAT, exFAT and AsteriaFS operations,
  partition enumeration and per-incarnation namespace views.
- Display, input, composition, terminal and toolkit components run in user space.
  Optional login services use a trusted seat and a 64 MiB Argon2id arena.
  The desktop has no principal-bound privacy guarantee. See
  [identity](Architecture/Identity.md).
- Processor/component watchdogs, terminal system stops, poweroff and reboot are
  implemented. See [watchdog](Architecture/Watchdog.md) and [power transitions](Architecture/PowerOff.md).

## Unsupported functionality

Thread migration, Component Manager lifecycle and application launch policies,
durable identity/settings, principal-bound views and ACLs, provisioning and
recovery, transferable file handles, broader storage/network protocols, native
GPU drivers and an AArch64 port are unsupported.

## Validation

The [README](../README.md#verification) covers hosted tests, sanitizers, static
analysis, header/stack checks, target ELF/PE and assembly checks, and QEMU
integration tests. Target profiles and subsystem contracts define their scope;
VM results do not establish physical hardware support or cryptographic timing.
