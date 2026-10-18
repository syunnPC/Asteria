// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_EXECUTIVE_OBJECT_DIRECTORY_OBJECT_DIRECTORY_CORE_HPP_INCLUDED
#define ASTERIA_EXECUTIVE_OBJECT_DIRECTORY_OBJECT_DIRECTORY_CORE_HPP_INCLUDED

#include "Executive/ObjectDirectory/ObjectDirectoryTypes.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"
#include "Kernel/Object/CapabilityTypes.hpp"

namespace Asteria::Executive::ObjectDirectory
{
enum class DirectoryCoreState : Foundation::UInt8
{
    InvalidConfiguration,
    Ready,
    Quiescing,
    Quiesced,
};

enum class DirectoryCoreError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    NotReady,
    InvalidName,
    InvalidCandidate,
    InvalidInterface,
    InvalidVersion,
    InvalidRights,
    AlreadyBound,
    NotFound,
    BindingChanged,
    CapabilityAlreadyOwned,
    CapacityExceeded,
    RevisionExhausted,
    OutputOccupied,
    NoOwnedCapability,
    LeaseUnavailable,
};

struct DirectoryCoreConfiguration
{
    Foundation::UInt64 DirectoryGeneration;
    Foundation::UInt64 LastIssuedRevision;
};

/// One endpoint capability whose transfer into the directory has already been
/// verified by the caller's IPC adapter. The core never queries capability
/// space and adopts Custody only when a mutation succeeds.
struct DirectoryBindingCandidate
{
    Kernel::Object::CapabilityHandle Custody;
    Kernel::Ipc::InterfaceIdentifier ServiceInterface;
    Foundation::UInt64 ServiceGeneration;
    Kernel::Object::MonotonicTime TargetDeadline;
    Kernel::Object::CapabilityRights PublishedRights;
    Kernel::Object::CapabilityRights CustodyRights;
    Kernel::Object::ObjectType Type;
    Foundation::UInt8 Reserved[3];
    InterfaceVersionRange Version;
};

struct DirectoryResolveQuery
{
    Kernel::Ipc::InterfaceIdentifier ServiceInterface;
    Kernel::Object::MonotonicTime RequestedDeadline;
    InterfaceVersionRange RequiredVersion;
    Kernel::Object::CapabilityRights RequiredRights;
    Foundation::UInt8 Reserved[6];
};

struct DirectoryMutationReceipt
{
    BindingVersion Version{};
    Kernel::Object::CapabilityHandle DisplacedEndpoint{};
};

/// A borrowed view of one stored endpoint capability. The caller must finish
/// the kernel Copy used to publish its reply before any later core mutation.
struct DirectoryResolution
{
    BindingVersion Version{};
    Kernel::Ipc::InterfaceIdentifier ServiceInterface{};
    Foundation::UInt64 ServiceGeneration{0U};
    Kernel::Object::MonotonicTime GrantedDeadline{};
    Kernel::Object::CapabilityRights GrantedRights{};
    Foundation::UInt16 Major{0U};
    Foundation::UInt16 SelectedMinor{0U};
    Foundation::UInt16 Reserved{0U};
    Kernel::Object::CapabilityHandle TargetEndpoint{};
};

struct DirectoryCoreSnapshot
{
    Foundation::Size BindingCount;
    Foundation::Size FreeSlotCount;
    Foundation::UInt64 DirectoryGeneration;
    Foundation::UInt64 LastIssuedRevision;
    DirectoryCoreState State;
    Foundation::UInt8 Reserved[7];
};

/// Fixed-capacity mechanical storage for one user-space Object Directory node.
///
/// The caller is the sole dispatch owner. This class is explicitly not
/// thread-safe and performs no locking, allocation, capability lookup, or
/// endpoint Close. Names are single components; hierarchy is supplied by the
/// client's starting directory endpoint rather than represented here.
class ObjectDirectoryCore final
{
public:
    static constexpr Foundation::Size MaximumBindings{64U};

    explicit constexpr ObjectDirectoryCore(const DirectoryCoreConfiguration configuration) noexcept
        : m_directoryGeneration{configuration.DirectoryGeneration},
          m_lastIssuedRevision{configuration.LastIssuedRevision},
          m_state{configuration.DirectoryGeneration == 0U ? DirectoryCoreState::InvalidConfiguration
                                                          : DirectoryCoreState::Ready}
    {
    }

    /// Does not close stored endpoints. The owner must quiesce and drain them.
    ~ObjectDirectoryCore() noexcept = default;

    ObjectDirectoryCore(const ObjectDirectoryCore&) = delete;
    ObjectDirectoryCore& operator=(const ObjectDirectoryCore&) = delete;
    ObjectDirectoryCore(ObjectDirectoryCore&&) = delete;
    ObjectDirectoryCore& operator=(ObjectDirectoryCore&&) = delete;

