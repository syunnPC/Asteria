// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_MEMORY_INITIAL_ADDRESS_SPACE_PREPARATION_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_MEMORY_INITIAL_ADDRESS_SPACE_PREPARATION_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Amd64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Amd64/Memory/InitialPageTablePreparation.hpp"
#include "Platform/Amd64/Memory/InitialPageTableVerifier.hpp"

namespace Asteria::Platform::Amd64::Memory
{
constexpr Foundation::Size InitialMemoryStackSizeBytes{static_cast<Foundation::Size>(BootstrapStackPageCount) *
                                                       Kernel::Memory::MappingPageSizeBytes};
constexpr Foundation::Size InitialGlobalDescriptorTableEntryCount{512U};
constexpr Foundation::Size InitialInterruptDescriptorTableEntryCount{256U};
constexpr Foundation::Size InitialInterruptDescriptorTableWordCount{InitialInterruptDescriptorTableEntryCount * 2U};
constexpr Foundation::Size InitialDescriptorRegisterImageSizeBytes{10U};
constexpr Foundation::Size InitialDescriptorTableStorageSizeBytes{
    static_cast<Foundation::Size>(BootstrapDescriptorTableStoragePageCount) * Kernel::Memory::MappingPageSizeBytes};
constexpr Foundation::Size MaximumInitialVariableMtrrCount{16U};
constexpr Foundation::Size InitialFixedMtrrRegisterCount{11U};

/// The control registers the activation establishes itself, whatever the
/// firmware left: OVMF sets CR0.WP and EFER.NXE and clears CR4.PGE, older
/// firmware often does not.
struct InitialControlState
{
    Foundation::UInt64 Cr0;
    Foundation::UInt64 Cr4;
    Foundation::UInt64 Efer;
};

/// Supervisor write protection (CR0.WP) and execute-disable (EFER.NXE) on;
/// global pages (CR4.PGE) and PCIDs (CR4.PCIDE) off. Every other bit is the
/// firmware's and stays subject to the preparation's checks; so is CR3, which
/// is not rewritten (a PCID left in CR3[11:0] is rejected, not cleared).
/// Idempotent, so the captured report and the current state compare equal
/// before and after the activation establishes it. EFER.NXE requires the
/// processor's execute-disable support, which the paging capability capture
/// demands.
[[nodiscard]] constexpr InitialControlState EstablishInitialControlState(const InitialControlState& firmware) noexcept
{
    constexpr Foundation::UInt64 Cr0WriteProtect{1U << 16U};
    constexpr Foundation::UInt64 Cr4GlobalPages{1U << 7U};
    constexpr Foundation::UInt64 Cr4ProcessContextIdentifiers{1U << 17U};
    constexpr Foundation::UInt64 EferExecuteDisable{1U << 11U};
    return InitialControlState{
        .Cr0 = firmware.Cr0 | Cr0WriteProtect,
        .Cr4 = firmware.Cr4 & ~(Cr4GlobalPages | Cr4ProcessContextIdentifiers),
        .Efer = firmware.Efer | EferExecuteDisable,
    };
}

enum class InitialAddressSpacePreparationStatus : Foundation::UInt32
{
    None,
    Prepared,
};

enum class InitialProcessorStateCaptureError : Foundation::UInt32
{
    None,
    InvalidCapabilities,
    MemoryTypeRangeRegistersUnavailable,
    TooManyVariableMemoryTypeRanges,
};

enum class InitialAddressSpacePreparationError : Foundation::UInt32
{
    None,
    AlreadyPrepared,
    PreviousPreparationFailed,
    StorageAliasesPreparation,
    InvalidMappingSummary,
    GlobalMappingUnsupported,
    InvalidActivationMapping,
    InvalidProcessorState,
    InvalidMemoryTypeConfiguration,
    InvalidMemoryStack,
    InvalidPageTableStorage,
    PageTablePreparationRejected,
    PageTableVerificationRejected,
    InvalidDescriptorTableStorage,
    MemoryStackNotZero,
    DescriptorTablesCorrupt,
    ProcessorStateChanged,
    UnsupportedProcessorState,
    UnexpectedAdapterReturn,
    NotPrepared,
};

struct WritableInitialMemoryStackView
{
    Foundation::UInt8* Data;
    Foundation::Size SizeBytes;
    Foundation::UInt64 PhysicalStart;
};

struct WritableInitialDescriptorTableView
{
    Foundation::UInt64* Data;
    Foundation::Size SizeBytes;
    Foundation::UInt64 PhysicalStart;
};

struct InitialProcessorStateReport
{
    Foundation::UInt64 Cr0;
    Foundation::UInt64 Cr3;
    Foundation::UInt64 Cr4;
    Foundation::UInt64 Efer;
    Foundation::UInt64 Pat;
    Foundation::UInt64 MtrrCapability;
    Foundation::UInt64 MtrrDefaultType;
    Foundation::UInt64 Rflags;
    Foundation::UInt64 VariableMtrrBase[MaximumInitialVariableMtrrCount];
    Foundation::UInt64 VariableMtrrMask[MaximumInitialVariableMtrrCount];
    Foundation::UInt64 FixedMtrr[InitialFixedMtrrRegisterCount];
    Foundation::UInt16 CodeSelector;
    Foundation::UInt16 StackSelector;
    Foundation::UInt8 VariableMtrrCount;
    bool FixedMtrrsSupported;
    Foundation::UInt16 Reserved16;
    Foundation::UInt32 Reserved32;
    Foundation::UInt32 ReservedTail;
};

struct InitialAddressSpacePreparationSummary
{
    Kernel::Memory::PhysicalAddress RootPhysicalAddress;
    Foundation::UInt64 StackPhysicalStart;
    Foundation::UInt64 InitialStackPointer;
    Foundation::UInt64 GlobalDescriptorTableAddress;
    Foundation::UInt64 InterruptDescriptorTableAddress;
    Foundation::Size UsedTablePages;
    Foundation::UInt16 CodeSelector;
    Foundation::UInt16 StackSelector;
    Foundation::UInt16 GlobalDescriptorTableLimit;
    Foundation::UInt16 InterruptDescriptorTableLimit;
    Foundation::UInt32 Reserved;
    Foundation::UInt32 ReservedTail;
};

struct InitialAddressSpacePreparationFailure
{
    InitialAddressSpacePreparationError Error;
    InitialProcessorStateCaptureError CaptureError;
    InitialPageTablePreparationError PageTableError;
    InitialPageTableBuildError PageTableBuildError;
    InitialPageTableVerificationError PageTableVerificationError;
    Kernel::Memory::InitialMappingPlanError MappingError;
    Foundation::UInt32 AdapterStatus;
    Foundation::UInt32 Reserved;
    Foundation::Size MappingIndex;
};

/// Owns the complete AMD64 state prepared before ExitBootServices and performs the later no-return
/// architectural commit. Prepare copies the sealed mapping plan into its page-table child, builds
/// the native root, clears the exact four-page memory stack, and constructs owned GDT/IDT images.
/// Prepared is published only after every byte has been verified. A stale-key retry may call
/// ValidatePreparedState, which performs no write. Activate takes no caller-supplied root, stack,
/// descriptor, callback, or authorization token. It revalidates only the retained state, claims the
/// single-use owner, and enters one local assembly corridor. A successful activation never returns.
/// All initial gates use IST zero and the selected CPL0 stack. A fault caused by an invalid stack
/// mapping may therefore fail to reach the owned fail-stop handler.
class InitialAddressSpacePreparation final
{
public:
    constexpr InitialAddressSpacePreparation() noexcept = default;

