// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_SYNCHRONIZATION_DEVICE_MEMORY_BARRIER_HPP_INCLUDED
#define ASTERIA_PLATFORM_SYNCHRONIZATION_DEVICE_MEMORY_BARRIER_HPP_INCLUDED

namespace Asteria::Platform::Synchronization
{
/// Full ordering of coherent DMA RAM and MMIO accesses, including store/load.
/// GCC/Clang's full barrier intrinsic is confined to this platform adapter;
/// unlike atomic_thread_fence it is supported by the hosted TSan profile.
/// It emits an AMD64 locked fence or IA-64 mf plus a compiler
/// barrier. Cache maintenance for noncoherent RAM remains the caller's job.
/// Bounded, allocation-free, and safe in interrupt context.
inline void DeviceMemoryBarrier() noexcept
{
    __sync_synchronize();
}
/// Drain and order scanout WC stores before publishing a rendered image.
/// AMD64 SFENCE orders WC writes; IA-64 MF.A accepts them, then MF orders
/// coherent publication. Hosted builds use the supported full fence intrinsic.
inline void OrderScanoutStorage() noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    __asm__ __volatile__("sfence" : : : "memory");
#elif defined(__ia64__)
    __asm__ __volatile__("mf.a;; mf" : : : "memory");
#else
    __sync_synchronize();
#endif
}
} // namespace Asteria::Platform::Synchronization

#endif // ASTERIA_PLATFORM_SYNCHRONIZATION_DEVICE_MEMORY_BARRIER_HPP_INCLUDED
