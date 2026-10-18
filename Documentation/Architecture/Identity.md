<!-- SPDX-License-Identifier: Apache-2.0 -->

# Account and authentication libraries

The account and authentication libraries own no kernel objects, filesystems, UI,
devices or service protocol. Boot-file CRC checks do not authenticate executable
provenance or writable account policy, and do not prevent rollback.

`Source/User/Identity` supplies a recovering `AccountStore`, an independent
`FormatCodec`, a `PrincipalContext` builder, a caller-owned `AttemptLimiter`
and `Authentication` using the existing Crypto implementation. All methods
are bounded, noexcept and allocation-free. Instances, codec snapshots and
password/Crypto workspaces belong in static storage or an injected arena;
callers serialize access and callbacks must not reenter a store, authenticator or limiter.
Account queries expose labels, IDs, roles, revisions and disable state, never
salts or tags. Low-level codec snapshots are credential-bearing private storage
for provisioning/tests, not management or IPC responses. The service must
perform actor/ACL checks before invoking mutations. Session and
operation identifiers are trusted owner inputs, never client credentials; a
nonzero operation may enable Administrators only after root confirmation. The
owner binds contexts to live sessions/operations and checks revisions before
publication. All inputs/outputs are disjoint from the store, limiter and private
authentication scratch; low-level codec buffers must be disjoint as well.

The version-1 disk format uses two 32,768-byte slots,
28,736-byte bodies, `ASTID001`, 64 × 160-byte account records, 64 × 96-byte
groups and 768 × 16-byte membership pairs; the `ASTIDC01` seal starts at
32,736. All integers are little-endian. Unused records, tails, reserved fields
and the gap are zero. Record placement is stable slot order, with holes allowed;
counts count occupied records. Names are 1–63 exact UTF-8 bytes, rejecting NUL,
ASCII controls (including DEL), slash and backslash. Built-ins, disjoint IDs,
references, duplicate names/pairs, twelve memberships and the persistent next-ID
high watermark are validated before decoding publishes any output.

Injected Storage uses the SettingsDatabase exact-range I/O and ordered durable
Flush contract. Explicit Format requires all 65,536 bytes to be zero; Open never
formats. Commit invalidates the inactive seal/Flush, writes the complete body
and zero gap/Flush, then writes the seal/Flush. It never writes the active slot.
Any write/flush failure closes and wipes the instance; reopen resolves an
uncertain commit. Highest valid generation wins; equal valid generations fail.
Mutations update every affected account revision and generation atomically;
IDs, revisions and generations stop at exhaustion. Provisioning readiness
requires an enabled administrator with a profile-1 password; no ready flag or
extra persisted fields are invented. Verification refuses an unprovisioned
store before admitting an attempt; it cannot issue a principal during recovery.

Enrollment copies 1–1,024 counted UTF-8 bytes without NUL into private storage,
gets a fresh salt through injected entropy, and hashes only with
Crypto::LoginArgon2Defaults / LoginArgon2SaltBytes / LoginArgon2TagBytes.
Verification copies credentials privately and uses Crypto::Equal over the full
tag. Unknown and disabled accounts execute the same full profile dummy hash
and fail. The exclusive caller-supplied arena must contain all 65,536 blocks;
insufficient or overlapping storage is refused before accepting a secret.
Input snapshots, copied credentials, tags, Crypto workspace and the full used
arena are observably wiped after terminal attempts. Caller inputs remain owned
by the caller and must be wiped there. Cancellation is sampled before/after the
bounded synchronous hash; cancellation after admission counts as failure and
suppresses success. Failure preserves caller result and enrollment outputs.