    InitialAddressSpacePreparation(const InitialAddressSpacePreparation&) = delete;
    InitialAddressSpacePreparation& operator=(const InitialAddressSpacePreparation&) = delete;
    InitialAddressSpacePreparation(InitialAddressSpacePreparation&&) = delete;
    InitialAddressSpacePreparation& operator=(InitialAddressSpacePreparation&&) = delete;

    /// Captures read-only architectural state, with CR0, CR4 and EFER as
    /// the activation will establish them (EstablishInitialControlState).
    /// Output is assigned only after all required MSRs have been captured
    /// within the fixed profile bound.
    [[nodiscard]] static InitialProcessorStateCaptureError
    CaptureProcessorState(const PagingCapabilities& capabilities, InitialProcessorStateReport& output) noexcept;

    [[nodiscard]] Foundation::Result<InitialAddressSpacePreparationStatus, InitialAddressSpacePreparationError>
    Prepare(const Kernel::Memory::InitialMappingPlan& mappings, const BootstrapMappingPlanSummary& mappingSummary,
            const PagingCapabilities& capabilities, const PageTableStorage& tableStorage,
            const WritableInitialMemoryStackView& memoryStack,
            const WritableInitialDescriptorTableView& descriptorTables,
            const InitialProcessorStateReport& processorState) noexcept;

