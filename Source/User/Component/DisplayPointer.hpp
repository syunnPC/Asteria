// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_DISPLAY_POINTER_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_DISPLAY_POINTER_HPP_INCLUDED

#include "User/Component/DisplayFrame.hpp"
#include "User/Graphics/Surface.hpp"

namespace Asteria::User::Component
{
/// Broker-owned logical position and sequence. Configure starts centred.
/// Apply rejects stale epoch/sequence atomically. Relative addition uses 64 bits;
/// normalized absolute coordinates map inclusively to the output's last pixel.
/// Supports one output.
class DisplayPointer final
{
public:
    void Configure(Foundation::UInt32 width, Foundation::UInt32 height, Foundation::UInt64 epoch) noexcept
    {
        m_width = width;
        m_height = height;
        m_epoch = epoch;
        m_position = {.X = static_cast<Foundation::Int32>(width / 2U),
                      .Y = static_cast<Foundation::Int32>(height / 2U)};
        m_sequence = 0U;
    }
    [[nodiscard]] bool Apply(const Foundation::UInt64* words) noexcept
    {
        if (!IsDisplayPointer(words) || (words[0U] != m_epoch) || (words[1U] <= m_sequence) || (m_width == 0U) ||
            (m_height == 0U))
        {
            return false;
        }
        Foundation::Int64 x{m_position.X};
        Foundation::Int64 y{m_position.Y};
        if (words[2U] == DisplayRelativeMotion)
        {
            x += Signed(static_cast<Foundation::UInt32>(words[3U]));
            y += Signed(static_cast<Foundation::UInt32>(words[3U] >> 32U));
        }
        else if (words[2U] == DisplayAbsoluteMotion)
        {
            x = static_cast<Foundation::Int64>((words[3U] & 0xFFFFFFFFU) * (m_width - 1U) / 65535U);
            y = static_cast<Foundation::Int64>((words[3U] >> 32U) * (m_height - 1U) / 65535U);
        }
        m_position = {.X = Clamp(x, m_width), .Y = Clamp(y, m_height)};
        m_sequence = words[1U];
        return true;
    }
    [[nodiscard]] Graphics::Point Position() const noexcept
    {
        return m_position;
    }
    [[nodiscard]] Foundation::UInt64 Sequence() const noexcept
    {
        return m_sequence;
    }

private:
    [[nodiscard]] static Foundation::Int32 Signed(Foundation::UInt32 value) noexcept
    {
        return value <= 0x7FFFFFFFU ? static_cast<Foundation::Int32>(value)
                                    : -1 - static_cast<Foundation::Int32>(0xFFFFFFFFU - value);
    }
    [[nodiscard]] static Foundation::Int32 Clamp(Foundation::Int64 value, Foundation::UInt32 extent) noexcept
    {
        if (value < 0)
        {
            return 0;
        }
        return value >= extent ? static_cast<Foundation::Int32>(extent - 1U) : static_cast<Foundation::Int32>(value);
    }
    Graphics::Point m_position{};
    Foundation::UInt32 m_width{0U};
    Foundation::UInt32 m_height{0U};
    Foundation::UInt64 m_epoch{0U};
    Foundation::UInt64 m_sequence{0U};
};
} // namespace Asteria::User::Component
#endif // ASTERIA_USER_COMPONENT_DISPLAY_POINTER_HPP_INCLUDED
