<!-- SPDX-License-Identifier: Apache-2.0 -->

# Asteria

Asteria is an experimental C++23 capability-based microkernel targeting IA-64
and AMD64. System policy, device drivers, and core
services run in separate user-space protection domains; explicitly trusted
driver profiles are also supported.

The kernel boots via UEFI, brings up to four CPUs online, and loads initial user
ELF images from the boot volume. It exercises synchronous IPC, timers, TLB
invalidation, and PCI/AHCI storage.

- [Implementation status and limits](Documentation/ImplementationStatus.md)
- [Architecture and target documentation](Documentation/Readme.md)

## License

Project-authored code and documentation are licensed under [Apache-2.0](LICENSE).
Files carry an SPDX identifier or, for formats without comments such as binary
fixtures and preset JSON, an adjacent `.license` file. The NetBSD bold8x16
bitmap, Vegur fonts, and IETF test vectors retain their original terms and attribution;
see [source provenance](Documentation/Provenance.md),
[font provenance](Data/Fonts/Vegur/NOTICE.md), and [license references](LICENSES/).

## Verification

Build with CMake 3.28 or later, Ninja, and a C++23 GCC toolchain on Linux.
Each preset configures in `Build/CMake/<preset>`.

```sh
cmake --workflow --preset host-debug # ASan/UBSan tests, header and stack checks
cmake --workflow --preset host-release # optimized tests with LTO
cmake --workflow --preset host-thread # ThreadSanitizer
cmake --workflow --preset host-analyze # GCC -fanalyzer over every source, tool, and test
cmake --workflow --preset ia64 # IA-64 firmware, O0/O2 compile checks, target verification
cmake --workflow --preset amd64 # AMD64 firmware, O0/O2 compile checks, target verification
```

Target presets build production firmware in `Firmware/` and guest-test firmware
in `FirmwareTest/` (with `Tests/Guest` as the initial user image). The base
`ia64` and `amd64` presets also compile every unit and relocatable closure at
`-O0` and `-O2`; diagnostic variants can disable these compile checks.
`TargetVerification` validates ELF/PE structures, symbol closure, assembly
properties, and disassembly goldens. Each property check verifies that mutants
are rejected on every run.
See [Tools/Verify](Tools/Verify/README.md).

Hosted builds discover `*Tests.cpp` under `Tests/`, excluding `Tests/Guest`;
compile-only ABI tests are built without CTest registration.
`ctest --preset host-debug -R <name>` runs a specific test subset;
`ASTERIA_TEST_SEED=<n>` replays a randomized model test seed. Shared testing
support resides in `Tests/Support`, while parser and codec fuzz harnesses are
in `Tests/Fuzz`.

Dedicated presets support VM-test variants: `ia64-relocated`,
`amd64-relocated` (4 GiB image base), `ia64-synthetic-probe`,
`ia64-preemption-probe`, `amd64-preemption-probe`, and
`amd64-preemption-probe-relocated`. Target builds are reproducible with the same
sources, toolchain and configuration.

Compiler selection:

- **Hosted builds**: Uses `CXX` on a fresh build tree or `CMAKE_CXX_COMPILER`.
- **Cross builds**: Discovers `ia64-linux-gnu-*` or `x86_64-elf-*` on `PATH`,
  overridable via `IA64_CXX`, `AMD64_CXX`, and corresponding `IA64_*` / `AMD64_*`
  binutils variables. `HOST_CXX` selects the host compiler for build tools.
  CMake caches resolved toolchain paths to prevent accidental mixing if `PATH` changes.
- **C23 header checks**: Uses `IA64_CC` / `AMD64_CC` or cross-compilers on `PATH`
  supporting `-std=c23`.
- **Coverage and fuzzing**: Uses `clang++` found on `PATH`, overridable via
  `-DCMAKE_CXX_COMPILER=<compiler>`.

