// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_OBJECT_REVOCATION_REGISTRY_HPP_INCLUDED
#define ASTERIA_KERNEL_OBJECT_REVOCATION_REGISTRY_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Object/CapabilityTypes.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Object
{
enum class RevocationError : Foundation::UInt8
{
    Ok,
    InvalidDomain,
    DomainExhausted,
    DepthExceeded,
    Revoked,
};

class RevocationRegistry final
{
public:
    static constexpr Foundation::Size MaximumDomains{64U};
    static constexpr Foundation::UInt8 MaximumDepth{8U};

    /// Creates an empty, fixed-capacity registry.
    /// Preconditions:
    /// - synchronization outlives this registry and every bound capability space.
    /// Blocking:
    /// - May spin while another registry receives its boot-unique provenance identifier.
    /// Execution context:
    /// - Construction is bootstrap-only; the caller prevents same-CPU preemption or reentry.
    /// Interrupt safety:
    /// - Must not be called from an ISR.
    explicit RevocationRegistry(Platform::Synchronization::SpinLock& synchronization) noexcept;

    RevocationRegistry(const RevocationRegistry&) = delete;
    RevocationRegistry& operator=(const RevocationRegistry&) = delete;
    RevocationRegistry(RevocationRegistry&&) = delete;
    RevocationRegistry& operator=(RevocationRegistry&&) = delete;

    /// These methods are internal kernel mechanisms. A syscall boundary must first authorize
    /// the caller through a capability carrying the required management or revocation right.
    /// Execution context:
    /// - The caller prevents same-CPU preemption or reentry by code using the shared lock.
    /// Blocking:
    /// - Each method may spin while another CPU holds the shared registry lock.
    [[nodiscard]] Foundation::Result<RevocationDomainId, RevocationError> CreateRoot() noexcept;

    [[nodiscard]] Foundation::Result<RevocationDomainId, RevocationError>
    CreateChild(RevocationDomainId parent) noexcept;

    [[nodiscard]] Foundation::Result<Foundation::UInt32, RevocationError>
    GetCurrentEpoch(RevocationDomainId domain) const noexcept;

    [[nodiscard]] RevocationError Revoke(RevocationDomainId domain) noexcept;

    /// Ends domain and every descendant: their identifiers become unknown
    /// (InvalidDomain), capabilities minted in them report Revoked and stay
    /// closeable, and their slots become reusable under an advanced 16-bit
    /// generation. A slot whose generation is exhausted is retired instead,
    /// so no identifier is ever issued twice. A revoked domain can be
    /// destroyed.
    [[nodiscard]] RevocationError Destroy(RevocationDomainId domain) noexcept;

    [[nodiscard]] bool IsCurrent(RevocationDomainId domain, Foundation::UInt32 expectedEpoch) const noexcept;

    /// Returns false only if the boot-unique registry identifier space was exhausted.
    [[nodiscard]] bool IsReady() const noexcept;

private:
    friend class CapabilitySpace;

    struct DomainEntry
    {
        bool IsOccupied;
        bool IsPermanentlyRevoked;
        Foundation::UInt16 Generation;
        Foundation::UInt32 Epoch;
        RevocationDomainId Parent;
        Foundation::UInt32 ParentEpochAtCreation;
        Foundation::UInt8 Depth;
        bool IsRetired;
    };

    static_assert(MaximumDomains <= 64U, "Destroy marks a subtree in one 64-bit set");

    [[nodiscard]] Foundation::Result<RevocationDomainId, RevocationError>
    CreateDomainLocked(RevocationDomainId parent, Foundation::UInt32 parentEpoch, Foundation::UInt8 depth) noexcept;

    [[nodiscard]] Foundation::Result<RevocationDomainId, RevocationError>
    CreateChildLocked(RevocationDomainId parent) noexcept;

    [[nodiscard]] RevocationError RevokeLocked(RevocationDomainId domain) noexcept;

    [[nodiscard]] bool IsInSubtreeLocked(const DomainEntry& entry, const DomainEntry& root) const noexcept;

    [[nodiscard]] Foundation::Result<Foundation::UInt32, RevocationError>
    GetCurrentEpochLocked(RevocationDomainId domain) const noexcept;

    [[nodiscard]] bool IsCurrentLocked(RevocationDomainId domain, Foundation::UInt32 expectedEpoch) const noexcept;

    [[nodiscard]] const DomainEntry* FindEntryLocked(RevocationDomainId domain) const noexcept;
    [[nodiscard]] DomainEntry* FindEntryLocked(RevocationDomainId domain) noexcept;
    [[nodiscard]] bool IsLineageCurrentLocked(const DomainEntry& entry) const noexcept;
    [[nodiscard]] Platform::Synchronization::SpinLock& GetSynchronization() const noexcept;

    Platform::Synchronization::SpinLock& m_synchronization;
    Foundation::UInt32 m_registryIdentifier;
    DomainEntry m_entries[MaximumDomains]{};
};
} // namespace Asteria::Kernel::Object

#endif // ASTERIA_KERNEL_OBJECT_REVOCATION_REGISTRY_HPP_INCLUDED
