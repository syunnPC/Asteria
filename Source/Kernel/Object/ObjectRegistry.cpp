// SPDX-License-Identifier: Apache-2.0

#include "ObjectRegistry.hpp"

#include "Kernel/Object/ObjectPin.hpp"

#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
#include "Runtime/Placement.hpp"
#else
#include <new>
#endif

namespace Asteria::Kernel::Object
{
namespace
{
constexpr Foundation::UInt64 ObjectSlotMask{0xFFFFULL};
constexpr Foundation::UInt64 ObjectGenerationShift{16U};
constexpr Foundation::UInt64 MaximumObjectGeneration{0x0000'FFFF'FFFF'FFFFULL};

[[nodiscard]] constexpr bool IsClaimed(const ObjectExclusiveRange range) noexcept
{
    return range.Start < range.End;
}

// Both ranges are nonempty or the unclaimed {0, 0}, which overlaps nothing.
[[nodiscard]] constexpr bool RangesOverlap(const ObjectExclusiveRange first, const ObjectExclusiveRange second) noexcept
{
    return (first.Start < second.End) && (second.Start < first.End);
}
}

bool ObjectRegistry::AttachEntries(void* const storage, const Foundation::Size capacity) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_entries != nullptr) || (storage == nullptr) || (capacity == 0U) || (capacity > MaximumObjects) ||
        ((reinterpret_cast<Foundation::UIntPointer>(storage) % EntryAlignment) != 0U))
    {
        return false;
    }
    // Begin each entry's and reference counter's lifetime in the attached
    // storage before any method accesses it. Placement constructs only;
    // the platform owns the slab and no allocation occurs.
    auto* const entries = static_cast<ObjectEntry*>(storage);
    for (Foundation::Size index{0U}; index < capacity; ++index)
    {
        ::new (static_cast<void*>(&entries[index])) ObjectEntry{};
        m_free.Insert(index);
    }
    m_entries = entries;
    m_capacity = capacity;
    return true;
}

Foundation::Size ObjectRegistry::GetCapacity() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_capacity;
}

Foundation::Size ObjectRegistry::GetActiveCount() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_activeCount;
}

ObjectPinError ObjectRegistry::PinObject(const ObjectId object, const ObjectType expectedType,
                                         ObjectPin& output) noexcept
{
    if (!object.IsValid() || !IsKnownObjectType(expectedType))
    {
        return ObjectPinError::InvalidArgument;
    }
    if (output.IsValid())
    {
        return ObjectPinError::OutputOccupied;
    }

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const auto informationResult = GetInformationLocked(object);
    if (!informationResult.HasValue())
    {
        return ObjectPinError::ObjectNotFound;
    }
    if (informationResult.GetValue().Type != expectedType)
    {
        return ObjectPinError::TypeMismatch;
    }

    const auto referenceResult = AcquireReferenceLocked(object);
    if (!referenceResult.HasValue())
    {
        // Information and reference acquisition share this lock, so saturation is the only
        // remaining failure after the successful identity lookup above.
        return ObjectPinError::ReferenceExhausted;
    }

    output.m_registry = this;
    output.m_object = referenceResult.GetValue();
    output.m_objectId = object;
    output.m_objectType = informationResult.GetValue().Type;
    return ObjectPinError::Ok;
}

ObjectRegistryError ObjectRegistry::ClaimExclusiveRange(const ObjectId object, const ObjectType expectedType,
                                                        const ObjectExclusiveRange range) noexcept
{
    if (!IsKnownObjectType(expectedType) || !IsClaimed(range))
    {
        return ObjectRegistryError::InvalidArgument;
    }

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    ObjectEntry* const claimant = FindOccupiedEntryLocked(object);
    if ((claimant == nullptr) || (claimant->Information.Type != expectedType))
    {
        return ObjectRegistryError::InvalidObject;
    }
    if (IsClaimed(claimant->ExclusiveRange))
    {
        return ObjectRegistryError::InvalidArgument;
    }
    // Claims of one type are disjoint, so of those starting before range
    // ends, only the last can reach into it.
    const ObjectEntry* const before = FindClaimBeforeLocked(expectedType, range.End);
    if ((before != nullptr) && (before->Information.Type == expectedType) &&
        RangesOverlap(range, before->ExclusiveRange))
    {
        return ObjectRegistryError::ExclusiveRangeConflict;
    }
    claimant->ExclusiveRange = range;
    InsertClaimLocked(*claimant);
    return ObjectRegistryError::Ok;
}

