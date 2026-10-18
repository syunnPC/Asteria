// SPDX-License-Identifier: Apache-2.0

#include "User/Input/Ps2Mouse.hpp"

namespace Asteria::User::Input
{
void Ps2Mouse::Reset(const MouseMode mode) noexcept
{
    m_mode = mode;
    m_at = 0U;
    m_buttons = 0U;
}

MouseResult Ps2Mouse::Feed(const UInt8 byte, EventBatch& output) noexcept
{
    output.Count = 0U;
    if ((m_at == 0U) && ((byte & 0x08U) == 0U))
    {
        return MouseResult::Unsynchronised;
    }
    m_bytes[m_at++] = byte;
    const UInt8 length = (m_mode == MouseMode::IntelliMouse) ? 4U : 3U;
    if (m_at < length)
    {
        return MouseResult::Pending;
    }
    m_at = 0U;
    const UInt8 flags = m_bytes[0U];
    const bool overflow = (flags & 0xC0U) != 0U;
    const Int32 dx = overflow ? 0 : static_cast<Int32>(m_bytes[1U]) - ((flags & 0x10U) != 0U ? 256 : 0);
    const Int32 dy = overflow ? 0 : static_cast<Int32>(m_bytes[2U]) - ((flags & 0x20U) != 0U ? 256 : 0);
    const Int32 wheel = (length == 4U) ? -SignedByte(m_bytes[3U]) : 0;
    PointerEvents(output, m_source, dx, -dy, static_cast<UInt8>(flags & 7U), wheel, m_buttons);
    return overflow ? MouseResult::Overflow : MouseResult::Packet;
}
} // namespace Asteria::User::Input
