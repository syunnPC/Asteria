<!-- SPDX-License-Identifier: Apache-2.0 -->

# Native runtime statuses

`Source/User/Runtime` provides libasteria status values. The central `Status.hpp`
contains the value and opaque `Facility` identity, with structural ADL constraints
instead of hosted type traits. Neither names a facility. A facility header owns
its UInt32 enum, constant, last-code bound and ADL validation/identity hooks. Its
source owns names and checks its C constants. `StatusRegistry.cpp` is never
included: the host test links its validation function. Native builds compile all
status and personality sources as unlinked user objects at O0 and O2 on both ISAs.

Success is zero. Failures have bit 31 set, a 7-bit facility at 24, and a nonzero
24-bit code. FromRaw preserves unknown facilities/codes, rejecting nonzero values
without the failure bit and failure encodings with code zero. Rejection returns
Result<Status, Status> with Generic InvalidArgument. TryAs validates identity and
known-code membership before casting, including sparse third-party enums. As has
the same checked-input precondition as Result::GetValue. There is no bool
conversion; hidden equality friends permit Status/enum comparisons while enums
of different facilities remain incomparable. Status is nodiscard; value-bearing
helpers return Foundation::Result<T, Status>. No allocator, blocking or hidden
initialization is involved.

## Facilities and code provenance

Facility numbers are fixed; additions append.
All listed codes are decimal and start at one.

