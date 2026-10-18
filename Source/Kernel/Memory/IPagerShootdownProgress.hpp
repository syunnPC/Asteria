// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_I_PAGER_SHOOTDOWN_PROGRESS_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_I_PAGER_SHOOTDOWN_PROGRESS_HPP_INCLUDED

namespace Asteria::Kernel::Memory
{
/// Waits once for remote processors to make progress on published
/// translation-invalidation work, for example by a bounded delay.
class IPagerShootdownProgress
{
public:
    virtual void Wait() noexcept = 0;

protected:
    constexpr IPagerShootdownProgress() noexcept = default;
    ~IPagerShootdownProgress() noexcept = default;
};
} // namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_I_PAGER_SHOOTDOWN_PROGRESS_HPP_INCLUDED
