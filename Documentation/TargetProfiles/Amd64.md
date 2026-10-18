<!-- SPDX-License-Identifier: Apache-2.0 -->

# AMD64 Target Profile

This profile supports Q35/OVMF with one through four CPUs. The
[boot contract](../Architecture/BootContract.md) defines the firmware boundary;
[Implementation Status](../ImplementationStatus.md) records integration and tests.

## Compiler and ABI

| Item | Validated configuration |
| --- | --- |
| Compiler | GCC 16.2.0, `x86_64-elf-g++` |
| Binutils | 2.47 development snapshot, `x86_64-elf-*` |
| Language | C++23, `-ffreestanding -nostdinc -nostdinc++` |
| Object ABI | ELF64 x86-64 little-endian, LP64, eight-bit bytes, signed plain `char` |
| Kernel calls | System V AMD64, 16-byte ABI stack alignment, `-mno-red-zone` |
| Firmware ABI | PE32+ `pei-x86-64`, subsystem 10, Microsoft x64 |
| Code generation | `-fno-pic -fno-pie -fno-jump-tables`; kernel `-mcmodel=kernel` |

Override tool discovery at configuration with `AMD64_CXX`, `AMD64_LD`,
`AMD64_OBJCOPY`, `AMD64_NM`, `AMD64_READELF` and `AMD64_OBJDUMP`
(`Tools/CMake/Toolchains/Amd64.cmake`).

EFI uses `-mcmodel=large -mlarge-data-threshold=4194304`, keeping initialized and
zero-filled data in `.data`/`.bss`. Absolute image addresses receive `DIR64`
relocations. The PE sections are `.text`, `.rdata`, `.rodata`, `.bss`, `.idata`,
`.reloc`, and `.data`, with page-aligned W^X protections. COMDAT selection closes
in ELF before PE emission; the build verifies every relocation before publication.

[`Foundation/Types.hpp`](../../Source/Foundation/Types.hpp) defines fixed-width
integer aliases, `UIntPointer` for addresses, and `Size` as the type of `sizeof`.
Signed integers use two's complement. Pointers and `UIntPointer` use flat eight-byte addresses;
pointer conversion preserves byte-address order within the same allocated object.
Verified size/alignment pairs
are 16/8 for `CapabilityHandle`, 48/8 for `CapabilityDescriptor`, and 32/8 for
`PhysicalPageRange`; `RevocationDomainId` is eight bytes. External formats use
explicit field encoding.

## Runtime and permitted extensions

- Target images link no external standard library or compiler runtime.
  Exceptions, RTTI, guarded local-static initialization, `__cxa_atexit`,
  stack protectors, and unwind tables are disabled.
- Namespace-scope state requires constant initialization; link checks reject
  constructor/destructor arrays. Retained owners use placement construction;
  target startup has no heap.
- Project C ABI memory primitives close compiler-generated copies and fills.
  Both kernel and user code are compiled with `-fno-builtin` to ensure copy/fill loops do not recurse
  into compiler-generated intrinsic calls. `RuntimeMemoryLeaves` verifies on every
  build that each image's memory routines are leaf functions. Compile-check
  closures and firmware kernels link `Runtime/CxxAbi.cpp` and `Platform/FatalTrap.cpp`;
  `__cxa_pure_virtual` and `FatalTrap` must be nonempty executable global symbols,
  and final images must have no unresolved symbols (including weak vtable references).
  `FatalTrap` executes `ud2` (`__builtin_trap`); upon an unhandled trap, the kernel
  records `KernelTrap`, reports `ASTERIA:STOP`, and halts via emergency terminal
  freeze and screen display.
