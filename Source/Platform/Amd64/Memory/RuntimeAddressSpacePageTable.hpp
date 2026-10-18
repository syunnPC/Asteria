// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_MEMORY_RUNTIME_ADDRESS_SPACE_PAGE_TABLE_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_MEMORY_RUNTIME_ADDRESS_SPACE_PAGE_TABLE_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Memory/IAddressSpacePageTable.hpp"
#include "Platform/Amd64/Memory/InitialPageTableVerifier.hpp"
#include "Platform/Amd64/Memory/RuntimePageTableMapper.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Platform::Amd64::Memory
{
enum class RuntimeAddressSpacePageTableState : Foundation::UInt8
{
    Uninitialized,
    Ready,
    Claimed,
    Faulted,
    Retired,
};

enum class RuntimeAddressSpacePageTableInitializationError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    InvalidCapabilities,
    InvalidStorage,
    VerificationRejected,
    StorageAlreadyOwned,
};

struct RuntimeAddressSpacePageTableSnapshot
{
    Kernel::Memory::PhysicalAddress RootPhysicalAddress;
    Foundation::Size PageCapacity;
    Foundation::Size UsedTablePages;
    RuntimeAddressSpacePageTableState State;
    Foundation::UInt8 Reserved[7];
};

/// Adapts one verified AMD64 bootstrap tree and its exclusively owned arena to the
/// architecture-neutral AddressSpace page-table contract. The supplied seed was accepted by
/// InitialPageTableVerifier. The arena and capabilities remain stable until this object and its
/// owning AddressSpace are quiesced. This object serializes initialization, competing ownership
/// claims, activation, mapping, snapshots, and release. Lock order is AddressSpace then this
/// backend; the backend never calls into an AddressSpace. Activation requires the target profile's
/// PCIDE/PGE/LA57-off state, writes the verified raw root, and treats a readback mismatch as
/// committed state unknown. Initialize consumes the preparation's unique arena ownership before
/// reading any table byte. A second wrapper for that preparation is rejected. The memory-manager
/// factory separately guarantees that no second preparation was constructed for the same physical
/// arena. Claim accepts only the exact seed published by that preparation, then clears the borrowed
/// pointer. The preparation itself need not outlive the claim.
class RuntimeAddressSpacePageTable final : public Hal::Memory::IAddressSpacePageTable
{
public:
    // Intel 64 and IA-32 SDM Vol. 3A, 253668-093US, §5.10.4.1:
    // CR3 drops the old root's translations with PCIDE and PGE clear.
    [[nodiscard]] bool DetachAcknowledgesShootdown(bool) const noexcept override
    {
        return true;
    }
    constexpr RuntimeAddressSpacePageTable() noexcept = default;

    [[nodiscard]] RuntimeAddressSpacePageTableInitializationError
    Initialize(const PagingCapabilities& capabilities, InitialPageTablePreparation& preparation,
               volatile Foundation::UInt64* writableEntries) noexcept;

    [[nodiscard]] bool IsReady() const noexcept override;

    [[nodiscard]] Hal::Memory::AddressSpacePageTableOwnershipError
    Claim(const Kernel::Memory::AddressSpace& owner,
          const Kernel::Memory::InitialMappingPlan& seedPlan) noexcept override;

    [[nodiscard]] Hal::Memory::AddressSpacePageTableOwnershipError
    Release(const Kernel::Memory::AddressSpace& owner) noexcept override;

    [[nodiscard]] Hal::Memory::AddressSpacePageTableActivationError
    Activate(const Kernel::Memory::AddressSpace& owner, Kernel::Scheduler::CpuId processor) noexcept override;

    [[nodiscard]] Hal::Memory::AddressSpacePageTableError
    Map(const Kernel::Memory::AddressSpaceMappingRecord& mapping,
        Hal::Memory::AddressSpacePageTableMapResult& output) noexcept override;

    /// Clears the record's leaves and keeps the intermediate tables.
    [[nodiscard]] Hal::Memory::AddressSpacePageTableError
    Unmap(const Kernel::Memory::AddressSpaceMappingRecord& mapping,
          Hal::Memory::AddressSpacePageTableMapResult& output) noexcept override;

    [[nodiscard]] RuntimeAddressSpacePageTableSnapshot GetSnapshot() const noexcept;

    RuntimeAddressSpacePageTable(const RuntimeAddressSpacePageTable&) = delete;
    RuntimeAddressSpacePageTable& operator=(const RuntimeAddressSpacePageTable&) = delete;
    RuntimeAddressSpacePageTable(RuntimeAddressSpacePageTable&&) = delete;
    RuntimeAddressSpacePageTable& operator=(RuntimeAddressSpacePageTable&&) = delete;

private:
    mutable Platform::Synchronization::SpinLock m_synchronization;
    PagingCapabilities m_capabilities{};
    RuntimePageTableStorage m_storage{};
    const Kernel::Memory::InitialMappingPlan* m_pendingSeedPlan{nullptr};
    const Kernel::Memory::AddressSpace* m_owner{nullptr};
    RuntimeAddressSpacePageTableState m_state{RuntimeAddressSpacePageTableState::Uninitialized};
};

static_assert(sizeof(RuntimeAddressSpacePageTableState) == 1U);
static_assert(sizeof(RuntimeAddressSpacePageTableInitializationError) == 1U);
static_assert(sizeof(RuntimeAddressSpacePageTableSnapshot) == 32U);
static_assert(alignof(RuntimeAddressSpacePageTableSnapshot) == 8U);
} // Namespace Asteria::Platform::Amd64::Memory

#endif // ASTERIA_PLATFORM_AMD64_MEMORY_RUNTIME_ADDRESS_SPACE_PAGE_TABLE_HPP_INCLUDED
