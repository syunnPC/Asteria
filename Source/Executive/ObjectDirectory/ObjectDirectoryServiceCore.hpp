// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_EXECUTIVE_OBJECT_DIRECTORY_OBJECT_DIRECTORY_SERVICE_CORE_HPP_INCLUDED
#define ASTERIA_EXECUTIVE_OBJECT_DIRECTORY_OBJECT_DIRECTORY_SERVICE_CORE_HPP_INCLUDED

#include "Executive/ObjectDirectory/ObjectDirectoryCore.hpp"
#include "Executive/ObjectDirectory/ObjectDirectoryProtocol.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"
#include "Kernel/Object/CapabilityTypes.hpp"

namespace Asteria::Kernel::Ipc
{
struct EndpointReceiveMetadata;
}

namespace Asteria::Executive::ObjectDirectory
{
enum class ObjectDirectoryServiceError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    OutputOccupied,
    InvocationRejected,
    ProtocolFailure,
    InternalFailure,
};

/// Service-layer failure with unwired nested diagnostics.
///
/// Protocol and Core may also describe a handled client-visible status while
/// Code is None. These diagnostics are local to the service process and are
/// never encoded in a reply.
struct ObjectDirectoryServiceFailure
{
    ObjectDirectoryServiceError Code;
    ObjectDirectoryProtocolError Protocol;
    DirectoryCoreError Core;
    Foundation::UInt8 Reserved;
};

struct ObjectDirectoryServiceConfiguration
{
    DirectoryCoreConfiguration Core;
    Foundation::UInt64 ExpectedSealedObjectSelector;
};

/// Passive caller-owned send and cleanup work for one dispatch.
///
/// Default construction produces the pristine state required by Dispatch.
/// Reusing an outcome before clearing it is rejected.
///
/// When HasPreparedReply is true, the caller must send PreparedReply through
/// ReplyEndpoint before closing CapabilitiesToClose and before any later core
/// mutation. Every listed capability must be closed after the send attempt.
/// A successful Resolve reply borrows its target attachment from the core;
/// that target is deliberately absent from CapabilitiesToClose.
struct ObjectDirectoryServiceDispatchOutcome
{
    Kernel::Ipc::MessageEnvelope PreparedReply{};
    Kernel::Object::CapabilityHandle ReplyEndpoint{};
    Kernel::Object::CapabilityHandle CapabilitiesToClose[Kernel::Ipc::MessageEnvelope::MaximumCapabilityAttachments]{};
    Foundation::Size CapabilityCloseCount{0U};
    bool HasPreparedReply{false};
    Foundation::UInt8 Reserved[7]{};
};

/// Thin, serialized user-space adapter for one Object Directory node.
///
/// This class performs no allocation, capability syscall, locking, access-list
/// evaluation, replay suppression, persistence, or revocation.
class ObjectDirectoryServiceCore final
{
public:
    explicit constexpr ObjectDirectoryServiceCore(const ObjectDirectoryServiceConfiguration configuration) noexcept
        : m_core{configuration.Core}, m_expectedSealedObjectSelector{configuration.ExpectedSealedObjectSelector}
    {
    }

    ~ObjectDirectoryServiceCore() noexcept = default;

    ObjectDirectoryServiceCore(const ObjectDirectoryServiceCore&) = delete;
    ObjectDirectoryServiceCore& operator=(const ObjectDirectoryServiceCore&) = delete;
    ObjectDirectoryServiceCore(ObjectDirectoryServiceCore&&) = delete;
    ObjectDirectoryServiceCore& operator=(ObjectDirectoryServiceCore&&) = delete;

    [[nodiscard]] ObjectDirectoryServiceFailure Dispatch(const Kernel::Ipc::MessageEnvelope& message,
                                                         const Kernel::Ipc::EndpointReceiveMetadata& metadata,
                                                         ObjectDirectoryServiceDispatchOutcome& output) noexcept;

    [[nodiscard]] DirectoryCoreError BeginQuiesce() noexcept;

