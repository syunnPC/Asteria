// SPDX-License-Identifier: Apache-2.0

#include "User/Identity/LoginRenderer.hpp"
#include "User/Graphics/Blit.hpp"
#include "User/Text/Utf8.hpp"
#include "User/Terminal/Cp437.hpp"
namespace Asteria::User::Identity
{
namespace
{
#define ASTERIA_LOGIN_STRING(name, value) constexpr char name[]{value};
#include "../../../Data/Strings/Login.en-US.inc"
#undef ASTERIA_LOGIN_STRING
void Text(const Graphics::MemorySurface& target, Foundation::Int32 x, Foundation::Int32 y, TextView text) noexcept
{
    User::Text::Utf8Decoder decoder{};
    for (Size i{0U}; i < text.Length && x < static_cast<Foundation::Int32>(target.Width); ++i)
    {
        if (!decoder.Feed(text.Bytes[i]))
        {
            return;
        }
        if (!decoder.Complete())
        {
            continue;
        }
        const auto point = decoder.Point();
        Graphics::DrawGlyph(target, x, y, Graphics::Bold8x16, Terminal::MapToCp437(point), LoginForeground,
                            LoginBackground);
        x += 8;
    }
}
template <Size N> void Line(const Graphics::MemorySurface& target, Foundation::Int32 y, const char (&text)[N]) noexcept
{
    Text(target, 24, y, {reinterpret_cast<const UInt8*>(text), N - 1U});
}
} // namespace
void RenderLogin(const Graphics::MemorySurface& target, const LoginPresentation& p) noexcept
{
    Graphics::FillRect(target, Graphics::Bounds(target), LoginBackground);
    Line(target, 24, Title);
    Line(target, 360, Fixture);
    switch (p.State)
    {
    case ScreenState::AwaitingAttention:
        Line(target, 136, Attention);
        return;
    case ScreenState::StartingSession:
        Line(target, 136, Starting);
        return;
    case ScreenState::LoggingOut:
        Line(target, 136, LoggingOut);
        return;
    case ScreenState::Recovery:
        Line(target, 136, Recovery);
        return;
    case ScreenState::Login:
    case ScreenState::Locked:
        break;
    default:
        return;
    }
    if (p.State == ScreenState::Locked)
    {
        Line(target, 72, Locked);
    }
    if (p.Jis)
    {
        Line(target, 296, KeyboardJis);
    }
    else
    {
        Line(target, 296, KeyboardUs);
    }
    Line(target, 120, User);
    Text(target, 128, 120, p.Account);
    Line(target, 168, Password);
    const Size masks = p.MaskCharacters < 55U ? p.MaskCharacters : 55U;
    for (Size i{0U}; i < masks; ++i)
    {
        Graphics::DrawGlyph(target, 128 + static_cast<Foundation::Int32>(8U * i), 168, Graphics::Bold8x16, '*',
                            LoginForeground, LoginBackground);
    }
    if (p.Edit == EditState::Verifying)
    {
        Line(target, 232, Verifying);
    }
    else if (p.Edit == EditState::RetryAt)
    {
        Line(target, 232, Retry);
        Line(target, 256, RetryTime);
        UInt8 digits[20U]{};
        Size first = sizeof(digits);
        UInt64 seconds = p.RetrySeconds;
        do
        {
            digits[--first] = static_cast<UInt8>('0' + seconds % 10U);
            seconds /= 10U;
        } while (seconds != 0U && first != 0U);
        Text(target, 24 + static_cast<Foundation::Int32>((sizeof(RetryTime) - 1U) * 8U), 256,
             {digits + first, sizeof(digits) - first});
    }
    else
    {
        if (p.State == ScreenState::Locked)
        {
            Line(target, 232, Unlock);
        }
        else
        {
            Line(target, 232, SignIn);
        }
        Size characters{0U};
        for (Size i = 0U; i < p.Account.Length; ++i)
        {
            if ((p.Account.Bytes[i] & 0xC0U) != 0x80U)
            {
                ++characters;
            }
        }
        const auto count = p.PasswordFocus ? masks : characters;
        Graphics::FillRect(target,
                           {.X = 128 + static_cast<Foundation::Int32>(8U * (count < 55U ? count : 55U)),
                            .Y = p.PasswordFocus ? 168 : 120,
                            .Width = 8U,
                            .Height = 16U},
                           LoginForeground);
    }
}
} // namespace Asteria::User::Identity
