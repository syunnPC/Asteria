// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_FAULT_TOKEN_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_FAULT_TOKEN_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Object/IKernelObject.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Scheduler/SchedulingContext.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Memory
{
/// Identifies one AddressSpace generation within one ObjectRegistry.
/// Lifetime:
/// - The originating registry outlives every copy and use of this value.
class AddressSpaceId final
{
public:
    constexpr AddressSpaceId() noexcept = default;

    constexpr AddressSpaceId(const Object::ObjectRegistry& registry, const Object::ObjectId object) noexcept
        : m_registry{&registry}, m_object{object}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetValue() const noexcept
    {
        return m_object.GetValue();
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_registry != nullptr) && m_object.IsValid();
    }

    [[nodiscard]] constexpr Object::ObjectId GetObjectId() const noexcept
    {
        return m_object;
    }

    [[nodiscard]] constexpr bool IsFromRegistry(const Object::ObjectRegistry& registry) const noexcept
    {
        return m_registry == &registry;
    }

    [[nodiscard]] constexpr bool IsEqualTo(const AddressSpaceId other) const noexcept
    {
        return (m_registry == other.m_registry) && (m_object.GetValue() == other.m_object.GetValue());
    }

private:
    const Object::ObjectRegistry* m_registry{nullptr};
    Object::ObjectId m_object{};
};

enum class FaultAccess : Foundation::UInt8
{
    Read = 1U,
    Write = 2U,
    Execute = 3U,
};

enum class FaultCause : Foundation::UInt8
{
    MissingMapping = 1U,
    ProtectionViolation = 2U,
    InvalidMappingEncoding = 3U,
    /// A user exception other than a page fault: VirtualAddress and InstructionPointer are the
    /// faulting instruction's address, ArchitectureSyndrome the platform's exception identity. No
    /// mapping resolves it; a supervisor can only terminate the thread.
    Exception = 4U,
};

enum class FaultPrivilege : Foundation::UInt8
{
    User = 1U,
    Supervisor = 2U,
};

struct FaultRecord
{
    Foundation::UInt64 VirtualAddress;
    Foundation::UInt64 InstructionPointer;
    Foundation::UInt64 ArchitectureSyndrome;
    FaultAccess Access;
    FaultCause Cause;
    FaultPrivilege Privilege;
    Foundation::UInt8 Reserved[5];
};

enum class FaultTokenState : Foundation::UInt8
{
    Uninitialized,
    Pending,
    Claimed,
    Resolved,
};

enum class FaultResolutionKind : Foundation::UInt8
{
    None,
    Map,
    Terminate,
    Redirect,
};

enum class FaultTokenError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    InvalidFaultRecord,
    InvalidThread,
    InvalidAddressSpace,
    ThreadRegistryMismatch,
    AddressSpaceRegistryMismatch,
    ThreadNotFound,
    ThreadTypeMismatch,
    ThreadPinUnavailable,
    AddressSpaceNotFound,
    AddressSpaceTypeMismatch,
    AddressSpacePinUnavailable,
    AlreadyInitialized,
    NotInitialized,
    AlreadyClaimed,
    NotClaimed,
    AlreadyResolved,
    InvalidClaim,
    InvalidResolution,
    ClaimSequenceExhausted,
};

class FaultClaimSequencePolicy final
{
public:
    static constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U}};

    FaultClaimSequencePolicy() = delete;

    [[nodiscard]] static constexpr bool TryAdvance(const Foundation::UInt64 current, Foundation::UInt64& next) noexcept
    {
        if (current == Maximum)
        {
            return false;
        }
        next = current + 1U;
        return true;
    }
};

class IPagerResolution;
class FaultToken;
class FaultTokenFactory;

/// Names one exclusive resolver claim on one immovable FaultToken.
/// Lifetime:
/// - The originating FaultToken outlives every copy and use of this token.
class FaultResolutionClaimToken final
{
public:
    constexpr FaultResolutionClaimToken() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetSequence() const noexcept
    {
        return m_sequence;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_faultToken != nullptr) && (m_sequence != 0U);
    }

    [[nodiscard]] constexpr bool IsEqualTo(const FaultResolutionClaimToken other) const noexcept
    {
        return (m_faultToken == other.m_faultToken) && (m_sequence == other.m_sequence);
    }

private:
    friend class FaultToken;

    constexpr FaultResolutionClaimToken(const FaultToken& faultToken, const Foundation::UInt64 sequence) noexcept
        : m_faultToken{&faultToken}, m_sequence{sequence}
    {
    }

    const FaultToken* m_faultToken{nullptr};
    Foundation::UInt64 m_sequence{0U};
};

struct FaultResolutionClaim
{
    FaultResolutionClaimToken Token;
    Scheduler::ThreadId Thread;
    AddressSpaceId AddressSpace;
    FaultRecord Fault;
};

struct FaultTokenSnapshot
{
    Scheduler::ThreadId Thread;
    AddressSpaceId AddressSpace;
    FaultRecord Fault;
    Foundation::UInt64 ClaimSequence;
    FaultTokenState State;
    FaultResolutionKind Resolution;
    Foundation::UInt8 Reserved[6];
};

