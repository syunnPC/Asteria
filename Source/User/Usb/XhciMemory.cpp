// SPDX-License-Identifier: Apache-2.0

#include "User/Usb/XhciMemory.hpp"

namespace Asteria::User::Usb
{
void XhciRing::Initialize(const DmaMemory memory) noexcept
{
    m_memory = memory;
    m_index = 0U;
    m_cycle = 1U;
    m_gate = nullptr;
    m_batch = false;
    Clear(memory);
}
void XhciRing::Begin() noexcept
{
    m_batch = true;
    m_gate = nullptr;
}
void XhciRing::Commit() noexcept
{
    if (m_gate != nullptr)
    {
        Store32(m_gate, m_gateControl);
    }
    m_gate = nullptr;
    m_batch = false;
}
UInt64 XhciRing::Push(const Trb trb, const bool chain) noexcept
{
    const UInt64 address = m_memory.Address + m_index * 16U;
    UInt8* const bytes = m_memory.Cpu + m_index * 16U;
    Store64(bytes, trb.Parameter);
    Store32(bytes + 8U, trb.Status);
    const UInt32 control = (trb.Control & ~1U) | m_cycle;
    if (m_batch && (m_gate == nullptr))
    {
        m_gate = bytes + 12U;
        m_gateControl = control;
        Store32(m_gate, control ^ 1U);
    }
    else
    {
        Store32(bytes + 12U, control);
    }
    ++m_index;
    if (m_index == (m_memory.Bytes / 16U - 1U))
    {
        UInt8* const link = m_memory.Cpu + m_index * 16U;
        Store64(link, m_memory.Address);
        Store32(link + 8U, 0U);
        Store32(link + 12U, (6U << 10U) | 2U | (chain ? 16U : 0U) | m_cycle);
        m_index = 0U;
        m_cycle ^= 1U;
    }
    return address;
}
} // namespace Asteria::User::Usb
