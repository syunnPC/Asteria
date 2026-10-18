// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_REGION_ACTIVATOR_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_REGION_ACTIVATOR_HPP_INCLUDED

#include "Foundation/Compiler.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeRegionSet.hpp"

namespace Asteria::Platform::Ia64::Memory
{
enum class Ia64RuntimeRegionActivationError : Foundation::UInt8
{
    None,
    InvalidRegionSet,
    RejectedBeforeCommit,
    CommittedStateUnknown,
};

/// Activates RR1 through RR7 from one immutable runtime region set. Both entry paths collect and
/// validate all eight words before the native helper is entered. The helper rejects visible PSR.i
/// or PSR.pk and a bad live RR0 before mutation. It saves PSR.ic, clears and serializes it
/// immediately before the first RR write, then restores the exact entry IC state after the
/// serialized readback. The first RR1 write is the commit point; any subsequent mismatch is
/// CommittedStateUnknown and is not rolled back. None means RR1 through RR7 were serialized and
/// read back exactly. Neither path writes RR0, purges translations, or publishes an active ledger.
class Ia64RuntimeRegionActivator final
{
public:
    /// Ordinary translated-runtime activation. Preconditions not representable by the value
    /// argument: - execution is at CPL0 with PSR.vm clear; - preemption, migration, and
    /// processor-local re-entry are excluded; - this code, its data, stack, RSE backing store, and
    /// every operand are pinned, shared, and Global under the fixed RR0 identity; - regionSet
    /// remains stable throughout the call.
    [[nodiscard]] static Ia64RuntimeRegionActivationError Activate(const Ia64RuntimeRegionSet& regionSet) noexcept;

    /// Physical Region-0 bootstrap activation for a post-seed AP. This entry is valid only while
    /// PSR.ic, PSR.i, PSR.pk, PSR.it, PSR.dt, and PSR.rt are clear. It checks those visible bits
    /// before entering the same exact native commit helper used by Activate, so the helper leaves
    /// IC clear on return. Execution is otherwise subject to the same CPL0, PSR.vm, exclusion, and
    /// stability preconditions. The complete direct call chain, GP-addressed data, memory stack,
    /// RSE backing store, regionSet, and every accessed operand must have physical Region-0
    /// addresses and remain resident; no translated or seed-map coverage is required by this entry.
    /// The caller owns the code/GP/stack/RSE proof.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA static Ia64RuntimeRegionActivationError
    ActivatePhysicalBootstrap(const Ia64RuntimeRegionSet& regionSet) noexcept;

private:
    Ia64RuntimeRegionActivator() = delete;
};

static_assert(sizeof(Ia64RuntimeRegionActivationError) == 1U);
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_REGION_ACTIVATOR_HPP_INCLUDED