    /// Rechecks the complete retained table, stack, descriptor, and processor
    /// snapshot without changing memory or architectural state.
    [[nodiscard]] InitialAddressSpacePreparationError ValidatePreparedState() const noexcept;

    [[nodiscard]] bool IsPrepared() const noexcept;
    [[nodiscard]] bool HasFailed() const noexcept;

    [[nodiscard]] Foundation::Result<const InitialAddressSpacePreparationSummary*, InitialAddressSpacePreparationError>
    TryGetSummary() const noexcept;

    [[nodiscard]] Foundation::Result<const InitialPageTablePreparation*, InitialAddressSpacePreparationError>
    TryGetPageTablePreparation() const noexcept;

    /// Claims the prepared owner, establishes the control state the report
    /// describes (EstablishInitialControlState: after ExitBootServices, so
    /// no firmware code runs under it), and enters the local privileged
    /// adapter, which asserts that state. Only pre-commit validation errors
    /// can return normally.
    [[nodiscard]] InitialAddressSpacePreparationError Activate() noexcept;

    /// Preconditions: HasFailed is true.
    [[nodiscard]] const InitialAddressSpacePreparationFailure& GetFailure() const noexcept;

private:
    enum class State : Foundation::UInt32
    {
        Fresh,
        Failed,
        Prepared,
        ActivationClaimed,
    };

