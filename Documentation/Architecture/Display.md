<!-- SPDX-License-Identifier: Apache-2.0 -->

# Display and composition

The user-mode System Display Broker owns the scanout and global pointer. It
maps the published DeviceRegion with WC preferred on AMD64 and UC fallback;
IA-64 uses UC. The kernel draws only terminal stop screens. Session window
composition belongs to the [desktop session compositor](DesktopSessionLibraries.md).

## Planes and output coordinates

`User/Graphics/Composition` is a pure, allocation-free library. `Rect` uses the
existing graphics rectangle. `DamageList` clips to configured logical desktop
bounds, merges touching rectangles, and collapses all entries to their bounding
union when its 16 entries fill. `Composer` borrows up to eight planes and four
outputs. Plane insertion order is bottom to top: background, console, cursor.
No application surface is accepted yet.

Each output has a signed logical desktop origin and a positive integer scale.
Negative origins permit outputs to the left or above the primary output. Physical
scanout dimensions must be multiples of the scale. Rendering uses nearest
neighbour replication. The broker installs one output at origin (0,0), scale 1.
These explicit transforms leave room for multiple outputs and remote scanouts;
input clamping is deliberately a single-output policy until output management
exists. No physical address or hardware dependency enters the library.

For every damaged rectangle, the composer clips to each output and asks every
plane to render physical pieces no larger than 64x64 into its caller-owned
static tile. The tile uses that output's RGBX/BGRX format. Only finished tiles
are copied to scanout, once per tile, without reading any framebuffer pixels or
writing pitch padding. One ordering callback follows a nonempty pass, after
all its tiles and outputs: AMD64 SFENCE, IA-64 MF.A then MF. Empty passes have
no stores. The compiled-code property follows the broker, callback descriptor,
composition loop and ISA adapter; it rejects removal of every ordering edge.

[`ScanoutStorage`](../../Source/User/Graphics/ScanoutStorage.hpp) packs two pixels
into each aligned eight-byte volatile store on little-endian RGBX/BGRX scanout.
Source and destination rows require four-byte alignment; a leading pixel aligns
the destination to eight bytes, and an odd tail uses a four-byte store. The adapter
performs no device reads or fences. Its UInt64 word uses GNU `may_alias` so hosted
UInt32 pixel arrays can share the implementation under strict aliasing. Tests
check pixel values, narrow/wide store counts and one ordering callback per pass.

`TextConsole::RenderRegion` draws arbitrary tile rectangles from the cell grid,
including the margin outside it, without consuming row damage. Console writes
and scrolls damage full rows; the broker converts those rows to rectangles and
composes them through the same pass as pointer changes. The public cell interface supplies zero attributes; the grid stores the character byte.

## Cursor and pointer

`CursorPlane` borrows a straight-alpha ARGB image of at most 64x64, with its hot
spot, logical pointer position and visibility. Blending happens in ordinary
memory, with channel rounding `(source*a + target*(255-a) + 127)/255`. The
built-in arrow uses the 8x16 mask in
`CursorImages.hpp`; constexpr expansion derives all neighbouring black outline
pixels. The result is 10x18 with hot spot (1,1). The alternate dot is 5x5 with
hot spot (2,2). `ICursorBackend` exposes image, move and visibility operations
beside `IPlane`; only the software implementation exists. Client-provided cursor images and hardware cursor selection are unsupported.

The broker starts at (width/2,height/2), integrates signed relative motion in
64 bits and clamps to [0,width-1] and [0,height-1]. Normalized absolute 0..65535
maps inclusively to the first/last pixel. Pointer sequence and epoch checks
precede mutation; replayed or older sequences and stale epochs do nothing.
Every accepted event records both old and new cursor bounds. Button/wheel
snapshots advance the fence even when position is unchanged.

## Endpoint protocol and authority

Display v2 uses interface `{0x4153545244495302,0x504F494E54455202}`. The old
interface is refused. The delivery header's kernel-authenticated rights decide
access; no sender-controlled payload claims authority.

| Method | Number | Payload | Right |
| --- | --- | --- | --- |
| ConsoleWrite | 1 | count, three zero words, 192 zero-padded CP437 bytes | Manage |
| Query | 2 | four zero words; one Invoke-copy reply Endpoint | Invoke |
| Reply | 3 | ten words below | Broker answer |
| SetMode | 4 | Reserved, refused | Manage when implemented |
| Pointer | 9 | epoch, sequence, kind, x/y, buttons, source, wheel, zero | Invoke |

Pointer kind is relative=1, absolute=2, snapshot=3. X/Y are two 32-bit fields
in word 3, signed deltas or normalized unsigned positions; snapshot motion is
zero. Buttons are bits 0..2, source is a nonzero u32, wheel is signed u32 bits
with a zero high half. Epoch/sequence are nonzero u64. Absolute values above
65535 and all noncanonical headers, attachments or padding are refused by the
failure-atomic decoder. Surface methods 5..8 remain reserved.

Reply words are transaction, Done/Refused, version, features, width/height,
columns/rows, epoch, pointer X/Y, last applied pointer sequence, zero. Feature
bits 1/2 mean console/pointer. The broker drains at most 16 deliveries, composes
once, then answers all that batch's Queries with the rendered state and closes
their capabilities. Readiness remains signalled for a continuing producer.
Foreign or unauthorized requests release installed attachments. ConsoleWrite
and Pointer are one-way; refusal is observed through unchanged fence state.

## Boot, storage and verification

Product init and the test pager install the Input Broker route with an
Invoke-only attachment of the display capability. The root retains Manage.
Route setup first queries the display's epoch/sequence, so restarting input
without restarting display continues its monotonic sequence. Fence observations
are printed only to the DiagnosticChannel as `# pointer: x,y sequence n`, never
to the console being observed. They use the existing reply Endpoint, or the
report Endpoint before a reply Endpoint exists, with bounded queue-full retry.

`ServiceScratch` explicitly owns mutually exclusive composer, input core and
AsteriaFS lifetimes in one zero-initialized arena. Each component incarnation
runs one immutable behaviour in a private address space. `PlaneStorage` starts
its resource-free polymorphic plane lifetimes explicitly, avoiding initialized
writable pointer relocations. Neither owner allocates or relies on lazy guards.
The 16 KiB tile thus fits the existing pager grants and component pools.

IA-64 initial-image staging admits at most 160 pages per segment and 352 pages
in total. Static ownership and stack gates enforce these bounds.

Host tests compare every pixel, including seams, pitch, edges/corners, hot spots,
outline, glyphs and scrolling. Seeded models compare incremental rendering to
an independent full renderer and route queues to a bounded stream model. Both
wire decoders have canonical round-trip/failure-atomic fuzz harnesses. The VM
runner parses the source mask and colors, draws the cursor at the reported
fenced position and compares complete screenshots, including product clamping
and text-overlap checks. IA-64 retains scripted pointer routing until its real
input device authority is published.