ObjectRegistryError ObjectRegistry::ReleaseExclusiveRange(const ObjectId object) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    ObjectEntry* const claimant = FindOccupiedEntryLocked(object);
    if (claimant == nullptr)
    {
        return ObjectRegistryError::InvalidObject;
    }
    if (!IsClaimed(claimant->ExclusiveRange))
    {
        return ObjectRegistryError::InvalidArgument;
    }
    RemoveClaimLocked(*claimant);
    claimant->ExclusiveRange = ObjectExclusiveRange{};
    return ObjectRegistryError::Ok;
}

Foundation::Result<ObjectId, ObjectRegistryError> ObjectRegistry::RegisterObject(IKernelObject& instance) noexcept
{
    const ObjectType type = instance.GetObjectType();
    const ResourceDomainId chargeOwner = instance.GetChargeOwner();
    if (!IsKnownObjectType(type) || !chargeOwner.IsValid())
    {
        return Foundation::Result<ObjectId, ObjectRegistryError>::Failure(ObjectRegistryError::InvalidArgument);
    }

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    // The lowest unoccupied entry that is not retired.
    const Foundation::Size index = m_free.FindFirst();
    if (index >= m_capacity)
    {
        return Foundation::Result<ObjectId, ObjectRegistryError>::Failure(ObjectRegistryError::RegistryExhausted);
    }
    ObjectEntry& entry = m_entries[index];
    if (entry.Generation == 0U)
    {
        entry.Generation = 1U;
    }

    entry.IsOccupied = true;
    entry.IsQuiescing = false;
    entry.InFlightCount.Reset();
    entry.Instance = &instance;
    entry.Information = ObjectInformation{
        .Type = type,
        .ChargeOwner = chargeOwner,
    };
    m_free.Remove(index);
    ++m_activeCount;

    const Foundation::UInt64 slotValue = static_cast<Foundation::UInt64>(index + 1U);
    const Foundation::UInt64 rawValue = slotValue | (entry.Generation << ObjectGenerationShift);
    return Foundation::Result<ObjectId, ObjectRegistryError>::Success(ObjectId{rawValue});
}

ObjectRegistryError ObjectRegistry::DestroyObject(const ObjectId object) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};

    const Foundation::UInt64 slotValue = GetSlotValue(object);
    const Foundation::UInt64 generation = object.GetValue() >> ObjectGenerationShift;
    if ((slotValue == 0U) || (generation == 0U))
    {
        return ObjectRegistryError::InvalidObject;
    }

    const Foundation::Size index = static_cast<Foundation::Size>(slotValue - 1U);
    ObjectEntry& entry = m_entries[index];
    if (!entry.IsOccupied || entry.IsQuiescing || (entry.Generation != generation))
    {
        return ObjectRegistryError::InvalidObject;
    }

    entry.IsQuiescing = true;
    m_quiescing.Insert(index);
    --m_activeCount;
    return ObjectRegistryError::Ok;
}

Foundation::Result<ObjectDestructionRecord, ObjectRegistryError> ObjectRegistry::CollectDestroyedObject() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};

    // Only the quiescing entries: each step finds the next one in the bitmap.
    for (Foundation::Size index = m_quiescing.FindFirst(); index < m_capacity;
         index = m_quiescing.FindFirst(index + 1U))
    {
        ObjectEntry& entry = m_entries[index];
        if (entry.InFlightCount.Load() != 0U)
        {
            continue;
        }

        const Foundation::UInt64 slotValue = static_cast<Foundation::UInt64>(index + 1U);
        const Foundation::UInt64 rawValue = slotValue | (entry.Generation << ObjectGenerationShift);
        const ObjectDestructionRecord record{*this, ObjectId{rawValue}, entry.Instance};
        FinalizeEntryLocked(entry);
        return Foundation::Result<ObjectDestructionRecord, ObjectRegistryError>::Success(record);
    }

    return Foundation::Result<ObjectDestructionRecord, ObjectRegistryError>::Failure(
        ObjectRegistryError::NoDestructionReady);
}

