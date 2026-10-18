// SPDX-License-Identifier: Apache-2.0

#include "User/Graphics/Composition/PlaneStorage.hpp"

namespace Asteria::User::Graphics::Composition
{
namespace
{
struct Placement
{
    void* Address;
};
} // namespace
} // namespace Asteria::User::Graphics::Composition

// Private placement overload confines lifetime construction to this owner;
// it neither allocates nor interposes the hosted standard placement overload.
[[nodiscard]] inline void* operator new(Asteria::Foundation::Size,
                                        Asteria::User::Graphics::Composition::Placement place) noexcept
{
    return place.Address;
}

namespace Asteria::User::Graphics::Composition
{
void PlaneStorage::Initialize(const Console::TextConsole& console) noexcept
{
    m_background = new (Placement{m_backgroundBytes}) BackgroundPlane{Console::ConsoleBackground};
    m_session = new (Placement{m_sessionBytes}) SessionPlane{};
    m_console = new (Placement{m_consoleBytes}) ConsolePlane{console};
    m_cursor = new (Placement{m_cursorBytes}) CursorPlane{};
    m_trusted = new (Placement{m_trustedBytes}) Identity::TrustedPlane{};
}
BackgroundPlane& PlaneStorage::Background() const noexcept
{
    return *m_background;
}
SessionPlane& PlaneStorage::Session() const noexcept
{
    return *m_session;
}
ConsolePlane& PlaneStorage::Console() const noexcept
{
    return *m_console;
}
CursorPlane& PlaneStorage::Cursor() const noexcept
{
    return *m_cursor;
}
Identity::TrustedPlane& PlaneStorage::Trusted() const noexcept
{
    return *m_trusted;
}
} // namespace Asteria::User::Graphics::Composition
