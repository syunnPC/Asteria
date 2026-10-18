// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_EXECUTIVE_OBJECT_DIRECTORY_OBJECT_DIRECTORY_PROTOCOL_HPP_INCLUDED
#define ASTERIA_EXECUTIVE_OBJECT_DIRECTORY_OBJECT_DIRECTORY_PROTOCOL_HPP_INCLUDED

#include "Executive/ObjectDirectory/ObjectDirectoryTypes.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"

namespace Asteria::Executive::ObjectDirectory
{
enum class ObjectDirectoryProtocolError : Foundation::UInt8
{
    None,
    InvalidTransactionIdentifier,
    InvalidTraceContext,
    InvalidInterface,
    UnsupportedVersion,
    InvalidMethod,
    InvalidMessageContract,
    InvalidName,
    InvalidVersionRange,
    InvalidServiceGeneration,
    InvalidRights,
    InvalidReplyCapability,
    InvalidTargetCapability,
    InvalidStatus,
    InvalidPayload,
    InvalidAttachment,
    NoncanonicalMessage,
};

/// Transport metadata retained by both sides of a directory exchange.
struct DirectoryMessageContext
{
    Kernel::Ipc::TraceContext Trace;
    Kernel::Object::MonotonicTime Deadline;
    Foundation::UInt64 TransactionIdentifier;
};

struct ResolveRequest
{
    DirectoryName Name;
    Kernel::Ipc::InterfaceIdentifier ServiceInterface;
    Kernel::Object::MonotonicTime RequestedDeadline;
    InterfaceVersionRange RequiredVersion;
    Kernel::Object::CapabilityRights RequiredRights;
    Kernel::Object::CapabilityHandle ReplyEndpoint;
    DirectoryMessageContext Context;
};

struct BindIfAbsentRequest
{
    DirectoryName Name;
    Kernel::Ipc::InterfaceIdentifier ServiceInterface;
    Foundation::UInt64 ServiceGeneration;
    InterfaceVersionRange Version;
    Kernel::Object::CapabilityRights PublishedRights;
    Kernel::Object::CapabilityHandle TargetEndpoint;
    Kernel::Object::MonotonicTime TargetDeadline;
    Kernel::Object::CapabilityHandle ReplyEndpoint;
    DirectoryMessageContext Context;
};

struct ReplaceIfVersionRequest
{
    DirectoryName Name;
    BindingVersion ExpectedVersion;
    Kernel::Ipc::InterfaceIdentifier ServiceInterface;
    Foundation::UInt64 ServiceGeneration;
    InterfaceVersionRange Version;
    Kernel::Object::CapabilityRights PublishedRights;
    Kernel::Object::CapabilityHandle TargetEndpoint;
    Kernel::Object::MonotonicTime TargetDeadline;
    Kernel::Object::CapabilityHandle ReplyEndpoint;
    DirectoryMessageContext Context;
};

struct RemoveIfVersionRequest
{
    DirectoryName Name;
    BindingVersion ExpectedVersion;
    Kernel::Object::CapabilityHandle ReplyEndpoint;
    DirectoryMessageContext Context;
};

struct ResolveReply
{
    DirectoryStatus Status;
    BindingVersion Version;
    Kernel::Ipc::InterfaceIdentifier ServiceInterface;
    Foundation::UInt64 ServiceGeneration;
    Kernel::Object::MonotonicTime GrantedDeadline;
    Foundation::UInt16 Major;
    Foundation::UInt16 SelectedMinor;
    Kernel::Object::CapabilityRights GrantedRights;
    Kernel::Object::CapabilityHandle TargetEndpoint;
    DirectoryMessageContext Context;
};

struct MutationReply
{
    DirectoryStatus Status;
    BindingVersion Version;
    DirectoryMessageContext Context;
};

/// Canonical Object Directory v1 typed-message profile.
///
/// Every API is allocation-free. A failure always leaves its output argument
/// unchanged. Payload fields are encoded individually in little-endian order;
/// no C++ object representation is placed on the wire.
class ObjectDirectoryProtocol final
{
public:
    static constexpr Kernel::Ipc::InterfaceIdentifier Interface{0x4F42'4A44'4952'0001ULL, 0x4153'5445'5249'4101ULL};
    static constexpr Foundation::UInt32 ResolveMethodIdentifier{1U};
    static constexpr Foundation::UInt32 BindIfAbsentMethodIdentifier{2U};
    static constexpr Foundation::UInt32 ReplaceIfVersionMethodIdentifier{3U};
    static constexpr Foundation::UInt32 RemoveIfVersionMethodIdentifier{4U};
    static constexpr Foundation::UInt16 VersionMajor{1U};
    static constexpr Foundation::UInt16 VersionMinor{0U};