Foundation::Result<ObjectDestructionRecord, ObjectRegistryError>
ObjectRegistry::CollectDestroyedObject(const ObjectId object) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};

    ObjectEntry* const entry = FindOccupiedEntryLocked(object);
    if ((entry == nullptr) || !entry->IsQuiescing)
    {
        return Foundation::Result<ObjectDestructionRecord, ObjectRegistryError>::Failure(
            ObjectRegistryError::InvalidObject);
    }
    if (entry->InFlightCount.Load() != 0U)
    {
        return Foundation::Result<ObjectDestructionRecord, ObjectRegistryError>::Failure(
            ObjectRegistryError::NoDestructionReady);
    }

    const ObjectDestructionRecord record{*this, object, entry->Instance};
    FinalizeEntryLocked(*entry);
    return Foundation::Result<ObjectDestructionRecord, ObjectRegistryError>::Success(record);
}

Foundation::Result<ObjectInformation, ObjectRegistryError>
ObjectRegistry::GetInformationLocked(const ObjectId object) const noexcept
{
    const Foundation::UInt64 slotValue = GetSlotValue(object);
    const Foundation::UInt64 generation = object.GetValue() >> ObjectGenerationShift;
    if ((slotValue == 0U) || (generation == 0U))
    {
        return Foundation::Result<ObjectInformation, ObjectRegistryError>::Failure(ObjectRegistryError::InvalidObject);
    }

    const Foundation::Size index = static_cast<Foundation::Size>(slotValue - 1U);
    const ObjectEntry& entry = m_entries[index];
    if (!entry.IsOccupied || entry.IsQuiescing || (entry.Generation != generation))
    {
        return Foundation::Result<ObjectInformation, ObjectRegistryError>::Failure(ObjectRegistryError::InvalidObject);
    }

    return Foundation::Result<ObjectInformation, ObjectRegistryError>::Success(entry.Information);
}

const IKernelObject* ObjectRegistry::GetInstanceLocked(const ObjectId object) const noexcept
{
    const Foundation::UInt64 slotValue = GetSlotValue(object);
    const Foundation::UInt64 generation = object.GetValue() >> ObjectGenerationShift;
    if ((slotValue == 0U) || (generation == 0U))
    {
        return nullptr;
    }

    const ObjectEntry& entry = m_entries[static_cast<Foundation::Size>(slotValue - 1U)];
    if (!entry.IsOccupied || entry.IsQuiescing || (entry.Generation != generation))
    {
        return nullptr;
    }
    return entry.Instance;
}

ObjectRegistry::ObjectEntry* ObjectRegistry::FindOccupiedEntryLocked(const ObjectId object) noexcept
{
    const Foundation::UInt64 slotValue = GetSlotValue(object);
    const Foundation::UInt64 generation = object.GetValue() >> ObjectGenerationShift;
    if ((slotValue == 0U) || (generation == 0U))
    {
        return nullptr;
    }

    ObjectEntry& entry = m_entries[static_cast<Foundation::Size>(slotValue - 1U)];
    if (!entry.IsOccupied || (entry.Generation != generation))
    {
        return nullptr;
    }
    return &entry;
}

Foundation::Result<IKernelObject*, ObjectRegistryError>
ObjectRegistry::AcquireReferenceLocked(const ObjectId object) noexcept
{
    const Foundation::UInt64 slotValue = GetSlotValue(object);
    const Foundation::UInt64 generation = object.GetValue() >> ObjectGenerationShift;
    if ((slotValue == 0U) || (generation == 0U))
    {
        return Foundation::Result<IKernelObject*, ObjectRegistryError>::Failure(ObjectRegistryError::InvalidObject);
    }

    const Foundation::Size index = static_cast<Foundation::Size>(slotValue - 1U);
    ObjectEntry& entry = m_entries[index];
    if (!entry.IsOccupied || entry.IsQuiescing || (entry.Generation != generation))
    {
        return Foundation::Result<IKernelObject*, ObjectRegistryError>::Failure(ObjectRegistryError::InvalidObject);
    }

    if (!entry.InFlightCount.TryAcquire(m_maximumReferencesPerObject))
    {
        return Foundation::Result<IKernelObject*, ObjectRegistryError>::Failure(
            ObjectRegistryError::ReferenceExhausted);
    }

    return Foundation::Result<IKernelObject*, ObjectRegistryError>::Success(entry.Instance);
}

