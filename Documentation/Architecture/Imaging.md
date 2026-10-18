<!-- SPDX-License-Identifier: Apache-2.0 -->

# Bounded image codecs

Asset parsing runs outside the display broker. Source/User/Imaging is a pure freestanding user
library; it has no allocator, file access, global mutable state or transport.
Callers lend immutable input, disjoint writable output, and an exclusive
workspace in static storage. A failed decode clears the image view; output
and scratch may be partially written and must not be published. Successful
views borrow output until its owner releases it. Inputs must remain stable.

## API and limits

`Bytes`, `Buffer`, `Image`, `Status`, `InflateReader` and `Workspace` use
Foundation types. Image bytes are packed, top-down, 8-bit straight RGBA.
InflateReader pulls bytes across caller-provided segments, retaining a 32 KiB
window and bit state. End verifies the zlib checksum. PNG uses this reader
one scanline at a time; IDAT is never concatenated into a temporary buffer.
Limits are defined only in Imaging.hpp: 4096 per axis, 1,048,576 pixels,
32 MiB input, 16 MiB inflated data, 1024 segments/chunks, 65536 DEFLATE blocks,
64 ICO entries. On both 64-bit ABIs InflateReader is 35,008 bytes, Workspace 117,952 bytes,
and CursorSet 12,888 bytes. Workspace contains two maximum PNG rows, palette, segment
views and inflate tables/window. Independent workspaces are reentrant.
Crc32 is a low-level checksum helper with a valid, bounded Bytes precondition;
all decoders validate input ranges before calling it. Each length is checked before dereference; dimensions bound derived products.

`DecodePng`, `DecodeBmp`, `DecodeDib` (packed DIB), `DecodeIco` and
`DecodeCursorSet` return Status::Ok only after complete validation. The
standalone Inflate convenience function reports bytes written even on refusal;
InflateReader::Next returns one byte or a terminal status, with End signaling
checksum-verified completion. Begin resets a reader after either success or
failure. Reuse a workspace only after the previous call finishes.

Refusals distinguish signature/header/order/size errors, truncation, unknown
critical formats, exhausted limits/buffers, CRC/Adler failures, invalid Huffman
codes/distances, palette indices, filters, masks and hotspots. Crc32 is only a
checksum helper; decode entry points supply typed validation. Metadata ignored
by policy is not interpreted, decompressed or used to access external files.

## Formats and references

Algorithms are written from definitions, not specification sample code.
DEFLATE: RFC 1951 (May 1996), sections 3.1.1 and 3.2.2–3.2.7: stored,
fixed and dynamic codes, lengths 3–258, distances 1–32768, overlapping copies.
Oversubscribed/incomplete trees are refused except the specified single-symbol
one-bit tree and absent distance alphabet for literal-only blocks. Zlib:
RFC 1950 (May 1996), sections 2.2–2.3: CM/FCHECK/window/FDICT and Adler-32.
Preset dictionaries and trailing bytes are refused. Code 284 is restricted to
227–257 as the RFC’s length table specifies; length 258 uses code 285.

PNG Specification, Third Edition (W3C Recommendation, 24 June 2025): sections
5.3–5.6 (chunk syntax/CRC/order), 6–10 (samples, Adam7, filters, zlib),
11.2 and 11.3.1.1 (IHDR/PLTE/IDAT/IEND/tRNS), 13.12 (sample rescaling).
All specified static colour types/depths are supported. Rescaling uses rounded
linear conversion to 8-bit, comparing tRNS at original precision first.
Ancillary chunks except tRNS are ignored after CRC validation, including gamma,
ICC, colour-space and significant-bit metadata. No colour management or APNG
animation is performed; animation ancillary chunks are skipped and the static
IDAT image is decoded. Unknown critical chunks are refused.

