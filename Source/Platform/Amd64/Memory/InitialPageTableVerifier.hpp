// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_MEMORY_INITIAL_PAGE_TABLE_VERIFIER_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_MEMORY_INITIAL_PAGE_TABLE_VERIFIER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Platform/Amd64/Memory/InitialPageTablePreparation.hpp"

namespace Asteria::Platform::Amd64::Memory
{
enum class InitialPageTableVerificationError : Foundation::UInt32
{
    None,
    PlanNotSealed,
    InvalidCapabilities,
    InvalidStorage,
    InvalidSummary,
    InvalidMapping,
    InvalidNonLeafEntry,
    DuplicateTablePage,
    MissingTablePage,
    UnexpectedLeafEntry,
    MissingLeafEntry,
    MappedPageCountMismatch,
    NonZeroUnusedStorage,
};

/// Performs a byte-exact, non-writing verification of the bootstrap four-level
/// page tables. Every reachable non-leaf and leaf entry is reconstructed from
/// the retained common plan (up to 64 owned table pages), every used table page
/// must have one parent, and
/// every unused owned word must remain zero.
class InitialPageTableVerifier final
{
public:
    [[nodiscard]] static InitialPageTableVerificationError
    Verify(const Kernel::Memory::InitialMappingPlan& plan, const PagingCapabilities& capabilities,
           const InitialPageTablePreparationSummary& summary) noexcept;

private:
    InitialPageTableVerifier() = delete;
};

using InitialPageTableVerificationFunction =
    InitialPageTableVerificationError (*)(const Kernel::Memory::InitialMappingPlan&, const PagingCapabilities&,
                                          const InitialPageTablePreparationSummary&) noexcept;

static_assert(__is_same(decltype(&InitialPageTableVerifier::Verify), InitialPageTableVerificationFunction));
static_assert(sizeof(InitialPageTableVerificationError) == 4U);
} // namespace Asteria::Platform::Amd64::Memory

#endif // ASTERIA_PLATFORM_AMD64_MEMORY_INITIAL_PAGE_TABLE_VERIFIER_HPP_INCLUDED
