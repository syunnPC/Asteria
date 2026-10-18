// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_MEMORY_I_TLB_SHOOTDOWN_TRANSPORT_HPP_INCLUDED
#define ASTERIA_HAL_MEMORY_I_TLB_SHOOTDOWN_TRANSPORT_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Memory
{
class TlbShootdownBatch;
}

namespace Asteria::Hal::Memory
{
enum class TlbShootdownTransportError : Foundation::UInt8
{
    None,
    InvalidTarget,
    QueueFull,
    Unavailable,
};

/// Publishes one complete set of remote invalidation work into bounded mailbox storage and requests
/// delivery on each target processor. Contract: - Publish is allocation-free, bounded, nonblocking,
/// and callback-free. - None means a value-copy of every listed work item was retained once. -
/// Every failure means no work from the batch became visible. - Publishing release-synchronizes
/// with target-side receipt before the work is passed to TlbShootdownDispatcher::HandleRemote. - A
/// successful receive transfers, rather than ends, the work's lifetime obligation. The owner joins
/// every remote handler before referenced AddressSpace storage may be quiesced and reclaimed; queue
/// emptiness alone is not a shutdown barrier. - Shutdown processes every still-published target
/// through its remote handler, joins all in-flight handlers, and verifies transport storage empty
/// before reclaiming any referenced transaction. - Publish is not called from an ISR.
class ITlbShootdownTransport
{
public:
    [[nodiscard]] virtual TlbShootdownTransportError
    Publish(const Kernel::Memory::TlbShootdownBatch& batch) noexcept = 0;

protected:
    constexpr ITlbShootdownTransport() noexcept = default;
    ~ITlbShootdownTransport() noexcept = default;
};

static_assert(sizeof(TlbShootdownTransportError) == 1U);
} // Namespace Asteria::Hal::Memory

#endif // ASTERIA_HAL_MEMORY_I_TLB_SHOOTDOWN_TRANSPORT_HPP_INCLUDED
