// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_KERNEL_ENTRY_ABI_HPP_INCLUDED
#define ASTERIA_BOOT_KERNEL_ENTRY_ABI_HPP_INCLUDED

#include "Boot/BootInfo.hpp"

namespace Asteria::Boot
{
constexpr Foundation::UInt32 KernelEntryAbiVersion{1U};

/// Scalar diagnostics cross the link boundary without borrowing a kernel
/// C++ object. Unknown selectors or invalid indices return zero. Address
/// selectors return identity addresses of kernel-owned immutable strings;
/// the loader must bound a string inside a read-only image range before use.
enum class KernelQuery : Foundation::UInt32
{
    EvidenceAddress,
    FirmwareApicId,
    PagingCapabilityError,
    ProcessorStateCaptureError,
    PreparationFailed,
    PreparationError,
    CaptureError,
    PageTableError,
    PageTableBuildError,
    PageTableVerificationError,
    MappingError,
    MappingIndex,
    MtrrCapability,
    MtrrDefaultType,
    VariableMtrrCount,
    VariableMtrrBase,
    VariableMtrrMask,
    RuntimeRejectionAddress,
    FinalMapRejectionAddress,
    TableRejected,
    TscSourcesPresent,
    TscSourcesTried,
    /// AMD64's metadata arena in 4 KiB pages for a managed span of index GiB: the slabs and the
    /// allocator bitmaps, rounded up to 2 MiB and capped; zero on IA-64.
    MetadataArenaPages,
};

using KernelPrepareFunction = Foundation::UInt32 (*)(BootInfo*) noexcept;
using KernelEnterFunction = void (*)(const BootInfo*) noexcept;
using KernelQueryFunction = Foundation::UInt64 (*)(Foundation::UInt32, Foundation::UInt32) noexcept;
} // Namespace Asteria::Boot

/// Identity-addressed, single BSP entries. Preparation imports the sealed
/// record, calls no firmware, and returns 1 on success or 0 on rejection.
/// Entry reimports the final record after ExitBootServices and never returns.
/// Neither entry retains the loader's pointer. Query allocates and writes
/// nothing; the loader may call it before or after preparation.
extern "C" Asteria::Foundation::UInt32 AsteriaKernelPrepare(Asteria::Boot::BootInfo*) noexcept;
extern "C" [[noreturn]] void AsteriaKernelEnter(const Asteria::Boot::BootInfo*) noexcept;
extern "C" Asteria::Foundation::UInt64 AsteriaKernelQuery(Asteria::Foundation::UInt32,
                                                          Asteria::Foundation::UInt32) noexcept;

#endif // ASTERIA_BOOT_KERNEL_ENTRY_ABI_HPP_INCLUDED
