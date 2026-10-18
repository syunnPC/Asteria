// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_GRAPHICS_COMPOSITION_PLANE_STORAGE_HPP_INCLUDED
#define ASTERIA_USER_GRAPHICS_COMPOSITION_PLANE_STORAGE_HPP_INCLUDED

#include "User/Graphics/Composition/SessionPlane.hpp"
#include "User/Identity/TrustedPlane.hpp"

namespace Asteria::User::Graphics::Composition
{
/// Explicit lifetime owner for the broker's planes. Zero-initialized storage
/// avoids writable pointer relocations in the freestanding component image.
/// Initialize on the serving thread before borrowing any plane; reinitializing
/// ends the previous resource-free lifetimes. No allocation or lazy guards.
class PlaneStorage final
{
public:
    constexpr PlaneStorage() noexcept = default;
    PlaneStorage(const PlaneStorage&) = delete;
    PlaneStorage& operator=(const PlaneStorage&) = delete;
    PlaneStorage(PlaneStorage&&) = delete;
    PlaneStorage& operator=(PlaneStorage&&) = delete;
    void Initialize(const Console::TextConsole& console) noexcept;
    [[nodiscard]] BackgroundPlane& Background() const noexcept;
    [[nodiscard]] SessionPlane& Session() const noexcept;
    [[nodiscard]] ConsolePlane& Console() const noexcept;
    [[nodiscard]] CursorPlane& Cursor() const noexcept;
    [[nodiscard]] Identity::TrustedPlane& Trusted() const noexcept;

private:
    alignas(BackgroundPlane) Foundation::UInt8 m_backgroundBytes[sizeof(BackgroundPlane)]{};
    alignas(SessionPlane) Foundation::UInt8 m_sessionBytes[sizeof(SessionPlane)]{};
    alignas(ConsolePlane) Foundation::UInt8 m_consoleBytes[sizeof(ConsolePlane)]{};
    alignas(CursorPlane) Foundation::UInt8 m_cursorBytes[sizeof(CursorPlane)]{};
    alignas(Identity::TrustedPlane) Foundation::UInt8 m_trustedBytes[sizeof(Identity::TrustedPlane)]{};
    BackgroundPlane* m_background{nullptr};
    SessionPlane* m_session{nullptr};
    ConsolePlane* m_console{nullptr};
    CursorPlane* m_cursor{nullptr};
    Identity::TrustedPlane* m_trusted{nullptr};
};
} // namespace Asteria::User::Graphics::Composition
#endif // ASTERIA_USER_GRAPHICS_COMPOSITION_PLANE_STORAGE_HPP_INCLUDED
