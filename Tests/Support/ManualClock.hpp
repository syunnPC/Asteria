// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_SUPPORT_MANUAL_CLOCK_HPP_INCLUDED
#define ASTERIA_TESTS_SUPPORT_MANUAL_CLOCK_HPP_INCLUDED

#include <atomic>

#include "Foundation/Types.hpp"
#include "Hal/Time/IMonotonicClock.hpp"

namespace Asteria::TestSupport
{
/// Monotonic clock whose ticks the test sets explicitly. Every read is
/// counted; in advance-on-read mode a read returns the current ticks and then
/// advances them by one. Ticks, mode, and count are sequentially consistent
/// atomics, so concurrent tests may read and update the clock from any thread.
/// The test keeps the ticks nondecreasing unless it exercises a regression.
class ManualClock final : public Hal::Time::IMonotonicClock
{
public:
    explicit constexpr ManualClock(const Foundation::UInt64 ticks = 10U) noexcept : m_ticks{ticks}
    {
    }

    [[nodiscard]] Foundation::UInt64 GetTicks() const noexcept override
    {
        static_cast<void>(m_readCount.fetch_add(1U));
        if (m_advanceOnRead.load())
        {
            return m_ticks.fetch_add(1U);
        }
        return m_ticks.load();
    }

    void Set(const Foundation::UInt64 ticks) noexcept
    {
        m_ticks.store(ticks);
    }

    void Advance(const Foundation::UInt64 ticks) noexcept
    {
        static_cast<void>(m_ticks.fetch_add(ticks));
    }

    void SetAdvanceOnRead(const bool enabled) noexcept
    {
        m_advanceOnRead.store(enabled);
    }

    [[nodiscard]] Foundation::UInt64 GetReadCount() const noexcept
    {
        return m_readCount.load();
    }

    /// Returns the reads since construction or the previous call and restarts
    /// the count.
    [[nodiscard]] Foundation::UInt64 TakeReadCount() noexcept
    {
        return m_readCount.exchange(0U);
    }

private:
    mutable std::atomic<Foundation::UInt64> m_ticks;
    std::atomic<bool> m_advanceOnRead{false};
    mutable std::atomic<Foundation::UInt64> m_readCount{0U};
};
} // namespace Asteria::TestSupport

#endif // ASTERIA_TESTS_SUPPORT_MANUAL_CLOCK_HPP_INCLUDED
