// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SCHEDULER_PROCESSOR_SET_HPP_INCLUDED
#define ASTERIA_KERNEL_SCHEDULER_PROCESSOR_SET_HPP_INCLUDED
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Scheduler
{
enum class ProcessorState : Foundation::UInt8
{
    Starting,
    Online,
    Offlining,
    Offline
};
// One writer under the set lock; readers use acquire snapshots. Offlining itself belongs to step
// (7); the caller must establish processor stop policy invariant 3 before BeginOfflining.
class ProcessorSet final
{
public:
    [[nodiscard]] bool Admit(const CpuId processor) noexcept
    {
        if (!processor.IsValid())
        {
            return false;
        }
        Platform::Synchronization::SpinLockGuard guard{m_lock};
        if (m_states[processor.GetValue()] != ProcessorState::Starting)
        {
            return false;
        }
        m_states[processor.GetValue()] = ProcessorState::Online;
        m_online.StoreRelease(m_online.LoadAcquire() | Bit(processor));
        return true;
    }
    [[nodiscard]] bool BeginOfflining(const CpuId processor) noexcept
    {
        if (!processor.IsValid() || processor.GetValue() == 0U)
        {
            return false;
        }
        Platform::Synchronization::SpinLockGuard guard{m_lock};
        if (m_states[processor.GetValue()] != ProcessorState::Online)
        {
            return false;
        }
        m_states[processor.GetValue()] = ProcessorState::Offlining;
        m_retiring.StoreRelease(m_retiring.LoadAcquire() | Bit(processor));
        m_online.StoreRelease(m_online.LoadAcquire() & ~Bit(processor));
        return true;
    }
    // Called only after forced detach and acknowledgement, before parking.
    [[nodiscard]] bool FinishOfflining(const CpuId processor) noexcept
    {
        if (!processor.IsValid())
        {
            return false;
        }
        Platform::Synchronization::SpinLockGuard guard{m_lock};
        if (m_states[processor.GetValue()] != ProcessorState::Offlining)
        {
            return false;
        }
        m_states[processor.GetValue()] = ProcessorState::Offline;
        return true;
    }
    // Clock exclusion preserves kernel service admission.
    void ExcludeUserPlacement(const CpuId processor) noexcept
    {
        Platform::Synchronization::SpinLockGuard guard{m_lock};
        m_clockExcluded.StoreRelease(m_clockExcluded.LoadAcquire() | Bit(processor));
    }
    // Terminal parking is separate from contained
    // offlining. Close admission before the freeze; retain every interrupted binding, receipt and
    // request, without waiting for a parked owner to release any of them. These paths take no lock.
    void BeginPowerPark() noexcept
    {
        m_powerStopping.StoreRelease(1U);
    }
    [[nodiscard]] bool PowerStopping() const noexcept
    {
        return m_powerStopping.LoadAcquire() != 0U;
    }
    [[nodiscard]] bool MarkPowerParked(const CpuId processor) noexcept
    {
        if (!processor.IsValid() || !PowerStopping())
        {
            return false;
        }
        const auto bit = Bit(processor);
        auto parked = m_powerParked.LoadAcquire();
        // Each failed strong CAS observes another CPU's unique publication;
        // at most MaximumProcessorCount bits can change in this terminal phase.
        for (Foundation::Size attempt{0U}; attempt < MaximumProcessorCount; ++attempt)
        {
            if ((parked & bit) != 0U)
            {
                return true;
            }
            if (m_powerParked.CompareExchangeStrongAcquireRelease(parked, parked | bit))
            {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] Foundation::ProcessorMask PowerParkedMask() const noexcept
    {
        return m_powerParked.LoadAcquire();
    }
    [[nodiscard]] Foundation::ProcessorMask UserPlacementMask() const noexcept
    {
        return PowerStopping() ? 0U : OnlineMask() & ~m_clockExcluded.LoadAcquire();
    }
    [[nodiscard]] Foundation::ProcessorMask ClockExcludedMask() const noexcept
    {
        return m_clockExcluded.LoadAcquire();
    }
    [[nodiscard]] Foundation::ProcessorMask OnlineMask() const noexcept
    {
        return m_online.LoadAcquire() & ~PowerParkedMask();
    }
    [[nodiscard]] Foundation::ProcessorMask SilentOnline(const Foundation::ProcessorMask silent) const noexcept
    {
        // Starting/unknown processors are fatal too: only a previously
        // contained retirement may be dropped, never an arbitrary stall.
        return silent & ~m_retiring.LoadAcquire();
    }
    [[nodiscard]] static constexpr Foundation::ProcessorMask Bit(const CpuId processor) noexcept
    {
        return processor.IsValid() ? Foundation::ProcessorMask{1U} << processor.GetValue() : 0U;
    }

private:
    Platform::Synchronization::SpinLock m_lock{};
    Platform::Synchronization::AtomicUInt64 m_online{};
    Platform::Synchronization::AtomicUInt64 m_retiring{};
    Platform::Synchronization::AtomicUInt64 m_clockExcluded{};
    Platform::Synchronization::AtomicUInt64 m_powerStopping{};
    Platform::Synchronization::AtomicUInt64 m_powerParked{};
    ProcessorState m_states[MaximumProcessorCount]{};
};
} // Namespace Asteria::Kernel::Scheduler
#endif