void ObjectRegistry::ReleaseReference(const ObjectId object) noexcept
{
    const Foundation::UInt64 slotValue = GetSlotValue(object);
    if (slotValue == 0U)
    {
        return;
    }

    const Foundation::Size index = static_cast<Foundation::Size>(slotValue - 1U);
    m_entries[index].InFlightCount.Release();
}

void ObjectRegistry::FinalizeEntryLocked(ObjectEntry& entry) noexcept
{
    const Foundation::Size index = IndexOf(entry);
    if (IsClaimed(entry.ExclusiveRange))
    {
        RemoveClaimLocked(entry);
    }
    m_quiescing.Remove(index);
    entry.IsOccupied = false;
    entry.IsQuiescing = false;
    entry.InFlightCount.Reset();
    entry.Instance = nullptr;
    entry.Information = ObjectInformation{};
    entry.ExclusiveRange = ObjectExclusiveRange{};

    if (entry.Generation == MaximumObjectGeneration)
    {
        entry.IsRetired = true;
    }
    else
    {
        ++entry.Generation;
        m_free.Insert(index);
    }
}

Foundation::UInt64 ObjectRegistry::GetSlotValue(const ObjectId object) const noexcept
{
    const Foundation::UInt64 slotValue = object.GetValue() & ObjectSlotMask;
    return (slotValue > m_capacity) ? 0U : slotValue;
}

Foundation::Size ObjectRegistry::IndexOf(const ObjectEntry& entry) const noexcept
{
    return static_cast<Foundation::Size>(&entry - m_entries);
}

ObjectRegistry::ObjectEntry& ObjectRegistry::ClaimNode(const Foundation::UInt16 link) const noexcept
{
    return m_entries[static_cast<Foundation::Size>(link) - 1U];
}

Foundation::UInt8 ObjectRegistry::ClaimHeightOf(const Foundation::UInt16 link) const noexcept
{
    return (link == 0U) ? Foundation::UInt8{0U} : ClaimNode(link).ClaimHeight;
}

bool ObjectRegistry::ClaimPrecedes(const ObjectEntry& entry, const ObjectType type,
                                   const Foundation::UInt64 start) noexcept
{
    const auto entryType = static_cast<Foundation::UInt8>(entry.Information.Type);
    const auto keyType = static_cast<Foundation::UInt8>(type);
    return (entryType < keyType) || ((entryType == keyType) && (entry.ExclusiveRange.Start < start));
}

const ObjectRegistry::ObjectEntry* ObjectRegistry::FindClaimBeforeLocked(const ObjectType type,
                                                                         const Foundation::UInt64 end) const noexcept
{
    const ObjectEntry* before{nullptr};
    Foundation::UInt16 link = m_claimRoot;
    while (link != 0U)
    {
        const ObjectEntry& node = ClaimNode(link);
        if (ClaimPrecedes(node, type, end))
        {
            before = &node;
            link = node.ClaimRight;
        }
        else
        {
            link = node.ClaimLeft;
        }
    }
    return before;
}

void ObjectRegistry::UpdateClaimHeight(ObjectEntry& entry) noexcept
{
    const Foundation::UInt8 left = ClaimHeightOf(entry.ClaimLeft);
    const Foundation::UInt8 right = ClaimHeightOf(entry.ClaimRight);
    entry.ClaimHeight = static_cast<Foundation::UInt8>(((left > right) ? left : right) + 1U);
}

Foundation::UInt16 ObjectRegistry::RotateClaimLeft(const Foundation::UInt16 link) noexcept
{
    ObjectEntry& node = ClaimNode(link);
    const Foundation::UInt16 rightLink = node.ClaimRight;
    ObjectEntry& right = ClaimNode(rightLink);
    node.ClaimRight = right.ClaimLeft;
    right.ClaimLeft = link;
    UpdateClaimHeight(node);
    UpdateClaimHeight(right);
    return rightLink;
}

Foundation::UInt16 ObjectRegistry::RotateClaimRight(const Foundation::UInt16 link) noexcept
{
    ObjectEntry& node = ClaimNode(link);
    const Foundation::UInt16 leftLink = node.ClaimLeft;
    ObjectEntry& left = ClaimNode(leftLink);
    node.ClaimLeft = left.ClaimRight;
    left.ClaimRight = link;
    UpdateClaimHeight(node);
    UpdateClaimHeight(left);
    return leftLink;
}

