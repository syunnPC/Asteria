// SPDX-License-Identifier: Apache-2.0

// InitialInterruptVectorTableApInstallStatus values for
// Memory/InitialInterruptVectorTableInstaller.S (see Platform/AsmLayout.hpp).

#include "Platform/AsmLayout.hpp"
#include "Platform/Ia64/Memory/InitialInterruptVectorTableInstaller.hpp"

namespace
{
using Asteria::Foundation::UInt32;
using Asteria::Platform::Ia64::Memory::InitialInterruptVectorTableApInstallStatus;
} // namespace

extern "C" void AsteriaInitialInterruptVectorTableInstallerAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_INITIAL_INTERRUPT_VECTOR_TABLE_SIZE_BYTES,
                       Asteria::Platform::Ia64::Memory::InitialInterruptVectorTableSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_INITIAL_INTERRUPT_VECTOR_BUNDLE_SIZE_BYTES,
                       Asteria::Platform::Ia64::Memory::InitialInterruptVectorBundleSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_INITIAL_INTERRUPT_VECTOR_TABLE_AP_INSTALL_STATUS_UNSUPPORTED_PROCESSOR_STATE,
                       static_cast<UInt32>(InitialInterruptVectorTableApInstallStatus::UnsupportedProcessorState));
    ASTERIA_ASM_LAYOUT(ASTERIA_INITIAL_INTERRUPT_VECTOR_TABLE_AP_INSTALL_STATUS_COMMITTED_READBACK_MISMATCH,
                       static_cast<UInt32>(InitialInterruptVectorTableApInstallStatus::CommittedReadbackMismatch));
}
