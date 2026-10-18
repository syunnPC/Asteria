// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_RSE_BACKING_STORE_PREPARER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_RSE_BACKING_STORE_PREPARER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Ia64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Ia64/Memory/InitialTranslationResourcePlan.hpp"

namespace Asteria::Platform::Ia64::Memory
{
class InitialAddressSpacePreparation;

constexpr Foundation::Size InitialRseBackingStoreSizeBytes{
    static_cast<Foundation::Size>(BootstrapRseBackingStorePageCount) * Kernel::Memory::MappingPageSizeBytes};

/// Describes a writable processor-visible alias of a physical byte range.
/// The caller owns the storage and must keep the alias valid for the call.
struct WritableRseBackingStoreView
{
    Foundation::UInt8* Data;
    Foundation::Size SizeBytes;
    Foundation::UInt64 PhysicalStart;
};

enum class InitialRseBackingStorePreparationError : Foundation::UInt32
{
    None,
    InvalidMappingPlan,
    InvalidMappingSummary,
    InvalidStorageView,
    StorageAddressMismatch,
    StorageAliasesInput,
    MissingBackingStoreMapping,
    InvalidBackingStoreMapping,
    ConflictingBackingStoreMapping,
    StorageAliasesPreparation,
};

/// Validates the complete sealed mapping contract before zeroing the exact RSE
/// backing-store range. A failure performs no write. Success initializes the
/// storage but does not configure or enable the Register Stack Engine.
class InitialRseBackingStorePreparer final
{
public:
    /// Performs the non-writing validation required before the owning
    /// preparation may modify either child plan. StorageAliasesPreparation
    /// means the destination intersects the owner itself; the caller must
    /// reject that case without changing the owner.
    [[nodiscard]] static InitialRseBackingStorePreparationError ValidatePreparationStorage(
        const InitialAddressSpacePreparation& preparation, const Firmware::Ia64LoadedImageLayout& image,
        const Boot::BootInfo& finalBootInfo, const Kernel::Memory::PhysicalPageRange& reservedPages,
        const Kernel::Memory::PhysicalPageRange& runtimeMemoryPages, const PalVirtualMemoryCapabilities& capabilities,
        const Firmware::SalSystemTableInformation& firmware, const InitialTranslationRegionSet& regions,
        const WritableRseBackingStoreView& storage) noexcept;

    [[nodiscard]] static InitialRseBackingStorePreparationError
    Prepare(const Kernel::Memory::InitialMappingPlan& mappingPlan, const BootstrapMappingPlanSummary& mappingSummary,
            WritableRseBackingStoreView storage) noexcept;

    /// Performs every mapping and view check without modifying the storage.
    [[nodiscard]] static InitialRseBackingStorePreparationError
    Validate(const Kernel::Memory::InitialMappingPlan& mappingPlan, const BootstrapMappingPlanSummary& mappingSummary,
             WritableRseBackingStoreView storage) noexcept;

private:
    InitialRseBackingStorePreparer() = delete;
};

static_assert(InitialRseBackingStoreSizeBytes == 32U * 1024U);
static_assert(sizeof(WritableRseBackingStoreView) == 24U);
static_assert(alignof(WritableRseBackingStoreView) == 8U);
static_assert(__is_aggregate(WritableRseBackingStoreView));
static_assert(__is_standard_layout(WritableRseBackingStoreView));
static_assert(__is_trivially_copyable(WritableRseBackingStoreView));
static_assert(__builtin_offsetof(WritableRseBackingStoreView, Data) == 0U);
static_assert(__builtin_offsetof(WritableRseBackingStoreView, SizeBytes) == 8U);
static_assert(__builtin_offsetof(WritableRseBackingStoreView, PhysicalStart) == 16U);
static_assert(sizeof(InitialRseBackingStorePreparationError) == 4U);
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_INITIAL_RSE_BACKING_STORE_PREPARER_HPP_INCLUDED