Microsoft Learn bitmap documentation: BITMAPFILEHEADER,
BITMAPCOREHEADER, BITMAPINFOHEADER, BITMAPV4HEADER, BITMAPV5HEADER (Members),
RGBQUAD, Bitmap Storage, Bitmap Header Types and Bitmap Compression.
Core palettes additionally follow BITMAPCOREINFO (revision 2022-09-01,
Members/Remarks) and RGBTRIPLE (revision 2024-02-22, Members):
[BITMAPCOREINFO](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapcoreinfo),
[RGBTRIPLE](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-rgbtriple).
Core/info/v4/v5 headers, 1/4/8/16/24/32 bpp, BI_RGB, BI_BITFIELDS, RLE4/RLE8,
bottom-up and top-down are supported. Core permits only 1/4/8/24 bpp as
BITMAPCOREHEADER Members defines. Masks must be contiguous, disjoint and
within the pixel. Only explicit v4/v5 bitfield alpha is used; BI_RGB unused
bytes are opaque. Embedded JPEG/PNG compression and colour profiles are not
processed. Nonempty V5 profile ranges must be bounded and follow the bitmap
bits (packed DIB/file layout). RLE skips use palette entry zero. Top-down RLE is refused. Core scanline padding must be zero.

Icons in Win32 (Microsoft, ms997538, created 29 September 1995), “The ICO File” (ICONRESDIR Members supplies the zero-as-256 dimension
convention): ICONDIR/ICONDIRENTRY, doubled INFO DIB height, XOR and AND masks.
ICO DIB payloads use the 40-byte BI_RGB header defined there; other
DIB header/compression variants and unspecified implicit 32-bit icon alpha
are refused or treated as opaque unused bits, respectively. PNG payloads
use the PNG decoder. Size choice minimizes total absolute axis difference,
then prefers larger area; ties retain directory order. AND=1 with nonzero
XOR requires destination inversion, which straight RGBA cannot represent;
those pixels are refused rather than silently changing their semantics.

## Unsupported cursor formats

CUR and ANI decoding is not implemented. The library uses the versioned cursor-set
container below.

## Cursor container version 1

All integers are unsigned little-endian. No native structs are serialized.
Header (16 bytes): ASCII `ACSR`, u16 version=1, u16 role count=10,
u32 total file bytes, u32 reserved=0. Exactly ten roles follow in order:
arrow, text, busy, resize-horizontal, resize-vertical, resize-nw-se,
resize-ne-sw, move, link, unavailable. A role header is u16 role id (0–9),
u16 frame count (1–32). Each frame: u16 width, height, hotspot x, hotspot y;
u32 delay milliseconds; u32 encoding (0=raw RGBA, 1=PNG); u32 payload bytes;
then that payload with no padding. A single frame has delay=0; animated frames
have delays 1–60000. Dimensions are 1–128, hotspots strictly inside, raw
payload is exactly width*height*4. PNG dimensions must match the record.
Total decoded cursor pixels are bounded to 1,048,576. No trailing data,
unknown roles/versions/encodings or duplicate/reordered roles are accepted.
`DecodeCursorSet` produces frame views into a caller arena and a bounded
caller-owned CursorSet; it publishes Count only after all frames validate.
First, Length and Frames must not be read
when Count is zero. Delay scheduling and broker publication remain the caller’s
responsibility.

Tools/Cursors/Generate.py draws original 32-square pixel art from geometric
primitives in Source.json and emits Default.acsr. No external artwork is used.
The generator is deterministic and its output is tested against the checked-in
file; busy has two frames. This library does not schedule animation or bind
the broker. The toolkit adapter borrows an Imaging::Image for PictureBox.

## Verification evidence

Decoder tests cover boundaries, refusal paths and seeded mutations across zlib,
PNG, BMP, DIB, ICO and cursor sets. They use authored byte vectors and independent
encoders. The PNG matrix covers all 15 type/depth pairs with all five filters, both
interlace modes and one-byte IDAT splits. Boundary vectors cover every length
and distance interval endpoint, stored length 65535, lengths 3 and 258,
distance 32768, non-byte block boundaries, code-length repeats 16/17/18,
empty Adam7 passes, original-precision tRNS and decompressed/block limits.
The generator check reproduces Default.acsr byte-for-byte.

Target compile checks include both O0 and O2 objects and the existing 512-byte
stack gate.
No library code is linked into a component or kernel image.
[The README](../../README.md#verification) lists the build and test commands.
The pure library has no standalone VM scenario.