| Facility | Number | Codes and current wire answers |
| --- | --- | --- |
| Generic | 0 | 1 Restarted (FsRestarted, SocketRestarted, BlockDriverRestarted); 2 Pending (SocketPending); 3 RefusedForCredit (FsRefused, SocketRefusedCredit, BlockRefused); 4 StaleObject (FsStaleObject); 5 StateLost, 6 RebindRequired, 7 CommitStatusUnknown (recovery vocabulary, no independent v0 wire kinds); 8 TooLate (BlockTooLate); 9 NotSupported (FsUnsupported); 10 InvalidArgument (invalid decoder/raw input); 11 OutOfResources, 12 AccessDenied (shared native preflight vocabulary, no dedicated v0 wire kinds) |
| Kernel | 1 | 1 UnknownSystemCall, 2 InvalidArguments, 3 InvalidConfiguration, 4 InvalidHandle, 5 ObjectNotFound, 6 ResourceExhausted, 7 SpaceExhausted, 8 RightsDenied, 9 RightsAmplification, 10 NoAttenuation, 11 TypeMismatch, 12 Revoked, 13 Expired, 14 LeaseExtension, 15 RevocationDomainExhausted, 16 WouldBlock, 17 UserMemoryFault, 18 TimedOut, 19 WriteValueDenied, 20 ReservedMemory, 21 DisplayReserved, 22 BridgeReserved: identity with SystemCallError |
| Memory | 2 | None: memory operations answer kernel errors |
| Time | 3 | None: WaitSetWait answers kernel TimedOut/WouldBlock and other kernel errors |
| Console | 4 | 1 Refused (DisplayRefused: malformed for broker state, not a credit refusal) |
| File | 5 | 1 NotFound (FsNotFound); 2 Corrupt (FsCorrupt); 3 IoFailed (FsFailed or BlockFailed); 4 Cancelled (BlockCancelled) |
| Datagram | 6 | 1 NoRule, 2 NotBound, 3 IdentityRefused, 4 Malformed (SocketRefused's closed details 2–5); 5 IoFailed (SocketFailed) |
| Ipc | 7 | None: endpoint/WaitSet operations answer kernel errors |
| Process | 8 | None: exit/termination answer kernel errors |

The extra generic outcomes have facility-independent meaning: unsupported
operations, malformed local arguments, exhausted local bounded resources, and
missing local authority apply across native operation groups. They do not erase
more precise kernel or protocol answers. Only filesystem names produce a service
not-found answer, represented by File NotFound; kernel object lookup retains
Kernel ObjectNotFound. File Cancelled represents cancellation of block I/O used
by the file group.

SystemCallError::Last aliases BridgeReserved and is pinned at 22 with zero, last-identity
and 24-bit bounds asserted in the ABI. Every mirrored value is asserted in
KernelStatus.cpp. FromKernelError validates the 64-bit word against the 24-bit code field
before narrowing; None explicitly yields success. New kernel codes need no
change to the encoder: old runtime enums still refuse unfamiliar codes through
TryAs, allowing the personality to return EIO even with an older ABI header. Named enum coverage and names/maps
are appended when the runtime learns the new outcome.

## Decoding and diagnostics

ReplyStatus.hpp/.cpp maps already decoded kind/detail words, without changing the
wire protocols. The closed SocketRefused reason selects a distinct outcome; open
failure details never do. FsFailed does not turn a diagnostic BlockDriverRestarted
number into Generic Restarted. FsRefused includes both excess credit and malformed
service state in its existing wire answer; the decoder cannot distinguish them.
The protocol's credit refusal vocabulary is retained.

FsDone, FsCommitted, FsNotCommitted, SocketDone, DisplayDone and BlockDone map to
success. DecodeCommitState returns Committed or NotCommitted as successful values;
other failure kinds propagate and an inappropriate FsDone is invalid input. The
volume generation remains a value in the frame. DecodedReply carries a 64-bit
DiagnosticDetail beside Outcome; DiagnosticExtra separately preserves SocketFailed's
second diagnostic word (including the kernel error when detail is zero). Successful
reply values remain in the decoded frame, not diagnostic storage. These pure mapping helpers provide no handle table or diagnostic call. Unknown kinds and closed refusal reasons
return Generic InvalidArgument without narrowing. Block restart remains Restarted:
its protocol cannot determine whether an issued write took effect, so callers must
retain that ambiguity rather than infer NotCommitted.

## Personality and C surface

Source/User/Posix owns ToErrno and Errno<Facility>.cpp, one translation source for
each facility. The native runtime never includes errno. Each mapping first TryAs,
then switches exhaustively without default. Unknown codes, unallocated facilities
and the four empty facilities return EIO. The numbers are the stable Asteria libc
ABI assignments in `Source/User/Libc/Include/errno.h`, independent of host values.
Their numeric assignments are project policy; POSIX requires the error names,
not these particular numbers.

Generic maps in code order to EINTR, EAGAIN, EAGAIN, ESTALE, EIO, ENOTCONN, EIO,
EALREADY, EOPNOTSUPP, EINVAL, ENOMEM, EACCES. Kernel maps to ENOSYS, EINVAL, EINVAL,
EBADF, ENOENT, ENOMEM, ENOSPC, EACCES, EACCES, EINVAL, EINVAL, ESTALE, ESTALE,
EINVAL, ENOSPC, EAGAIN, EFAULT, ETIMEDOUT. Console Refused is EINVAL. File maps to
ENOENT, EUCLEAN, EIO, ECANCELED. Datagram maps to EACCES, ENOTCONN, EACCES, EINVAL,
EIO. Success maps to zero. Translation never changes errno storage.

Runtime/Include/AsStatus.h defines uint32_t AsStatus and enum encoding constants.
Each facility's C23 header defines a fixed-underlying-type enum whose constants are
complete AsStatus encodings, asserted against Status{CppCode}.Raw() in that
facility's source. Empty groups use opaque fixed-width enum declarations (C has no
empty enum definition). Constants are enums, not macros; preprocessing uses only
include guards. The CTest HeaderCompile static check compiles headers independently
and together, tests their sizes and a direct AsStatus return, with the AMD64 and
IA-64 cross C drivers in freestanding C23 mode. CMake enables only C++/ASM.

Host tests cover all names and errno mappings, all actual reply kinds/refusal
reasons through frame writers/readers, diagnostic independence, commit values,
layout, raw boundary rejection, unknown codes, equality constraints, sparse vendor
extension, kernel identity and registry ranges/uniqueness. NativeStatusFuzz replays
mutations of raw values and arbitrary 64-bit reply words, checking round trips,
validated enum conversion and lossless, outcome-independent diagnostics.
