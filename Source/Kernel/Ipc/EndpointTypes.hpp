// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_IPC_ENDPOINT_TYPES_HPP_INCLUDED
#define ASTERIA_KERNEL_IPC_ENDPOINT_TYPES_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Object/CapabilityTypes.hpp"

namespace Asteria::Kernel::Ipc
{
/// The value types an Endpoint's deliveries carry, apart from the Endpoint, so that the delivery
/// codec and a user-space service that decodes deliveries need no kernel object.
enum class EndpointSendClass : Foundation::UInt8
{
    Invalid = 0U,
    Normal = 1U,
    ReservedControl = 2U,
};

struct EndpointReceiveMetadata
{
    Foundation::UInt64 Sequence;
    Object::ResourceDomainId Sender;
    Foundation::UInt64 SenderEndpointSealedObjectSelector;
    Object::CapabilityRights SenderEndpointRights;
    Foundation::UInt16 FrameSizeBytes;
    Foundation::UInt8 AttachmentCount;
    EndpointSendClass SendClass;
};

static_assert(sizeof(EndpointReceiveMetadata) == 32U);
static_assert(alignof(EndpointReceiveMetadata) == 8U);
} // Namespace Asteria::Kernel::Ipc

#endif // ASTERIA_KERNEL_IPC_ENDPOINT_TYPES_HPP_INCLUDED
