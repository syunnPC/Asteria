// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SESSION_SCENE_HPP_INCLUDED
#define ASTERIA_USER_SESSION_SCENE_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Graphics/Composition/Composition.hpp"
#include "User/Graphics/PixelFormat.hpp"
#include "User/Graphics/Surface.hpp"
#include "User/Session/Decorations.hpp"

namespace Asteria::User::Session
{
/// A client image as the compositor sees it: a read-only view of a rectangle of a pool the
/// compositor maps read-only.
using ImageView = Graphics::PixelSurface<const Foundation::UInt32>;

/// What a window's decorations show besides its title.
struct WindowLook
{
    TitleState State;
    bool NotResponding;
    bool Maximized;
    ButtonState Close;
    ButtonState Maximize;
};

/// The compositor's scene: up to MaximumWindows windows in a z-order, each a frame rectangle on the
/// desktop with server-side decorations around its content, the client's current image (borrowed,
/// never copied), a title and a look; plus the Alt+Tab switcher and an outline-move rectangle drawn
/// above every window. Damage. Changes of geometry, stacking, look, title or overlay add the
/// rectangles they change to the scene's damage list (Graphics::Composition::DamageList, clipped to
/// the output): a shown or hidden window its frame, a move its old and new frames, a raise its
/// frame, a look or title change its title bar, an overlay its old and new boxes. A Present's
/// rectangles are moved to desktop coordinates, clipped to the content rectangle and unioned per
/// window first; the union joins the damage list when Compose runs. The first image of a window and
/// an image whose size differs from the previous one damage the whole content rectangle (decision
/// 8); otherwise the client's rectangles are relative to its previous image. Composition is
/// CPU-only into the caller's write-back output buffer, which has the output's size: for each
/// damaged rectangle the topmost window whose frame covers it is found (nothing below it is drawn;
/// windows are opaque), the desktop is filled when none does, the windows from there up are drawn
/// bottom to top (decorations, then the content copied from the image with CopyRect, Face where the
/// image does not reach, larger images cropped), then the outline and the switcher. RenderAll draws
/// the whole output the naive way and is the reference the tests compare Compose with.
/// Single-threaded; no allocation; every call is bounded by MaximumWindows, the damage list's
/// capacity and the damaged area.
class Scene final
{
public:
    static constexpr Foundation::Size MaximumWindows{16U};
    static constexpr Foundation::Size NoWindow{~Foundation::Size{0U}};
    static constexpr Foundation::Size MaximumPresentDamage{16U};

    /// Sets the output's size and format, hides every window and the
    /// overlays, forgets every title and image, and damages the output. A
    /// width or height of zero, more than 32,768 pixels, or an invalid format
    /// leaves an inert scene.
    void SetUiText(UiText* ui) noexcept
    {
        m_ui = ui;
        DamageAll();
    }
    void Configure(Foundation::UInt32 width, Foundation::UInt32 height, Graphics::PixelFormat format) noexcept;
    [[nodiscard]] Graphics::Rect Output() const noexcept;
    [[nodiscard]] Graphics::PixelFormat Format() const noexcept;

    /// Shows a hidden slot on top of the z-order. False (nothing changes) for
    /// a slot out of range or already shown.
    [[nodiscard]] bool Show(Foundation::Size slot, Graphics::Rect frame) noexcept;
    /// Hides a shown slot and forgets its image and title; nothing for another slot.
    void Hide(Foundation::Size slot) noexcept;
    [[nodiscard]] bool IsShown(Foundation::Size slot) const noexcept;
    /// Moves a shown slot to the top of the z-order.
    void Raise(Foundation::Size slot) noexcept;
    /// Sets a shown slot's frame (a move, a resize or both).
    void SetFrame(Foundation::Size slot, Graphics::Rect frame) noexcept;
    void SetLook(Foundation::Size slot, const WindowLook& look) noexcept;
    /// The trusted name (1 to TitleNameBytes bytes of printable UTF-8) and
    /// class, from the client's registration. False, changing nothing, for
    /// anything else or a hidden slot.
    [[nodiscard]] bool SetTitle(Foundation::Size slot, const Foundation::UInt8* name, Foundation::Size length,
                                ClientClass client) noexcept;
    /// The client's subtitle (0 to SubtitleBytes bytes; UTF-8 controls and the class symbol, are drawn as SubtitleReplacement).
    [[nodiscard]] bool SetSubtitle(Foundation::Size slot, const Foundation::UInt8* text,
                                   Foundation::Size length) noexcept;
    /// Makes image the slot's current image with up to MaximumPresentDamage
    /// rectangles (image coordinates) of damage; see the class comment. An
    /// unusable image or one whose format is not the output's shows Face
    /// (the compositor refuses such images earlier).
    void Present(Foundation::Size slot, const ImageView& image, const Graphics::Rect* damage,
                 Foundation::Size count) noexcept;

