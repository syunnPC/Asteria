<!-- SPDX-License-Identifier: Apache-2.0 -->

# Host Simulator Target Profile

## Purpose, Compiler, and ABI

This profile runs architecture-independent kernel models and boundary tests in
hosted processes. It uses GCC 15.2.0 in C++23 mode; Clang 21 supports fuzzing
and source-based coverage. GCC uses `-Wcast-align=strict`, Clang uses
`-Wcast-align`, and the static analyzer requires GCC.

The ABI is x86-64 System V, LP64, little-endian, with eight-bit bytes, signed
plain `char` and eight-byte pointers. Object pointers and `Foundation::UIntPointer`
use flat addresses; conversion preserves byte-address order within the same
allocated object. Signed integers use two's complement.

Kernel translation units use `-ffreestanding`; test translation units use the
host libraries. Exceptions, RTTI, guarded local-static initialization and
`__cxa_atexit` are disabled in both groups.

`Foundation/Compiler.hpp` defines the compiler adapters:

- `ASTERIA_CLEAR_PADDING` uses GCC `__builtin_clear_padding` before trivially
  copyable diagnostics are packed into atomic words. Readers reconstruct bytes
  after release publication. On Clang the adapter does nothing; diagnostic
  padding is unspecified and never interpreted.
- `ASTERIA_NOINLINE_NOIPA` uses GCC `noinline`/`noipa` or Clang `noinline`.
  Clang retains the call boundary without an interprocedural optimization barrier.

## Startup, Runtime, and Memory

Tests start at `main`, use the host C runtime and return nonzero on failure.
Kernel namespace-scope state is constant-initialized and data structures use
caller-owned storage with fixed capacities. Hosted placement construction uses
`<new>`; native `-nostdinc` builds supply placement-new declarations and definitions.

Test fixtures use C++ and Linux/POSIX libraries for reference models, threads,
file I/O, mappings and subprocess isolation. Host tools use these libraries as
well; neither dependency enters target images. Fixtures may allocate through
the host library. Allocation or thread-creation failure terminates the process
because exceptions are disabled.

## Concurrency, Interrupts, MMIO, and Failure

Concurrency tests use up to four racing workers plus the main thread. Hosted
kernel-context models can park up to 20 contexts on separate host threads,
passing control to one at a time. Shared kernel state uses the same lock-free
byte spin locks and 32-bit atomic reference counters as target builds.

Privileged instructions, hardware interrupts, MMIO and cache control are
validated by native builds and VM tests. Hosted tests exercise their models.
Recoverable kernel failures return typed results; test failures print diagnostics
and return nonzero. Hosted tests do not exercise native panic, watchdog or reset paths.

## Bounds and Verification

Capability spaces contain at most 256 entries; object registries borrow storage
for up to 16,384 entries. Revocation registries support 64 live domains and up to
eight parent edges, reusing destroyed slots. Synchronized operations have bounded
table work but may spin under contention and are not interrupt-safe.

The hosted stack probe enforces a 256 KiB per-function limit for large fixtures.
Target frame budgets and full call-chain bounds are checked separately.
Each public header compiles independently, warnings are errors, and
`Tools/Build/CheckSeparation.py` rejects test dependencies under `Source/`.
Native AMD64 context-switch tests use separate O0/O2 builds without sanitizers.

The [README](../../README.md#verification) lists sanitizer, optimization, analyzer,
fuzzing and coverage presets. Fuzz harnesses define `LLVMFuzzerTestOneInput` and
`AsteriaFuzzSeeds`. Outside `host-fuzz`, `Tests/Support/FuzzDriver.cpp` replays seeds
and deterministic mutations. Campaign corpora and crash artifacts belong under
`Build/`; reproducible inputs are kept as harness seeds.
