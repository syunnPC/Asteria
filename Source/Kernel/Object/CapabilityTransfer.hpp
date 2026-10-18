// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_OBJECT_CAPABILITY_TRANSFER_HPP_INCLUDED
#define ASTERIA_KERNEL_OBJECT_CAPABILITY_TRANSFER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Object/CapabilityTypes.hpp"

namespace Asteria::Kernel::Object
{
enum class CapabilityTransferDisposition : Foundation::UInt8
{
    Invalid = 0U,
    Move = 1U,
    Copy = 2U,
};

struct CapabilityTransferRequest
{
    CapabilityHandle Source;
    MonotonicTime RequestedDeadline;
    CapabilityRights RequestedRights;
    ObjectType ExpectedType;
    CapabilityTransferDisposition Disposition;
    Foundation::UInt32 Reserved;
};

static_assert(sizeof(CapabilityTransferRequest) == 32U);
static_assert(alignof(CapabilityTransferRequest) == 8U);

struct CapabilityTransferRequestView
{
    const CapabilityTransferRequest* Data;
    Foundation::Size Count;
};

static_assert(sizeof(CapabilityTransferRequestView) == 16U);
static_assert(alignof(CapabilityTransferRequestView) == 8U);

struct CapabilityTransferBatch
{
    static constexpr Foundation::Size MaximumTransfers{4U};

    CapabilityHandle DestinationHandles[MaximumTransfers];
    Foundation::UInt8 Count;
    Foundation::UInt8 Reserved[7];
};

static_assert(sizeof(CapabilityTransferBatch) == 72U);
static_assert(alignof(CapabilityTransferBatch) == 8U);
} // namespace Asteria::Kernel::Object

#endif // ASTERIA_KERNEL_OBJECT_CAPABILITY_TRANSFER_HPP_INCLUDED
