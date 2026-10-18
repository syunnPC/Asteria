// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_TRANSLATION_ACTIVATOR_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_TRANSLATION_ACTIVATOR_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Ia64::Memory
{
enum class InitialTranslationActivationError : Foundation::UInt32
{
    None,
    UnsupportedProcessorState,
    UnexpectedAdapterReturn,
};

/// Enters the fixed translated Region-0 kernel continuation. Preconditions: - the boot coordinator
/// has installed the runtime IVA and ar.k3, activated RR1 through RR7, and published the exact
/// active translation ledger; - execution owns its stack and RSE context and has ar.rsc equal to
/// zero; - execution is at CPL0 with PSR.vm, PSR.ic, PSR.i, PSR.pk, PSR.it, PSR.dt, and PSR.rt
/// clear; - local interrupts and preemption are excluded. A compatible processor enters
/// AsteriaIa64InitialTranslatedExecution, which directly calls the boot-owned translated kernel
/// continuation and never returns. A returned native status reports rejection before commit or an
/// unexpected adapter return.
class InitialTranslationActivator final
{
public:
    [[nodiscard]] static InitialTranslationActivationError Activate() noexcept;

private:
    InitialTranslationActivator() = delete;
};

static_assert(sizeof(InitialTranslationActivationError) == 4U);
static_assert(static_cast<Foundation::UInt32>(InitialTranslationActivationError::None) == 0U);
static_assert(static_cast<Foundation::UInt32>(InitialTranslationActivationError::UnsupportedProcessorState) == 1U);
static_assert(static_cast<Foundation::UInt32>(InitialTranslationActivationError::UnexpectedAdapterReturn) == 2U);
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_TRANSLATION_ACTIVATOR_HPP_INCLUDED
