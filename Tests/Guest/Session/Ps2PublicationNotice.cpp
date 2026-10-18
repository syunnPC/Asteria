// SPDX-License-Identifier: Apache-2.0
#include "User/Component/Ps2Publication.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "User/Component/InputFrame.hpp"

namespace Asteria::User::Component
{
namespace
{
// Only a broker Done establishes source order. Frozen notices survive WouldBlock and never delay
// the device drain.
struct Publication
{
    Foundation::UInt8 Frame[DisplayMaximumFrameBytes]{};
    Foundation::Size Bytes{0U};
    bool Pointer{false};
    bool Key{false};
    bool FrozenPointer{false};
};
constinit Publication g_publication{};
} // Namespace
void ObservePs2Publication(const Input::InputEvent* const events, const Foundation::Size count) noexcept
{
    for (Foundation::Size i{0U}; i < count && i < InputMaximumEvents; ++i)
    {
        if (events[i].Kind == Input::EventKind::Buttons && events[i].Buttons == 1U)
        {
            g_publication.Pointer = true;
        }
        if (events[i].Kind == Input::EventKind::KeyDown && events[i].Usage == 5U)
        {
            g_publication.Key = true;
        }
    }
}
bool SendPs2Publication(const System::SystemCallClient& client, const System::CapabilityHandle root) noexcept
{
    if (g_publication.Bytes == 0U)
    {
        if (!g_publication.Pointer && !g_publication.Key)
        {
            return true;
        }
        constexpr char pointer[]{"# session-input: source pointer-down\n"};
        constexpr char key[]{"# session-input: source key-b\n"};
        g_publication.FrozenPointer = g_publication.Pointer;
        const auto* const line = g_publication.FrozenPointer ? pointer : key;
        const auto count = g_publication.FrozenPointer ? sizeof(pointer) - 1U : sizeof(key) - 1U;
        g_publication.Bytes =
            WriteDisplayConsoleWrite(g_publication.Frame, 1U, reinterpret_cast<const Foundation::UInt8*>(line), count);
    }
    const auto error = client.Send(root, System::AddressOf(g_publication.Frame), g_publication.Bytes).Error;
    if (error == Abi::SystemCallError::WouldBlock)
    {
        return true;
    }
    if (error != Abi::SystemCallError::None)
    {
        return false;
    }
    if (g_publication.FrozenPointer)
    {
        g_publication.Pointer = false;
    }
    else
    {
        g_publication.Key = false;
    }
    g_publication.Bytes = 0U;
    return true;
}
bool Ps2PublicationPending() noexcept
{
    return g_publication.Bytes != 0U || g_publication.Pointer || g_publication.Key;
}
} // Namespace Asteria::User::Component
