// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_MEMORY_I_TLB_SHOOTDOWN_NOTIFIER_HPP_INCLUDED
#define ASTERIA_HAL_MEMORY_I_TLB_SHOOTDOWN_NOTIFIER_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Hal::Memory
{
/// Requests interrupt-side draining of already published TLB-shootdown work. Contract: -
/// targetProcessorMask is nonzero and contains only online processors. - Every target's mailbox
/// work has been release-published before this call. - The request is allocation-free, wait-free,
/// callback-free, and cannot fail. Multiple requests may be coalesced by the platform. - Calls from
/// different processors may overlap. - Each target eventually enters its processor-local drain
/// path, whose acquire load observes the published work before invalidating locally. - Shutdown
/// disables new asynchronous notification, synchronously drains previously published work, and
/// joins every entered drain path; merely observing empty mailbox storage does not end in-flight
/// work.
class ITlbShootdownNotifier
{
public:
    virtual void Notify(Foundation::ProcessorMask targetProcessorMask) noexcept = 0;

protected:
    constexpr ITlbShootdownNotifier() noexcept = default;
    ~ITlbShootdownNotifier() noexcept = default;
};
} // Namespace Asteria::Hal::Memory

#endif // ASTERIA_HAL_MEMORY_I_TLB_SHOOTDOWN_NOTIFIER_HPP_INCLUDED
