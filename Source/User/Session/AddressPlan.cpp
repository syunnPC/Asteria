// SPDX-License-Identifier: Apache-2.0

#include "User/Session/AddressPlan.hpp"
namespace Asteria::User::Session
{
void AddressPlan::Configure(const Foundation::UInt32 budget, const Foundation::UInt32 seed,
                            const Foundation::UInt32 records, const bool lifetime) noexcept
{
    for (Foundation::Size i{0U}; i < 9U; ++i)
    {
        m_high[i] = 0U;
        m_live[i] = 0U;
    }
    m_budget = budget;
    m_tables = seed;
    m_records = records;
    m_lifetime = lifetime;
    m_directory = false;
}
bool AddressPlan::Map(const Foundation::Size slot, const Foundation::UInt32 pool, const Foundation::UInt32 pages,
                      const Foundation::UInt32 frames) noexcept
{
    if ((slot > 4U) || (pool < 1U) || (pool > (slot == 4U ? 1U : 2U)) || (pages == 0U) ||
        (pages > SessionMaximumPoolPages) || (frames == 0U) || (frames > SessionMaximumPoolFrames) ||
        (pages < frames) || (pages > frames * SessionMaximumFramePages))
    {
        return false;
    }
    const Foundation::Size index = slot * 2U + pool - 1U;
    const Foundation::UInt32 leaves = (pages + 511U) >> 9U;
    const Foundation::UInt32 extra = (leaves > m_high[index] ? leaves - m_high[index] : 0U) + (m_directory ? 0U : 1U);
    if ((m_live[index] != 0U) || (m_tables > m_budget) || (extra > m_budget - m_tables) || (m_records > 256U) ||
        (frames > 256U - m_records))
    {
        return false;
    }
    m_tables += extra;
    m_directory = true;
    if (leaves > m_high[index])
    {
        m_high[index] = leaves;
    }
    m_live[index] = frames;
    m_records += frames;
    return true;
}
bool AddressPlan::Unmap(const Foundation::Size slot, const Foundation::UInt32 pool) noexcept
{
    if ((slot > 4U) || (pool < 1U) || (pool > (slot == 4U ? 1U : 2U)))
    {
        return false;
    }
    const Foundation::Size index = slot * 2U + pool - 1U;
    if ((m_live[index] == 0U) || (m_lifetime && (m_live[index] > 256U - m_records)))
    {
        return false;
    }
    if (m_lifetime)
    {
        m_records += m_live[index];
    }
    else
    {
        m_records -= m_live[index];
    }
    m_live[index] = 0U;
    return true;
}
} // namespace Asteria::User::Session
