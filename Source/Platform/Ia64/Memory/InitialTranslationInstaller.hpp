// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_TRANSLATION_INSTALLER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_TRANSLATION_INSTALLER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/InitialTranslationResourcePlan.hpp"
#include "Platform/Ia64/Memory/PalTranslationCachePurgeDecoder.hpp"

namespace Asteria::Platform::Ia64::Memory
{
enum class InitialTranslationInstallError : Foundation::UInt32
{
    None,
    InvalidPurgePlan,
    ResourcePlanNotBuilt,
    InvalidCapabilities,
    InvalidRegionConfiguration,
    InvalidFirmwareTranslation,
    InvalidResourceEntry,
    OverlappingResourceEntries,
    UnsupportedProcessorState,
};

struct InitialTranslationInstallFailure
{
    InitialTranslationInstallError Error;
    Foundation::UInt32 Reserved;
    Foundation::Size EntryIndex;
};

struct InitialTranslationInstallSummary
{
    Foundation::UInt64 PurgeOperationCount;
    Foundation::UInt16 EntryCount;
    Foundation::UInt16 DataRegisterCount;
    Foundation::UInt16 InstructionRegisterCount;
    Foundation::UInt16 Reserved;
};

/// Installs the already-planned native IA-64 region and translation registers
/// while instruction, data, and RSE translation remain disabled. The caller
/// must execute on the processor whose translation state is being initialized,
/// at CPL0 in the inherited physical SAL environment, with interrupts and
/// same-CPU preemption excluded. All inputs must remain stable throughout the
/// call. The capability and purge records must describe that processor.
///
/// Every operand is validated before PSR.ic is cleared or any translation
/// state is changed. After that commit point the operation cannot fail: the
/// local translation cache is purged, firmware ITR(0) is removed, all eight
/// native region registers are written, serialized, and read back, and all
/// planned DTR/ITR entries are installed. A region-register readback mismatch
/// is unrecoverable and enters a local fail-stop before any success result.
/// This class performs no allocation, I/O, PAL call, firmware callback, stack
/// switch, translation enable, or RSE transition. On success PSR.ic remains
/// clear. Subsequent physical-mode code must stay within the installed locked
/// mappings until an architecture transition explicitly enables translation;
/// it must not resume a firmware continuation.
class InitialTranslationInstaller final
{
public:
    [[nodiscard]] static Foundation::Result<InitialTranslationInstallSummary, InitialTranslationInstallFailure>
    Install(const InitialTranslationResourcePlan& plan, const PalVirtualMemoryCapabilities& capabilities,
            const PalTranslationCachePurgePlan& purgePlan) noexcept;

private:
    InitialTranslationInstaller() = delete;
};

static_assert(sizeof(InitialTranslationInstallError) == 4U);
static_assert(sizeof(InitialTranslationInstallFailure) == 16U);
static_assert(alignof(InitialTranslationInstallFailure) == 8U);
static_assert(sizeof(InitialTranslationInstallSummary) == 16U);
static_assert(alignof(InitialTranslationInstallSummary) == 8U);
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_TRANSLATION_INSTALLER_HPP_INCLUDED
