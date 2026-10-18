// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_USER_EXECUTION_PROBE_PAYLOAD_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_USER_EXECUTION_PROBE_PAYLOAD_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Scheduler/Ia64UserExecutionProbe.hpp"

namespace Asteria::Platform::Ia64::Scheduler
{
constexpr Foundation::Size Ia64UserExecutionProbePayloadSizeBytes{0x450U};
constexpr Foundation::Size Ia64UserExecutionProbePayloadBundleCount{69U};
constexpr Foundation::Size Ia64UserExecutionProbeTerminalLoopOffsetBytes{0x100U};
constexpr Foundation::Size Ia64UserExecutionProbeChildOffsetBytes{0x110U};
constexpr Foundation::Size Ia64UserExecutionProbeChildSizeBytes{0x340U};
constexpr Foundation::Size Ia64UserExecutionProbeParentResumeOffsetBytes{0x60U};
constexpr Foundation::Size Ia64UserExecutionProbeChildFlushInstructionOffset{0x16U};
constexpr Foundation::Size Ia64UserExecutionProbeChildQueryBreakInstructionOffset{0x7CU};
constexpr Foundation::Size Ia64UserExecutionProbeChildSendBreakInstructionOffset{0x10CU};
constexpr Foundation::Size Ia64UserExecutionProbeChildReceiveBreakInstructionOffset{0x17CU};
constexpr Foundation::Size Ia64UserExecutionProbeChildWaitBreakInstructionOffset{0x22CU};
constexpr Foundation::UInt64 Ia64UserExecutionProbeSentinel{0x4153'5445'5249'4133ULL};
constexpr Foundation::Size Ia64UserExecutionProbeStackedSentinelCount{4U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeRseSentinelAddress{0x2000'0000'0300'4FF0ULL};
constexpr Foundation::UInt64 Ia64UserExecutionProbeStackSentinelOffsetFromEndBytes{16U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeSystemCallResponseOffsetFromStackPointerBytes{24U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeSystemCallResponseSizeBytes{24U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeQueryResponseVirtualAddress{0x2000'0000'0200'4FD8ULL};
constexpr Foundation::UInt64 Ia64UserExecutionProbeCapabilityQuerySystemCallNumber{2U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeIpcSendSystemCallNumber{6U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeIpcReceiveSystemCallNumber{7U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeWaitSetWaitSystemCallNumber{8U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeExpectedCapabilityQueryError{0U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeExpectedCapabilityQueryInformation{0x0000'0000'0001'0001ULL};
constexpr Foundation::UInt64 Ia64UserExecutionProbeExpectedCapabilityQueryLeaseDeadlineTicks{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 Ia64UserExecutionProbeExpectedProtectionDomainCapabilitySelector{0x100U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeExpectedEndpointCapabilitySelector{0x101U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeExpectedWaitSetCapabilitySelector{0x102U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeIpcBootstrapVirtualAddress{0x2000'0000'0200'4100ULL};
constexpr Foundation::Size Ia64UserExecutionProbeIpcBootstrapSelectorOffsetBytes{0U};
constexpr Foundation::Size Ia64UserExecutionProbeIpcBootstrapAuthenticatorOffsetBytes{8U};
constexpr Foundation::Size Ia64UserExecutionProbeIpcBootstrapFrameOffsetBytes{16U};
constexpr Foundation::Size Ia64UserExecutionProbeIpcBootstrapFrameSizeBytes{88U};
constexpr Foundation::Size Ia64UserExecutionProbeIpcBootstrapEndpointRecordSizeBytes{104U};
constexpr Foundation::Size Ia64UserExecutionProbeIpcBootstrapWaitSetSelectorOffsetBytes{104U};
constexpr Foundation::Size Ia64UserExecutionProbeIpcBootstrapWaitSetAuthenticatorOffsetBytes{112U};
constexpr Foundation::Size Ia64UserExecutionProbeIpcBootstrapRecordSizeBytes{120U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeIpcEvidenceVirtualAddress{0x2000'0000'0200'4200ULL};
constexpr Foundation::Size Ia64UserExecutionProbeIpcSendErrorOffsetBytes{0U};
constexpr Foundation::Size Ia64UserExecutionProbeIpcSendInformationOffsetBytes{8U};
constexpr Foundation::Size Ia64UserExecutionProbeIpcSendTransferredBytesOffsetBytes{16U};
constexpr Foundation::Size Ia64UserExecutionProbeIpcReceiveErrorOffsetBytes{24U};
constexpr Foundation::Size Ia64UserExecutionProbeIpcReceiveInformationOffsetBytes{32U};
constexpr Foundation::Size Ia64UserExecutionProbeIpcReceiveTransferredBytesOffsetBytes{40U};
constexpr Foundation::Size Ia64UserExecutionProbeIpcCompletionOffsetBytes{48U};
constexpr Foundation::Size Ia64UserExecutionProbeIpcEvidenceSizeBytes{56U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeIpcDeliveryVirtualAddress{0x2000'0000'0200'4300ULL};
constexpr Foundation::Size Ia64UserExecutionProbeIpcDeliveryCapacityBytes{136U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeWaitResponseVirtualAddress{0x2000'0000'0200'44D0ULL};
constexpr Foundation::Size Ia64UserExecutionProbeWaitResponseSizeBytes{24U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeExpectedWaitTag{0x5741'4954'5345'5401ULL};
constexpr Foundation::UInt64 Ia64UserExecutionProbeExpectedWaitRegistrationSequence{1U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeExpectedIpcError{0U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeExpectedIpcInformation{1U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeExpectedSendTransferredBytes{0U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeExpectedReceiveTransferredBytes{
    Ia64UserExecutionProbeIpcDeliveryCapacityBytes};

/// Retained byte-oriented bootstrap ABI staged into the mapped user stack.
/// Integer fields are explicitly little-endian; the frame is the canonical
/// MessageCodec wire representation and is never a native-layout overlay.
struct alignas(Foundation::UInt64) Ia64UserExecutionProbeIpcBootstrapRecord
{
    Foundation::UInt8 EndpointCapabilitySelector[8U];
    Foundation::UInt8 EndpointCapabilityAuthenticator[8U];
    Foundation::UInt8 Frame[Ia64UserExecutionProbeIpcBootstrapFrameSizeBytes];
    Foundation::UInt8 WaitSetCapabilitySelector[8U];
    Foundation::UInt8 WaitSetCapabilityAuthenticator[8U];
};

struct Ia64UserExecutionProbePayloadView
{
    const Foundation::UInt8* Data;
    Foundation::Size SizeBytes;
};

/// Returns one relocation-free, self-contained IA-64 payload.
///
/// The payload creates a parent frame with four distinct stacked-register
/// sentinels and calls one relocation-free local child. The child preserves its
/// b0/ar.pfs state and the parent's r14/r15, executes one group-stopped flushrs,
/// and uses one six-register frame (SOF 6, SOL 0) for CapabilityQuery, IpcSend,
/// IpcReceive, and WaitSetWait. Each native syscall is a stopped
/// slot-two break.i 0x100000. The Query response is saved immediately and
/// restored to r8-r10 before return. Send and Receive consume the opaque staged
/// Endpoint handle; WaitSetWait consumes the separately staged WaitSet handle
/// and records its response only after the suspended syscall resumes. Their
/// live responses and one conditional completion sentinel remain in the user
/// evidence area. After the child unwinds, the parent validates all four
/// refilled stacked values and records the independent RSE oracle. The terminal
/// state keeps r4/r5, r8-r10, r12, and parent r32-r35 intact while clearing
/// r14-r31. The payload does not use gp, invoke firmware, add mappings, or
/// manipulate translation-cache registers. The caller copies these bytes into
/// the fixed executable user page before that page is published read/execute.
[[nodiscard]] Ia64UserExecutionProbePayloadView GetIa64UserExecutionProbePayload() noexcept;

static_assert(Ia64UserExecutionProbePayloadSizeBytes == 69U * 16U);
static_assert(Ia64UserExecutionProbePayloadBundleCount == 69U);
static_assert(Ia64UserExecutionProbeTerminalLoopOffsetBytes == 0x100U);
static_assert(Ia64UserExecutionProbeChildOffsetBytes == 0x110U);
static_assert(Ia64UserExecutionProbeChildSizeBytes == 0x340U);
static_assert(Ia64UserExecutionProbeChildOffsetBytes + Ia64UserExecutionProbeChildSizeBytes ==
              Ia64UserExecutionProbePayloadSizeBytes);
static_assert(Ia64UserExecutionProbeParentResumeOffsetBytes == 0x60U);
static_assert(Ia64UserExecutionProbeChildFlushInstructionOffset == 0x16U);
static_assert(Ia64UserExecutionProbeChildQueryBreakInstructionOffset == 0x7CU);
static_assert(Ia64UserExecutionProbeChildSendBreakInstructionOffset == 0x10CU);
static_assert(Ia64UserExecutionProbeChildReceiveBreakInstructionOffset == 0x17CU);
static_assert(Ia64UserExecutionProbeChildWaitBreakInstructionOffset == 0x22CU);
static_assert(Ia64UserExecutionProbeStackedSentinelCount == 4U);
static_assert(Ia64UserExecutionProbeStackSentinelOffsetFromEndBytes == 16U);
static_assert(Ia64UserExecutionProbeSystemCallResponseOffsetFromStackPointerBytes == 24U);
static_assert(Ia64UserExecutionProbeSystemCallResponseSizeBytes == 3U * 8U);
static_assert(Ia64UserExecutionProbeQueryResponseVirtualAddress == 0x2000'0000'0200'4FD8ULL);
static_assert(Ia64UserExecutionProbeCapabilityQuerySystemCallNumber == 2U);
static_assert(Ia64UserExecutionProbeIpcSendSystemCallNumber == 6U);
static_assert(Ia64UserExecutionProbeIpcReceiveSystemCallNumber == 7U);
static_assert(Ia64UserExecutionProbeWaitSetWaitSystemCallNumber == 8U);
static_assert(Ia64UserExecutionProbeExpectedCapabilityQueryError == 0U);
static_assert(Ia64UserExecutionProbeExpectedCapabilityQueryInformation == 0x0000'0000'0001'0001ULL);
static_assert(Ia64UserExecutionProbeExpectedCapabilityQueryLeaseDeadlineTicks == ~Foundation::UInt64{0U});
static_assert(Ia64UserExecutionProbeExpectedProtectionDomainCapabilitySelector == 0x100U);
static_assert(Ia64UserExecutionProbeExpectedEndpointCapabilitySelector == 0x101U);
static_assert(Ia64UserExecutionProbeExpectedWaitSetCapabilitySelector == 0x102U);
static_assert(Ia64UserExecutionProbeIpcBootstrapVirtualAddress == 0x2000'0000'0200'4100ULL);
static_assert(Ia64UserExecutionProbeIpcBootstrapSelectorOffsetBytes == 0U);
static_assert(Ia64UserExecutionProbeIpcBootstrapAuthenticatorOffsetBytes == 8U);
static_assert(Ia64UserExecutionProbeIpcBootstrapFrameOffsetBytes == 16U);
static_assert(Ia64UserExecutionProbeIpcBootstrapFrameSizeBytes == 88U);
static_assert(Ia64UserExecutionProbeIpcBootstrapEndpointRecordSizeBytes == 104U);
static_assert(Ia64UserExecutionProbeIpcBootstrapWaitSetSelectorOffsetBytes == 104U);
static_assert(Ia64UserExecutionProbeIpcBootstrapWaitSetAuthenticatorOffsetBytes == 112U);
static_assert(Ia64UserExecutionProbeIpcBootstrapRecordSizeBytes == 120U);
static_assert(Ia64UserExecutionProbeIpcEvidenceVirtualAddress == 0x2000'0000'0200'4200ULL);
static_assert(Ia64UserExecutionProbeIpcSendErrorOffsetBytes == 0U);
static_assert(Ia64UserExecutionProbeIpcSendInformationOffsetBytes == 8U);
static_assert(Ia64UserExecutionProbeIpcSendTransferredBytesOffsetBytes == 16U);
static_assert(Ia64UserExecutionProbeIpcReceiveErrorOffsetBytes == 24U);
static_assert(Ia64UserExecutionProbeIpcReceiveInformationOffsetBytes == 32U);
static_assert(Ia64UserExecutionProbeIpcReceiveTransferredBytesOffsetBytes == 40U);
static_assert(Ia64UserExecutionProbeIpcCompletionOffsetBytes == 48U);
static_assert(Ia64UserExecutionProbeIpcEvidenceSizeBytes == 7U * 8U);
static_assert(Ia64UserExecutionProbeIpcDeliveryVirtualAddress == 0x2000'0000'0200'4300ULL);
static_assert(Ia64UserExecutionProbeIpcDeliveryCapacityBytes == 136U);
static_assert(Ia64UserExecutionProbeWaitResponseVirtualAddress == 0x2000'0000'0200'44D0ULL);
static_assert(Ia64UserExecutionProbeWaitResponseSizeBytes == 3U * 8U);
static_assert(Ia64UserExecutionProbeExpectedWaitTag == 0x5741'4954'5345'5401ULL);
static_assert(Ia64UserExecutionProbeExpectedWaitRegistrationSequence == 1U);
static_assert(Ia64UserExecutionProbeIpcDeliveryVirtualAddress + Ia64UserExecutionProbeIpcDeliveryCapacityBytes <=
              Ia64UserExecutionProbeWaitResponseVirtualAddress);
static_assert(Ia64UserExecutionProbeWaitResponseVirtualAddress + Ia64UserExecutionProbeWaitResponseSizeBytes <=
              Ia64UserExecutionProbeQueryResponseVirtualAddress);
static_assert(Ia64UserExecutionProbeQueryResponseVirtualAddress + Ia64UserExecutionProbeSystemCallResponseSizeBytes <=
              Ia64UserExecutionProbeMemoryStackVirtualStart + Ia64UserExecutionProbeMemoryStackSizeBytes -
                  Ia64UserExecutionProbeStackSentinelOffsetFromEndBytes);
static_assert(Ia64UserExecutionProbeMemoryStackVirtualStart + Ia64UserExecutionProbeMemoryStackSizeBytes -
                  Ia64UserExecutionProbeStackSentinelOffsetFromEndBytes + sizeof(Ia64UserExecutionProbeSentinel) <=
              Ia64UserExecutionProbeMemoryStackVirtualStart + Ia64UserExecutionProbeMemoryStackSizeBytes);
static_assert(Ia64UserExecutionProbeExpectedIpcError == 0U);
static_assert(Ia64UserExecutionProbeExpectedIpcInformation == 1U);
static_assert(Ia64UserExecutionProbeExpectedSendTransferredBytes == 0U);
static_assert(Ia64UserExecutionProbeExpectedReceiveTransferredBytes == 136U);
static_assert(sizeof(Ia64UserExecutionProbeIpcBootstrapRecord) == Ia64UserExecutionProbeIpcBootstrapRecordSizeBytes);
static_assert(alignof(Ia64UserExecutionProbeIpcBootstrapRecord) == 8U);
static_assert(__is_aggregate(Ia64UserExecutionProbeIpcBootstrapRecord));
static_assert(__is_standard_layout(Ia64UserExecutionProbeIpcBootstrapRecord));
static_assert(__is_trivially_copyable(Ia64UserExecutionProbeIpcBootstrapRecord));
static_assert(__builtin_offsetof(Ia64UserExecutionProbeIpcBootstrapRecord, EndpointCapabilitySelector) ==
              Ia64UserExecutionProbeIpcBootstrapSelectorOffsetBytes);
static_assert(__builtin_offsetof(Ia64UserExecutionProbeIpcBootstrapRecord, EndpointCapabilityAuthenticator) ==
              Ia64UserExecutionProbeIpcBootstrapAuthenticatorOffsetBytes);
static_assert(__builtin_offsetof(Ia64UserExecutionProbeIpcBootstrapRecord, Frame) ==
              Ia64UserExecutionProbeIpcBootstrapFrameOffsetBytes);
static_assert(__builtin_offsetof(Ia64UserExecutionProbeIpcBootstrapRecord, WaitSetCapabilitySelector) ==
              Ia64UserExecutionProbeIpcBootstrapWaitSetSelectorOffsetBytes);
static_assert(__builtin_offsetof(Ia64UserExecutionProbeIpcBootstrapRecord, WaitSetCapabilityAuthenticator) ==
              Ia64UserExecutionProbeIpcBootstrapWaitSetAuthenticatorOffsetBytes);
static_assert(sizeof(Ia64UserExecutionProbePayloadView) == 16U);
static_assert(alignof(Ia64UserExecutionProbePayloadView) == 8U);
static_assert(__is_aggregate(Ia64UserExecutionProbePayloadView));
static_assert(__is_standard_layout(Ia64UserExecutionProbePayloadView));
static_assert(__is_trivially_copyable(Ia64UserExecutionProbePayloadView));
} // namespace Asteria::Platform::Ia64::Scheduler

#endif // ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_USER_EXECUTION_PROBE_PAYLOAD_HPP_INCLUDED
