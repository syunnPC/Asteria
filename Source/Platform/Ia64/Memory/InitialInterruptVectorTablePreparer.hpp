// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_INTERRUPT_VECTOR_TABLE_PREPARER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_INTERRUPT_VECTOR_TABLE_PREPARER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Ia64/Memory/BootstrapMappingPlan.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
struct SalSystemTableInformation;
}

namespace Asteria::Platform::Ia64::Memory
{
class InitialAddressSpacePreparation;
struct InitialTranslationRegionSet;
struct PalVirtualMemoryCapabilities;
struct WritableRseBackingStoreView;

constexpr Foundation::Size InitialInterruptVectorBundleSizeBytes{16U};
constexpr Foundation::Size InitialInterruptVectorTableSizeBytes{
    static_cast<Foundation::Size>(BootstrapInterruptVectorTablePageCount) * Kernel::Memory::MappingPageSizeBytes};

struct WritableInterruptVectorTableView
{
    Foundation::UInt8* Data;
    Foundation::Size SizeBytes;
    Foundation::UInt64 PhysicalStart;
};

enum class InitialInterruptVectorTablePreparationError : Foundation::UInt32
{
    None,
    InvalidMappingPlan,
    InvalidMappingSummary,
    InvalidStorageView,
    StorageAddressMismatch,
    StorageAliasesInput,
    StorageAliasesPreparation,
    MissingTableMapping,
    InvalidTableMapping,
    ConflictingTableMapping,
};

/// Creates a 32 KiB fail-stop IA-64 interruption-vector table and publishes
/// its instruction bytes. The table is not installed into cr.iva and no
/// interruption, translation, stack, or RSE register is changed.
class InitialInterruptVectorTablePreparer final
{
public:
    /// Proves that the IVT view is disjoint from the owner, retained inputs,
    /// and a well-formed RSE view before either child plan mutates the owner.
    /// The coordinator must also call the RSE preparer's storage preflight to
    /// validate that view and prove its own separation from retained inputs.
    [[nodiscard]] static InitialInterruptVectorTablePreparationError ValidatePreparationStorage(
        const InitialAddressSpacePreparation& preparation, const Firmware::Ia64LoadedImageLayout& image,
        const Boot::BootInfo& finalBootInfo, const Kernel::Memory::PhysicalPageRange& reservedPages,
        const Kernel::Memory::PhysicalPageRange& runtimeMemoryPages, const PalVirtualMemoryCapabilities& capabilities,
        const Firmware::SalSystemTableInformation& firmware, const InitialTranslationRegionSet& regions,
        const WritableInterruptVectorTableView& storage, const WritableRseBackingStoreView& rseStorage) noexcept;

    [[nodiscard]] static InitialInterruptVectorTablePreparationError
    Prepare(const Kernel::Memory::InitialMappingPlan& mappingPlan, const BootstrapMappingPlanSummary& mappingSummary,
            WritableInterruptVectorTableView storage) noexcept;

    /// Performs every mapping and view check without modifying the table.
    [[nodiscard]] static InitialInterruptVectorTablePreparationError
    Validate(const Kernel::Memory::InitialMappingPlan& mappingPlan, const BootstrapMappingPlanSummary& mappingSummary,
             WritableInterruptVectorTableView storage) noexcept;

private:
    InitialInterruptVectorTablePreparer() = delete;
};

static_assert(InitialInterruptVectorBundleSizeBytes == 16U);
static_assert(InitialInterruptVectorTableSizeBytes == 32U * 1024U);
static_assert(sizeof(WritableInterruptVectorTableView) == 24U);
static_assert(alignof(WritableInterruptVectorTableView) == 8U);
static_assert(__is_aggregate(WritableInterruptVectorTableView));
static_assert(__is_standard_layout(WritableInterruptVectorTableView));
static_assert(__is_trivially_copyable(WritableInterruptVectorTableView));
static_assert(__builtin_offsetof(WritableInterruptVectorTableView, Data) == 0U);
static_assert(__builtin_offsetof(WritableInterruptVectorTableView, SizeBytes) == 8U);
static_assert(__builtin_offsetof(WritableInterruptVectorTableView, PhysicalStart) == 16U);
static_assert(sizeof(InitialInterruptVectorTablePreparationError) == 4U);
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_INTERRUPT_VECTOR_TABLE_PREPARER_HPP_INCLUDED
