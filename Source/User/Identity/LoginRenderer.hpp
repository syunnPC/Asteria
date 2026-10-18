// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_IDENTITY_LOGIN_RENDERER_HPP_INCLUDED
#define ASTERIA_USER_IDENTITY_LOGIN_RENDERER_HPP_INCLUDED
#include "User/Identity/TrustedScreen.hpp"
#include "User/Graphics/BitmapFont.hpp"
namespace Asteria::User::Identity
{
inline constexpr Graphics::Color LoginBackground{0x10U, 0x20U, 0x40U};
inline constexpr Graphics::Color LoginForeground{0xFFU, 0xFFU, 0xFFU};
// Contains only public presentation. The password is never a rendering input.
struct LoginPresentation
{
    ScreenState State{ScreenState::AwaitingAttention};
    EditState Edit{EditState::Editing};
    TextView Account{};
    Size MaskCharacters{0U};
    bool PasswordFocus{false};
    bool Jis{false};
    UInt64 RetrySeconds{0U};
};
void RenderLogin(const Graphics::MemorySurface& target, const LoginPresentation& presentation) noexcept;
} // namespace Asteria::User::Identity
#endif
