<!-- SPDX-License-Identifier: Apache-2.0 -->

# Shell library

`Shell.hpp` exposes `Asteria::User::Shell`.
[ShellEngine.md](../../../Documentation/Architecture/ShellEngine.md) defines
authority boundaries, memory bounds, host contracts, and resource ownership.

Keep `Engine` and optional `Program` in caller-owned storage off target stacks.
Bind the required `Filesystem` and `ProcessHost` interfaces,
then pass UTF-8 command input to `Run(Text)`. Check both `Result::Code` and
`Result::Status`: built-in command and utility failures return shell exit statuses,
whereas syntax errors, capacity exhaustion, and host operation failures return
library errors. Honor `Environment.ExitRequested` before accepting subsequent commands.

`Parse` produces tokens and ASTs; `Execute` validates spans and indices prior
to execution. `Expand` appends expanded words to caller-owned `Fields`, distinguishing
quoted bytes, literal input, and substitution output. `Set`, `Get`, and
`Unset` manage shell variables. Initialize positional parameters, process identity,
and the shell name in `Environment`, and mark imported variables as exported.

Isolated child processes instantiate a shell engine with dedicated descriptor
and filesystem contexts, clone environment state via `CopyStateFrom(parent)`, and
execute the AST or script source. In pipelines, a `Pipeline` node links commands
via `Left` and successive `Next` stages; subshell and background requests reference
child command nodes directly. The host's `BeginRedirects` hook applies ordered,
fully expanded redirections and returns an opaque restoration cookie; failure
must leave host state unchanged. After a successful begin, the engine invokes
`EndRedirects` on every execution outcome.
Here-document payload data is stored in the `Target` field.

Host callbacks and borrowed string views are bounded and synchronous. The host
coordinates concurrent child process scheduling, connects inter-process pipes
prior to executing command redirections, waits for foreground completion, and
preserves child isolation. Process execution and file descriptor operations
require an injected backend implementation.

`Tests/User/Shell` covers grammar, expansion, and execution.
`Tests/Fuzz/ShellParserFuzz.cpp` supplies grammar seeds and invariants for
deterministic mutations. Tests use no external shell oracle.
