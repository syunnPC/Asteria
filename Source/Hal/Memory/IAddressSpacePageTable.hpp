// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_MEMORY_I_ADDRESS_SPACE_PAGE_TABLE_HPP_INCLUDED
#define ASTERIA_HAL_MEMORY_I_ADDRESS_SPACE_PAGE_TABLE_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/AddressSpaceMappingRecord.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"

namespace Asteria::Kernel::Memory
{
class AddressSpace;
class InitialMappingPlan;
}

namespace Asteria::Hal::Memory
{
enum class AddressSpacePageTableOwnershipError : Foundation::UInt8
{
    None,
    NotReady,
    AlreadyOwned,
    SeedMismatch,
    InvalidOwner,
};

enum class AddressSpacePageTableError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidPlan,
    MappingConflict,
    CapacityExceeded,
    CorruptState,
    UnsupportedConfiguration,
    CommittedStateUnknown,
    InternalFailure,
};

enum class AddressSpacePageTableActivationError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidOwner,
    UnsupportedConfiguration,
    RejectedBeforeCommit,
    CommittedStateUnknown,
};

struct AddressSpacePageTableMapResult
{
    Kernel::Memory::VirtualAddress InvalidationStart;
    Kernel::Memory::PageCount InvalidationPages;
    Foundation::UInt64 Reserved;
};

/// Owns the target-specific page-table state for one AddressSpace. The implementation serializes
/// ownership transitions, including competing claims from different AddressSpaces. After a
/// successful claim, the owning AddressSpace serializes Map and Release. Map is bounded,
/// allocation-free, and callback-free. Every failure except CommittedStateUnknown leaves both
/// native page-table state and output unchanged. CommittedStateUnknown means a native write may
/// have occurred; the owner must become terminal and retain all mapping resources until the root is
/// quiesced and released. Success installs the complete mapping and returns the exact range that
/// every resident processor must invalidate before the mapping is exposed. Map receives an
/// authority-free record only after AddressSpace validates the originating operation and retains
/// the named object generations.
class IAddressSpacePageTable
{
public:
    [[nodiscard]] virtual bool IsReady() const noexcept = 0;

    /// Claims this native root for one exact owner and verified seed. A native
    /// root cannot be shared by independently serialized AddressSpaces.
    [[nodiscard]] virtual AddressSpacePageTableOwnershipError
    Claim(const Kernel::Memory::AddressSpace& owner, const Kernel::Memory::InitialMappingPlan& seedPlan) noexcept = 0;

    /// Retires the exact claimed root after every processor has detached.
    [[nodiscard]] virtual AddressSpacePageTableOwnershipError
    Release(const Kernel::Memory::AddressSpace& owner) noexcept = 0;

    /// Makes this exact claimed root current on the calling processor. Every result other than
    /// CommittedStateUnknown guarantees that a failure left the processor's previously active root
    /// unchanged. None guarantees that the complete root switch and its target-required local
    /// translation invalidation completed before return. A CommittedStateUnknown result means this
    /// root may already be active; the caller must conservatively retain processor residency and
    /// fail-stop. The implementation validates owner and processor before its native commit point.
    /// It is bounded, allocation-free, callback-free, and is called with local interrupts disabled
    /// while the owning AddressSpace serializes activation against mapping and quiescence.
    [[nodiscard]] virtual AddressSpacePageTableActivationError
    Activate(const Kernel::Memory::AddressSpace& owner, Kernel::Scheduler::CpuId processor) noexcept = 0;

    [[nodiscard]] virtual AddressSpacePageTableError Map(const Kernel::Memory::AddressSpaceMappingRecord& mapping,
                                                         AddressSpacePageTableMapResult& output) noexcept = 0;

    /// Removes the translations of one record a successful Map installed and
    /// returns the exact range every resident processor must invalidate
    /// before the record's objects are released. It shares Map's contract:
    /// bounded, allocation-free and callback-free, serialized by the owning
    /// AddressSpace, and every failure except CommittedStateUnknown leaves
    /// native state and output unchanged. A record whose translations are
    /// not the ones Map installed is CorruptState; a backend that cannot
    /// remove translations reports UnsupportedConfiguration.
    [[nodiscard]] virtual AddressSpacePageTableError Unmap(const Kernel::Memory::AddressSpaceMappingRecord& mapping,
                                                           AddressSpacePageTableMapResult& output) noexcept = 0;

    /// Called under AddressSpace's lock, after the caller has switched away: does detach invalidate
    /// this pending work?
    [[nodiscard]] virtual bool DetachAcknowledgesShootdown(const bool unmapping) const noexcept
    {
        static_cast<void>(unmapping);
        return false;
    }

protected:
    /// Backend storage is externally owned and is never deleted through this
    /// interface.
    constexpr IAddressSpacePageTable() noexcept = default;
    ~IAddressSpacePageTable() noexcept = default;
};

static_assert(sizeof(AddressSpacePageTableError) == 1U);
static_assert(sizeof(AddressSpacePageTableOwnershipError) == 1U);
static_assert(sizeof(AddressSpacePageTableActivationError) == 1U);
static_assert(sizeof(AddressSpacePageTableMapResult) == 24U);
static_assert(alignof(AddressSpacePageTableMapResult) == 8U);
} // Namespace Asteria::Hal::Memory

#endif // ASTERIA_HAL_MEMORY_I_ADDRESS_SPACE_PAGE_TABLE_HPP_INCLUDED