/// Retains one faulting Thread and AddressSpace generation while a pager resolves one fault exactly
/// once. Claiming is separate from mapping. The mapper performs its validation
/// and page-table transaction while it owns a claim, then calls CommitResolution only after that
/// transaction succeeds. A failed transaction calls CancelResolution and leaves the token pending.
/// The object registry and the retained Thread and AddressSpace storage outlive this object.
/// Operations are internally serialized and may spin; callers prevent same-CPU reentry and do not
/// call them from interrupt context.
class FaultToken final : public Object::IKernelObject
{
public:
    constexpr FaultToken() noexcept = default;

    /// Preconditions:
    /// - No resolver retains a claim on this object.
    /// - No concurrent operation accesses this object during destruction.
    ~FaultToken() noexcept = default;

    FaultToken(const FaultToken&) = delete;
    FaultToken& operator=(const FaultToken&) = delete;
    FaultToken(FaultToken&&) = delete;
    FaultToken& operator=(FaultToken&&) = delete;

    /// Pins the exact Thread and AddressSpace generations and publishes one
    /// pending fault. Failure changes no externally observable state.
    [[nodiscard]] FaultTokenError Initialize(Object::ObjectRegistry& registry, Object::ResourceDomainId chargeOwner,
                                             Scheduler::ThreadId thread, AddressSpaceId addressSpace,
                                             const FaultRecord& fault) noexcept;

    /// Acquires exclusive authority to resolve this fault. At most one claim is
    /// active. Cancelled claim generations can never be reused.
    [[nodiscard]] Foundation::Result<FaultResolutionClaim, FaultTokenError> ClaimResolution() noexcept;

    /// Returns the authoritative data for the exact active claim. The caller
    /// owns that claim exclusively and does not cancel or commit it concurrently.
    [[nodiscard]] Foundation::Result<FaultResolutionClaim, FaultTokenError>
    GetResolutionClaim(FaultResolutionClaimToken claim) const noexcept;

    /// Cancels the exact active claim and makes the fault pending again.
    [[nodiscard]] FaultTokenError CancelResolution(FaultResolutionClaimToken claim) noexcept;

    /// Consumes the exact active claim after Map, Terminate, or Redirect has
    /// completed successfully. A resolved token can never be claimed again.
    [[nodiscard]] FaultTokenError CommitResolution(FaultResolutionClaimToken claim,
                                                   FaultResolutionKind resolution) noexcept;

    // Immutable while registered; the issuing channel is pinned by this token.
    [[nodiscard]] Object::ObjectPinError PinResolutionChannel(Object::ObjectPin& pin) const noexcept;
    [[nodiscard]] IPagerResolution* GetResolution() const noexcept
    {
        return m_issuerResolution;
    }
    [[nodiscard]] Object::ObjectId GetChannelIdentity() const noexcept
    {
        return m_channelPin.IsValid() ? m_channelPin.GetObjectId() : Object::ObjectId{};
    }
    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] FaultTokenSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

private:
    friend class FaultTokenFactory;

    /// Returns ClaimSequenceExhausted after a successful reset when this
    /// storage must be retired instead of reused. Other failures leave the
    /// token unchanged.
    [[nodiscard]] FaultTokenError ResetForFactoryReuse() noexcept;
    [[nodiscard]] FaultTokenSnapshot GetSnapshotLocked() const noexcept;

    mutable Platform::Synchronization::SpinLock m_synchronization;
    Platform::Synchronization::AtomicUInt64 m_chargeOwnerValue;
    Object::ObjectRegistry* m_registry{nullptr};
    IPagerResolution* m_issuerResolution{nullptr};
    Object::ObjectPin m_channelPin;
    Object::ObjectPin m_threadPin;
    Object::ObjectPin m_addressSpacePin;
    Scheduler::ThreadId m_thread;
    AddressSpaceId m_addressSpace;
    FaultRecord m_fault{};
    Foundation::UInt64 m_claimSequence{0U};
    FaultTokenState m_state{FaultTokenState::Uninitialized};
    FaultResolutionKind m_resolution{FaultResolutionKind::None};
};

static_assert(sizeof(AddressSpaceId) == 16U);
static_assert(alignof(AddressSpaceId) == 8U);
static_assert(sizeof(FaultRecord) == 32U);
static_assert(alignof(FaultRecord) == 8U);
static_assert(sizeof(FaultResolutionClaimToken) == 16U);
static_assert(alignof(FaultResolutionClaimToken) == 8U);
static_assert(sizeof(FaultResolutionClaim) == 80U);
static_assert(alignof(FaultResolutionClaim) == 8U);
static_assert(sizeof(FaultTokenSnapshot) == 80U);
static_assert(alignof(FaultTokenSnapshot) == 8U);
static_assert(sizeof(FaultAccess) == 1U);
static_assert(sizeof(FaultCause) == 1U);
static_assert(sizeof(FaultPrivilege) == 1U);
static_assert(sizeof(FaultTokenState) == 1U);
static_assert(sizeof(FaultResolutionKind) == 1U);
static_assert(sizeof(FaultTokenError) == 1U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_FAULT_TOKEN_HPP_INCLUDED
