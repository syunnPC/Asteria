<!-- SPDX-License-Identifier: Apache-2.0 -->

# Font engine

This pure user library supplies scalable text to the toolkit and
[desktop compositor](DesktopSessionLibraries.md). Fonts remain borrowed immutable
data; the kernel has no font dependency. Parser sandboxing requires a process
boundary supplied by the integrating service.

## API and ownership

`User/Font/Font.hpp` exposes `Font::Open`, Unicode glyph lookup, horizontal and
line metrics, UTF-8 family/style names, and PostScript metadata. `Outline.hpp`
exposes a bounded Type 2 interpreter and `LoadOutline`; commands use signed
64-bit design coordinates with 16 fractional bits, including quadratic and cubic curves. The caller
supplies command storage and a reusable `OutlineWorkspace`. `Raster.hpp` accepts
16.16 pixel sizes and produces borrowed eight-bit coverage masks with baseline
bearings. `Layout.hpp` shapes a bounded left-to-right run, measures it and
provides line wrapping over shaped clusters. `Cache.hpp` owns no memory: the
caller lends a byte arena and entries, and a full arena evicts all entries.
`Draw.hpp` blends coverage and optional opacity into both Graphics surface types.
All public operations return explicit errors or bounded results; outputs from
failed outline/layout operations must be discarded. Font bytes must outlive
all views and cache entries. Workspaces/cache are single-owner, without global
mutable state; large objects belong in static storage on targets.

`RasterScratch` accepts caller-owned edge/crossing/sample arrays with explicit
capacities; `RasterWorkspace` supplies the unchanged full-capacity defaults and
remains constant-initializable for static target storage.
`GlyphCache::GetCff` uses a Type2Workspace directly for a CFF-only owner, avoiding
a reserved TrueType point/composite workspace. Both paths share Rasterize and
cache insertion/eviction; insufficient scratch returns Limit.

A typical owner opens its borrowed font bytes once, calls `Shape` into its
run buffer, then calls `GlyphCache::Get` for each shaped glyph and `DrawMask`
with that glyph's baseline and colour. Layout coordinates are y-up; raster mask
bearings and surface coordinates are y-down. `Measure` returns advance-based
line dimensions rather than ink bounds. `Wrap` wraps an already shaped run;
callers that need fresh kerning at each break should shape each resulting line.

## Formats and validation

Big-endian sfnt directories accept OTTO/CFF1 or version 1 TrueType glyf/loca.
Every directory record is checked, with duplicate tags, overlaps and directory
aliases rejected. Used tables and referenced structures are checked before
access; glyph program syntax is validated lazily by `LoadOutline`; unsupported optional table formats are reported or ignored without
interpreting their payload. Unicode cmap formats 4/12, hhea/hmtx, head, maxp,
OS/2, Unicode name records (typographic IDs 16/17 before legacy 1/2) and
post metadata are supported. CFF INDEX/DICT,
local/global subroutines and Type 2 hints, width, arithmetic and flex operators
are bounded. TrueType instructions are range checked and ignored; composites
support transformations and point attachment with an explicit workspace stack.
Layout validates common script/feature/lookup structures and applies kern pair
positioning (formats 1/2, including extension type 9) and liga substitution
(type 4, including extension type 7). Unsupported lookup flags are not applied.

The rasterizer uses deterministic integer scaling, bounded curve subdivision,
non-zero winding scanlines and an 8 by 8 coverage grid. Coverage is linear byte
coverage, not a gamma or LCD filter. Mask dimensions and sample work are bounded.
There is no hinting in v1, including flex depth hints and device adjustments.

Limits have one source of truth in `Limits.hpp`: 64 MiB input, 128 tables,
65,535 glyphs, 4,096 outline commands (at most 12,288 control/end points)
and 4,096 TrueType points, 16 composite levels,
10 Type 2 subroutine levels, 48 operands, 32 transient operands, 96 stems,
65,536 interpreted tokens, 64 KiB per charstring/subroutine, 16 curve subdivision levels, 16,384 raster
edges, 512 pixels per mask axis, 4,096 glyphs per run and 1 MiB cache bytes.
Structural validation also has a shared work budget to bound aliased tables.
No heap, exceptions, RTTI, floating-point runtime or libc dependency is needed.

## Specification provenance

The supported formats follow these specifications:

- Microsoft **OpenType 1.9** / ISO/IEC 14496-22, as presented by the compatible
  [1.9.1 specification](https://learn.microsoft.com/en-us/typography/opentype/spec/):
  “The OpenType Font File / Table Directory”; head, maxp, hhea, hmtx, OS/2,
  name, post, cmap “Format 4” and “Format 12”, loca, glyf “Simple Glyph” and
  “Composite Glyph”; “OpenType Layout Common Table Formats” (ScriptList,
  FeatureList, LookupList, Coverage, Class Definition); GPOS “Lookup Type 2”
  and “Lookup Type 9”, GSUB “Lookup Type 4” and “Lookup Type 7”.
- Adobe **The Compact Font Format Specification**, Technical Note #5176,
  4 December 2003, sections 4–5 (DICT/INDEX), 6–10 (header and name/top/string indexes),
  9 (Top DICT), 14 (CharStrings INDEX), 15 (Private DICT),
  16 (local/global subroutine indexes).
  [Specification](https://adobe-type-tools.github.io/font-tech-notes/pdfs/5176.CFF.pdf).
- Adobe **The Type 2 Charstring Format**, Technical Note #5177, 16 March 2000,
  sections 3–4 (encoding/operators), Appendix B (limits), Appendix C
  (deprecated composite endchar is rejected).
  [Specification](https://adobe-type-tools.github.io/font-tech-notes/pdfs/5177.Type2.pdf).
- The project's UTF-8 decoder cites Unicode Standard 16.0, section 3.9,
  table 3-7; malformed bytes consume one byte and produce U+FFFD.

## Unsupported features

Font collections, CFF2/variable fonts, CID-keyed CFF, nonstandard FontMatrix,
Type 1/deprecated seac, colour/bitmap outlines, vertical layout, script-specific
shaping, bidi, language selection, mark positioning, GDEF filtering, fallback
font selection and Japanese line-breaking rules are outside v1. Unsupported
outline kinds fail explicitly. Unused OpenType tables are range checked only.
Phantom-point attachment in TrueType composites is outside v1; actual contour
point attachment is supported. Unhinted advances use hmtx, without instruction
or phantom-point metric adjustment. Host tests synthesize their own fonts;
Vegur tests read the original data with SHA-256 checks, and derived coverage
goldens are test artifacts rather than converted font programs.

## Layout defaults

The conservative defaults are hhea line metrics unless OS/2 USE_TYPO_METRICS
requests typographic metrics; DFLT then latn default-language kern/liga only;
linear coverage blending; explicit LF breaks and space breaks with emergency
cluster wrapping; whole-arena cache eviction. Callers can choose different policies
through its caller without changing font data. Full multilingual shaping and
font-service topology require service integration.
