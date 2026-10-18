<!-- SPDX-License-Identifier: Apache-2.0 -->

# Desktop session libraries

The desktop session libraries live in
`Source/User/Session`, `Source/User/Terminal` and `Source/User/Input/Keymap`.
They have no kernel dependencies, heap, exceptions, RTTI, clocks, IPC transport or
component entry points. Owners keep the scene, event queues, manager, ordering
queues and terminal grids in static storage; calls use the existing target
512-byte stack budget. `Source/CMakeLists.txt` lists their target compilation
checks at O0 and O2 on AMD64 and IA-64; `Source/Hosted.cmake` includes them through
the same hosted source discovery as Graphics. The display module binds the face
codec; the compositor binds the scene, window manager and client events.

## Protocols

`SessionFrame.hpp/.cpp` implement all twenty methods and eighteen event kinds. `WriteSessionFrame`, `ReadSessionFrame`, `WriteSessionEvents`
and `GetSessionEvent` share field validation. Invalid encodes write nothing;
invalid decodes leave the destination untouched. They check canonical asynchronous
AIPC headers, exact lengths, attachment copy mode, types and rights, transaction,
padding, UTF-8 text, ids, flags, damage bounds and every per-Frame page count. Geometry
uses 64-bit arithmetic, including `SessionImageFits`: offset plus stride times
height times four must fit the actual pool byte count. The decoder can check the
maximum pool size; SessionCompositor calls that helper with the registered
pool size and checks epochs, format agreement and slot ownership.

The Events payload is 256 bytes: epoch, seven kind bytes and the count in
byte seven, more-pending flag, dropped-input count, and seven four-word events.
`SessionEventCount` reads the count. The client payloads use fixed word counts;
maximum Frame attachments keep a CreatePool at 280 bytes. SetSizeHints is five
words/128 bytes and SetSubtitle ten words/168 bytes. Methods 14–20 retain the stated
numbering; client ids pack slot and generation into one word. PoolReady is just
`{pool}`; pool Frames carry exactly Map.

`SessionFaceFrame.hpp/.cpp` implement broker messages, including the
Reply, CreateSurface, AddSurfaceFrames, Present, DestroySurface, SessionCredit,
OpenSessionFace, ShowSession, DropSession, DetachSession and PointerSnapshots.
`FaceFence` 17 is an Invoke request with
an epoch and reply, ordered after the stopped producer's queued requests and
answered Done before control Detach clears its credit/snapshots. Input v3 adds
Manage RebindClient 14 (including final retirement), Resume 15 and Baseline 16;
the two input codecs share these fields and keep the existing wire sizes.
`InputV3Frame.hpp/.cpp` implement the Input Broker v3 codec and stamped
pointer route. The existing broker identifiers remain; registration replies
advertise version 3. RegisterClient accepts the broker-pointer flag; SetFocus
accepts the focus lock; Events word three accepts the pointer horizon and the
route word seven the key stamp. `Wire.hpp` shares strict canonical AIPC
encoding for these codecs without building a full frame on the stack.

The codecs validate information on the wire. Session-face geometry must also be
checked against the current output at binding time. Reply kinds are Presented 3,
Busy 4 and Stale 5, following DisplayDone 1 and
DisplayRefused 2. An OpenSessionFace success transfers two Endpoints: the face
with Manage|Invoke|Duplicate and the control Endpoint with Invoke|Duplicate.