// Restores the AVL balance of the subtree link names after one insertion or
// removal below it, and its height.
void ObjectRegistry::RebalanceClaim(Foundation::UInt16& link) noexcept
{
    ObjectEntry& node = ClaimNode(link);
    UpdateClaimHeight(node);
    const Foundation::UInt8 left = ClaimHeightOf(node.ClaimLeft);
    const Foundation::UInt8 right = ClaimHeightOf(node.ClaimRight);
    if (left > (right + 1U))
    {
        const ObjectEntry& child = ClaimNode(node.ClaimLeft);
        if (ClaimHeightOf(child.ClaimLeft) < ClaimHeightOf(child.ClaimRight))
        {
            node.ClaimLeft = RotateClaimLeft(node.ClaimLeft);
        }
        link = RotateClaimRight(link);
    }
    else if (right > (left + 1U))
    {
        const ObjectEntry& child = ClaimNode(node.ClaimRight);
        if (ClaimHeightOf(child.ClaimRight) < ClaimHeightOf(child.ClaimLeft))
        {
            node.ClaimRight = RotateClaimRight(node.ClaimRight);
        }
        link = RotateClaimLeft(link);
    }
}

void ObjectRegistry::InsertClaimLocked(ObjectEntry& entry) noexcept
{
    Foundation::UInt16* path[ClaimTreeMaximumDepth]{};
    Foundation::Size depth{0U};
    Foundation::UInt16* link = &m_claimRoot;
    while ((*link != 0U) && (depth < ClaimTreeMaximumDepth))
    {
        path[depth++] = link;
        ObjectEntry& node = ClaimNode(*link);
        link =
            ClaimPrecedes(entry, node.Information.Type, node.ExclusiveRange.Start) ? &node.ClaimLeft : &node.ClaimRight;
    }
    entry.ClaimLeft = 0U;
    entry.ClaimRight = 0U;
    entry.ClaimHeight = 1U;
    *link = static_cast<Foundation::UInt16>(IndexOf(entry) + 1U);
    while (depth != 0U)
    {
        RebalanceClaim(*path[--depth]);
    }
}

void ObjectRegistry::RemoveClaimLocked(ObjectEntry& entry) noexcept
{
    const auto target = static_cast<Foundation::UInt16>(IndexOf(entry) + 1U);
    Foundation::UInt16* path[ClaimTreeMaximumDepth]{};
    Foundation::Size depth{0U};
    Foundation::UInt16* link = &m_claimRoot;
    while ((*link != 0U) && (*link != target) && (depth < ClaimTreeMaximumDepth))
    {
        path[depth++] = link;
        ObjectEntry& node = ClaimNode(*link);
        link =
            ClaimPrecedes(entry, node.Information.Type, node.ExclusiveRange.Start) ? &node.ClaimLeft : &node.ClaimRight;
    }
    if (*link != target)
    {
        return;
    }
    if ((entry.ClaimLeft == 0U) || (entry.ClaimRight == 0U))
    {
        *link = (entry.ClaimLeft != 0U) ? entry.ClaimLeft : entry.ClaimRight;
    }
    else
    {
        // The successor, the leftmost entry of the right subtree, takes the
        // removed entry's place; the path continues through it.
        const Foundation::Size replaced = depth;
        path[depth++] = link;
        Foundation::UInt16* successorLink = &entry.ClaimRight;
        while ((ClaimNode(*successorLink).ClaimLeft != 0U) && (depth < ClaimTreeMaximumDepth))
        {
            path[depth++] = successorLink;
            successorLink = &ClaimNode(*successorLink).ClaimLeft;
        }
        const Foundation::UInt16 successorValue = *successorLink;
        ObjectEntry& successor = ClaimNode(successorValue);
        *successorLink = successor.ClaimRight;
        successor.ClaimLeft = entry.ClaimLeft;
        successor.ClaimRight = entry.ClaimRight;
        successor.ClaimHeight = entry.ClaimHeight;
        *link = successorValue;
        if (depth > (replaced + 1U))
        {
            path[replaced + 1U] = &successor.ClaimRight;
        }
    }
    entry.ClaimLeft = 0U;
    entry.ClaimRight = 0U;
    entry.ClaimHeight = 0U;
    while (depth != 0U)
    {
        RebalanceClaim(*path[--depth]);
    }
}

Platform::Synchronization::SpinLock& ObjectRegistry::GetSynchronization() const noexcept
{
    return m_synchronization;
}
} // namespace Asteria::Kernel::Object
