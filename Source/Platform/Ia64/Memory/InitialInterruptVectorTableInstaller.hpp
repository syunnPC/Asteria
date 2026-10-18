// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_INTERRUPT_VECTOR_TABLE_INSTALLER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_INTERRUPT_VECTOR_TABLE_INSTALLER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/InitialInterruptVectorTablePreparer.hpp"

namespace Asteria::Platform::Ia64::Memory
{
class InitialAddressSpacePreparation;

enum class InitialInterruptVectorTableInstallError : Foundation::UInt32
{
    None,
    PreparationNotReady,
    PreparedTableRejected,
    InvalidTableContents,
    UnsupportedProcessorState,
};

struct InitialInterruptVectorTableInstallFailure
{
    InitialInterruptVectorTableInstallError Error;
    InitialInterruptVectorTablePreparationError PreparationError;
    Foundation::Size ByteOffset;
};

struct InitialInterruptVectorTableInstallSummary
{
    Foundation::UInt64 PhysicalStart;
    Foundation::UInt32 SizeBytes;
    Foundation::UInt16 BundleCount;
    Foundation::UInt16 Reserved;
};

enum class InitialInterruptVectorTableApInstallStatus : Foundation::UInt32
{
    Installed,
    InvalidSummary,
    UnsupportedProcessorState,
    CommittedReadbackMismatch,
};

/// Revalidates and installs the prepared 32 KiB fail-stop IVT in cr.iva while
/// PSR.it remains clear. The caller must execute on the non-preemptible BSP at
/// CPL0 in the inherited physical SAL environment, with interrupts excluded.
/// The prepared owner and table bytes must remain stable throughout the call.
/// After success, the physical table range must remain reserved, compatibly
/// mapped, and byte-immutable until cr.iva is replaced or an explicit IVT
/// update republishes instruction bytes and performs the required serialization.
///
/// Every owner, mapping, storage, and byte-content check completes before the
/// privileged adapter is entered. The adapter rejects incompatible visible
/// PSR state without changing cr.iva. After writing cr.iva it instruction- and
/// data-serializes, reads the register back, and enters a local fail-stop if
/// the value differs; that committed failure cannot return. This operation
/// does not change PSR, region or translation registers, the stack, or RSE
/// state, and it does not enable interruption collection or translation.
class InitialInterruptVectorTableInstaller final
{
public:
    /// Performs all non-privileged checks without changing memory or CPU state.
    [[nodiscard]] static Foundation::Result<InitialInterruptVectorTableInstallSummary,
                                            InitialInterruptVectorTableInstallFailure>
    Validate(const InitialAddressSpacePreparation& preparation, const WritableInterruptVectorTableView& table) noexcept;

    [[nodiscard]] static Foundation::Result<InitialInterruptVectorTableInstallSummary,
                                            InitialInterruptVectorTableInstallFailure>
    Install(const InitialAddressSpacePreparation& preparation, const WritableInterruptVectorTableView& table) noexcept;

    /// Installs the BSP-retained table on one application processor without
    /// reading the shared table or revisiting its full preparation owner.
    /// The retained summary and BSP-prepared 32 KiB table are boot-lifetime,
    /// immutable shared storage. The caller must execute at CPL0 in the
    /// inherited physical SAL environment with local interruption collection,
    /// interrupts, and preemption excluded.
    ///
    /// InvalidSummary and UnsupportedProcessorState are returned before
    /// cr.iva is written. CommittedReadbackMismatch is returned only after the
    /// adapter has written cr.iva, serialized, and observed a different value.
    /// On that status the fixed [[noreturn]] AP continuation must immediately
    /// release-publish failure and park permanently; it must not return to SAL
    /// or continue through any path that assumes the prior cr.iva remains
    /// installed. Installed is the only status authorizing normal AP startup.
    [[nodiscard]] static InitialInterruptVectorTableApInstallStatus
    InstallForApplicationProcessor(InitialInterruptVectorTableInstallSummary summary) noexcept;

private:
    InitialInterruptVectorTableInstaller() = delete;
};

static_assert(sizeof(InitialInterruptVectorTableInstallError) == 4U);
static_assert(sizeof(InitialInterruptVectorTableInstallFailure) == 16U);
static_assert(alignof(InitialInterruptVectorTableInstallFailure) == 8U);
static_assert(sizeof(InitialInterruptVectorTableInstallSummary) == 16U);
static_assert(alignof(InitialInterruptVectorTableInstallSummary) == 8U);
static_assert(sizeof(InitialInterruptVectorTableApInstallStatus) == 4U);
static_assert(static_cast<Foundation::UInt32>(InitialInterruptVectorTableApInstallStatus::Installed) == 0U);
static_assert(static_cast<Foundation::UInt32>(InitialInterruptVectorTableApInstallStatus::InvalidSummary) == 1U);
static_assert(static_cast<Foundation::UInt32>(InitialInterruptVectorTableApInstallStatus::UnsupportedProcessorState) ==
              2U);
static_assert(static_cast<Foundation::UInt32>(InitialInterruptVectorTableApInstallStatus::CommittedReadbackMismatch) ==
              3U);
static_assert(__is_aggregate(InitialInterruptVectorTableInstallFailure));
static_assert(__is_aggregate(InitialInterruptVectorTableInstallSummary));
static_assert(__is_standard_layout(InitialInterruptVectorTableInstallFailure));
static_assert(__is_standard_layout(InitialInterruptVectorTableInstallSummary));
static_assert(__is_trivially_copyable(InitialInterruptVectorTableInstallFailure));
static_assert(__is_trivially_copyable(InitialInterruptVectorTableInstallSummary));
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_INTERRUPT_VECTOR_TABLE_INSTALLER_HPP_INCLUDED
