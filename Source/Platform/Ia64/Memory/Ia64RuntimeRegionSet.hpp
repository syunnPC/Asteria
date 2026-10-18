// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_REGION_SET_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_REGION_SET_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/Ia64RegionIdentifierAllocator.hpp"
#include "Platform/Ia64/Memory/InitialTranslationEncoder.hpp"

namespace Asteria::Platform::Ia64::Memory
{
constexpr Foundation::Size Ia64RuntimeRegionCount{8U};
constexpr Foundation::UInt32 Ia64SharedBootstrapRegionIdentifier{0x2000U};
constexpr Foundation::UInt8 Ia64BootstrapPreferredPageShift{12U};
constexpr Foundation::UInt64 Ia64SharedBootstrapRegionRegister{
    (static_cast<Foundation::UInt64>(Ia64SharedBootstrapRegionIdentifier) << 8U) |
    (static_cast<Foundation::UInt64>(Ia64BootstrapPreferredPageShift) << 2U)};

enum class Ia64RuntimeRegionSetError : Foundation::UInt8
{
    None,
    OutputAlreadyBuilt,
    RegionSetNotBuilt,
    InvalidCapabilities,
    InvalidReceipt,
    InvalidBootstrapRegionRegister,
    UnsupportedBootstrapRegionRegister,
    UnrepresentableRegionIdentifier,
    BootstrapRegionSetMismatch,
    SharedBootstrapAnchorMismatch,
    RegionIndexOutOfBounds,
};

/// Passive source for exact bootstrap RR words. It carries no mapping-policy,
/// activation, or provenance claim.
struct Ia64BootstrapRegionRegisterWords
{
    Foundation::UInt64 RegionRegisters[Ia64RuntimeRegionCount];
};

/// Owns exactly eight immutable IA-64 region-register words. AdoptBootstrap retains the exact
/// eight-word bootstrap profile after validating only the architected RR representation used here.
/// Construct applies the initial runtime identity policy: RR0 remains the exact shared bootstrap
/// anchor, while RR1 through RR7 receive private RID candidates one through seven. Candidate zero
/// is deliberately never selected. Private runtime RRs use the supported 4 KiB preferred page size
/// and have their VHPT walkers disabled. "Shared" and "private" describe RID-selection policy only.
/// RR words cannot prove that seed mappings are Global, establish a VHPT/refill implementation,
/// activate the set, or admit an address space as a runnable root. A separate mapping-policy and
/// activation stage must establish those properties. Allocation: - Does not allocate memory. Both
/// construction paths perform eight or fewer bounded iterations.
class Ia64RuntimeRegionSet final
{
public:
    constexpr Ia64RuntimeRegionSet() noexcept = default;

    Ia64RuntimeRegionSet(const Ia64RuntimeRegionSet&) = delete;
    Ia64RuntimeRegionSet& operator=(const Ia64RuntimeRegionSet&) = delete;
    Ia64RuntimeRegionSet(Ia64RuntimeRegionSet&&) = delete;
    Ia64RuntimeRegionSet& operator=(Ia64RuntimeRegionSet&&) = delete;

    /// Adopts only the exact RID 0x2000+i, PS 12, VE-off bootstrap profile.
    /// Failure leaves output unchanged.
    [[nodiscard]] static Ia64RuntimeRegionSetError AdoptBootstrap(const Ia64BootstrapRegionRegisterWords& bootstrap,
                                                                  const TranslationCapabilities& capabilities,
                                                                  Ia64RuntimeRegionSet& output) noexcept;

    /// Constructs the initial runtime set from the fixed shared-bootstrap
    /// anchor policy and a monotonic allocator receipt. Failure leaves output
    /// unchanged. This does not inspect or validate live region registers.
    [[nodiscard]] static Ia64RuntimeRegionSetError Construct(const Ia64RegionIdentifierAllocationReceipt& receipt,
                                                             const TranslationCapabilities& capabilities,
                                                             Ia64RuntimeRegionSet& output) noexcept;

    [[nodiscard]] Foundation::Result<Foundation::UInt64, Ia64RuntimeRegionSetError>
    GetRegionRegister(Foundation::Size regionIndex) const noexcept;

private:
    Foundation::UInt64 m_regionRegisters[Ia64RuntimeRegionCount]{};
    bool m_built{false};
};

static_assert(Ia64RuntimeRegionCount == Ia64RegionIdentifierCountPerAddressSpace);
static_assert(Ia64SharedBootstrapRegionIdentifier < (Foundation::UInt32{1U} << 18U));
static_assert(Ia64SharedBootstrapRegionRegister == 0x0020'0030U);
static_assert(sizeof(Ia64RuntimeRegionSetError) == 1U);
static_assert(sizeof(Ia64BootstrapRegionRegisterWords) == 64U);
static_assert(alignof(Ia64BootstrapRegionRegisterWords) == 8U);
static_assert(sizeof(Ia64RuntimeRegionSet) == 72U);
static_assert(alignof(Ia64RuntimeRegionSet) == 8U);
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_REGION_SET_HPP_INCLUDED
