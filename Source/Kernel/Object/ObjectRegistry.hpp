// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_OBJECT_OBJECT_REGISTRY_HPP_INCLUDED
#define ASTERIA_KERNEL_OBJECT_OBJECT_REGISTRY_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Object/CapabilityTypes.hpp"
#include "Kernel/Object/IKernelObject.hpp"
#include "Kernel/Object/SlotBitmap.hpp"
#include "Platform/Synchronization/AtomicReferenceCount.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Object
{
class CapabilitySpace;
class CapabilityLease;
class ObjectPin;
class ObjectRegistry;
class RootCapabilityIssuer;
enum class ObjectPinError : Foundation::UInt8;

enum class ObjectRegistryError : Foundation::UInt8
{
    Ok,
    InvalidArgument,
    InvalidObject,
    RegistryExhausted,
    ReferenceExhausted,
    NoDestructionReady,
    ExclusiveRangeConflict,
};

struct ObjectInformation
{
    ObjectType Type;
    ResourceDomainId ChargeOwner;
};

/// One half-open interval [Start, End) of a resource that a registered object
/// may claim exclusively among the registered objects of its type. The type
/// defines the unit: a Frame claims physical byte addresses.
struct ObjectExclusiveRange
{
    Foundation::UInt64 Start;
    Foundation::UInt64 End;
};

/// Copyable proof that the registry has removed one exact object generation
/// after observing that its final lease and pin were released. Callers can
/// default-construct only an invalid record; only ObjectRegistry can create a
/// valid one.
class ObjectDestructionRecord final
{
public:
    constexpr ObjectDestructionRecord() noexcept = default;

    const ObjectId Object{};
    IKernelObject* const Instance{nullptr};

    [[nodiscard]] constexpr bool IsFromRegistry(const ObjectRegistry& registry) const noexcept
    {
        return m_registry == &registry;
    }

private:
    friend class ObjectRegistry;

    constexpr ObjectDestructionRecord(const ObjectRegistry& registry, const ObjectId object,
                                      IKernelObject* const instance) noexcept
        : Object{object}, Instance{instance}, m_registry{&registry}
    {
    }

    const ObjectRegistry* m_registry{nullptr};
};

/// The objects of one kernel: their identities, lifetimes, pins and claims. The entries live in
/// storage the platform places and attaches once (AttachEntries; the
/// AMD64 metadata arena, IA-64's runtime page). A bitmap selects the lowest free entry,
/// the collector visits only quiescing entries, and claims are
/// kept in a balanced tree of the claimed entries, so no operation scans the entry table.
class ObjectRegistry
{
public:
    // The ECAM catalog can publish 256 PCI functions; the Frame and MemoryObject pools hold 4,096
    // each, and the rest are boot objects and the bounded run-time pools.
    static constexpr Foundation::Size MaximumObjects{16384U};

    /// Execution context for operations that acquire the shared registry lock:
    /// - The caller prevents same-CPU preemption or reentry by code using that lock.

    /// Creates an empty registry without entries: every registration is
    /// RegistryExhausted until AttachEntries.
    /// Preconditions:
    /// - synchronization outlives this registry.
    /// - this registry outlives every object lease, pin, or destruction receipt
    /// obtained from it.
    /// - maximumReferencesPerObject is the real per-object capacity for the
    /// combined lease-and-pin count. Zero deliberately disables acquisition;
    /// the default retains the full UInt32 saturation limit.
    /// Interrupt safety:
    /// - Must not be called from an ISR.
    explicit constexpr ObjectRegistry(Platform::Synchronization::SpinLock& synchronization,
                                      Foundation::UInt32 maximumReferencesPerObject =
                                          Platform::Synchronization::AtomicReferenceCount::MaximumValue) noexcept
        : m_synchronization{synchronization}, m_maximumReferencesPerObject{maximumReferencesPerObject}
    {
    }

    ObjectRegistry(const ObjectRegistry&) = delete;
    ObjectRegistry& operator=(const ObjectRegistry&) = delete;
    ObjectRegistry(ObjectRegistry&&) = delete;
    ObjectRegistry& operator=(ObjectRegistry&&) = delete;

    /// Builds capacity free entries (1 to MaximumObjects) in storage,
    /// capacity * EntryBytes bytes aligned to EntryAlignment that outlive the
    /// registry and nothing else uses; once only, before any registration.
    /// False leaves the registry without entries.
    [[nodiscard]] bool AttachEntries(void* storage, Foundation::Size capacity) noexcept;

    /// The entries AttachEntries built; zero before.
    [[nodiscard]] Foundation::Size GetCapacity() const noexcept;

    /// Returns the number of alive objects; quiescing objects are excluded.
    /// Blocking:
    /// - May spin while another CPU holds the shared registry lock.
    [[nodiscard]] Foundation::Size GetActiveCount() const noexcept;

    /// Acquires a persistent, generation-safe kernel reference to an object of the expected
    /// type. This operation grants no authority; callers authorize the relationship separately.
    /// On failure output remains unchanged.
    /// Preconditions:
    /// - The caller has exclusive access to output.
    /// - The caller prevents same-CPU preemption or reentry by code using the registry lock.
    /// - Installing the pin preserves the kernel's acyclic strong-reference graph.
    /// Blocking:
    /// - May spin while another CPU holds the shared registry lock.
    [[nodiscard]] ObjectPinError PinObject(ObjectId object, ObjectType expectedType, ObjectPin& output) noexcept;

    /// Records range as the exclusive claim of one exact registered object
    /// generation of expectedType, alive or quiescing. The claim is refused
    /// with ExclusiveRangeConflict while another occupied entry of the same
    /// type holds an overlapping claim; it ends with ReleaseExclusiveRange or
    /// when the collector returns the object. An empty range, an unknown type,
    /// or an object that already holds a claim is an InvalidArgument; an
    /// identity that names no occupied entry of expectedType is an
    /// InvalidObject. Failure changes nothing. Claims of one type are
    /// disjoint, so the check reads only the claim that precedes range's end
    /// in the claim tree.
    /// Blocking:
    /// - May spin while another CPU holds the shared registry lock.
    [[nodiscard]] ObjectRegistryError ClaimExclusiveRange(ObjectId object, ObjectType expectedType,
                                                          ObjectExclusiveRange range) noexcept;

    /// Ends the claim of one exact occupied object generation. An identity
    /// without an occupied entry is an InvalidObject; an entry without a
    /// claim is an InvalidArgument.
    /// Blocking:
    /// - May spin while another CPU holds the shared registry lock.
    [[nodiscard]] ObjectRegistryError ReleaseExclusiveRange(ObjectId object) noexcept;

private:
    friend class CapabilitySpace;
    friend class CapabilityLease;
    friend class ObjectPin;
    friend class RootCapabilityIssuer;

    struct ObjectEntry
    {
        bool IsOccupied;
        bool IsQuiescing;
        bool IsRetired;
        // The claim tree's links (entry index + 1, zero for none) and the
        // height of this entry's subtree; zero while it holds no claim.
        Foundation::UInt8 ClaimHeight;
        Foundation::UInt16 ClaimLeft;
        Foundation::UInt16 ClaimRight;
        Foundation::UInt64 Generation;
        Platform::Synchronization::AtomicReferenceCount InFlightCount;
        IKernelObject* Instance;
        ObjectInformation Information;
        ObjectExclusiveRange ExclusiveRange; // Claimed when Start < End
    };

public:
    /// The storage AttachEntries takes per entry.
    static constexpr Foundation::Size EntryBytes{sizeof(ObjectEntry)};
    static constexpr Foundation::Size EntryAlignment{alignof(ObjectEntry)};

private:
    /// An AVL tree of 16,384 entries is at most 19 deep (its minimum size
    /// at depth h is the Fibonacci number F(h + 2) - 1, and F(22) - 1 =
    /// 17,710 exceeds MaximumObjects), so 24 links bound every path.
    static constexpr Foundation::Size ClaimTreeMaximumDepth{24U};

    [[nodiscard]] Foundation::Result<ObjectId, ObjectRegistryError> RegisterObject(IKernelObject& instance) noexcept;

    [[nodiscard]] ObjectRegistryError DestroyObject(ObjectId object) noexcept;

    [[nodiscard]] Foundation::Result<ObjectDestructionRecord, ObjectRegistryError> CollectDestroyedObject() noexcept;

    [[nodiscard]] Foundation::Result<ObjectDestructionRecord, ObjectRegistryError>
    CollectDestroyedObject(ObjectId object) noexcept;

    [[nodiscard]] Foundation::Result<ObjectInformation, ObjectRegistryError>
    GetInformationLocked(ObjectId object) const noexcept;

    /// Returns the registered instance of a live (not quiescing) object, or
    /// nullptr. Takes no reference; the result is only compared.
    [[nodiscard]] const IKernelObject* GetInstanceLocked(ObjectId object) const noexcept;

    /// Returns the occupied entry of the exact generation, quiescing or not.
    [[nodiscard]] ObjectEntry* FindOccupiedEntryLocked(ObjectId object) noexcept;

    [[nodiscard]] Foundation::Result<IKernelObject*, ObjectRegistryError>
    AcquireReferenceLocked(ObjectId object) noexcept;
    void ReleaseReference(ObjectId object) noexcept;
    void FinalizeEntryLocked(ObjectEntry& entry) noexcept;
    /// The slot value (index + 1) of an identity naming one of the attached
    /// entries, or zero.
    [[nodiscard]] Foundation::UInt64 GetSlotValue(ObjectId object) const noexcept;
    [[nodiscard]] Foundation::Size IndexOf(const ObjectEntry& entry) const noexcept;

    // The claim tree, keyed by (type, start): claims of one type are
    // disjoint, so (type, start) is unique among claimed entries.
    [[nodiscard]] ObjectEntry& ClaimNode(Foundation::UInt16 link) const noexcept;
    [[nodiscard]] Foundation::UInt8 ClaimHeightOf(Foundation::UInt16 link) const noexcept;
    [[nodiscard]] static bool ClaimPrecedes(const ObjectEntry& entry, ObjectType type,
                                            Foundation::UInt64 start) noexcept;
    /// The claimed entry with the greatest key below (type, end), or null.
    [[nodiscard]] const ObjectEntry* FindClaimBeforeLocked(ObjectType type, Foundation::UInt64 end) const noexcept;
    void InsertClaimLocked(ObjectEntry& entry) noexcept;
    void RemoveClaimLocked(ObjectEntry& entry) noexcept;
    void RebalanceClaim(Foundation::UInt16& link) noexcept;
    [[nodiscard]] Foundation::UInt16 RotateClaimLeft(Foundation::UInt16 link) noexcept;
    [[nodiscard]] Foundation::UInt16 RotateClaimRight(Foundation::UInt16 link) noexcept;
    void UpdateClaimHeight(ObjectEntry& entry) noexcept;

    [[nodiscard]] Platform::Synchronization::SpinLock& GetSynchronization() const noexcept;

    Platform::Synchronization::SpinLock& m_synchronization;
    const Foundation::UInt32 m_maximumReferencesPerObject;
    ObjectEntry* m_entries{nullptr};
    Foundation::Size m_capacity{0U};
    // Unoccupied entries that are not retired, and occupied quiescing ones.
    SlotBitmap<MaximumObjects> m_free{};
    SlotBitmap<MaximumObjects> m_quiescing{};
    Foundation::UInt16 m_claimRoot{0U};
    Foundation::Size m_activeCount{0U};
};

static_assert(ObjectRegistry::EntryBytes == 64U);
} // Namespace Asteria::Kernel::Object

#endif // ASTERIA_KERNEL_OBJECT_OBJECT_REGISTRY_HPP_INCLUDED