    alignas(2) Foundation::UInt8 m_globalDescriptorRegister[InitialDescriptorRegisterImageSizeBytes]{};
    alignas(2) Foundation::UInt8 m_interruptDescriptorRegister[InitialDescriptorRegisterImageSizeBytes]{};
    InitialPageTablePreparation m_pageTables{};
    BootstrapMappingPlanSummary m_mappingSummary{};
    PagingCapabilities m_capabilities{};
    WritableInitialMemoryStackView m_memoryStack{};
    WritableInitialDescriptorTableView m_descriptorTables{};
    InitialProcessorStateReport m_processorState{};
    InitialAddressSpacePreparationSummary m_summary{};
    InitialAddressSpacePreparationFailure m_failure{};
    State m_state{State::Fresh};
    Foundation::UInt32 m_reserved{0U};
};

static_assert(sizeof(InitialControlState) == 24U);
static_assert(InitialMemoryStackSizeBytes == 16U * 1024U);
static_assert(InitialGlobalDescriptorTableEntryCount == 512U);
static_assert(InitialInterruptDescriptorTableEntryCount == 256U);
static_assert(InitialInterruptDescriptorTableWordCount == 512U);
static_assert(InitialDescriptorTableStorageSizeBytes == 8U * 1024U);
static_assert(sizeof(WritableInitialMemoryStackView) == 24U);
static_assert(alignof(WritableInitialMemoryStackView) == 8U);
static_assert(__is_aggregate(WritableInitialMemoryStackView));
static_assert(__is_standard_layout(WritableInitialMemoryStackView));
static_assert(__is_trivially_copyable(WritableInitialMemoryStackView));
static_assert(__builtin_offsetof(WritableInitialMemoryStackView, Data) == 0U);
static_assert(__builtin_offsetof(WritableInitialMemoryStackView, SizeBytes) == 8U);
static_assert(__builtin_offsetof(WritableInitialMemoryStackView, PhysicalStart) == 16U);
static_assert(sizeof(WritableInitialDescriptorTableView) == 24U);
static_assert(alignof(WritableInitialDescriptorTableView) == 8U);
static_assert(__is_aggregate(WritableInitialDescriptorTableView));
static_assert(__is_standard_layout(WritableInitialDescriptorTableView));
static_assert(__is_trivially_copyable(WritableInitialDescriptorTableView));
static_assert(__builtin_offsetof(WritableInitialDescriptorTableView, Data) == 0U);
static_assert(__builtin_offsetof(WritableInitialDescriptorTableView, SizeBytes) == 8U);
static_assert(__builtin_offsetof(WritableInitialDescriptorTableView, PhysicalStart) == 16U);
static_assert(sizeof(InitialProcessorStateCaptureError) == 4U);
static_assert(sizeof(InitialAddressSpacePreparationError) == 4U);
static_assert(sizeof(InitialProcessorStateReport) == 424U);
static_assert(alignof(InitialProcessorStateReport) == 8U);
static_assert(__is_aggregate(InitialProcessorStateReport));
static_assert(__is_standard_layout(InitialProcessorStateReport));
static_assert(__is_trivially_copyable(InitialProcessorStateReport));
static_assert(__builtin_offsetof(InitialProcessorStateReport, Cr0) == 0U);
static_assert(__builtin_offsetof(InitialProcessorStateReport, VariableMtrrBase) == 64U);
static_assert(__builtin_offsetof(InitialProcessorStateReport, VariableMtrrMask) == 192U);
static_assert(__builtin_offsetof(InitialProcessorStateReport, FixedMtrr) == 320U);
static_assert(__builtin_offsetof(InitialProcessorStateReport, CodeSelector) == 408U);
static_assert(__builtin_offsetof(InitialProcessorStateReport, Reserved32) == 416U);
static_assert(__builtin_offsetof(InitialProcessorStateReport, ReservedTail) == 420U);
static_assert(__builtin_offsetof(InitialProcessorStateReport, ReservedTail) +
                  sizeof(InitialProcessorStateReport::ReservedTail) ==
              sizeof(InitialProcessorStateReport));
static_assert(sizeof(InitialAddressSpacePreparationSummary) == 64U);
static_assert(alignof(InitialAddressSpacePreparationSummary) == 8U);
static_assert(__is_aggregate(InitialAddressSpacePreparationSummary));
static_assert(__is_standard_layout(InitialAddressSpacePreparationSummary));
static_assert(__is_trivially_copyable(InitialAddressSpacePreparationSummary));
static_assert(__builtin_offsetof(InitialAddressSpacePreparationSummary, RootPhysicalAddress) == 0U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationSummary, StackPhysicalStart) == 8U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationSummary, InitialStackPointer) == 16U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationSummary, GlobalDescriptorTableAddress) == 24U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationSummary, InterruptDescriptorTableAddress) == 32U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationSummary, UsedTablePages) == 40U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationSummary, CodeSelector) == 48U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationSummary, Reserved) == 56U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationSummary, ReservedTail) == 60U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationSummary, ReservedTail) +
                  sizeof(InitialAddressSpacePreparationSummary::ReservedTail) ==
              sizeof(InitialAddressSpacePreparationSummary));
static_assert(sizeof(InitialAddressSpacePreparationFailure) == 40U);
static_assert(alignof(InitialAddressSpacePreparationFailure) == 8U);
static_assert(__is_aggregate(InitialAddressSpacePreparationFailure));
static_assert(__is_standard_layout(InitialAddressSpacePreparationFailure));
static_assert(__is_trivially_copyable(InitialAddressSpacePreparationFailure));
static_assert(__builtin_offsetof(InitialAddressSpacePreparationFailure, Error) == 0U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationFailure, AdapterStatus) == 24U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationFailure, Reserved) == 28U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationFailure, MappingIndex) == 32U);
static_assert(__builtin_offsetof(InitialAddressSpacePreparationFailure, MappingIndex) + sizeof(Foundation::Size) ==
              sizeof(InitialAddressSpacePreparationFailure));
} // Namespace Asteria::Platform::Amd64::Memory

#endif // ASTERIA_PLATFORM_AMD64_MEMORY_INITIAL_ADDRESS_SPACE_PREPARATION_HPP_INCLUDED
