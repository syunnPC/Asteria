// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SCHEDULER_THREAD_REQUEST_HPP_INCLUDED
#define ASTERIA_KERNEL_SCHEDULER_THREAD_REQUEST_HPP_INCLUDED

#include "Kernel/Scheduler/Thread.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"

namespace Asteria::Kernel::Scheduler
{
enum class ThreadRequestKind : Foundation::UInt64
{
    None,
    Bind,
    Terminate,
    // Internal ownership states; no requester waits for any of them.
    Free,
    Reserved,
    Serving,
};

/// One CAS request word per native thread slot. Reserve owns unpublished storage. Serving owns the
/// immutable payload; retirement reserves None before freeing it, so a poster cannot race reuse.
class ThreadRequest final
{
public:
    [[nodiscard]] bool Reserve() noexcept
    {
        return !Platform::Synchronization::ProcessorPowerStopping() &&
               Claim(ThreadRequestKind::Free, ThreadRequestKind::Reserved);
    }
    void PublishReserved(Thread& thread, const CpuId home) noexcept
    {
        __atomic_store_n(&m_thread, &thread, __ATOMIC_RELEASE);
        __atomic_store_n(&m_home, home.GetValue(), __ATOMIC_RELEASE);
    }
    void Publish(Thread& thread, const CpuId home, const bool remote) noexcept
    {
        __atomic_store_n(&m_thread, &thread, __ATOMIC_RELEASE);
        __atomic_store_n(&m_home, home.GetValue(), __ATOMIC_RELEASE);
        Store(remote ? ThreadRequestKind::Bind : ThreadRequestKind::None);
    }
    [[nodiscard]] bool Matches(const Thread& thread) const noexcept
    {
        return __atomic_load_n(&m_thread, __ATOMIC_ACQUIRE) == &thread;
    }
    [[nodiscard]] CpuId Home() const noexcept
    {
        return CpuId{__atomic_load_n(&m_home, __ATOMIC_ACQUIRE)};
    }
    [[nodiscard]] bool PostTerminate(Thread& thread, const Foundation::UInt64 exitCode,
                                     CpuId* const home = nullptr) noexcept
    {
        if (Platform::Synchronization::ProcessorPowerStopping() ||
            !Claim(ThreadRequestKind::None, ThreadRequestKind::Reserved))
        {
            return false;
        }
        if (!Matches(thread))
        {
            Store(ThreadRequestKind::None);
            return false;
        }
        if (home != nullptr)
        {
            *home = Home();
        }
        m_exitCode = exitCode;
        Store(ThreadRequestKind::Terminate);
        return true;
    }
    [[nodiscard]] ThreadRequestKind Take(const CpuId processor, const Thread* const protectedThread) noexcept
    {
        // Avoid claiming another home's word; revalidate after the CAS to
        // cover retirement/reuse between this fast check and ownership.
        if (Platform::Synchronization::ProcessorPowerStopping() || !processor.IsValid() ||
            processor.GetValue() != Home().GetValue())
        {
            return ThreadRequestKind::None;
        }
        const auto request = static_cast<ThreadRequestKind>(__atomic_load_n(&m_word, __ATOMIC_ACQUIRE));
        if ((request != ThreadRequestKind::Bind && request != ThreadRequestKind::Terminate) ||
            !Claim(request, ThreadRequestKind::Serving))
        {
            return ThreadRequestKind::None;
        }
        // Validate the payload after claiming: retirement/reuse could have
        // replaced the request between the initial load and the CAS.
        if (processor.GetValue() != Home().GetValue() ||
            (request == ThreadRequestKind::Terminate &&
             __atomic_load_n(&m_thread, __ATOMIC_ACQUIRE) == protectedThread))
        {
            Store(request);
            return ThreadRequestKind::None;
        }
        return request;
    }
    [[nodiscard]] Foundation::UInt64 ExitCode() const noexcept
    {
        return m_exitCode;
    }
    void Complete() noexcept
    {
        Store(ThreadRequestKind::None);
    }
    void Defer(const ThreadRequestKind request) noexcept
    {
        Store(request);
    }
    [[nodiscard]] bool Retire(const Thread& thread) noexcept
    {
        if (Platform::Synchronization::ProcessorPowerStopping() ||
            !Claim(ThreadRequestKind::None, ThreadRequestKind::Reserved))
        {
            return false;
        }
        if (Matches(thread))
        {
            return true;
        }
        Store(ThreadRequestKind::None);
        return false;
    }
    void Release() noexcept
    {
        __atomic_store_n(&m_thread, static_cast<Thread*>(nullptr), __ATOMIC_RELEASE);
        Store(ThreadRequestKind::Free);
    }

private:
    [[nodiscard]] bool Claim(const ThreadRequestKind from, const ThreadRequestKind to) noexcept
    {
        auto expected = static_cast<Foundation::UInt64>(from);
        return __atomic_compare_exchange_n(&m_word, &expected, static_cast<Foundation::UInt64>(to), false,
                                           __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    void Store(const ThreadRequestKind value) noexcept
    {
        __atomic_store_n(&m_word, static_cast<Foundation::UInt64>(value), __ATOMIC_RELEASE);
    }
    Foundation::UInt64 m_word{static_cast<Foundation::UInt64>(ThreadRequestKind::Free)};
    Thread* m_thread{};
    Foundation::UInt64 m_exitCode{};
    Foundation::UInt8 m_home{CpuId{}.GetValue()};
};
static_assert(sizeof(ThreadRequest) == 32U);
} // Namespace Asteria::Kernel::Scheduler
#endif