- `-mgeneral-regs-only` excludes x87/MMX/XMM/YMM/ZMM use, and Verify
  `Amd64KernelGeneralRegistersOnly` checks the result: the registers hold user threads' state, which the context
  switch saves and restores eagerly ([extended processor state preservation](../Architecture/Amd64KernelContext.md#switch-boundary)).
- GCC atomics belong to `Platform/Synchronization` and must compile to inline,
  lock-free instructions. Platform adapters may use `__builtin_offsetof`,
  `ms_abi`, `__builtin_unreachable`, and the target `__builtin_trap`.
  Assembly is confined to CPU/MMIO access, native entry/context switching,
  AP startup, diagnostics, the boot entropy sources (`RDSEED`, `RDRAND`,
  `RDTSC` after their CPUID checks), and terminal halt.
- The common run loop uses `noinline` on bounded recovery helpers. A compiler
  ignoring the attribute must still pass the 512-byte function-frame gate.

## Execution and memory constraints

Firmware validates zero-fill and loaded-image ownership before constructing
kernel state. Boot services end permanently at successful `ExitBootServices`.
The [initial address space](../Architecture/InitialAddressSpace.md) requires a
23-page activation extent and ten mappings. Its table/stack/descriptor state is
sealed before exit; a stale-key retry validates those exact bytes.

The [native runtime](../Architecture/Amd64BootRuntime.md) owns a 3,529-page pool below
4 GiB and two startup pages below 1 MiB. The current layout is defined by
`Source/Platform/Amd64/Boot/Amd64RuntimePoolLayout.hpp`. Shared and private roots
preserve the kernel corridor; private roots contain user mappings. User image pages
have no supervisor writable identity alias. User text is RX, data/stacks are
RW/NX, and stack guards remain unmapped.

The root backend requires PCIDE and PGE off. The entry install gives every CPU
the same control registers: CR4 exactly PAE, MCE, OSFXSR and OSXMMEXCPT, CR0.MP/NE set and CR0.EM/TS/AM
clear ([native entry](../Architecture/Amd64BootRuntime.md#native-entry-contract)). It validates the owned table arena,
loads the raw PML4 address into CR3, and reads it back; an indeterminate commit is
terminal. See [runtime mappings](../Architecture/Amd64RuntimePageTableMapper.md).
[Cooperative contexts](../Architecture/Amd64KernelContext.md) require caller-owned,
page-aligned 4-64 KiB stacks and save the ABI-preserved integer state. Complete
interrupt and user-return frames belong to native entry.

Each CPU owns GDT/IDT/TSS/GS state, RSP0, a double-fault IST, and a separate terminal
NMI/machine-check IST. SYSCALL uses SWAPGS, LFENCE, an exclusive supervisor stack,
and IRETQ; it returns the program's RSP unchanged and its RFLAGS sanitized, and
stops only on a frame field the kernel itself produced wrongly. User I/O and FSGSBASE are disabled. User threads may
execute x87/SSE instructions, whose state the context switch keeps. The images the root service builds (component,
Directory, client, interpreter) compile with SSE; the initial image, the pager and the spinner stay
`-mgeneral-regs-only`, since their hand-written xmm patterns span compiled code. A CPL3 #MF or #XM of
a thread the pager created reaches the pager, which terminates it; one of a boot thread (initial image, pager,
spinner) stops the machine, like any other boot-thread exception. Created user
threads can run on the BSP and APs. Migration and more than four processors
are unsupported ([processor scheduling](../Architecture/ProcessorScheduler.md)).
Function frames are limited to 512 bytes at O0/O2; whole-path nesting
bounds remain a separate requirement.

## Concurrency, interrupts, and I/O

Shared memory uses platform atomics and locks. Spin locks are neither fair nor
bounded under contention; callers exclude same-CPU preemption and reentry.
ISRs cannot acquire registry locks, allocate, block, or run unbounded work.
[Endpoint bounds](../Architecture/EndpointQueue.md#concurrency-and-bounds) exclude
contended spin time and do not make those operations interrupt-safe.

Q35 boot selects `Amd64ApicTimer`, a local APIC countdown timer calibrated against the
owning CPU's TSC. IRQ entry masks/reads back the source and acknowledges EOI before
normal-context TimerQueue/Notification work. The runtime uses xAPIC or x2APIC
according to the firmware handoff, with native shootdown and reschedule IPIs.
AP user execution has local timer preemption. The separate
[TSC-deadline backend](../Architecture/Amd64LocalTimer.md) is target-built and
host-tested but not boot-selected. Processor admission checks clock skew;
migration still requires cross-CPU time guarantees. Stop the time domain before wrap.

The production initial image starts the root service; guest-test firmware exercises
IPC, timer-driven waits and AP delivery. The optional BSP preemption probe captures a fixed kernel
loop, replenishes its budget, replaces the idle continuation, and restores its
integer frame before user launch. This diagnostic is separate from user-thread preemption.

PCI ECAM and AHCI bootstrap diagnostics use ordered DWORD MMIO through a temporary UC/NX aperture on
CPU0. Every access restores the sealed seed leaf and invalidates locally; the
aperture closes before AP startup. DMA is coherent, identity-addressed, below
4 GiB, and uses no IOMMU. The one-disk polling IDENTIFY/READ DMA EXT diagnostic
retains DMA storage for boot lifetime. User-space AHCI/filesystem services and
DMA isolation have separate [driver contracts](../Architecture/IommuDomain.md).

Recoverable operations return typed status. Pre-exit failures may return an EFI
error; post-exit failures retain bounded diagnostics and stop. Exceptions capture
integer state and CR2. Supported user faults reach their supervisor for resolution
or termination. Watchdog and power-transition paths are implemented; persistent
crash records and broader recovery policy are unsupported. See
[watchdog](../Architecture/Watchdog.md) and [power transitions](../Architecture/PowerOff.md).

## Emulator and validation

| Item | Configuration |
| --- | --- |
| Emulator | `/usr/bin/qemu-system-x86_64`, QEMU 10.2.1 |
| Machine / acceleration | `q35` / TCG |
| Firmware code | `/usr/share/OVMF/OVMF_CODE_4M.fd` |
| Variable template | `/usr/share/OVMF/OVMF_VARS_4M.fd` |
| VM matrix | 1-4 CPUs, SATA, and four-CPU forced relocation; 512 MiB per VM |

Tests use a temporary variable-store copy, VirtIO boot, no default ATAPI devices,
Unix VNC sockets, and retained logs/memory evidence. The optional 2 MiB SATA disk
provides the LBA-8 test sector. The kernel reports each completed boot step as
a neutral `ASTERIA:STAGE:<NAME>` event and stores what its diagnostic scenarios
observe (runtime pool and roots, PCI, AHCI, each AP's timer, delivery and
service, the shared-root shootdown, the WaitSet fixture's resumed waits, native
preemption) in the evidence record `Amd64EvidenceRecord`, whose address it
reports once as `ASTERIA:EVIDENCE:0x<address>`; unexpected STOP and firmware FAIL/ERROR/FAULT lines remain
the only kernel verdicts. The VM runner (`Tools/Vm/RunVmTests.py`) checks the
stage order, judges the record against the case, QEMU and the build's
constants, and owns the stopped-state oracles; the [README](../../README.md#firmware-and-vm-tests)
lists commands and environment overrides. Native-preemption testing repeats all
six AMD64 cases on the preemption-probe presets.

### Outside the runner

The UEFI application accesses its boot medium only prior to ExitBootServices
and can boot from any medium supported by the platform firmware.
`Tools/Build/MakeBootMedia.py --esp Build/CMake/amd64/FirmwareTest/Esp --output <directory> [--vmdk] [--iso]`
uses host-side tools to write a raw GPT disk with a single FAT32 ESP (for USB
media or virtual disks), and can optionally generate VMDK and El Torito ISO
images. Automated test scenarios assume the runner's specific QEMU setup;
on other platforms, skipped or failing test cases terminate the root service
while the kernel remains running.
