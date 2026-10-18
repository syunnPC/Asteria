// SPDX-License-Identifier: Apache-2.0

// User-execution probe addresses, offsets, system-call numbers and expected
// responses for Scheduler/Ia64UserExecutionProbePayload.S (see
// Platform/AsmLayout.hpp).

#include "Abi/SystemCallAbi.hpp"
#include "Platform/AsmLayout.hpp"
#include "Platform/Ia64/Scheduler/Ia64UserExecutionProbePayload.hpp"
#include "Platform/Ia64/System/Ia64SystemCallEntry.hpp"

namespace
{
using Asteria::Abi::SystemCallError;
using Asteria::Foundation::UInt64;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeCapabilityQuerySystemCallNumber;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedEndpointCapabilitySelector;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedIpcError;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedIpcInformation;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedReceiveTransferredBytes;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedWaitRegistrationSequence;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedWaitSetCapabilitySelector;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedWaitTag;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcBootstrapFrameSizeBytes;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcBootstrapVirtualAddress;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcBootstrapWaitSetSelectorOffsetBytes;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcCompletionOffsetBytes;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcDeliveryCapacityBytes;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcDeliveryVirtualAddress;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcEvidenceVirtualAddress;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcReceiveSystemCallNumber;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcSendSystemCallNumber;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeRseSentinelAddress;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeSentinel;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeSystemCallResponseOffsetFromStackPointerBytes;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeWaitResponseVirtualAddress;
using Asteria::Platform::Ia64::Scheduler::Ia64UserExecutionProbeWaitSetWaitSystemCallNumber;
using Asteria::Platform::Ia64::System::Ia64SystemCallBreakImmediate;
} // namespace

extern "C" void AsteriaIa64UserExecutionProbePayloadAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_SENTINEL, Ia64UserExecutionProbeSentinel);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_RSE_SENTINEL_ADDRESS,
                       Ia64UserExecutionProbeRseSentinelAddress);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_SYSTEM_CALL_RESPONSE_OFFSET_FROM_STACK_POINTER_BYTES,
                       Ia64UserExecutionProbeSystemCallResponseOffsetFromStackPointerBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_CAPABILITY_QUERY_SYSTEM_CALL_NUMBER,
                       Ia64UserExecutionProbeCapabilityQuerySystemCallNumber);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_SEND_SYSTEM_CALL_NUMBER,
                       Ia64UserExecutionProbeIpcSendSystemCallNumber);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_RECEIVE_SYSTEM_CALL_NUMBER,
                       Ia64UserExecutionProbeIpcReceiveSystemCallNumber);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_WAIT_SET_WAIT_SYSTEM_CALL_NUMBER,
                       Ia64UserExecutionProbeWaitSetWaitSystemCallNumber);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_EXPECTED_ENDPOINT_CAPABILITY_SELECTOR,
                       Ia64UserExecutionProbeExpectedEndpointCapabilitySelector);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_EXPECTED_WAIT_SET_CAPABILITY_SELECTOR,
                       Ia64UserExecutionProbeExpectedWaitSetCapabilitySelector);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_BOOTSTRAP_VIRTUAL_ADDRESS,
                       Ia64UserExecutionProbeIpcBootstrapVirtualAddress);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_BOOTSTRAP_FRAME_SIZE_BYTES,
                       Ia64UserExecutionProbeIpcBootstrapFrameSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_BOOTSTRAP_WAIT_SET_SELECTOR_OFFSET_BYTES,
                       Ia64UserExecutionProbeIpcBootstrapWaitSetSelectorOffsetBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_EVIDENCE_VIRTUAL_ADDRESS,
                       Ia64UserExecutionProbeIpcEvidenceVirtualAddress);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_COMPLETION_OFFSET_BYTES,
                       Ia64UserExecutionProbeIpcCompletionOffsetBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_DELIVERY_VIRTUAL_ADDRESS,
                       Ia64UserExecutionProbeIpcDeliveryVirtualAddress);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_IPC_DELIVERY_CAPACITY_BYTES,
                       Ia64UserExecutionProbeIpcDeliveryCapacityBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_EXPECTED_IPC_ERROR, Ia64UserExecutionProbeExpectedIpcError);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_EXPECTED_IPC_INFORMATION,
                       Ia64UserExecutionProbeExpectedIpcInformation);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_EXPECTED_RECEIVE_TRANSFERRED_BYTES,
                       Ia64UserExecutionProbeExpectedReceiveTransferredBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_WAIT_RESPONSE_VIRTUAL_ADDRESS,
                       Ia64UserExecutionProbeWaitResponseVirtualAddress);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_EXPECTED_WAIT_TAG, Ia64UserExecutionProbeExpectedWaitTag);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXECUTION_PROBE_EXPECTED_WAIT_REGISTRATION_SEQUENCE,
                       Ia64UserExecutionProbeExpectedWaitRegistrationSequence);
    ASTERIA_ASM_LAYOUT(ASTERIA_SYSTEM_CALL_ERROR_NONE, static_cast<UInt64>(SystemCallError::None));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_BREAK_IMMEDIATE, Ia64SystemCallBreakImmediate);
}