Validated toolchain versions and environment assumptions are detailed in the
[target profiles](Documentation/Readme.md#target-profiles).

Place local workflow overrides in `CMakeUserPresets.json` (ignored by git).
Formatting rules are defined by `.clang-format` and `.editorconfig`
(`.clang-format-ignore` excludes assembly files).
`python3 Tools/Build/CheckStyle.py` validates whitespace, trailing newlines,
and SPDX coverage as part of the `SourceStyle` CTest suite.
Naming conventions, ownership models, ABI invariants, and hardware contracts
require manual review.

Optional external ACPI tests read system tables only when explicitly configured:

| Variable | External input |
| --- | --- |
| `ASTERIA_QEMU_ACPI_DIRECTORY` | QEMU's `tests/data/acpi/x86` directory |
| `ASTERIA_LARGE_REGISTER_DMAR` | A DMAR table exercising large register addresses |

Unconfigured external test cases are skipped; if an external input is configured
but missing or invalid, the test fails. Hardware captures and third-party tables
remain strictly outside the repository.

### Fuzzing, coverage and mutation

```sh
cmake --workflow --preset host-fuzz # Clang 21: libFuzzer, hosted ASan/UBSan tests
F=MessageCodecFuzz # any Tests/Fuzz/*Fuzz.cpp harness
mkdir -p Build/Fuzz/Corpus/$F
Build/CMake/host-fuzz/Tests/Fuzz/$F -max_total_time=600 -artifact_prefix=Build/Fuzz/ \
    Build/Fuzz/Corpus/$F Build/CMake/host-fuzz/Tests/Fuzz/Seeds/$F
Build/CMake/host-fuzz/Tests/Fuzz/$F Build/Fuzz/crash-<hash> # replay one input
cmake --workflow --preset host-coverage # Clang source-based coverage of every test
Tools/Coverage/Report.py --prefix Source/Kernel --sort uncovered --top 20 --html
Tools/Mutation/Mutate.py --source Source/Kernel/Ipc/MessageCodec.cpp \
    --test Kernel/Ipc/MessageCodecTests --test Fuzz/MessageCodecFuzz --json Build/Mutation/MessageCodec.json
```

## Firmware and VM tests

VM tests require Python 3, QEMU and OVMF for AMD64, the project's IA-64
QEMU/firmware environment, and `xorriso` for IA-64 disk fixtures.
QEMU binaries are discovered via `PATH`. `IA64_FIRMWARE` must be specified
explicitly, whereas AMD64 automatically detects an installed 4 MiB OVMF
code/variables pair. If the BIOS directory is omitted, QEMU's default search
path is used.

Override paths using `IA64_QEMU`, `IA64_FIRMWARE`, `IA64_BIOS_DIRECTORY`,
`AMD64_QEMU`, `AMD64_FIRMWARE_CODE`, and `AMD64_FIRMWARE_VARIABLES`.
The `Ia64Vtd` suite exercises the VT-d fork via `IA64_VTD_QEMU`, `IA64_VTD_FIRMWARE`,
and `IA64_VTD_BIOS_DIRECTORY`.

```sh
Tools/Vm/RunVmTests.py # the All suite
Tools/Vm/RunVmTests.py --suite Ia64 --jobs 4
Tools/Vm/RunVmTests.py --case 'Amd64.Smp*' --junit Build/Vm/Junit.xml
Tools/Vm/RunVmTests.py --list # cases, presets and drivers
```

Unless `--no-build` is specified, the runner automatically builds the selected
preset. Each VM runs in an isolated environment with its own ESP, QMP/diagnostic
sockets, and `Build/Vm/<run>/<case>/` output directory to support concurrent
headless execution. Guest tests report results via TAP, while host observers
monitor boot stages, register states, address translations, memory allocation,
and entropy evidence. Production smoke tests verify stable idle-loop execution.
Logs and screenshots are saved in each case directory; the runner exits with code
zero only if all selected test cases pass.

| Suite | Scope |
| --- | --- |
| `All` (default) | Both ISAs: 1–4 CPUs, storage formats, relocation, fault channels, desktop/session cases, and production smoke tests |
| `Ia64`, `Amd64` | One ISA's integration cases from `All` |
| `Ia64Ahci` | IA-64 diskless, AHCI, FAT12/16/32 and exFAT cases |
| `Ia64I2000` | HP i2000 (Merced, 460GX) production boot to the root service on 1–2 CPUs; diagnostics on SPCR COM1 |
| `Ia64SyntheticProbe` | Dedicated user syscall, RSE, timer, and WaitSet diagnostic test |
| `Ia64NativePreemption` | One-shot IA-64 BSP preemption test |
| `Ia64Vtd` | `itanium2-vpc-iommu`: confined `edu`, AHCI/filesystem service, ECAP.C set, registers above 2^48, and no-IOMMU case |
| `Amd64NativePreemption` | Six AMD64 test cases with one-shot BSP preemption enabled |
| `Amd64Trusted` | q35 trusted AHCI/e1000e/xHCI in production and test firmware without an IOMMU; absent and unused entries behind VT-d |
| `Ia64Trusted` | itanium2-vpc trusted AHCI/filesystem service without virtio-iommu; unused entry behind virtio-iommu |
| `Amd64UsbHid` | q35 ICH9 EHCI/UHCI production boot: keyboard/mouse split across controllers or sharing either; VT-d, AMD-Vi, and trusted profiles |
| `Ia64UsbHid` | itanium2-vpc HID behind virtio-iommu: NEC EHCI/OHCI and ICH9 EHCI/UHCI companion pairs |

The table summarizes selected suites; `--list` gives the current case inventory.
Dedicated diagnostics are separate from the `All` suite. Hardware
assumptions and validation limits are defined in the
[boot contract](Documentation/Architecture/BootContract.md).

## Source layout

| Directory | Contents |
| --- | --- |
| `Source/Kernel` | Architecture-independent objects, IPC, scheduling, memory, and devices |
| `Source/Platform` | ISA and platform firmware adapters |
| `Source/Boot` | UEFI bootstrap and normalized handoff protocol |
| `Source/Executive` | User-space service core implementations |
| `Source/User` | User runtime libraries, syscall clients, and initial user images |
| `Source/Foundation`, `Source/Hal`, `Source/Runtime` | Fundamental types, hardware contracts, and freestanding runtime support |
| `Tests`, `Tools` | Verification suites, test fixtures, and build tools |
| `Documentation` | Architecture contracts, target profiles, and implementation status |
| `Build` | Generated build artifacts (not source-controlled) |
