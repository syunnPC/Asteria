// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_I_FRAME_SCRUBBER_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_I_FRAME_SCRUBBER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/MemoryTypes.hpp"

namespace Asteria::Kernel::Memory
{
enum class FrameScrubError : Foundation::UInt8
{
    /// Every byte of the run is zero.
    None,
    /// The run cannot be zeroed now; nothing was written.
    Deferred,
    /// The scrubber found its own state corrupt; a chunk is rejected before
    /// any write; earlier successful chunks may have written zeros.
    Faulted,
};

/// Zeroes a run of frame-pool pages the kernel reaches by physical address only. The implementation
/// is the platform's: a temporary window on AMD64, physical mode on IA-64. Callers hold no pool
/// lock and keep the run claimed.
class IFrameScrubber
{
public:
    [[nodiscard]] virtual FrameScrubError Zero(PhysicalAddress physicalStart, Foundation::UInt64 pages) noexcept = 0;

protected:
    constexpr IFrameScrubber() noexcept = default;
    ~IFrameScrubber() noexcept = default;
};

static_assert(sizeof(FrameScrubError) == 1U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_I_FRAME_SCRUBBER_HPP_INCLUDED
