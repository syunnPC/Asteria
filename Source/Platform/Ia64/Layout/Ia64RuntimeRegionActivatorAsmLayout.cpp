// SPDX-License-Identifier: Apache-2.0

// The shared bootstrap region-register value for
// Memory/Ia64RuntimeRegionActivator.S (see Platform/AsmLayout.hpp).

#include "Platform/AsmLayout.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeRegionSet.hpp"

namespace
{
using Asteria::Platform::Ia64::Memory::Ia64SharedBootstrapRegionRegister;
} // namespace

extern "C" void AsteriaIa64RuntimeRegionActivatorAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SHARED_BOOTSTRAP_REGION_REGISTER, Ia64SharedBootstrapRegionRegister);
}
