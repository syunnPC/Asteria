// SPDX-License-Identifier: Apache-2.0

#include "User/Session/InputOrder.hpp"
#include "User/Session/ClientEvents.hpp"
namespace Asteria::User::Session
{
void InputOrder::Drop() noexcept
{
    for (Foundation::Size i{0U}; i < m_keyCount; ++i)
    {
        m_dropped += m_keyQueue[(m_keyFirst + i) & (Capacity - 1U)].Count;
    }
    m_dropped += m_pointerCount;
    m_keyFirst = 0U;
    m_keyCount = 0U;
    m_pointerFirst = 0U;
    m_pointerCount = 0U;
}
void InputOrder::Configure(const Foundation::UInt64 input, const Foundation::UInt64 face) noexcept
{
    if ((input == m_inputEpoch) && (face == m_faceEpoch) && !m_stale)
    {
        return;
    }
    Drop();
    m_inputEpoch = input;
    m_faceEpoch = face;
    m_keys = 0U;
    m_pointer = 0U;
    m_lastSequence = 0U;
    m_stale = (input == 0U) || (face == 0U);
}
void InputOrder::Resume(const Foundation::UInt64 keys, const Foundation::UInt64 pointer) noexcept
{
    Drop();
    m_keys = keys;
    m_pointer = pointer;
    m_lastSequence = pointer;
}
bool InputOrder::Keys(const Foundation::UInt64 horizon, const Foundation::UInt64 count, const Foundation::UInt64 token,
                      const Foundation::UInt64 now) noexcept
{
    if (m_stale || (m_keyCount == Capacity) || (count == 0U) || (count > 8U))
    {
        return false;
    }
    if ((m_keyCount != 0U) && (horizon < m_keyQueue[(m_keyFirst + m_keyCount - 1U) & (Capacity - 1U)].Dependency))
    {
        return false;
    }
    m_keyQueue[(m_keyFirst + m_keyCount++) & (Capacity - 1U)] = {Kind::Keys, horizon, count, token, now};
    return true;
}
bool InputOrder::Pointer(const Foundation::UInt64 sequence, const Foundation::UInt64 stamp,
                         const Foundation::UInt64 token, const Foundation::UInt64 now) noexcept
{
    if (m_stale || (m_pointerCount == Capacity) || (sequence == 0U) || (sequence <= m_lastSequence))
    {
        return false;
    }
    if ((m_pointerCount != 0U) &&
        (stamp < m_pointerQueue[(m_pointerFirst + m_pointerCount - 1U) & (Capacity - 1U)].Dependency))
    {
        return false;
    }
    m_lastSequence = sequence;
    m_pointerQueue[(m_pointerFirst + m_pointerCount++) & (Capacity - 1U)] = {Kind::Pointer, stamp, sequence, token,
                                                                             now};
    return true;
}
InputOrder::Item InputOrder::Take(const Foundation::UInt64 now) noexcept
{
    if (((m_keyCount != 0U) && (now >= m_keyQueue[m_keyFirst].Since) &&
         (now - m_keyQueue[m_keyFirst].Since >= ClientTimeoutUs)) ||
        ((m_pointerCount != 0U) && (now >= m_pointerQueue[m_pointerFirst].Since) &&
         (now - m_pointerQueue[m_pointerFirst].Since >= ClientTimeoutUs)))
    {
        Drop();
        m_stale = true;
    }
    if ((m_keyCount != 0U) && (m_keyQueue[m_keyFirst].Dependency <= m_pointer))
    {
        const Item item = m_keyQueue[m_keyFirst];
        if (item.Count > ~Foundation::UInt64{0U} - m_keys)
        {
            Drop();
            m_stale = true;
            return {};
        }
        m_keys += item.Count;
        m_keyFirst = (m_keyFirst + 1U) & (Capacity - 1U);
        --m_keyCount;
        return item;
    }
    if ((m_pointerCount != 0U) && (m_pointerQueue[m_pointerFirst].Dependency <= m_keys))
    {
        const Item item = m_pointerQueue[m_pointerFirst];
        m_pointer = item.Count;
        m_pointerFirst = (m_pointerFirst + 1U) & (Capacity - 1U);
        --m_pointerCount;
        return item;
    }
    return {};
}
} // namespace Asteria::User::Session
