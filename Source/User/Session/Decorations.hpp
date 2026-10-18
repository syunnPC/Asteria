// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SESSION_DECORATIONS_HPP_INCLUDED
#define ASTERIA_USER_SESSION_DECORATIONS_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Graphics/PixelFormat.hpp"
#include "User/Graphics/Surface.hpp"

namespace Asteria::User::Session
{
class UiText;
/// The compositor's server-side decorations, drawn by this code with FillRect and DrawGlyph. Every
/// constant is a plain literal in braces; the runner's model (Tools/Vm/AsteriaVm/Session.py) reads
/// them from this header, so keep that form. A frame rectangle (x, y, w, h) in desktop pixels
/// holds: - the frame, FrameWidth pixels on each side: ring 0 (outermost) Light on the top and
/// left, DarkShadow on the bottom and right; ring 1 Highlight and Shadow; rings 2-3 Face; - the
/// title bar at (x + 4, y + 4), w - 8 wide and TitleBarHeight high; - the separator, row y + 24, in
/// Face; - the content at (x + 4, y + 25), w - 8 by h - 29. So a frame is its content plus
/// FrameExtraWidth by FrameExtraHeight.
inline constexpr Foundation::UInt32 FrameWidth{4U};
inline constexpr Foundation::UInt32 TitleBarHeight{20U};
inline constexpr Foundation::UInt32 SeparatorHeight{1U};
inline constexpr Foundation::UInt32 FrameExtraWidth{8U};
inline constexpr Foundation::UInt32 FrameExtraHeight{29U};
static_assert(FrameExtraWidth == (2U * FrameWidth));
static_assert(FrameExtraHeight == ((2U * FrameWidth) + TitleBarHeight + SeparatorHeight));
/// The title's first glyph, from the title bar's left edge and top: the
/// text starts at (x + 8, y + 6).
inline constexpr Foundation::UInt32 TitleTextInset{4U};
inline constexpr Foundation::UInt32 TitleTextTop{2U};
/// The text is clipped this many pixels before the first button.
inline constexpr Foundation::UInt32 TitleTextGap{4U};
/// The 16x14 buttons, their tops ButtonTop pixels below the title bar's
/// (y + 7): close at x + w - 22 (ButtonRightInset from the title bar's right
/// edge), maximize ButtonGap pixels to its left, at x + w - 40.
inline constexpr Foundation::UInt32 ButtonWidth{16U};
inline constexpr Foundation::UInt32 ButtonHeight{14U};
inline constexpr Foundation::UInt32 ButtonTop{3U};
inline constexpr Foundation::UInt32 ButtonRightInset{2U};
inline constexpr Foundation::UInt32 ButtonGap{2U};
/// The resize zone, measured from the frame's outer edge, and how far along
/// an edge from a corner a press drags both edges.
inline constexpr Foundation::UInt32 ResizeZone{4U};
inline constexpr Foundation::UInt32 CornerExtent{16U};
/// The Alt+Tab switcher: a raised panel (the frame's rings 0-1, then Face)
/// centred on the output, SwitcherHeight high and as wide as its text plus
/// twice SwitcherPadding, at most the output's width less SwitcherMargin;
/// the text (Glyph colour) starts SwitcherPadding pixels from its left and
/// SwitcherTextTop pixels from its top.
inline constexpr Foundation::UInt32 SwitcherHeight{36U};
inline constexpr Foundation::UInt32 SwitcherPadding{16U};
inline constexpr Foundation::UInt32 SwitcherMargin{32U};
inline constexpr Foundation::UInt32 SwitcherTextTop{10U};
/// An outline move's rectangle: OutlineWidth pixels of DarkShadow.
inline constexpr Foundation::UInt32 OutlineWidth{2U};
/// The trusted part of a title comes first: a volume bundle's name is
/// preceded by Unicode VolumeCodePoint and a space, a system
/// module's by nothing. Then SubtitleMark and the client's subtitle in the
/// subtitle colour, controls and VolumeCodePoint drawn as SubtitleReplacement
/// so no subtitle can show the class glyph; a window the compositor judges
/// not responding ends with NotRespondingMark, in the trusted colour.
inline constexpr Foundation::UInt32 VolumeCodePoint{0x2666U};
inline constexpr char SubtitleMark[]{" - "};
inline constexpr char NotRespondingMark[]{" (not responding)"};
inline constexpr Foundation::UInt8 SubtitleReplacement{0x3FU};
/// The longest registered name and subtitle.
inline constexpr Foundation::Size TitleNameBytes{16U};
inline constexpr Foundation::Size SubtitleBytes{64U};
/// The longest whole title: the class glyph and space, the name, the
/// subtitle and its mark, and the not-responding mark.
inline constexpr Foundation::Size TitleTextBytes{4U + TitleNameBytes + 3U + SubtitleBytes + 17U};

/// The button glyphs, one row per element, the leftmost pixel in the row's
/// most significant of Width bits (each literal has exactly Width digits),
/// drawn in Glyph with their top-left pixel at (Left, Top) inside the
/// button, one pixel right and down while it is pressed. Close: two crossing
/// diagonals 2 pixels wide in an 8x7 box at (4, 3); maximize: a 9x8 outline
/// with a 2-pixel top edge at (3, 2); restore (a maximized window's): two
/// overlapping 6x6 outlines in the same box.
inline constexpr Foundation::UInt32 CloseGlyphWidth{8U};
inline constexpr Foundation::UInt32 CloseGlyphHeight{7U};
inline constexpr Foundation::UInt32 CloseGlyphLeft{4U};
inline constexpr Foundation::UInt32 CloseGlyphTop{3U};
inline constexpr Foundation::UInt16 CloseGlyph[CloseGlyphHeight]{
    0b11000011, 0b01100110, 0b00111100, 0b00011000, 0b00111100, 0b01100110, 0b11000011,
};
inline constexpr Foundation::UInt32 MaximizeGlyphWidth{9U};
inline constexpr Foundation::UInt32 MaximizeGlyphHeight{8U};
inline constexpr Foundation::UInt32 MaximizeGlyphLeft{3U};
inline constexpr Foundation::UInt32 MaximizeGlyphTop{2U};
inline constexpr Foundation::UInt16 MaximizeGlyph[MaximizeGlyphHeight]{
    0b111111111, 0b111111111, 0b100000001, 0b100000001, 0b100000001, 0b100000001, 0b100000001, 0b111111111,
};
inline constexpr Foundation::UInt32 RestoreGlyphWidth{9U};
inline constexpr Foundation::UInt32 RestoreGlyphHeight{8U};
inline constexpr Foundation::UInt32 RestoreGlyphLeft{3U};
inline constexpr Foundation::UInt32 RestoreGlyphTop{2U};
inline constexpr Foundation::UInt16 RestoreGlyph[RestoreGlyphHeight]{
    0b000111111, 0b000100001, 0b111111001, 0b100001001, 0b100001001, 0b100001111, 0b100001000, 0b111111000,
};

/// Where a desktop point falls on a window.
enum class FramePart : Foundation::UInt8
{
    None = 0U,
    Content = 1U,
    Title = 2U,
    CloseButton = 3U,
    MaximizeButton = 4U,
    /// The rest of the frame: the separator, and the face ring of a
    /// maximized window (whose resize zone is off).
    Frame = 5U,
    ResizeLeft = 6U,
    ResizeRight = 7U,
    ResizeTop = 8U,
    ResizeBottom = 9U,
    ResizeTopLeft = 10U,
    ResizeTopRight = 11U,
    ResizeBottomLeft = 12U,
    ResizeBottomRight = 13U,
};

/// How a title bar is drawn.
enum class TitleState : Foundation::UInt8
{
    Unfocused = 0U,
    Focused = 1U,
    /// Unfocused, raised without focus and wanting attention.
    Attention = 2U,
};

/// Whether a button is drawn raised or pressed (sunken).
enum class ButtonState : Foundation::UInt8
{
    Raised = 0U,
    Pressed = 1U,
};

/// The registered class of a window's client, which its trusted title shows.
enum class ClientClass : Foundation::UInt8
{
    SystemModule = 1U,
    VolumeBundle = 2U,
};

/// The text and look of one title bar. Name is the registered name (printable UTF-8, validated at
/// registration); Subtitle is drawn with SubtitleReplacement for controls and the class symbol.
/// Both are borrowed. Glyph lookup maps Unicode to Bold8x16 through
/// Terminal/Cp437.hpp.
struct TitleView
{
    const Foundation::UInt8* Name;
    Foundation::Size NameLength;
    const Foundation::UInt8* Subtitle;
    Foundation::Size SubtitleLength;
    ClientClass Class;
    TitleState State;
    bool NotResponding;
    bool Maximized;
    ButtonState Close;
    ButtonState Maximize;
};

/// The frame's parts, from a frame rectangle (desktop coordinates). A frame
/// too small for a part gives an empty part.
[[nodiscard]] Graphics::Rect ContentRect(Graphics::Rect frame) noexcept;
[[nodiscard]] Graphics::Rect TitleBarRect(Graphics::Rect frame) noexcept;
[[nodiscard]] Graphics::Rect CloseButtonRect(Graphics::Rect frame) noexcept;
[[nodiscard]] Graphics::Rect MaximizeButtonRect(Graphics::Rect frame) noexcept;
/// The frame around content of width x height whose top-left pixel is at
/// contentOrigin.
[[nodiscard]] Graphics::Rect FrameAroundContent(Graphics::Point contentOrigin, Foundation::UInt32 width,
                                                Foundation::UInt32 height) noexcept;
/// A maximized window's frame: the output's rectangle.
[[nodiscard]] Graphics::Rect MaximizedFrame(Graphics::Rect output) noexcept;
/// The switcher's panel for text of length bytes on an output of width x
/// height (left or top zero when the output is narrower or lower).
[[nodiscard]] Graphics::Rect SwitcherRect(Foundation::UInt32 width, Foundation::UInt32 height,
                                          Foundation::Size length) noexcept;
[[nodiscard]] Graphics::Rect SwitcherRectPixels(Foundation::UInt32 width, Foundation::UInt32 height,
                                                Foundation::UInt32 textPixels) noexcept;

/// The whole title as bytes, as the title bar draws it (the class glyph and
/// space, the name, the subtitle mark and subtitle with replacements, the
/// not-responding mark) into out (TitleTextBytes); returns its length. The
/// switcher shows this text.
[[nodiscard]] Foundation::Size ComposeTitle(const TitleView& title, Foundation::UInt8 (&out)[TitleTextBytes]) noexcept;

/// The part of a frame under point. The resize ring (ResizeZone pixels from
/// the outer edge; within CornerExtent pixels of a corner it is a corner) is
/// tested first and does not exist on a maximized window; then the buttons,
/// the content and the title bar.
[[nodiscard]] FramePart HitTest(Graphics::Rect frame, Graphics::Point point, bool maximized) noexcept;

/// Draws a frame's rings, title bar, title, buttons and separator into the
/// desktop-sized surface (surface pixel (x, y) is desktop point (x, y)),
/// clipped to clip, a rectangle inside the surface. Content is not drawn.
/// The title and the buttons are also clipped to the title bar. Bounded by
/// the frame's perimeter, the title bar's area and the title's length.
void PaintFrame(const Graphics::MemorySurface& surface, Graphics::Rect clip, Graphics::Rect frame,
                const TitleView& title, UiText* ui = nullptr) noexcept;

/// Draws the switcher panel box with text (bytes drawn as code page 437
/// glyphs, cut where the panel ends), clipped as PaintFrame is.
void PaintSwitcher(const Graphics::MemorySurface& surface, Graphics::Rect clip, Graphics::Rect box,
                   const Foundation::UInt8* text, Foundation::Size length, UiText* ui = nullptr) noexcept;

/// Draws an outline move's rectangle, clipped as PaintFrame is.
void PaintOutline(const Graphics::MemorySurface& surface, Graphics::Rect clip, Graphics::Rect outline) noexcept;

/// The surface view of a rectangle of surface (already inside it): its
/// top-left pixel becomes (0, 0). Every paint of the session draws through
/// such views, so FillRect, CopyRect and DrawGlyph clip to them.
[[nodiscard]] Graphics::MemorySurface ClipSurface(const Graphics::MemorySurface& surface, Graphics::Rect clip) noexcept;
} // Namespace Asteria::User::Session

#endif // ASTERIA_USER_SESSION_DECORATIONS_HPP_INCLUDED
