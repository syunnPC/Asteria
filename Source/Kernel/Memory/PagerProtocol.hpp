// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_PAGER_PROTOCOL_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_PAGER_PROTOCOL_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Abi/PagerProtocol.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"
#include "Kernel/Memory/FaultToken.hpp"

namespace Asteria::Kernel::Memory
{
enum class PagerProtocolError : Foundation::UInt8
{
    None,
    InvalidTransactionIdentifier,
    InvalidTraceContext,
    InvalidFaultRecord,
    InvalidFaultToken,
    InvalidInterface,
    UnsupportedVersion,
    InvalidMethod,
    InvalidMessageContract,
    InvalidPayload,
    InvalidAttachment,
    NoncanonicalMessage,
};

struct PagerFaultNotification
{
    FaultRecord Fault;
    /// The faulting domain's supervision tag, which its creator chose; zero for a boot domain.
    Foundation::UInt64 SupervisionTag;
    // Registry slot and generation, assigned by the kernel.
    Foundation::UInt64 ProtectionDomainIdentity{0U};
    Ipc::TraceContext Trace;
    Object::CapabilityHandle FaultToken;
    Object::MonotonicTime Deadline;
    Foundation::UInt64 TransactionIdentifier;
};

/// Canonical typed envelope used to deliver one external-pager fault.
/// The fixed payload is 48 little-endian bytes: FaultRecord, the tag and kernel ProtectionDomain
/// identity. Exactly one moved FaultToken attachment carries Invoke
/// and no other right. No C++ object layout is exposed on the wire.
class PagerProtocol final
{
public:
    static constexpr Ipc::InterfaceIdentifier Interface{0x5041'4745'522D'0001ULL, 0x4153'5445'5249'4101ULL};
    static constexpr Foundation::UInt32 FaultMethodIdentifier{1U};
    static constexpr Foundation::UInt16 VersionMajor{Abi::PagerProtocolMajor};
    static constexpr Foundation::UInt16 VersionMinor{Abi::PagerProtocolMinor};
    static constexpr Foundation::UInt16 FaultRecordSizeBytes{Abi::PagerFaultRecordBytes};
    static constexpr Foundation::UInt16 FaultPayloadSizeBytes{Abi::PagerFaultPayloadBytes};

    PagerProtocol() = delete;

    /// Builds one canonical asynchronous fault envelope. Every failure leaves
    /// output unchanged.
    [[nodiscard]] static PagerProtocolError BuildFaultNotification(const PagerFaultNotification& notification,
                                                                   Ipc::MessageEnvelope& output) noexcept;

    /// Parses only the exact canonical fault-envelope profile. Every failure
    /// leaves output unchanged.
    [[nodiscard]] static PagerProtocolError ParseFaultNotification(const Ipc::MessageEnvelope& message,
                                                                   PagerFaultNotification& output) noexcept;
};

static_assert(sizeof(PagerProtocolError) == 1U);
static_assert(sizeof(PagerFaultNotification) == 104U);
static_assert(alignof(PagerFaultNotification) == 8U);
static_assert(PagerProtocol::FaultRecordSizeBytes == sizeof(FaultRecord));
static_assert(PagerProtocol::FaultPayloadSizeBytes == PagerProtocol::FaultRecordSizeBytes + 16U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_PAGER_PROTOCOL_HPP_INCLUDED
