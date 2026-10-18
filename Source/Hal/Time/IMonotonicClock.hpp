// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_TIME_I_MONOTONIC_CLOCK_HPP_INCLUDED
#define ASTERIA_HAL_TIME_I_MONOTONIC_CLOCK_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Hal::Time
{
/// One fixed-frequency clock domain shared by every deadline consumer that
/// receives this exact instance.
class IMonotonicClock
{
public:
    /// Returns ticks since the current boot using the implementation's fixed
    /// tick period.
    /// Preconditions for implementations:
    /// - Finite values are nondecreasing. The all-ones value is reserved as a
    /// terminal clock-exhaustion signal and is never a usable timestamp;
    /// its observer must fail-stop the owning time domain.
    /// - The operation is thread-safe, bounded, nonblocking, and
    /// allocation-free.
    /// - The operation acquires no lock and invokes no callback or kernel
    /// service.
    /// Interrupt safety:
    /// - Implementations are callable with local interrupts disabled.
    [[nodiscard]] virtual Foundation::UInt64 GetTicks() const noexcept = 0;

protected:
    /// Clock storage is externally owned and is never deleted through this
    /// interface in the freestanding kernel.
    constexpr IMonotonicClock() noexcept = default;
    ~IMonotonicClock() noexcept = default;
};
} // Namespace Asteria::Hal::Time

#endif // ASTERIA_HAL_TIME_I_MONOTONIC_CLOCK_HPP_INCLUDED
