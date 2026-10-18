// SPDX-License-Identifier: Apache-2.0

#include "Platform/AsmLayout.hpp"
#include "Platform/Ia64/Device/Ia64VtdHardware.hpp"

extern "C" void AsteriaIa64VtdCacheMaintenanceAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_VTD_FLUSH_LINE_SHIFT, Asteria::Platform::Ia64::Device::Ia64VtdFlushLineShift);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_VTD_FLUSH_LINE_BYTES, Asteria::Platform::Ia64::Device::Ia64VtdFlushLineBytes);
}