    static constexpr Foundation::UInt16 ResolveRequestPayloadSizeBytes{32U};
    static constexpr Foundation::UInt16 BindIfAbsentRequestPayloadSizeBytes{40U};
    static constexpr Foundation::UInt16 ReplaceIfVersionRequestPayloadSizeBytes{56U};
    static constexpr Foundation::UInt16 RemoveIfVersionRequestPayloadSizeBytes{24U};
    static constexpr Foundation::UInt16 MutationReplyPayloadSizeBytes{24U};
    static constexpr Foundation::UInt16 ResolveReplyPayloadSizeBytes{64U};

    static constexpr Foundation::Size MaximumWireFrameSizeBytes{280U};

    ObjectDirectoryProtocol() = delete;

    [[nodiscard]] static ObjectDirectoryProtocolError
    BuildResolveRequest(const ResolveRequest& request, Kernel::Ipc::MessageEnvelope& output) noexcept;

    [[nodiscard]] static ObjectDirectoryProtocolError ParseResolveRequest(const Kernel::Ipc::MessageEnvelope& message,
                                                                          ResolveRequest& output) noexcept;

    [[nodiscard]] static ObjectDirectoryProtocolError
    BuildBindIfAbsentRequest(const BindIfAbsentRequest& request, Kernel::Ipc::MessageEnvelope& output) noexcept;

    [[nodiscard]] static ObjectDirectoryProtocolError
    ParseBindIfAbsentRequest(const Kernel::Ipc::MessageEnvelope& message, BindIfAbsentRequest& output) noexcept;

    [[nodiscard]] static ObjectDirectoryProtocolError
    BuildReplaceIfVersionRequest(const ReplaceIfVersionRequest& request, Kernel::Ipc::MessageEnvelope& output) noexcept;

    [[nodiscard]] static ObjectDirectoryProtocolError
    ParseReplaceIfVersionRequest(const Kernel::Ipc::MessageEnvelope& message, ReplaceIfVersionRequest& output) noexcept;

    [[nodiscard]] static ObjectDirectoryProtocolError
    BuildRemoveIfVersionRequest(const RemoveIfVersionRequest& request, Kernel::Ipc::MessageEnvelope& output) noexcept;

    [[nodiscard]] static ObjectDirectoryProtocolError
    ParseRemoveIfVersionRequest(const Kernel::Ipc::MessageEnvelope& message, RemoveIfVersionRequest& output) noexcept;

    [[nodiscard]] static ObjectDirectoryProtocolError BuildResolveReply(const ResolveReply& reply,
                                                                        Kernel::Ipc::MessageEnvelope& output) noexcept;

    [[nodiscard]] static ObjectDirectoryProtocolError ParseResolveReply(const Kernel::Ipc::MessageEnvelope& message,
                                                                        ResolveReply& output) noexcept;

    [[nodiscard]] static ObjectDirectoryProtocolError
    BuildBindIfAbsentReply(const MutationReply& reply, Kernel::Ipc::MessageEnvelope& output) noexcept;

    [[nodiscard]] static ObjectDirectoryProtocolError
    ParseBindIfAbsentReply(const Kernel::Ipc::MessageEnvelope& message, MutationReply& output) noexcept;

    [[nodiscard]] static ObjectDirectoryProtocolError
    BuildReplaceIfVersionReply(const MutationReply& reply, Kernel::Ipc::MessageEnvelope& output) noexcept;

    [[nodiscard]] static ObjectDirectoryProtocolError
    ParseReplaceIfVersionReply(const Kernel::Ipc::MessageEnvelope& message, MutationReply& output) noexcept;

    [[nodiscard]] static ObjectDirectoryProtocolError
    BuildRemoveIfVersionReply(const MutationReply& reply, Kernel::Ipc::MessageEnvelope& output) noexcept;

    [[nodiscard]] static ObjectDirectoryProtocolError
    ParseRemoveIfVersionReply(const Kernel::Ipc::MessageEnvelope& message, MutationReply& output) noexcept;
};

static_assert(sizeof(ObjectDirectoryProtocolError) == 1U);
static_assert(ObjectDirectoryProtocol::MaximumWireFrameSizeBytes <= Kernel::Ipc::MessageCodec::MaximumWireSizeBytes);
} // namespace Asteria::Executive::ObjectDirectory

#endif // ASTERIA_EXECUTIVE_OBJECT_DIRECTORY_OBJECT_DIRECTORY_PROTOCOL_HPP_INCLUDED
