// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_I_PAGER_RESOLUTION_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_I_PAGER_RESOLUTION_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/FaultFrameMapping.hpp"
#include "Kernel/Memory/FaultToken.hpp"
#include "Kernel/Object/CapabilityTypes.hpp"

namespace Asteria::Kernel::Object
{
class CapabilitySpace;
}

namespace Asteria::Kernel::Memory
{
enum class PagerResolutionError : Foundation::UInt8
{
    None,
    /// The resolution kind is neither Map nor Terminate, the request has
    /// nonzero reserved bytes, or a Terminate names a mapping.
    InvalidRequest,
    /// A handle failed its lookup; Capability holds the reason.
    CapabilityRejected,
    /// The FaultToken names no fault awaiting this pager's resolution.
    NotPending,
    /// The AddressSpace handle names another space than the one that faulted.
    AddressSpaceMismatch,
    /// The mapping does not cover the fault or exceeds its objects, cache
    /// policy, access, or W^X, or the fault is an exception, which no
    /// mapping resolves. The fault stays pending.
    MappingRejected,
    /// The kernel could not finish an authorized resolution.
    Faulted,
    Busy,
};

struct PagerResolutionResult
{
    PagerResolutionError Error;
    Object::CapabilityError Capability;
    Foundation::UInt8 Reserved[6];
};

/// One pager request, decoded from user memory. The handles name objects in
/// the caller's capability space: the delivered FaultToken (Invoke), the
/// AddressSpace that faulted, and the MemoryObject and Frame to map (each
/// Map). A Terminate names only the FaultToken; every other field is zero.
struct PagerResolveRequest
{
    Object::CapabilityHandle FaultToken;
    Object::CapabilityHandle AddressSpace;
    Object::CapabilityHandle MemoryObject;
    Object::CapabilityHandle Frame;
    FaultFrameMappingRequest Mapping;
    FaultResolutionKind Kind;
    Foundation::UInt8 Reserved[7];
};

/// The capability layer between a pager's FaultResolve system call and the
/// fault it resolves. The implementation proves the caller's authority from
/// its own capability space; nothing in the request is trusted.
class IPagerResolution
{
public:
    [[nodiscard]] virtual PagerResolutionResult Resolve(Object::CapabilitySpace& caller,
                                                        const PagerResolveRequest& request) noexcept = 0;

protected:
    constexpr IPagerResolution() noexcept = default;
    ~IPagerResolution() noexcept = default;
};

static_assert(sizeof(PagerResolutionResult) == 8U);
} // namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_I_PAGER_RESOLUTION_HPP_INCLUDED
