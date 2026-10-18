// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_RUNTIME_C_KERNEL_STATUS_H_INCLUDED
#define ASTERIA_RUNTIME_C_KERNEL_STATUS_H_INCLUDED

#include "AsStatus.h"

/* Complete AsStatus encodings, usable directly with the C ABI. */
enum AsKernelStatus : uint32_t
{
    AsKernelUnknownSystemCall = 0x81000001U,
    AsKernelInvalidArguments = 0x81000002U,
    AsKernelInvalidConfiguration = 0x81000003U,
    AsKernelInvalidHandle = 0x81000004U,
    AsKernelObjectNotFound = 0x81000005U,
    AsKernelResourceExhausted = 0x81000006U,
    AsKernelSpaceExhausted = 0x81000007U,
    AsKernelRightsDenied = 0x81000008U,
    AsKernelRightsAmplification = 0x81000009U,
    AsKernelNoAttenuation = 0x8100000AU,
    AsKernelTypeMismatch = 0x8100000BU,
    AsKernelRevoked = 0x8100000CU,
    AsKernelExpired = 0x8100000DU,
    AsKernelLeaseExtension = 0x8100000EU,
    AsKernelRevocationDomainExhausted = 0x8100000FU,
    AsKernelWouldBlock = 0x81000010U,
    AsKernelUserMemoryFault = 0x81000011U,
    AsKernelTimedOut = 0x81000012U,
    AsKernelWriteValueDenied = 0x81000013U,
    AsKernelReservedMemory = 0x81000014U,
    AsKernelDisplayReserved = 0x81000015U,
    AsKernelBridgeReserved = 0x81000016U,
};
enum AsKernelFacility : uint32_t
{
    AsKernelFacilityNumber = 1U,
};

#endif
