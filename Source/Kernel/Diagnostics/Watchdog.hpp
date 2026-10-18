// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DIAGNOSTICS_WATCHDOG_HPP_INCLUDED
#define ASTERIA_KERNEL_DIAGNOSTICS_WATCHDOG_HPP_INCLUDED

#include "Abi/Watchdog.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Kernel::Diagnostics
{
using Scheduler::MaximumProcessorCount;
struct WatchdogProcessorEvidence
{
    Foundation::UInt64 AgeMilliseconds;
    Foundation::UInt64 HeartbeatSite;
    Foundation::UInt64 Location;
    Foundation::UInt64 Thread;
    Foundation::UInt64 Component;
    Foundation::UInt64 Captured;
};
struct WatchdogEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 PeriodSeconds;
    Foundation::UInt64 StalledMask;
    Foundation::UInt64 Detector;
    WatchdogProcessorEvidence Processors[MaximumProcessorCount];
};
static_assert(sizeof(WatchdogProcessorEvidence) == 48U);
static_assert(__builtin_offsetof(WatchdogProcessorEvidence, AgeMilliseconds) == 0U);
static_assert(__builtin_offsetof(WatchdogProcessorEvidence, HeartbeatSite) == 8U);
static_assert(__builtin_offsetof(WatchdogProcessorEvidence, Location) == 16U);
static_assert(__builtin_offsetof(WatchdogProcessorEvidence, Thread) == 24U);
static_assert(__builtin_offsetof(WatchdogProcessorEvidence, Component) == 32U);
static_assert(__builtin_offsetof(WatchdogProcessorEvidence, Captured) == 40U);
static_assert(__builtin_offsetof(WatchdogEvidence, Published) == 0U);
static_assert(__builtin_offsetof(WatchdogEvidence, PeriodSeconds) == 8U);
static_assert(__builtin_offsetof(WatchdogEvidence, StalledMask) == 16U);
static_assert(__builtin_offsetof(WatchdogEvidence, Detector) == 24U);

static_assert(sizeof(WatchdogEvidence) == 224U);
static_assert(__builtin_offsetof(WatchdogEvidence, Processors) == 32U);

