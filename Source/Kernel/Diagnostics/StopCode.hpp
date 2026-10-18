// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DIAGNOSTICS_STOP_CODE_HPP_INCLUDED
#define ASTERIA_KERNEL_DIAGNOSTICS_STOP_CODE_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Diagnostics
{
/// Terminal kernel diagnostics, never a user Status.
enum class StopCode : Foundation::UInt32
{
    BootStage = 1U,
    KernelTrap = 2U,
    ApplicationProcessorStage = 3U,
    InvariantViolated = 4U,
    UnexpectedNmi = 5U,
    TestInjected = 6U,
    ShootdownTimeout = 7U,
    LockOwnerLost = 8U,
    LastProcessorLost = 9U,
    MemoryGrantBelowMinimum = 10U,
    Lockup = 11U,
};

struct StopCodeName
{
    StopCode Code;
    const char* Name;
};

inline constexpr StopCodeName StopCodeNames[]{
    {StopCode::BootStage, "BootStage"},
    {StopCode::KernelTrap, "KernelTrap"},
    {StopCode::ApplicationProcessorStage, "ApplicationProcessorStage"},
    {StopCode::InvariantViolated, "InvariantViolated"},
    {StopCode::UnexpectedNmi, "UnexpectedNmi"},
    {StopCode::TestInjected, "TestInjected"},
    {StopCode::ShootdownTimeout, "ShootdownTimeout"},
    {StopCode::LockOwnerLost, "LockOwnerLost"},
    {StopCode::LastProcessorLost, "LastProcessorLost"},
    {StopCode::MemoryGrantBelowMinimum, "MemoryGrantBelowMinimum"},
    {StopCode::Lockup, "Lockup"},
};

[[nodiscard]] constexpr const char* StopName(const StopCode code) noexcept
{
    for (const auto& entry : StopCodeNames)
    {
        if (entry.Code == code)
        {
            return entry.Name;
        }
    }
    return "UnknownStop";
}
} // Namespace Asteria::Kernel::Diagnostics

#endif // ASTERIA_KERNEL_DIAGNOSTICS_STOP_CODE_HPP_INCLUDED
