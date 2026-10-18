// SPDX-License-Identifier: Apache-2.0

#include "Platform/AsmLayout.hpp"
#include "Boot/BootInfo.hpp"
#include "Boot/KernelEntryAbi.hpp"
#include "Boot/KernelElf.hpp"
#include "Platform/Amd64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Amd64/Boot/Amd64RuntimePoolLayout.hpp"

extern "C" void AsteriaKernelNoteAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_KERNEL_NOTE_TYPE, Asteria::Boot::KernelElfNoteType);
    ASTERIA_ASM_LAYOUT(ASTERIA_KERNEL_NOTE_DESCRIPTOR_BYTES, Asteria::Boot::KernelElfNoteDescriptorBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_KERNEL_ENTRY_ABI_VERSION, Asteria::Boot::KernelEntryAbiVersion);
    ASTERIA_ASM_LAYOUT(ASTERIA_KERNEL_BOOT_INFO_VERSION, Asteria::Boot::BootInfoVersion);
    ASTERIA_ASM_LAYOUT(ASTERIA_KERNEL_BOOTSTRAP_PAGES, Asteria::Platform::Amd64::Memory::BootstrapReservedPageCount);
    ASTERIA_ASM_LAYOUT(ASTERIA_KERNEL_RUNTIME_PAGES, Asteria::Platform::Amd64::Bootstrap::RuntimePoolPages);
}
