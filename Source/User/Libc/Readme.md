<!-- SPDX-License-Identifier: Apache-2.0 -->

# Application libc core

This OS-independent C library provides memory/string functions, C-locale ctype,
integer/float conversion, sorting/searching, deterministic rand, allocation,
assertions and string-output printf routines. It has no kernel, POSIX descriptor,
stream-I/O or CRT-startup dependency. `FILE` is incomplete.

## Public interface

Add `Source/User/Libc/Include` to application includes and select
`ASTERIA_TARGET_AMD64` or `ASTERIA_TARGET_IA64`. The ABI requires LP64, flat
addresses, little-endian representations, two's complement integers and IEEE
binary32/binary64.
Public C17 headers also support C++23 and reuse compiler freestanding types;
`inttypes.h` format widths follow the compiler's integer-fast types.
`ASTERIA_LIBC_NAME(name)` exports standard names on target and
`asteria_libc_name` on host. Hosted tests include headers by repository path
and compare with glibc without interposing it. Host errno uses
`ASTERIA_LIBC_ERRNO`; target errno uses `(*__asteria_errno_location())`.

Buffers follow C/POSIX size, lifetime and overlap rules. `strerror` storage is
read-only to callers. Returned search/token pointers and end pointers may be
modified only when the original object is mutable. Comparators must not throw
across the C boundary. Calls are unsuitable for ISRs: hooks may block/allocate
and parsing, formatting and sorting have input-dependent execution time.

## Personality hooks

Install `LibcHooks.hpp`'s `Hooks` once, before concurrent use. Incomplete callback
pairs are rejected without changing configuration. Hooks and contexts must
remain valid for the process lifetime and must not throw or reenter libc.

| Hook | Contract and default |
| --- | --- |
| `Pages.Reserve` | Contiguous writable 4096-byte pages, aligned to 4096; absent/failure gives ENOMEM. |
| `Pages.Release` | True releases exactly the reservation; false leaves it mapped for reuse. |
| `HeapLock` | Paired lock/unlock around allocator/provider calls and rand state; default no-ops. |
| `ConversionLock` | Separate paired lock/unlock around conversion scratch and sinks; default no-ops. |
| `ErrnoLocation()` | Calling thread's valid errno storage; default a global integer. |
| `AssertFail` | Must terminate; absent/returning hooks fall through to a compiler trap. |

Provider/sink callbacks run under their corresponding lock. Errno/assert hooks
must work under either lock; libc never holds both locks at once.
Concurrent use requires thread-local errno and appropriate locks; the default
hooks support single-threaded use.

## Allocation and conversion contracts

The heap uses 64 segregated bins, splitting/coalescing within page reservations
and reclaiming empty regions. Failed releases remain reusable. Realloc can
shrink or extend in place; allocation failure preserves the original block.
Arithmetic is checked, but free-list scans have no constant-time guarantee.
Zero-size malloc is freeable; realloc(p, 0) frees and returns NULL. POSIX memalign
errors preserve errno/output. Allocation failures set ENOMEM; invalid
aligned_alloc inputs set EINVAL. Successful calls preserve errno.

Qsort is iterative heapsort with O(n log n) comparisons and O(1) scratch.
Rand uses `state = (1103515245 * state + 12345) mod 2^31`, initial seed 1 and
RAND_MAX 2147483647.

One `Format` engine serves sprintf/snprintf and va_list variants. Sinks receive
bytes, count and repetition count; failure must set their error. Snprintf
terminates nonzero-capacity output and returns the would-be count; capacity zero
permits NULL. Counts above INT_MAX return -1/EOVERFLOW. Standard flags,
width/precision, length modifiers, integer/string/pointer/`%n` and floating
conversions are supported. `%lc`/`%ls` accept ASCII in the fixed C locale;
other characters give EILSEQ. Invalid syntax gives EINVAL. Positional arguments,
GNU `%m`, locale grouping and locale changes are unsupported.

Binary64 formatting and strtod/strtof use exact integer arithmetic with
nearest/even rounding. Binary32 parses round directly. Incomplete syntax follows
C endptr rules; overflow and inexact tiny results set ERANGE, while exact
hexadecimal subnormals do not. `%L` accepts binary80/binary128 arguments but
formats after rounding to binary64. Long-double adapters use integer arithmetic
without libm or conversion runtime calls. Full long-double precision and fenv
are unsupported. Tininess is detected after rounding to normal
precision with an unbounded exponent, before rounding to the subnormal grid.

Conversion uses one constant-initialized workspace under `ConversionLock`,
containing three 8192-bit integers and a 1201-byte digit buffer. Parsing retains
1200 significant decimal digits or 300 hexadecimal digits and tracks discarded
nonzero digits with a sticky bit. It uses no recursion, heap allocation or
kernel calls.

## Verification

`Tests/User/Libc` contains glibc differential suites, randomized allocator
models, concurrent hook checks and C17/C++23 header checks. `ASTERIA_TEST_SEED`
replays a seed; failures report the input/operation. Fuzz harnesses run
20,000 deterministic mutations in hosted presets; `host-fuzz` enables libFuzzer.
AMD64/IA-64 builds check O0/O2 compilation and 512-byte function frames.
Libc is compiled separately with the application floating-point ABI and is not
linked into kernel images. Target builds use the compiler's freestanding C
headers under `-nostdinc -nostdinc++`, without C++ standard-library dependencies,
initialization guards, exceptions or RTTI. IA-64's `CompilerArithmetic` adapter
supplies `__divdi3`, `__moddi3`, `__udivdi3` and `__umoddi3` with 64-step restoring
division. AMD64 exports none of these helpers. The complete target libc closures
have no undefined symbols or compiler-runtime library dependencies.

Measure AMD64 section sizes and exported symbols with:

```sh
cmake --preset amd64 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
python3 Tools/Build/LibcFootprint.py --build Build/CMake/amd64 \
  --size x86_64-elf-size --nm x86_64-elf-nm
```

The tool recompiles application units under `-Os` in a temporary directory and
also audits external dependencies.