`Text/Utf8.hpp` validates shortest encodings of Unicode
scalars, rejecting stray continuations, truncation, overlongs, surrogates and values
above U+10FFFF. It follows **The Unicode Standard 16.0, section 3.9.3, table 3-7**
([specification](https://www.unicode.org/versions/Unicode16.0.0/core-spec/chapter-3/)).
Registration names/subtitles keep their 16/64-byte bounds, printable controls rule
and zero padding. Scene titles decode code points before glyph lookup and clipping;
the bitmap fallback counts code points; scalable UI layout uses shaped advances. Subtitle controls and U+2666 still render `?` to
preserve the trusted class marker. Other unsupported scalars render 0xFE.

## Scene and policy

`Palette.hpp` is the desktop and terminal colour source.
`Decorations` supplies frame/content/button rectangles, hit testing, sanitized
trusted titles, glyphs and clipped paints. Rings are four pixels, the title is
20 high, title buttons are 16x14 and corner resize zones extend 16 pixels.
Bottom/right bevels are painted last at shared corner pixels. `Scene` holds sixteen windows, their z-order,
borrowed read-only client images and the switcher/outline. A window's Present
damage is unioned before joining the global Graphics DamageList. First images
and changed image sizes damage all content. `Compose` paints
damage with CPU Graphics primitives and an opaque-window occlusion shortcut;
`RenderAll` supplies the naive reference. Smaller images leave Face around them;
larger ones are cropped. `Scene::Title` gives the switcher the same trusted name,
class marker, sanitized subtitle and not-responding suffix as the frame.

`WindowManager` owns window policy. Slot `client * 4 + surface - 1`
binds its policy to the scene and four `ClientEvents` owners. `Map` cascades from
(32,32) in steps of 24, restarts the cascade at the output boundary and places
oversized frames at (0,0). It enforces the latest UserLaunch serial or empty-focus
rule for a client's first window; subsequent windows take focus when their client
already owns focus. A non-user map or raise ignores presses for 300 ms, including
the held button state until release. Clicks focus/raise; closing chooses the MRU
survivor. Focus loss removes undelivered keys for the old surface and synthesizes
releases for delivered keys. A pressed surface retains pointer routing until all
buttons are up, with signed content-local coordinates.

Title drags retain at least 32 horizontal title pixels and a visible title bar.
A last full-pass duration above 50 ms uses a two-pixel outline until release.
Resize edges snap content sizes from the client's base, constrained by min/max;
when contradictory hints contain no grid point in the interval, bounds win.
Maximize/double-click (500 ms, four-pixel tolerance) saves and restores the frame.
Arrange serials allow one outstanding request per surface; AcceptArrange only
advances after the corresponding Present acknowledgment. Pending resize changes
coalesce until then. Alt+Tab follows MRU, Shift reverses, Alt release commits;
Alt+F4 requests CloseAsked and Ctrl+Alt+T sets a request for the CM. Shortcut
key releases are consumed. Tick updates not-responding decoration state.

Sixty-four FIFO input entries hold keys,
buttons, wheel and motion; only adjacent same-surface motions merge. State events
are latest-wins slots, delivered before FIFO entries: image-return bitmaps union,
request-refusal counts accumulate, and no state event competes with input capacity.
Build freezes an answer and its AnswerDropped/AnswerMorePending header values;
Commit consumes it only after a successful send.
WouldBlock retains answer/credit and retries after 10 ms; another send failure
retains state for a fresh credit and increments FailedSends. Overflow drops all pending input entries, counts
those and the incoming entry, and queues releases for delivered keys/buttons.
A frozen answer is retained, and its makes are conservatively included in releases.
The Input Broker's thirty-two held-key bound plus four surface button releases
fits beside a seven-event frozen prefix in the sixty-four-entry queue. Queue input
must respect that upstream held-key bound. Five seconds without credit while
input waits, or without DestroySurface after CloseAsked, marks not responding;
a fresh credit followed by an empty input queue clears the mark.

`InputOrder` implements two-way ordering, with two 64-entry FIFO streams.
Keys wait for applied pointer sequence >= horizon; pointers wait for applied key
count >= stamp. Take advances counters and returns a caller-owned token, without
saving a focus target. Source sequence can skip coalesced motion snapshots but
must increase. Reset on input epoch/face change drops both streams; five-second
wait expiry drops/counts both and refuses more work until reconfigured. Consumers
must preserve each incoming stream's order. It is a pure ordering model, not the
broker's session plane or pointer route transport.

`AddressPlan` models pool mappings. Each client has two 32 MiB pool slots separated by
2 MiB gaps, clients are 68 MiB apart, output starts at 272 MiB, and the region is
306 MiB relative to a caller-supplied 1 GiB aligned base. Map charges one AMD64
leaf-table page per touched 2 MiB granule and a directory page once; Unmap never
refunds tables, and slot reuse reuses the high-water charge. Seed table/record
counts and 64/256-page arena budgets are model inputs. The runtime binds
the fixed ISA bases in ComponentBootstrap.hpp and selects the large class
through the session AddressSpace flag and ResourceDomain limit.
Live record accounting is bounded at 256; runtime ledgers reuse unmapped entries.

## Terminal

`TerminalScreen` stores four-byte `TerminalCell`s: bits 0–20 hold the Unicode
code point, 21–24 foreground, 25–28 background, and 29–31 bold/underline/reverse.
Explicit shifts fix the layout on both targets. Two default-colour bitmaps add
7,200 bounded bytes beside the 115,200-byte grid: the four-bit indices retain all
sixteen ANSI colours while SGR 39/49 retain the separate white/0x102040 defaults.
`CellAt` returns an unpacked `TerminalCellView`, resolving those default markers.
The screen also holds cursor/saved cursor, margins, modes and row damage. It uses a bounded
320x90 grid, defaults to 80x25, truncates/pads on resize and never reflows.
`TerminalParser` keeps at most sixteen parameters clamped to 9,999 and no string
contents. `Terminal` owns these and a 128-byte queue of whole report answers.
`RenderTerminal`/`RenderTerminalDamage` use Graphics surfaces and the public-domain
Bold8x16 font. The steady block cursor reverses resolved colours; underline uses
font row 13. Normal SGR colours brighten under bold; the separate white-on-0x102040
default pair stays unchanged. `TerminalGridForOutput` gives 79x23 at 640x400.

The control-function vocabulary and byte grammar follow **ECMA-48, Control
Functions for Coded Character Sets, fifth edition, June 1991**, clauses 5.2–5.6
and 8.3 ([specification](https://ecma-international.org/wp-content/uploads/ECMA-48_5th_edition_june_1991.pdf)).
The implementation supports DEC private save/restore, margins, autowrap and
application-cursor behavior as a bounded subset of terminal control functions.

In this implementation BS moves left without erasing, HT advances to the next
fixed eight-column stop, CR returns to column zero, and LF/VT/FF index the row.
IND scrolls at the bottom margin, RI scrolls at the top, NEL combines CR and IND;
RIS restores defaults. Cursor movement clips to screen/margins as documented in
TerminalScreen. ED/EL erase inclusive ranges; ICH/DCH and IL/DL shift cells/rows
and fill blanks; SU/SD scroll the region. Erased cells use the current background
and default foreground without attributes. Saved cursor state includes rendition
and pending wrap. Positioning and editing that changes the cursor cancel pending
wrap; ED/EL/ECH and scrolling preserve it. DSR replies are ready or one-based
cursor position; primary DA replies `ESC[?1;2c`. Private modes 1, 7 and 25 control
application cursor keys, wrap and cursor visibility.

OSC, DCS, APC, PM and SOS contents are discarded until ST; BEL also terminates OSC.
CAN/SUB cancel a sequence/string. A string's ESC followed by a non-backslash starts
a fresh escape sequence. Intermediate sequences, colon parameters, unsupported
private markers and excess parameters are ignored whole; extended SGR colours
skip their own arguments. UTF-8 decoding stores every scalar unchanged, including
unsupported CJK; malformed units store U+FFFD and encoded C1 controls are ignored.
Rendering alone uses `Cp437.hpp`'s one table of the Unicode meanings of glyphs
0x01–0xFE, shared with scene titles; unsupported code points and U+FFFD draw 0xFE.
No glyph is assigned to NBSP (0xFF is outside the font range).
No alternate screen, bracketed paste, mouse reporting or scrollback is implemented.

`LayoutKeyCharacter(KeyboardLayout, usage, modifiers)` returns Unicode through
US/JIS tables selected by the caller for each keyboard; there is no mutable global
layout. Unknown ids return no character, and the US convenience entry has explicit
US semantics. `EncodeTerminalKey` accepts the layout and emits UTF-8. JIS 106/109
punctuation follows its physical positions: International3 (0x89) yields U+00A5
yen or shifted `|`, International1 (0x87, ro) U+005C backslash or shifted `_`.
Ctrl+yen/ro yields U+001C, Ctrl+Shift+ro U+001F; kana/conversion keys yield no text here.
The positions come from **USB HID Usage Tables v1.5, 2024-01-26, section 10,
table 10.1** ([specification](https://usb.org/sites/default/files/hut1_5.pdf)); the
Japanese key legends use the US/JIS keymap tables.
No GUI/Super shortcut is recognized; Ctrl+Alt+T, Alt+Tab and Alt+F4 remain.
Backspace sends DEL;
Enter CR, Tab HT, arrows choose CSI or SS3 from application mode, Alt prefixes ESC,
and Ctrl letters send control bytes. The library also supports F5–F12,
Ctrl+[ / backslash / ] / Space and keypad arithmetic/Enter.

## Verification

Hosted tests under `Tests/User/{Session,Terminal,Input,Text}` cover codec
validation, credits, window policy, input ordering and terminal control functions.
Seeded models compare incremental rendering with whole-output references and
accept `ASTERIA_TEST_SEED` for replay. Codec and parser fuzz harnesses cover
malformed inputs.

`RenderModelCompare` compares the C++ scene and terminal corpora against the
Python models in `Tools/Vm/AsteriaVm/{Session,Terminal}.py`; checked-in SHA-256
digests also pin rendering.

## Runtime binding

`User/Component/SessionCompositor` owns these libraries in a separate compositor
module. Its bounded client table retains pool mappings, image definitions,
surface lending, credits and manager replies. Scene views borrow read-only
client pixels; composition copies them into one compositor-owned output,
which the display broker borrows read-only. Store ordering precedes Present,
and the output is frozen through Presented. FrameShown follows that fence
without waiting for diagnostics. An immutable diagnostic snapshot and a
coalesced complete queued snapshot describe completed fences independently
of composition. Test-only PatternClient
and CompositorDemo bind the components without Component Manager startup.

PatternClient sizes returned-image buffers from the bootstrap's session output
mode. Native
buffers use owned RAM Frames in runs of at most 512 pages. A double buffer
uses one pool when it fits the 32 MiB slot; otherwise each image uses a pool,
and a two-window test uses separate client slots in the same process.
The explicit small test mode has four complete 136x112 images. Broker snapshot
exhaustion preserves a blocked
credit's bytes, drops and counts the remaining routed input, and reports
Stale without blocking physical pointer application or cursor rendering.
Ordered client input then fails closed while local compositor shortcuts
remain available. Input Broker replies retry per route, and service drains
are bounded. A revoked credit receiver retires its focus and queue, counting
discarded input and suppressing releases of its held keys; its registration
cannot be focused again. Physical pointer batches and secure attention
continue after that loss. Live WouldBlock credits retain their bytes.
SessionStressSmp2/Smp4 exercise clipped frames, slow presents,
independent healthy and unread client endpoints, and cursor screendumps.

CPU lookup enforces borrowed Frames' Map-only authority.
The root-started test enables one writable-MemoryObject negative probe in
the compositor; RightsDenied is reported before ordinary read-only mapping.

## UI fonts and the mask oracle

`UiText` uses Vegur Bold for title bars and Vegur Regular
for the switcher and its other UI-text entry point. The terminal grid/parser and
trusted console retain Bold8x16. The default is 13 px, a baseline 12 px below the
text area's top, in a 16 px text area inset 2 px vertically inside the unchanged
20 px title bar. This is the default font-setting value; persisted selection,
live replacement, themes/ThemeChanged and parser-process isolation are still
unsupported.

`UiText` borrows both immutable CFF faces, shapes with kern and liga enabled,
rounds baselines to the nearest integral pixel (ties away from zero), and blends
the font engine's unhinted 8x8 coverage. It shares an eight-entry, 1,024-byte
whole-arena-eviction glyph cache across faces. The caller-sized `RasterScratch`
uses 128 edges, 32 crossings and 32 sample columns; Type 2 output has 40 commands.
The pinned Vegur files need at most 34 commands and 112 edges at this size.
Across every glyph in both faces, mask extents are -12..4 relative to the
baseline, fitting the 16 px area fully; 14 px would crop an accented top row.
Every glyph is loaded and rasterized before a face is accepted; failed syntax,
capacity or initial shaping validation closes that face and independently selects
Bold8x16. The general engine retains its full default workspace and limits.
Format interpretation follows **OpenType 1.9, GPOS lookup type 2 and GSUB lookup
type 4**, **Adobe CFF Technical Note #5176 (4 December 2003), sections 4–5 and
14–16**, and **Type 2 Technical Note #5177 (16 March 2000), sections 3–4 and
Appendix B**; links and full provenance are in [FontEngine.md](FontEngine.md).

Title ink, including bearings/overhang, is clipped to the text area before the
first title button. A fitting run retains all bytes. Overflow takes the longest
prefix ending at a cluster boundary in the original shaped run, then reshapes
`prefix + "..." + suffix`; kerning at the cut is therefore recomputed. The trusted
` (not responding)` suffix is reserved if it plus the ellipsis fits; otherwise
the suffix is omitted and the ellipsis itself is clipped if necessary. UTF-8 and
ligature clusters are never split. The leading class diamond stays the trusted
Bold8x16 glyph with a following face-space advance, cannot be supplied by a
subtitle, and is clipped even when the box is too narrow for text. Trusted name,
class marker and liveness suffix retain their title colour; subtitle ink and
an ellipsis cutting the subtitle use the subtitle colour. Switcher box sizing
uses Regular's shaped advance plus existing padding/margins, with the same
clipping/ellipsis policy. The fallback uses 8 px advances and the same policy.

Boot-module receipts `ui-bold` and `ui-regular` carry original OTF bytes.
Product and test ESPs install VEGURB.OTF, VEGURR.OTF and FONTNOTE.TXT with normal
manifest CRCs and module digests. Root copies both faces into two 16 KiB slots
appended to the compositor stack allocation and grants only Map authority.
Compositor maps this tail Read|User, WB/NX at `base + AddressUiFontOffset`,
outside the 306 MiB session region and stack mapping; all guard gaps remain
unmapped. Domain drain reclaims it with the original allocation. An attempted
writable mapping must return RightsDenied. Missing receipts select Bold8x16;
corrupted receipts fail normal boot-manifest validation.

`Tools/Build/UiTextMasks.cpp` links the font engine and UiText with a mask-emitting
`UiTextDrawing` adapter. It supplies glyph coverage, placement, clipping and
colour to the independent Python scene renderer; `ASTERIA_UI_TEXT_TOOL` overrides
tool discovery. The oracle checks composition and blending but shares font
parsing, shaping and rasterization with the compositor. Synthetic font tests,
pinned Vegur tests and scene digests provide separate checks. Session and
missing-font VM cases require exact screenshot agreement after a completed
`presented` diagnostic and display fence.

Build and VM checks derive page/object budgets from linked images. COMPOSIT.ELF
uses RX constants and RW/NX state; font data is NX. Target stack frames remain
within 512 bytes.

The terminal shutdown phase additionally overlaps storage flushing; its ordering
is specified in [PowerOff](PowerOff.md). The linked-image budget model covers
this phase as well as the font and client phases.
