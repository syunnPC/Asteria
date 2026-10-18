<!-- SPDX-License-Identifier: Apache-2.0 -->

# User time-zone library

## Purpose and specifications

This library reads compiled IANA zone data and uses UTF-8 native strings. This is a pure library;
RTC configuration, clock synchronization and component integration remain outside
it. No database source text or third-party implementation is included.

Specifications: *The Time Zone Information Format (TZif)*, RFC 8536,
February 2019, §§3.1–3.3.1 (layout, validation, footer and version 3 extensions),
§4 (bounds checks) and §5.1 (unspecified truncated tails); *The Open Group Base
Specifications*, Issue 7, 2018 edition, §8.3, TZ (POSIX rule grammar);
*Date and time — Representations for information interchange — Part 1: Basic
rules*, ISO 8601-1:2019, §§4.2.1–4.2.2 (Gregorian/week calendars), 5.2.2,
5.2.4, 5.3.3–5.3.4 and 5.4.2 (extended calendar/week dates, UTC/offsets and
complete date/time representations). The section inventory is available in the
[BSI/ANSI preview](https://webstore.ansi.org/preview-pages/BSI/preview_30427329.pdf).
The [RFC](https://www.rfc-editor.org/rfc/rfc8536.html) and
[POSIX TZ specification](https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap08.html)
are the binary/rule references.
Calendar calculations derive directly from the Gregorian 400-year cycle and
ISO's Monday-based week containing January 4.

## API and ownership

`User/Time/Civil.hpp` provides validated civil dates, Unix-second conversion,
day addition, Monday=1 weekday and ISO week dates. Astronomical year numbering
(year zero), proleptic Gregorian dates, and years -999999..999999 are supported.
Seconds are POSIX seconds from 1970-01-01; civil seconds are 0..59.
`FormatIsoDate` and `FormatIsoWeekDate` write extended calendar/week dates.
`FormatIso8601` writes YYYY-MM-DDTHH:MM:SSZ or a numeric offset into a caller
buffer, including a terminator. Formatting supports years 0000..9999 and offsets
below 24 hours. Second-granularity offsets use an explicitly documented
`:ss` extension to the ISO offset representation; callers requiring strict ISO
8601 must use minute-aligned offsets. Failure leaves output untouched.

`User/Time/TimeZone.hpp` validates TZif v1, v2 and v3 and retains an immutable
borrowed byte view. Input must remain alive and unchanged until the zone is no
longer used. Failed loading invalidates the destination. Local results include
the offset, DST flag and a borrowed UTF-8 designation view. Leap records are
validated but ignored: transition numbers and input seconds are interpreted on
the POSIX timeline without applying leap corrections or emitting second 60.
`FromLocal` returns Gap, Ok (unique), Ambiguous, Unspecified or Invalid
(including civil dates beyond the supported year range), with the number of distinct UTC matches and the earliest/latest match; it never
chooses a fold or normalizes a gap silently. Unknown candidate coverage makes
the result Unspecified even if another candidate matches.

## Data formats and bounds

No new persisted format. TZif integers are decoded bytewise, big-endian two's
complement, without aligned loads. Both blocks are checked, including transition
ordering/indices, type records, UTF-8 designations and termination, leap spacing
and corrections, and standard/UT indicator relationships. Counts are bounded
before offsets are formed: file <=1 MiB, transitions <=65536, types <=256,
designation bytes <=65536, leaps <=4096, footer <=1024 bytes. Reserved header
bytes are ignored for forward compatibility. The two version fields must agree;
trailing bytes and malformed or inconsistent nonempty footers are rejected.
A type 0 fixed zone applies everywhere if no table or footer exists. With a
table and empty/missing footer, times on/after its last transition are unspecified,
as RFC 8536 §3.2 states. A nonempty footer applies from that final transition.
DST footer rules require explicit start/end rules: no host/environment default
is consulted. Jn, n and Mm.w.d forms, offset seconds, southern seasons, negative
DST, and signed/out-of-day v3 transition hours are supported.

No allocation, exceptions, RTTI, global mutable state, recursion or I/O. Parsing
is bounded by the above counts; lookup uses binary search. Inverse lookup tries
at most 258 offsets and deduplicates them with bounded scans; recurring rule
lookup examines four adjacent rule years (v3 spillover is at most seven days).
Objects and per-function target stack frames fit the 512-byte target budget;
large test buffers use static storage.

## Unsupported functionality

Database acquisition, zone-name/path lookup, compiling IANA text, a compiler tool,
localization, TZif v4, leap-aware time scales, fractional seconds, ISO parsing,
month/year arithmetic with clamping, and clock/RTC/NTP policy are unsupported.

