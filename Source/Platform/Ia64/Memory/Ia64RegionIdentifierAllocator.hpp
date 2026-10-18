// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_REGION_IDENTIFIER_ALLOCATOR_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_REGION_IDENTIFIER_ALLOCATOR_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/PalVirtualMemoryDecoder.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Platform::Ia64::Memory
{
constexpr Foundation::Size Ia64RegionIdentifierCountPerAddressSpace{8U};
constexpr Foundation::UInt32 Ia64FirstRuntimeRegionIdentifier{0x2008U};

enum class Ia64RegionIdentifierAllocatorState : Foundation::UInt8
{
    Uninitialized,
    Ready,
};

enum class Ia64RegionIdentifierAllocatorError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    InvalidCapabilities,
    NotInitialized,
    Exhausted,
    InvalidReceipt,
    RegionIndexOutOfBounds,
};

class Ia64RegionIdentifierAllocator;

/// Identifies one immutable, consecutive eight-RID candidate set, indexed by
/// IA-64 virtual-region number zero through seven.
class Ia64RegionIdentifierAllocationReceipt final
{
public:
    constexpr Ia64RegionIdentifierAllocationReceipt() noexcept = default;

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_firstRegionIdentifier >= Ia64FirstRuntimeRegionIdentifier) &&
               ((m_firstRegionIdentifier &
                 static_cast<Foundation::UInt32>(Ia64RegionIdentifierCountPerAddressSpace - 1U)) == 0U);
    }

    [[nodiscard]] Foundation::Result<Foundation::UInt32, Ia64RegionIdentifierAllocatorError>
    GetRegionIdentifier(Foundation::Size regionIndex) const noexcept;

private:
    friend class Ia64RegionIdentifierAllocator;

    explicit constexpr Ia64RegionIdentifierAllocationReceipt(const Foundation::UInt32 firstRegionIdentifier) noexcept
        : m_firstRegionIdentifier{firstRegionIdentifier}
    {
    }

    Foundation::UInt32 m_firstRegionIdentifier{0U};
};

struct Ia64RegionIdentifierAllocatorSnapshot
{
    Foundation::UInt32 NextRegionIdentifier;
    Foundation::UInt32 RepresentableLimitExclusive;
    Foundation::UInt32 AllocatedAddressSpaceCount;
    Ia64RegionIdentifierAllocatorState State;
    Foundation::UInt8 RegionIdentifierBits;
    Foundation::UInt8 Reserved[2];
};

/// Issues one never-reused eight-RID block for each runtime AddressSpace. Exactly one authoritative
/// instance owns the system-wide RID namespace for the complete boot lifetime. Independently
/// initialized instances are forbidden because each would begin at the same block and issue
/// duplicate identifiers. The integration owner enforces this lifetime and uniqueness contract;
/// this class does not provide singleton or global-state machinery. The supplied RID width must be
/// the admitted system-wide minimum across every CPU on which these address spaces may run. The
/// runtime processor-capability profile supplies that minimum. A receipt contains uncommitted per-region candidates only.
/// The region-set builder keeps RR0 on one fixed shared bootstrap RID and
/// deliberately ignores candidate zero; RR1 through RR7 use candidates one through seven as
/// never-reused private identities. A region may use a shared bootstrap RID only when every mapping in that region is Global.
/// Thread safety: - Initialize, Allocate, and GetSnapshot are serialized internally. - Callers
/// prevent same-CPU preemption or re-entry by code using this lock. Interrupt safety: - Must not be
/// called from an ISR. Allocation: - Does not allocate memory. Work is constant after lock
/// acquisition.
class Ia64RegionIdentifierAllocator final
{
public:
    constexpr Ia64RegionIdentifierAllocator() noexcept = default;

    Ia64RegionIdentifierAllocator(const Ia64RegionIdentifierAllocator&) = delete;
    Ia64RegionIdentifierAllocator& operator=(const Ia64RegionIdentifierAllocator&) = delete;
    Ia64RegionIdentifierAllocator(Ia64RegionIdentifierAllocator&&) = delete;
    Ia64RegionIdentifierAllocator& operator=(Ia64RegionIdentifierAllocator&&) = delete;

    [[nodiscard]] Ia64RegionIdentifierAllocatorError Initialize(Foundation::UInt8 commonRegionIdentifierBits) noexcept;

    /// Compatibility entry point for callers that own an already-reduced PAL
    /// record. New runtime integration passes the common RID width directly.
    [[nodiscard]] Ia64RegionIdentifierAllocatorError
    Initialize(const PalVirtualMemoryCapabilities& capabilities) noexcept;

    /// Returns Exhausted without changing any allocator state when no complete
    /// eight-RID block remains representable.
    [[nodiscard]] Foundation::Result<Ia64RegionIdentifierAllocationReceipt, Ia64RegionIdentifierAllocatorError>
    Allocate() noexcept;

    [[nodiscard]] Ia64RegionIdentifierAllocatorSnapshot GetSnapshot() const noexcept;

private:
    [[nodiscard]] Ia64RegionIdentifierAllocatorError
    InitializeLocked(Foundation::UInt8 commonRegionIdentifierBits) noexcept;

    Foundation::UInt32 m_nextRegionIdentifier{0U};
    Foundation::UInt32 m_representableLimitExclusive{0U};
    Foundation::UInt32 m_allocatedAddressSpaceCount{0U};
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Ia64RegionIdentifierAllocatorState m_state{Ia64RegionIdentifierAllocatorState::Uninitialized};
    Foundation::UInt8 m_regionIdentifierBits{0U};
};

static_assert(Ia64RegionIdentifierCountPerAddressSpace == 8U);
static_assert((Ia64FirstRuntimeRegionIdentifier &
               static_cast<Foundation::UInt32>(Ia64RegionIdentifierCountPerAddressSpace - 1U)) == 0U);
static_assert(sizeof(Ia64RegionIdentifierAllocatorState) == 1U);
static_assert(sizeof(Ia64RegionIdentifierAllocatorError) == 1U);
static_assert(sizeof(Ia64RegionIdentifierAllocationReceipt) == 4U);
static_assert(alignof(Ia64RegionIdentifierAllocationReceipt) == 4U);
static_assert(sizeof(Ia64RegionIdentifierAllocatorSnapshot) == 16U);
static_assert(alignof(Ia64RegionIdentifierAllocatorSnapshot) == 4U);
static_assert(__builtin_offsetof(Ia64RegionIdentifierAllocatorSnapshot, NextRegionIdentifier) == 0U);
static_assert(__builtin_offsetof(Ia64RegionIdentifierAllocatorSnapshot, RepresentableLimitExclusive) == 4U);
static_assert(__builtin_offsetof(Ia64RegionIdentifierAllocatorSnapshot, AllocatedAddressSpaceCount) == 8U);
static_assert(__builtin_offsetof(Ia64RegionIdentifierAllocatorSnapshot, State) == 12U);
static_assert(__builtin_offsetof(Ia64RegionIdentifierAllocatorSnapshot, RegionIdentifierBits) == 13U);
static_assert(__builtin_offsetof(Ia64RegionIdentifierAllocatorSnapshot, Reserved) == 14U);
static_assert(sizeof(Ia64RegionIdentifierAllocator) == 32U);
static_assert(alignof(Ia64RegionIdentifierAllocator) == 8U);
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_REGION_IDENTIFIER_ALLOCATOR_HPP_INCLUDED
