// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_FAULT_TOKEN_FACTORY_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_FAULT_TOKEN_FACTORY_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/FaultToken.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Memory
{
enum class FaultTokenFactoryState : Foundation::UInt8
{
    Ready,
    Faulted,
};

enum class FaultTokenFactoryError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    CapacityExceeded,
    TokenRejected,
    RegistrationRejected,
    CapabilityRejected,
    RollbackRejected,
    InvalidToken,
    TokenNotOwned,
    TokenBusy,
    DestroyRejected,
    InvalidDestructionRecord,
    RecycleRejected,
    Faulted,
};

struct FaultTokenCreateRequest
{
    Object::CapabilitySpace* Destination;
    Object::ResourceDomainId ChargeOwner;
    Scheduler::ThreadId Thread;
    AddressSpaceId AddressSpace;
    FaultRecord Fault;
    Object::RevocationDomainId RevocationDomain;
    Object::MonotonicTime LeaseDeadline;
    IPagerResolution* Resolution{nullptr};
    Object::ObjectId ChannelIdentity{};
};

struct FaultTokenCreation
{
    FaultToken* Storage;
    Object::ObjectId Token;
    Object::CapabilityHandle Capability;
};

struct FaultTokenFactoryFailure
{
    Object::ObjectId Token;
    FaultTokenError TokenError;
    Object::ObjectRegistryError RegistryError;
    Object::CapabilityError CapabilityError;
    FaultTokenFactoryError Error;
    Foundation::UInt8 Reserved[4];
};

struct FaultTokenFactorySnapshot
{
    Foundation::Size FreeSlots;
    Foundation::Size ActiveSlots;
    Foundation::Size QuiescingSlots;
    Foundation::Size RetiredSlots;
    Foundation::Size FaultedSlots;
    FaultTokenFactoryState State;
    Foundation::UInt8 Reserved[7];
};

/// Owns a fixed set of reusable FaultToken objects and the privileged register/mint/retire
/// transaction around them. The factory never allocates. A successful Create publishes exactly one
/// Invoke+Transfer capability. Retire first removes the object from future lookup; a central
/// registry collector later passes the exact destruction record to Reclaim so the slot can be
/// reused without claim-token ABA.
class FaultTokenFactory final
{
public:
    static constexpr Foundation::Size MaximumTokens{16U};

    /// Preconditions:
    /// - issuer is bound to objectRegistry;
    /// - both collaborators and every capability space used by Create outlive
    /// this factory and all tokens it publishes;
    /// - callers prevent same-CPU reentry through the shared registry lock.
    constexpr FaultTokenFactory(Object::ObjectRegistry& objectRegistry, Object::RootCapabilityIssuer& issuer) noexcept
        : m_objectRegistry{objectRegistry}, m_issuer{issuer}
    {
    }

    /// Preconditions:
    /// - every slot is Free or Retired;
    /// - no concurrent operation accesses this object.
    ~FaultTokenFactory() noexcept = default;

    FaultTokenFactory(const FaultTokenFactory&) = delete;
    FaultTokenFactory& operator=(const FaultTokenFactory&) = delete;
    FaultTokenFactory(FaultTokenFactory&&) = delete;
    FaultTokenFactory& operator=(FaultTokenFactory&&) = delete;

    /// Initializes, registers, and mints one exact FaultToken. Before registry
    /// publication, failure restores the slot immediately. A mint failure
    /// starts registry destruction and leaves one Quiescing slot for Reclaim.
    [[nodiscard]] Foundation::Result<FaultTokenCreation, FaultTokenFactoryFailure>
    Create(const FaultTokenCreateRequest& request) noexcept;

    /// Starts destruction of one owned Pending or Resolved token. The caller
    /// has exclusive lifecycle access, no claim is active, and all dispatchers
    /// have released or will release their retained pins before collection.
    [[nodiscard]] Foundation::Result<Object::ObjectId, FaultTokenFactoryFailure>
    Retire(Object::ObjectId token) noexcept;

    /// Reclaims the exact record returned by the shared registry collector.
    /// The record has not been offered to any other storage owner.
    [[nodiscard]] Foundation::Result<Object::ObjectId, FaultTokenFactoryFailure>
    Reclaim(const Object::ObjectDestructionRecord& record) noexcept;

    [[nodiscard]] FaultTokenFactorySnapshot GetSnapshot() const noexcept;

private:
    enum class SlotState : Foundation::UInt8
    {
        Free,
        Active,
        Quiescing,
        Retired,
        Faulted,
    };

    struct Slot
    {
        FaultToken Token;
        Object::ObjectId Identity;
        SlotState State{SlotState::Free};
    };

    [[nodiscard]] static FaultTokenFactoryFailure
    MakeFailure(FaultTokenFactoryError error, Object::ObjectId token = {},
                FaultTokenError tokenError = FaultTokenError::None,
                Object::ObjectRegistryError registryError = Object::ObjectRegistryError::Ok,
                Object::CapabilityError capabilityError = Object::CapabilityError::Ok) noexcept;
    [[nodiscard]] Slot* FindFreeSlotLocked() noexcept;
    [[nodiscard]] Slot* FindSlotLocked(Object::ObjectId token) noexcept;
    [[nodiscard]] Slot* FindSlotLocked(Object::IKernelObject* instance) noexcept;
    void EnterFaultedLocked(Slot& slot) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    Object::RootCapabilityIssuer& m_issuer;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Slot m_slots[MaximumTokens]{};
    FaultTokenFactoryState m_state{FaultTokenFactoryState::Ready};
};

static_assert(sizeof(FaultTokenFactoryState) == 1U);
static_assert(sizeof(FaultTokenFactoryError) == 1U);
static_assert(sizeof(FaultTokenCreateRequest) == 112U);
static_assert(alignof(FaultTokenCreateRequest) == 8U);
static_assert(sizeof(FaultTokenCreation) == 32U);
static_assert(alignof(FaultTokenCreation) == 8U);
static_assert(sizeof(FaultTokenFactoryFailure) == 16U);
static_assert(alignof(FaultTokenFactoryFailure) == 8U);
static_assert(sizeof(FaultTokenFactorySnapshot) == 48U);
static_assert(alignof(FaultTokenFactorySnapshot) == 8U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_FAULT_TOKEN_FACTORY_HPP_INCLUDED