/// Wait-free owner publications, bounded peer scans. Observer clocks are local; no peer counter or
/// synchronized TSC is assumed.
class Watchdog final
{
public:
    void Configure(const Foundation::UInt64 seconds, const Foundation::UInt64 frequency) noexcept
    {
        m_seconds = seconds <= Abi::WatchdogMaximumSeconds ? seconds : Abi::WatchdogDefaultSeconds;
        m_frequency = frequency;
        m_period = frequency <= ~Foundation::UInt64{0U} / Abi::WatchdogMaximumSeconds ? frequency * m_seconds : 0U;
    }
    [[nodiscard]] Foundation::UInt64 Seconds() const noexcept
    {
        return m_seconds;
    }
    [[nodiscard]] Foundation::UInt64 CheckTicks() const noexcept
    {
        return m_frequency / Abi::WatchdogChecksPerSecond;
    }
    void Online(const Foundation::Size cpu, const Foundation::UInt64 site) noexcept
    {
        if (cpu >= Scheduler::MaximumProcessorCount)
        {
            return;
        }
        for (auto& observation : m_observers[cpu])
        {
            observation = {};
        }
        m_progress[cpu] = {};
        Beat(cpu, site);
        m_online.FetchOrRelease(Foundation::UInt64{1U} << cpu);
    }
    void Beat(const Foundation::Size cpu, const Foundation::UInt64 site) noexcept
    {
        if (cpu >= Scheduler::MaximumProcessorCount)
        {
            return;
        }
        auto& heartbeat = m_heartbeats[cpu];
        heartbeat.Site.StoreRelease(site);
        heartbeat.Sequence.StoreRelease(heartbeat.Sequence.LoadAcquire() + 1U);
    }
    void Running(const Foundation::Size cpu, const Foundation::UInt64 thread,
                 const Foundation::UInt64 component) noexcept
    {
        if (cpu >= Scheduler::MaximumProcessorCount)
        {
            return;
        }
        auto& heartbeat = m_heartbeats[cpu];
        const auto version = heartbeat.Version.LoadAcquire();
        heartbeat.Version.StoreRelease(version + 1U);
        heartbeat.Thread.StoreRelease(thread);
        heartbeat.Component.StoreRelease(component);
        heartbeat.Version.StoreRelease(version + 2U);
    }
    [[nodiscard]] Foundation::UInt64 Check(const Foundation::Size cpu, const Foundation::UInt64 now,
                                           const Foundation::UInt64 onlineMask = ~Foundation::UInt64{0U}) noexcept
    {
        if (cpu >= Scheduler::MaximumProcessorCount || m_period == 0U)
        {
            return 0U;
        }
        const auto online = m_online.LoadAcquire() & onlineMask;
        // Watchdog.md / processor stop policy: counter elapsed time alone cannot prove that this
        // observer was running. Credit at most one checking cadence for an advanced heartbeat. An
        // observer absent for a whole period starts fresh, including the first tick after VM
        // suspend or a long early-boot scheduling gap.
        auto& progress = m_progress[cpu];
        const auto ownSequence = m_heartbeats[cpu].Sequence.LoadAcquire();
        const bool reset = !progress.Valid || now < progress.CheckedAt || now - progress.CheckedAt >= m_period ||
                           (online & (Foundation::UInt64{1U} << cpu)) == 0U;
        Foundation::UInt64 credit{0U};
        if (!reset && ownSequence != progress.Sequence)
        {
            const auto elapsed = now - progress.CheckedAt;
            credit = elapsed < CheckTicks() ? elapsed : CheckTicks();
        }
        progress = {ownSequence, now, true};
        Foundation::UInt64 stalled{0U};
        for (Foundation::Size peer{0U}; peer < Scheduler::MaximumProcessorCount; ++peer)
        {
            auto& observation = m_observers[cpu][peer];
            const auto bit = Foundation::UInt64{1U} << peer;
            if ((online & bit) == 0U || peer == cpu)
            {
                observation = {};
                continue;
            }
            const auto sequence = m_heartbeats[peer].Sequence.LoadAcquire();
            if (reset || !observation.Valid || sequence != observation.Sequence)
            {
                observation = {sequence, 0U, true};
            }
            else
            {
                // Saturation bounds uptime arithmetic; diagnostic age is
                // the confirmed period, never time spent outside the guest.
                observation.AgeTicks =
                    credit >= m_period - observation.AgeTicks ? m_period : observation.AgeTicks + credit;
                if (observation.AgeTicks >= m_period)
                {
                    stalled |= bit;
                }
            }
        }
        return stalled;
    }
    [[nodiscard]] Foundation::UInt64 AgeMilliseconds(const Foundation::Size cpu, const Foundation::Size peer,
                                                     const Foundation::UInt64 now) const noexcept
    {
        if (cpu >= Scheduler::MaximumProcessorCount || peer >= Scheduler::MaximumProcessorCount || m_frequency < 1000U)
        {
            return 0U;
        }
        const auto& observation = m_observers[cpu][peer];
        return observation.Valid && m_progress[cpu].Valid && now >= m_progress[cpu].CheckedAt
                   ? observation.AgeTicks / (m_frequency / 1000U)
                   : 0U;
    }
    void Snapshot(WatchdogEvidence& evidence, const Foundation::Size cpu, const Foundation::UInt64 now,
                  const Foundation::UInt64 stalled) const noexcept
    {
        evidence.PeriodSeconds = m_seconds;
        evidence.StalledMask = stalled;
        evidence.Detector = cpu;
        for (Foundation::Size peer{0U}; peer < Scheduler::MaximumProcessorCount; ++peer)
        {
            Sample(evidence.Processors[peer], peer);
            evidence.Processors[peer].AgeMilliseconds = AgeMilliseconds(cpu, peer, now);
        }
    }
    void Sample(WatchdogProcessorEvidence& sample, const Foundation::Size cpu) const noexcept
    {
        const auto& heartbeat = m_heartbeats[cpu];
        const auto version = heartbeat.Version.LoadAcquire();
        sample.HeartbeatSite = heartbeat.Site.LoadAcquire();
        sample.Thread = heartbeat.Thread.LoadAcquire();
        sample.Component = heartbeat.Component.LoadAcquire();
        if ((version & 1U) != 0U || version != heartbeat.Version.LoadAcquire())
        {
            sample.Thread = 0U;
            sample.Component = Abi::WatchdogUpdatingComponent;
        }
        sample.Location = sample.HeartbeatSite;
        sample.Captured = 0U;
    }

private:
    using Atomic = Platform::Synchronization::AtomicUInt64;
    struct Heartbeat
    {
        Atomic Sequence;
        Atomic Site;
        Atomic Version;
        Atomic Thread;
        Atomic Component;
    };
    struct Observation
    {
        Foundation::UInt64 Sequence{};
        Foundation::UInt64 AgeTicks{};
        bool Valid{};
    };
    struct Progress
    {
        Foundation::UInt64 Sequence{};
        Foundation::UInt64 CheckedAt{};
        bool Valid{};
    };
    Heartbeat m_heartbeats[Scheduler::MaximumProcessorCount]{};
    Observation m_observers[Scheduler::MaximumProcessorCount][Scheduler::MaximumProcessorCount]{};
    Progress m_progress[Scheduler::MaximumProcessorCount]{};
    Foundation::UInt64 m_onlineWord{};
    Platform::Synchronization::AtomicUInt64Reference m_online{m_onlineWord};
    Foundation::UInt64 m_seconds{Abi::WatchdogDefaultSeconds};
    Foundation::UInt64 m_frequency{};
    Foundation::UInt64 m_period{};
};
} // Namespace Asteria::Kernel::Diagnostics
#endif