    [[nodiscard]] Foundation::Size Title(Foundation::Size slot,
                                         Foundation::UInt8 (&out)[TitleTextBytes]) const noexcept;

    /// The switcher showing text (at most TitleTextBytes bytes, decoded as UTF-8; the window manager composes it), or none.
    void ShowSwitcher(const Foundation::UInt8* text, Foundation::Size length) noexcept;
    void HideSwitcher() noexcept;
    /// The outline of an outline move, or none.
    void ShowOutline(Graphics::Rect outline) noexcept;
    void HideOutline() noexcept;
    void DamageAll() noexcept;

    /// The z-order, bottom first: Count shown slots.
    [[nodiscard]] Foundation::Size Count() const noexcept;
    [[nodiscard]] Foundation::Size At(Foundation::Size index) const noexcept;
    /// The topmost shown slot whose frame holds point, or NoWindow.
    [[nodiscard]] Foundation::Size TopmostAt(Graphics::Point point) const noexcept;
    [[nodiscard]] Graphics::Rect Frame(Foundation::Size slot) const noexcept;
    [[nodiscard]] WindowLook Look(Foundation::Size slot) const noexcept;
    [[nodiscard]] bool HasDamage() const noexcept;

    /// Composes every damaged rectangle into output (the output's size and
    /// format), then keeps those rectangles as Composed and clears the
    /// damage. Returns the number of rectangles composed; nothing is drawn
    /// into an output of another size or format.
    Foundation::Size Compose(const Graphics::MemorySurface& output) noexcept;
    /// The rectangles the last Compose drew, for the Present to the broker.
    [[nodiscard]] const Graphics::Composition::DamageList& Composed() const noexcept;
    /// Draws the whole output without damage or the occlusion shortcut.
    void RenderAll(const Graphics::MemorySurface& output) const noexcept;

private:
    struct Window
    {
        Graphics::Rect Frame;
        ImageView Image;
        Graphics::Rect Pending;
        WindowLook Look;
        Foundation::UInt8 Name[TitleNameBytes];
        Foundation::UInt8 Subtitle[SubtitleBytes];
        Foundation::UInt8 NameLength;
        Foundation::UInt8 SubtitleLength;
        ClientClass Class;
        bool Shown;
        bool HasImage;
    };

    [[nodiscard]] bool Usable(const Graphics::MemorySurface& output) const noexcept;
    [[nodiscard]] Foundation::Size OrderIndex(Foundation::Size slot) const noexcept;
    [[nodiscard]] Graphics::Rect SwitcherBox() const noexcept;
    void DamageTitle(Foundation::Size slot) noexcept;
    void ComposeRect(const Graphics::MemorySurface& output, Graphics::Rect clip, bool shortcut) const noexcept;
    void PaintWindow(const Graphics::MemorySurface& output, Graphics::Rect clip, const Window& window) const noexcept;

    UiText* m_ui{nullptr};
    Window m_windows[MaximumWindows]{};
    Foundation::UInt8 m_order[MaximumWindows]{};
    Foundation::Size m_count{0U};
    Graphics::Composition::DamageList m_damage{};
    Graphics::Composition::DamageList m_composed{};
    Foundation::UInt32 m_width{0U};
    Foundation::UInt32 m_height{0U};
    Graphics::PixelFormat m_format{Graphics::PixelFormat::Invalid};
    Foundation::UInt8 m_switcherText[TitleTextBytes]{};
    Foundation::Size m_switcherLength{0U};
    bool m_switcher{false};
    Graphics::Rect m_outline{};
    bool m_outlineShown{false};
};

/// The smallest rectangle holding both (either may be empty).
[[nodiscard]] Graphics::Rect UnionRect(Graphics::Rect first, Graphics::Rect second) noexcept;
/// Whether outer holds every pixel of inner (an empty inner is held by any).
[[nodiscard]] bool ContainsRect(Graphics::Rect outer, Graphics::Rect inner) noexcept;
/// Whether text is length bytes of printable UTF-8 (0x20-0x7E).
[[nodiscard]] bool IsPrintableUtf8(const Foundation::UInt8* text, Foundation::Size length) noexcept;
} // Namespace Asteria::User::Session

#endif // ASTERIA_USER_SESSION_SCENE_HPP_INCLUDED