    [[nodiscard]] DirectoryCoreError TakeNextOwnedEndpoint(Kernel::Object::CapabilityHandle& output) noexcept;

    [[nodiscard]] DirectoryCoreSnapshot GetSnapshot() const noexcept;

private:
    [[nodiscard]] static bool IsOutcomePristine(const ObjectDirectoryServiceDispatchOutcome& output) noexcept;
    static void CollectReceivedCapabilities(const Kernel::Ipc::MessageEnvelope& message,
                                            ObjectDirectoryServiceDispatchOutcome& output) noexcept;
    static void RemoveCleanupCapability(Kernel::Object::CapabilityHandle capability,
                                        ObjectDirectoryServiceDispatchOutcome& output) noexcept;
    [[nodiscard]] static bool AddCleanupCapability(Kernel::Object::CapabilityHandle capability,
                                                   ObjectDirectoryServiceDispatchOutcome& output) noexcept;
    [[nodiscard]] static DirectoryStatus MapMutationStatus(DirectoryCoreError error) noexcept;
    [[nodiscard]] static DirectoryStatus MapResolveStatus(DirectoryCoreError error) noexcept;
    [[nodiscard]] static ObjectDirectoryServiceFailure MakeFailure(ObjectDirectoryServiceError service,
                                                                   ObjectDirectoryProtocolError protocol,
                                                                   DirectoryCoreError core) noexcept;

    [[nodiscard]] ObjectDirectoryServiceFailure DispatchResolve(const Kernel::Ipc::MessageEnvelope& message,
                                                                ObjectDirectoryServiceDispatchOutcome& output) noexcept;
    [[nodiscard]] ObjectDirectoryServiceFailure
    DispatchBindIfAbsent(const Kernel::Ipc::MessageEnvelope& message, bool hasManage,
                         ObjectDirectoryServiceDispatchOutcome& output) noexcept;
    [[nodiscard]] ObjectDirectoryServiceFailure
    DispatchReplaceIfVersion(const Kernel::Ipc::MessageEnvelope& message, bool hasManage,
                             ObjectDirectoryServiceDispatchOutcome& output) noexcept;
    [[nodiscard]] ObjectDirectoryServiceFailure
    DispatchRemoveIfVersion(const Kernel::Ipc::MessageEnvelope& message, bool hasManage,
                            ObjectDirectoryServiceDispatchOutcome& output) noexcept;
    [[nodiscard]] static ObjectDirectoryServiceFailure
    BuildMutationReply(Foundation::UInt32 methodIdentifier, DirectoryStatus status, BindingVersion version,
                       const DirectoryMessageContext& context, Kernel::Object::CapabilityHandle replyEndpoint,
                       DirectoryCoreError coreError, ObjectDirectoryServiceDispatchOutcome& output) noexcept;
    [[nodiscard]] static ObjectDirectoryServiceFailure
    BuildResolveReply(DirectoryStatus status, const DirectoryResolution& resolution,
                      const DirectoryMessageContext& context, Kernel::Object::CapabilityHandle replyEndpoint,
                      DirectoryCoreError coreError, ObjectDirectoryServiceDispatchOutcome& output) noexcept;

    ObjectDirectoryCore m_core;
    Foundation::UInt64 m_expectedSealedObjectSelector{0U};
};

static_assert(sizeof(ObjectDirectoryServiceError) == 1U);
static_assert(sizeof(ObjectDirectoryServiceFailure) == 4U);
static_assert(sizeof(ObjectDirectoryServiceConfiguration) == 24U);
static_assert(alignof(ObjectDirectoryServiceConfiguration) == 8U);
static_assert(sizeof(ObjectDirectoryServiceDispatchOutcome) == 592U);
static_assert(alignof(ObjectDirectoryServiceDispatchOutcome) == 8U);
} // namespace Asteria::Executive::ObjectDirectory

#endif // ASTERIA_EXECUTIVE_OBJECT_DIRECTORY_OBJECT_DIRECTORY_SERVICE_CORE_HPP_INCLUDED
