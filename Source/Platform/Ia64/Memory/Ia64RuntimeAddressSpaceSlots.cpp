// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64RuntimeAddressSpaceSlots.hpp"

#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"

// Target images define placement new once (Kernel/Boot/KernelEntry.cpp).
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Platform::Ia64::Memory
{
bool Ia64RuntimeAddressSpaceSlots::Configure(const Ia64RuntimeProcessorCapabilities& capabilities,
                                             Ia64RegionIdentifierAllocator& allocator,
                                             const InitialAddressSpacePreparation& preparation,
                                             Ia64ActiveRuntimeTranslationBindings& bindings,
                                             Ia64LocalTranslationCachePurge& purge) noexcept
{
    if (m_capabilities != nullptr)
    {
        return false;
    }
    const auto common = capabilities.GetCommonCapabilities();
    const auto plan = preparation.TryGetMappingPlan();
    const Ia64RegionIdentifierAllocatorSnapshot allocated = allocator.GetSnapshot();
    if (!common.HasValue() || !capabilities.GetAdmittedProcessorMask().HasValue() || !plan.HasValue() ||
        (plan.GetValue() == nullptr) || !Ia64RuntimeAddressSpacePageTable::IsCanonicalSeed(*plan.GetValue()) ||
        (allocated.State != Ia64RegionIdentifierAllocatorState::Ready) ||
        (allocated.RegionIdentifierBits != common.GetValue().RegionIdentifierBits))
    {
        return false;
    }
    m_capabilities = &capabilities;
    m_allocator = &allocator;
    m_preparation = &preparation;
    m_plan = plan.GetValue();
    m_bindings = &bindings;
    m_purge = &purge;
    m_regionIdentifierBits = allocated.RegionIdentifierBits;
    return true;
}

Foundation::Size Ia64RuntimeAddressSpaceSlots::GetSlotCount() const noexcept
{
    return (m_capabilities != nullptr) ? Ia64RuntimeAddressSpaceSlotCount : 0U;
}

Foundation::UInt64 Ia64RuntimeAddressSpaceSlots::GetSlotBytes(bool) const noexcept
{
    return SlotBytes;
}

bool Ia64RuntimeAddressSpaceSlots::CanPrepare(const Slot& slot) const noexcept
{
    const Ia64RegionIdentifierAllocatorSnapshot allocated = m_allocator->GetSnapshot();
    const auto blockSize = static_cast<Foundation::UInt32>(Ia64RegionIdentifierCountPerAddressSpace);
    return (slot.State == Ia64RuntimeAddressSpaceSlotState::Free) &&
           (allocated.State == Ia64RegionIdentifierAllocatorState::Ready) &&
           (allocated.RegionIdentifierBits == m_regionIdentifierBits) &&
           (allocated.RepresentableLimitExclusive >= blockSize) &&
           (allocated.NextRegionIdentifier <= (allocated.RepresentableLimitExclusive - blockSize)) &&
           (slot.PageTable.GetState() == Ia64RuntimeAddressSpacePageTableState::Uninitialized) &&
           (slot.Space.GetSnapshot().State == Kernel::Memory::AddressSpaceState::Uninitialized);
}

Kernel::Memory::AddressSpace*
Ia64RuntimeAddressSpaceSlots::InitializeSlot(const Foundation::Size index,
                                             const Kernel::Object::ResourceDomainId chargeOwner, bool) noexcept
{
    if ((m_capabilities == nullptr) || (index >= Ia64RuntimeAddressSpaceSlotCount) || !chargeOwner.IsValid())
    {
        return nullptr;
    }
    Slot& slot = m_slots[index];
    if (!CanPrepare(slot))
    {
        return nullptr;
    }
    const auto receipt = m_allocator->Allocate();
    if (!receipt.HasValue())
    {
        return nullptr;
    }
    // Configure and CanPrepare proved every condition the backend and the
    // AddressSpace check, so nothing below fails unless that proof is
    // broken; if it is, the receipt is lost and the slot spent.
    slot.Receipt = receipt.GetValue();
    if ((slot.PageTable.Initialize(*m_capabilities, slot.Receipt, *m_preparation, *m_bindings) !=
         Ia64RuntimeAddressSpacePageTableInitializationError::None) ||
        !slot.PageTable.ConfigureTranslationCachePurge(*m_purge) ||
        (slot.Space.Initialize(chargeOwner, slot.PageTable, *m_plan) != Kernel::Memory::AddressSpaceError::None))
    {
        slot.State = Ia64RuntimeAddressSpaceSlotState::Spent;
        ++m_spent;
        return nullptr;
    }
    slot.State = Ia64RuntimeAddressSpaceSlotState::Held;
    ++m_prepared;
    return &slot.Space;
}

bool Ia64RuntimeAddressSpaceSlots::ReleaseSlot(const Foundation::Size index) noexcept
{
    if (index >= Ia64RuntimeAddressSpaceSlotCount)
    {
        return false;
    }
    Slot& slot = m_slots[index];
    if ((slot.State != Ia64RuntimeAddressSpaceSlotState::Held) ||
        (slot.Space.GetSnapshot().State != Kernel::Memory::AddressSpaceState::Quiesced) ||
        (slot.PageTable.GetState() != Ia64RuntimeAddressSpacePageTableState::Retired))
    {
        return false;
    }
    // The space quiesced and its backend retired with its ledger bound
    // nowhere, so no refill reads it: both start over in place, and the next
    // incarnation takes new RIDs.
    slot.Space.~AddressSpace();
    slot.PageTable.~Ia64RuntimeAddressSpacePageTable();
    ::new (static_cast<void*>(&slot.PageTable)) Ia64RuntimeAddressSpacePageTable{};
    ::new (static_cast<void*>(&slot.Space)) Kernel::Memory::AddressSpace{};
    slot.Receipt = Ia64RegionIdentifierAllocationReceipt{};
    slot.State = Ia64RuntimeAddressSpaceSlotState::Free;
    ++m_released;
    return true;
}

Ia64RuntimeAddressSpaceSlotState Ia64RuntimeAddressSpaceSlots::GetSlotState(const Foundation::Size index) const noexcept
{
    return (index < Ia64RuntimeAddressSpaceSlotCount) ? m_slots[index].State : Ia64RuntimeAddressSpaceSlotState::Spent;
}

Ia64RuntimeAddressSpaceSlotsSnapshot Ia64RuntimeAddressSpaceSlots::GetSnapshot() const noexcept
{
    Foundation::Size free{0U};
    for (const Slot& slot : m_slots)
    {
        free += (slot.State == Ia64RuntimeAddressSpaceSlotState::Free) ? 1U : 0U;
    }
    return Ia64RuntimeAddressSpaceSlotsSnapshot{
        .FreeSlots = (m_capabilities != nullptr) ? free : 0U,
        .Prepared = m_prepared,
        .Released = m_released,
        .Spent = m_spent,
    };
}
} // namespace Asteria::Platform::Ia64::Memory