Password encoding validation scans the complete counted input with arithmetic
masks and public indices, avoiding secret-conditioned early exits. Argon2id's
RFC 9106 §3.4.1.3 data-dependent reference reads retain Crypto's sole timing
exception; this adds none. Specifications: *Argon2 Memory-Hard Function for Password Hashing and
Proof-of-Work Applications*, RFC 9106, September 2021, v1.3, §§3–4, §5.3
and §7.4 ([specification](https://www.rfc-editor.org/rfc/rfc9106.txt));
*The Unicode Standard*, version 16.0, §3.9,
Table 3-7 (the project's UTF-8 rules). No external implementation is used.

The limiter uses injected monotonic nanoseconds, so truncating completion time
to whole seconds cannot shorten any delay. State is explicitly owned
outside the authenticator: 64 stable account buckets plus one shared unknown
bucket, global deadline, accepted serial/account and monotonic high watermark.
Account buckets are never evicted to reset failures. Inactive failures expire
at exactly 900 seconds; delays are 1/2/4/8, 30 for failures 5–9 and 300 from
10 onward. Every completed attempt sets a global one-second deadline. Retry
refusals do not extend timers. Saturating failure counters never wrap; serial
and unrepresentable deadline exhaustion block admission. Unavailable/backward
time blocks admission; an accepted attempt remains recorded until terminal
checkpoint/restart recovery can account for it. Restart recovery converts an
accepted attempt into failure, without restoring a principal-bearing outcome.

The pure library interfaces still own no kernel objects. The integrating
owners below use those same libraries and keep their transport buffers in
static storage. ACL evaluation, principal-bound filesystem/settings views,
CM handover, product provisioning and durable account writes remain later
steps.

## Trusted seat wiring

`AuthService` and `LoginService` are distinct root-started system modules on
both ISAs. `InitializeAuth`/`AdvanceAuth` and
`InitializeLogin`/`AdvanceLogin` expose the same bounded steps to injected
host transports and the target loops. Each target incarnation has one static
owner and a 512-byte individual stack-frame gate. Root's `ProductLogin`
owns `TrustedScreen`, the installed control faces, visibility, input lock,
account/session join and nonreused screen epochs. The compositor receives
neither credential Frames nor visibility/focus-lock authority.

The admission extension is a behavior-specific loader quota and runtime
allocation through the existing 24-byte all-RAM Frame request. There is no
64 MiB ELF segment: auth allocates 32 contiguous 512-page runs and maps them
in two 8,192-page MemoryObjects at its fixed private arena address. Ordinary
page-table arenas admit this layout; no new large-arena slot or reduced
profile is requested. RFC 9106, September 2021, v1.3, §4 item 2 remains
64 MiB, three passes, four lanes, a 16-byte salt and a 32-byte tag.

| Resident owner | Domains | Endpoint limit | Frame / MemoryObject limits | Page budget beyond writable ELF and stack/start block |
| --- | ---: | ---: | ---: | --- |
| auth | 1 | 3 | 36 / 8 | 16,384 private arena + 16 fixture-tail pages |
| login | 1 | 1 | 6 / 6 | 250 public pixel pages + 1 secret submission page |
| root seat support | existing root | 4 private report/reply/attention Endpoints | 1 retained limiter Frame; temporary build MemoryObject | 1 nonsecret limiter page |
| root display failure cover | existing root | none | 1 MMIO Frame / 1 MemoryObject | no RAM pages; up to the broker's 8,192-page uncached scanout bound |
| display broker addition | existing broker | 4 | 1 borrowed pixel Frame + 1 read-only MemoryObject | login's 250 pages remain charged to login |
| Input Broker addition | existing broker | main + up to 6 sources + 3 clients | unchanged | root console, login and desktop clients; no extra domain |
| watchdog addition | existing root | 2 additional private reply Endpoints | unchanged | auth/login use the existing 60-second readiness gate |

The minimal PS/2 desktop uses seven seat/service domains, with hardware domains
admitted separately. The 16-domain and 48-Endpoint pool limits apply unchanged.
Admission gates derive committed/resident page budgets from linked writable
segments, stack/start blocks and private runtime pages. Shared executable and
read-only segments remain root-owned and are not charged twice.

Four version-1, counted, canonical faces in `SeatFrame` bound every message
to 32 words and at most one attachment. Kernel delivery supplies sender,
sealed selector and authority; peer identities are root-installed. Auth's
Submit face borrows only Map on one secret page. Auth snapshots the counted
password into private storage, unmaps and closes the borrow, then acknowledges
Copied to login; login wipes its page before reporting that acknowledgement
to root. Only auth's root-installed result face carries a principal outcome.
Root waits for that wipe acknowledgement and an authenticated account-revision
check before consuming a current success.
Query replies retain their exact transaction and bytes on WouldBlock and close
borrowed reply Endpoints after delivery. Malformed messages dispose every
in-bounds transferred attachment within the fixed frame bound.
Auth discards a retained outcome when its transaction is cancelled or replaced.
Only a non-principal cancellation/cleanup receipt may still reach root for a
retired screen; it cannot change the seat state.

Root retains the limiter Frame outside RD_auth and unmaps its own build
window before granting auth Map|Write. Auth publishes accepted and terminal
nonsecret state in that Frame; a new incarnation calls `AttemptLimiter::Recover`
before Ready. Restarts preserve failure accounting; reboot starts fresh.
Root never maps the password-submission Frame and installs no per-session
store handle. For this explicitly synthetic fixture, the existing boot-module
transport copies opaque account-store bytes (including stored salts/tags)
through root's temporary loader window. Root does not parse them; auth alone
opens the read-only store. Product credential-store isolation still requires
an auth-only filesystem view.

The broker creates a separate sealed trusted drawing Endpoint. Only root
receives Manage|Invoke|Duplicate; login receives Invoke. Enroll accepts a
strictly newer screen epoch and retires the prior pixel mapping. Draw accepts
only the enrolled kernel sender at the current epoch and a newer serial.
Show/Hide require root Manage. The opaque seat-sized `TrustedPlane` sits above
ordinary session, console and cursor planes. ConsoleWrite and pointer snapshots
are excluded during its visible interval. It samples only the fixed public
640×400 bitmap, mapped read-only. The broker acknowledges reuse after the
render pass. Root keeps focus at zero and locked until that screen's fence;
login accepts editing only after the fence and auth transaction installation.
Root also retains zero locked focus through Hide; the new epoch's draw fence
proves trusted pixels have been replaced before session focus is restored.

`LoginRenderer` has public presentation fields only: state, editing status,
account label, mask count, field selection and remaining retry seconds.
Root rounds the public delay up and updates it on second boundaries; only
the exact monotonic deadline admits another attempt. Built-in Bold8x16, palette,
cursor and protected `Data/Strings/Login.en-US.inc` are compiled into it.
Each registered keyboard retains US/JIS selection; F2 changes its layout and
clears an incomplete password. Character input, names and passwords remain
UTF-8, with whole-codepoint backspace and the existing CP437 glyph lookup
confined to rendering.
It opens no toolkit, theme, font or cursor file. SecretEntry and the whole
submission page wipe on transitions, cancellation and normal/error exits.
Hash polling occurs at RFC 9106 §3.4's public segment boundaries, draining one
control delivery and one pending answer; it never runs in root and never
shortens the fixed hash. Terminal cleanup checks workspace, private password
and all arena words for zero and reports only a boolean plus public timing.

StartingSession calls the root-owned product desktop.
A failed desktop startup returns to Login only after `SeatStop` has settled
borrowed pixels, drained the partial domain and released its pools and faces.
The new epoch clears the provisional account/revision/session; a failed drain
or trusted-service failure instead remains in Recovery.
Locked preserves that session but hides its pixels and excludes seat input;
only its account can unlock it. Repeated SAS refreshes Locked. F12 there
requests logout; detach, thread/domain drain and root pool release precede
AwaitingAttention. ElevationPrompt is reserved. **This interim desktop has no
principal-bound views, CM ownership or privacy guarantee.** Authentication alone does not provide them.

The synthetic receipt permits an Invoke-only console probe, attempted
Invoke-only focus/visibility changes and an unissued draw epoch, followed by
pixel comparison. F9/F10/F11 on that explicit test UI ask root to terminate
auth/login/display respectively; the resulting seat must remain locked.
Those hooks are absent from the product receipt. They are failure evidence,
not recovery, provisioning or an elevation interface.
When the display owner dies, root first locks input and drains that owner,
then maps its retained scanout Region to apply a fixed opaque background.
This removes even a frozen old desktop frame. No renderer, credential or
user-owned pixel buffer enters this emergency cover. It remains until reboot;
failure to admit that cover is reported and input stays locked.

## Integration limits

Principal-bound views, Component Manager handover, product provisioning and
durable account writes remain unimplemented. Password recovery, entropy
credit/reseed policy and throttling across reboot require service policy.
