// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_ADDRESS_SPACE_SLOTS_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_ADDRESS_SPACE_SLOTS_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/AddressSpacePool.hpp"
#include "Platform/Ia64/Memory/Ia64ActiveRuntimeTranslationBindings.hpp"
#include "Platform/Ia64/Memory/Ia64RegionIdentifierAllocator.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeAddressSpacePageTable.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeProcessorCapabilities.hpp"
#include "Platform/Ia64/Memory/Ia64TranslationCachePurge.hpp"

namespace Asteria::Platform::Ia64::Memory
{
class InitialAddressSpacePreparation;

/// The run-time AddressSpaces IA-64 provides: one per pool slot, as on AMD64. Each is embedded in
/// the BSP runtime memory, whose corridor the refill handler reads with PSR.ic clear, so every slot
/// is pinned by the data translation registers that map the image's writable range; the final-image
/// residue proof bounds their count (Ia64BspRuntimeMemory.md, slot capacity).
inline constexpr Foundation::Size Ia64RuntimeAddressSpaceSlotCount{16U};
static_assert(Ia64RuntimeAddressSpaceSlotCount == Kernel::Memory::AddressSpacePool::Capacity);

enum class Ia64RuntimeAddressSpaceSlotState : Foundation::UInt8
{
    Free,
    Held,
    /// A preparation failed after its RID receipt: the slot is never used
    /// again (the pool spends it too).
    Spent,
};

struct Ia64RuntimeAddressSpaceSlotsSnapshot
{
    /// Slots that could serve a creation now, preparations that succeeded,
    /// slots released for reuse, and slots spent.
    Foundation::Size FreeSlots;
    Foundation::UInt64 Prepared;
    Foundation::UInt64 Released;
    Foundation::UInt64 Spent;
};

/// IAddressSpaceProvider for IA-64: each slot is a private RID block, an
/// Ia64RuntimeAddressSpacePageTable seeded from the boot preparation, and an AddressSpace over it.
/// RIDs and ledgers are single-use, so every incarnation takes a fresh receipt from the boot's one
/// allocator: InitializeSlot checks everything that can still fail before it allocates (the slot
/// free, the allocator Ready with the common RID width and room for another block, the backend and
/// AddressSpace unused); Configure proved the rest once (the canonical seed the boot roots were
/// built from, the common capabilities and admitted processors). A failure after the receipt would
/// mean those proofs no longer hold; it spends the slot, and the receipt is lost (the allocator
/// still counts it). ReleaseSlot requires the AddressSpace Quiesced and the backend Retired
/// (Release refuses while its ledger is bound on any processor; a processor binds its next root's
/// ledger before it leaves a root, so a space resident nowhere is bound nowhere), then constructs
/// both afresh in place. The provider registers and pins nothing: AddressSpacePool registers each
/// space, binds its identity and mints its capability, and its collection must find no pin of the
/// provider's. Each backend purges its processor's translation cache after an unmap
/// (Ia64RuntimeAddressSpacePageTable). The object must lie inside the runtime-object corridor of
/// the binding table (Ia64BspRuntimeMemory embeds it), which every backend's region set and ledger
/// must; it is immovable and lives for the boot. Calls are serialized by the pool's lock.
class Ia64RuntimeAddressSpaceSlots final : public Kernel::Memory::IAddressSpaceProvider
{
public:
    /// The kernel-metadata bytes of one slot: its AddressSpace and backend.
    static constexpr Foundation::UInt64 SlotBytes{sizeof(Kernel::Memory::AddressSpace) +
                                                  sizeof(Ia64RuntimeAddressSpacePageTable)};

    constexpr Ia64RuntimeAddressSpaceSlots() noexcept = default;
    ~Ia64RuntimeAddressSpaceSlots() noexcept = default;

    Ia64RuntimeAddressSpaceSlots(const Ia64RuntimeAddressSpaceSlots&) = delete;
    Ia64RuntimeAddressSpaceSlots& operator=(const Ia64RuntimeAddressSpaceSlots&) = delete;

    /// Keeps what every slot is built from; once, before any creation. False
    /// unless the common capabilities and admitted processors exist,
    /// preparation's mapping plan is the canonical seed and the allocator is
    /// Ready with the common RID width. Every reference outlives the
    /// provider.
    [[nodiscard]] bool Configure(const Ia64RuntimeProcessorCapabilities& capabilities,
                                 Ia64RegionIdentifierAllocator& allocator,
                                 const InitialAddressSpacePreparation& preparation,
                                 Ia64ActiveRuntimeTranslationBindings& bindings,
                                 Ia64LocalTranslationCachePurge& purge) noexcept;

    [[nodiscard]] Foundation::Size GetSlotCount() const noexcept override;
    [[nodiscard]] Foundation::UInt64 GetSlotBytes(bool = false) const noexcept override;
    [[nodiscard]] Kernel::Memory::AddressSpace*
    InitializeSlot(Foundation::Size slot, Kernel::Object::ResourceDomainId chargeOwner, bool = false) noexcept override;
    [[nodiscard]] bool ReleaseSlot(Foundation::Size slot) noexcept override;

    [[nodiscard]] Ia64RuntimeAddressSpaceSlotState GetSlotState(Foundation::Size slot) const noexcept;
    [[nodiscard]] Ia64RuntimeAddressSpaceSlotsSnapshot GetSnapshot() const noexcept;

private:
    struct Slot
    {
        Ia64RegionIdentifierAllocationReceipt Receipt;
        Ia64RuntimeAddressSpacePageTable PageTable;
        Kernel::Memory::AddressSpace Space;
        Ia64RuntimeAddressSpaceSlotState State;
    };

    /// Everything InitializeSlot checks before the receipt.
    [[nodiscard]] bool CanPrepare(const Slot& slot) const noexcept;

    const Ia64RuntimeProcessorCapabilities* m_capabilities{nullptr};
    Ia64RegionIdentifierAllocator* m_allocator{nullptr};
    const InitialAddressSpacePreparation* m_preparation{nullptr};
    const Kernel::Memory::InitialMappingPlan* m_plan{nullptr};
    Ia64ActiveRuntimeTranslationBindings* m_bindings{nullptr};
    Ia64LocalTranslationCachePurge* m_purge{nullptr};
    Foundation::UInt8 m_regionIdentifierBits{0U};
    Slot m_slots[Ia64RuntimeAddressSpaceSlotCount]{};
    Foundation::UInt64 m_prepared{0U};
    Foundation::UInt64 m_released{0U};
    Foundation::UInt64 m_spent{0U};
};
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_ADDRESS_SPACE_SLOTS_HPP_INCLUDED
