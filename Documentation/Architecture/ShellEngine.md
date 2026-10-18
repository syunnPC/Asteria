<!-- SPDX-License-Identifier: Apache-2.0 -->

# Shell language engine

## Purpose and authority

`Source/User/Shell` is a reusable language core. It implements
shell syntax from IEEE Std 1003.1-2024, XCU chapter 2, “Shell Command Language”
(especially §§2.2–2.10 and 2.14–2.15). Utility behavior
uses the same edition's utility descriptions.

The [boot pager](BootPager.md) supplies a terminal Endpoint for application
integration. This library accepts complete byte strings; it owns no
Endpoint, capability, process, terminal, kernel code, or boot component.

## API and formats

`Parse(Text, Program&)` tokenizes and builds an indexed AST in caller-owned
storage. Tokens retain source spelling and here-document spans. Outer unquoted line
continuations get canonical word spelling in a second bounded source area;
original spans remain available for diagnostics and function bodies. Node zero
means no node; node links and word/redirection chains refer to arrays in the
same Program. A failed parse is diagnostic-only and must never be executed.
Programs are transient memory formats, never disk formats.

`Engine` owns bounded variable, positional-parameter, function and scratch
storage. It is serialized by its caller. Keep Engine and Program in static
storage or an owner-provided arena, never on the target thread stack.
The hosted 64-bit layout occupies 4501456 bytes for Engine and
41088 bytes for Program; these are implementation measurements, not ABI
promises. Independent child engines need independent storage. Most of the
engine allocation is bounded per-depth scratch, reserved without heap use.
`Run` parses and executes source; `Execute` executes a successfully parsed
Program. Expansion retains per-byte quote and expansion provenance through
field splitting and pattern expansion; syntax introduced by an expansion is
never reparsed (except the explicit eval and dot utilities).

Filesystem callbacks enumerate directory basenames, obtain user homes, inspect attributes,
change/get working directories, and read scripts. Enumeration excludes `.` and
`..`. Pathname expansion passes literal dot/parent components to Inspect and
later callbacks as directory traversal, preserving their spelling and symlink
resolution; it does not normalize them away. ProcessHost callbacks launch
commands, write/read descriptors, bracket ordered redirections, and execute
isolated ASTs or capture command substitutions. Resolve receives shell state
for PATH-based searches; IsTerminal queries the host's descriptor table.
Inspect supplies followed attributes, file identity and modification time,
plus the final entry's symlink flag without following it. CDPATH lookup and
test comparisons stay in the core. Pipeline, subshell and
background requests carry an AST and the current engine; the host must clone
shell state into caller-owned storage, arrange descriptors, execute the child
AST with Execute, wait where required, and return the last pipeline status.
Background execution reports its process identifier. The pipeline callback
honors the current pipefail flag; child execution inherits the calling
conditional context for errexit. Unassigned export declarations have NoValue
set and are excluded from the external environment. Host callbacks are
synchronous except ownership transfer explicitly implemented by that host;
all borrowed strings expire when a callback returns. Captured output excludes
trailing newlines in the core. Export flags accompany external environments.
Descriptor writes carry stream bytes: printf can emit NUL and arbitrary
byte escapes. These are not native API names or paths. Captured bytes must
be valid UTF-8 without NUL before becoming expansion text; transport may
split a UTF-8 sequence across writes, so the receiver maintains decoder state.

## Bounds and errors

Constants in Shell.hpp are the source of truth: 8192 input bytes, 512 tokens,
512 AST nodes and words, 128 redirections, 32 syntax/execution nesting levels,
8 nested scripts/expansions and child environments (including the root),
8192 bytes per expanded command, 64 fields and
variables, 64 bytes per name/here delimiter, 1024 bytes per variable/positional value, 16 functions and command
redirections, and 100000 execution steps per top-level call. Capacity errors,
invalid syntax/UTF-8, arithmetic faults and host failures are explicit Error
values with an offset. No allocation, exceptions, RTTI or global mutable
scratch is used. Callbacks must themselves enforce deadlines and bounds;
this library cannot preempt a blocking injected callback. Loop iterations and
pattern matching consume bounded work. Numeric operations avoid C++ signed
overflow. Individual target stack frames must fit 512 bytes; large temporary
arrays live in owner storage.
CopyStateFrom inherits the active child depth. This also bounds dynamically
generated eval/substitution cycles that create a fresh engine at each step.
A failing child returns through the host's boolean result as Error::Host;
the child retains its own precise diagnostic for the host to report.

## Unsupported functionality

Endpoint framing, launcher capability binding, interactive line editing,
job control, signals/traps, aliases, command history, executable image loading,
and the remaining POSIX utilities are not implemented by this library. Set supports
allexport, errexit, noglob, noexec, nounset, xtrace, noclobber and pipefail;
interactive monitoring, asynchronous notification and verbose input tracing
are outside this complete-string API. Optional
printf floating-point conversions are omitted (the printf utility
description explicitly permits this). The language
core does not claim a complete POSIX environment or certification. Resource
limits are intentional departures from POSIX's unlimited input-line model.

## Language policies

* The host supplies the shell's memory grant within the library's storage bounds.
* UTF-8 scalar matching and byte-order pathname sorting are deterministic;
  portable ASCII character classes are supported; locale collation, extended
  character classes and collating/equivalence elements are unsupported. Invalid UTF-8 and embedded NUL are rejected.
* Echo uses literal operands and a final newline; the implementation-defined
  `-n` and backslash cases are literal. Arithmetic uses 64-bit integers and
  modulo arithmetic for overflow; division by zero and invalid shifts fail.
* In assignment/redirection contexts, positional expansions join with the
  first IFS character (space when unset), including the unspecified quoted
  `@` case. Normal argument contexts preserve `"$@"` fields.
* Unspecified test argument shapes return status 2. If physical cd succeeds
  but the host cannot determine its pathname, PWD is unset; cd -P returns 0
  and cd -Pe returns 1. Logical path normalization coalesces leading slashes.
  Unknown tilde users retain literal spelling. The legacy single '-' set
  operand is treated as a positional value; the supported options are listed
  above.
* Missing services never cause implicit native syscalls or launcher authority
  escalation. Launch/write/capture/redirection failures produce Error::Host;
  lookup and utility probes can return ordinary failure or literal tilde
  spelling. The host owns isolation, descriptor lifetimes and cancellation.