    /// Mutation and resolution outputs are inspected before use. Default
    /// construction makes them pristine; a non-pristine output is rejected.
    [[nodiscard]] DirectoryCoreError BindIfAbsent(const DirectoryName& name, DirectoryBindingCandidate& candidate,
                                                  DirectoryMutationReceipt& output) noexcept;

    [[nodiscard]] DirectoryCoreError ReplaceIfVersion(const DirectoryName& name, BindingVersion expectedVersion,
                                                      DirectoryBindingCandidate& candidate,
                                                      DirectoryMutationReceipt& output) noexcept;

    [[nodiscard]] DirectoryCoreError RemoveIfVersion(const DirectoryName& name, BindingVersion expectedVersion,
                                                     DirectoryMutationReceipt& output) noexcept;

    /// Borrows the stored handle without mutation. No core mutation may occur
    /// until the caller has completed the Copy used for its reply.
    [[nodiscard]] DirectoryCoreError Resolve(const DirectoryName& name, const DirectoryResolveQuery& query,
                                             DirectoryResolution& output) const noexcept;

    [[nodiscard]] DirectoryCoreError BeginQuiesce() noexcept;

    /// Transfers one stored handle to output for caller-owned Close. Every
    /// binding is returned at most once, in ascending slot order.
    [[nodiscard]] DirectoryCoreError TakeNextOwnedEndpoint(Kernel::Object::CapabilityHandle& output) noexcept;

    [[nodiscard]] DirectoryCoreSnapshot GetSnapshot() const noexcept;

private:
    struct BindingEntry
    {
        DirectoryName Name{};
        Kernel::Object::CapabilityHandle Custody{};
        Kernel::Ipc::InterfaceIdentifier ServiceInterface{};
        Foundation::UInt64 ServiceGeneration{0U};
        Kernel::Object::MonotonicTime TargetDeadline{};
        Kernel::Object::CapabilityRights PublishedRights{};
        Kernel::Object::CapabilityRights CustodyRights{};
        Kernel::Object::ObjectType Type{Kernel::Object::ObjectType::Invalid};
        InterfaceVersionRange Version{};
        Foundation::UInt64 Revision{0U};
        bool IsOccupied{false};
    };

    [[nodiscard]] static DirectoryCoreError ValidateCandidate(const DirectoryBindingCandidate& candidate) noexcept;
    [[nodiscard]] static bool IsMutationOutputOccupied(const DirectoryMutationReceipt& output) noexcept;
    [[nodiscard]] static bool IsResolutionOutputOccupied(const DirectoryResolution& output) noexcept;
    [[nodiscard]] static bool HandlesAreEqual(Kernel::Object::CapabilityHandle left,
                                              Kernel::Object::CapabilityHandle right) noexcept;
    [[nodiscard]] static bool InterfacesAreEqual(Kernel::Ipc::InterfaceIdentifier left,
                                                 Kernel::Ipc::InterfaceIdentifier right) noexcept;
    [[nodiscard]] Foundation::Size FindName(const DirectoryName& name) const noexcept;
    [[nodiscard]] Foundation::Size FindFreeSlot() const noexcept;
    [[nodiscard]] bool IsCustodyAlreadyOwned(Kernel::Object::CapabilityHandle custody) const noexcept;
    void StoreBinding(BindingEntry& entry, const DirectoryName& name, const DirectoryBindingCandidate& candidate,
                      Foundation::UInt64 revision) noexcept;

    BindingEntry m_entries[MaximumBindings]{};
    Foundation::Size m_bindingCount{0U};
    Foundation::UInt64 m_directoryGeneration{0U};
    Foundation::UInt64 m_lastIssuedRevision{0U};
    DirectoryCoreState m_state{DirectoryCoreState::InvalidConfiguration};
};

static_assert(sizeof(DirectoryCoreState) == 1U);
static_assert(sizeof(DirectoryCoreError) == 1U);
static_assert(sizeof(DirectoryCoreConfiguration) == 16U);
static_assert(alignof(DirectoryCoreConfiguration) == 8U);
static_assert(sizeof(DirectoryBindingCandidate) == 64U);
static_assert(alignof(DirectoryBindingCandidate) == 8U);
static_assert(sizeof(DirectoryResolveQuery) == 40U);
static_assert(alignof(DirectoryResolveQuery) == 8U);
static_assert(sizeof(DirectoryMutationReceipt) == 32U);
static_assert(alignof(DirectoryMutationReceipt) == 8U);
static_assert(sizeof(DirectoryResolution) == 72U);
static_assert(alignof(DirectoryResolution) == 8U);
static_assert(sizeof(DirectoryCoreSnapshot) == 40U);
static_assert(alignof(DirectoryCoreSnapshot) == 8U);
} // namespace Asteria::Executive::ObjectDirectory

#endif // ASTERIA_EXECUTIVE_OBJECT_DIRECTORY_OBJECT_DIRECTORY_CORE_HPP_INCLUDED
