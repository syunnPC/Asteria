// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_EXECUTABLE_IA64_INITIAL_USER_IMAGE_OWNER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_EXECUTABLE_IA64_INITIAL_USER_IMAGE_OWNER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Platform/Ia64/Executable/Ia64InitialUserImageSource.hpp"
#include "Platform/Ia64/Executable/Ia64P64Image.hpp"

namespace Asteria::Platform::Ia64::Executable
{
// Bounds of an image this owner stages: the boot pager (the test pager's
// scenarios and embedded spinner included) and the initial image. They bound
// the one contiguous arena (load pages plus the two four-page stacks) and the
// boot's copy and verification work, nothing else: the image is mapped by
// ledger records refilled as translation-cache entries, one record per
// segment, so no translation register depends on them. The build gate
// (Tools/Build/CheckImageStaging.py) reads these three literals.
inline constexpr Foundation::Size Ia64InitialUserImageMaximumSegmentCount{8U};
inline constexpr Foundation::UInt64 Ia64InitialUserImageMaximumSegmentPages{160U};
inline constexpr Foundation::UInt64 Ia64InitialUserImageMaximumLoadPages{352U};
static_assert(Ia64InitialUserImageMaximumSegmentPages <= Ia64InitialUserImageMaximumLoadPages);
inline constexpr Foundation::Size Ia64InitialUserImageNoSegment{~Foundation::Size{0U}};

class Ia64InitialUserImageMemoryOwner;

enum class Ia64InitialUserImageOwnerState : Foundation::UInt8
{
    Fresh,
    Failed,
    Staged,
    Claimed,
};

enum class Ia64InitialUserImageOwnerError : Foundation::UInt32
{
    None,
    AlreadyAttempted,
    InvalidPhysicalAddressProfile,
    SourceRejected,
    InvalidSourceExtent,
    SourceAliasesControlStorage,
    P64ImageRejected,
    LoadPlanAccessRejected,
    SegmentOrderInvalid,
    SegmentPageCapacityExceeded,
    AggregatePageCapacityExceeded,
    BssSizeOverflow,
    MissingRealBss,
    GuardedVirtualRangeConflict,
    ArenaAllocationRejected,
    InvalidArenaAllocation,
    InvalidArenaAllocationAttributes,
    InvalidArenaAlias,
    ArenaAliasesSource,
    ArenaAliasesControlStorage,
    InvalidPhysicalSlice,
    StagedBytesMismatch,
};

enum class Ia64InitialUserImageOwnerAccessError : Foundation::UInt32
{
    None,
    NotStaged,
    IndexOutOfBounds,
    NoFailure,
};

struct Ia64InitialUserImageOwnerFailure
{
    Ia64InitialUserImageOwnerError Error;
    Ia64InitialUserImageSourceError Source;
    Ia64P64ImageFailure P64;
    Ia64P64LoadPlanAccessError LoadPlan;
    Kernel::Memory::BootstrapPageAllocatorError Allocator;
    Foundation::Size SegmentIndex;
};

struct Ia64InitialUserImageSegmentPlan
{
    ::Asteria::Executable::Elf::Elf64LoadSegment Segment;
    Kernel::Memory::PhysicalPageRange PhysicalSlice;
};

struct Ia64InitialUserImageStageSummary
{
    Ia64P64ImageInformation Image;
    Kernel::Memory::PhysicalPageRange AggregateArena;
    Kernel::Memory::PhysicalPageRange MemoryStack;
    Kernel::Memory::PhysicalPageRange RseBackingStore;
    Foundation::UInt64 AggregateLoadPageCount;
    Foundation::UInt64 TotalBssSizeBytes;
    Foundation::Size SegmentCount;
};

/// Owns the pointer-free bootstrap staging record for the initial IA-64 image.
///
/// Stage is externally serialized on the BSP before translation takeover. The
/// first call is terminal: validation failure leaves Failed, including after
/// allocator mutation, and there is no retry, release, or rollback operation.
/// Success retains only the aggregate allocator receipt and physical subrange
/// descriptions. A later memory owner may claim the staged receipt exactly
/// once; Claimed still owns that allocator receipt and exposes its immutable
/// summary and segment plans for boot-lifetime verification. Source and
/// identity-writable pointers are never retained.
class alignas(16U) Ia64InitialUserImageOwner final
{
public:
    constexpr Ia64InitialUserImageOwner() noexcept = default;
    ~Ia64InitialUserImageOwner() noexcept = default;

    Ia64InitialUserImageOwner(const Ia64InitialUserImageOwner&) = delete;
    Ia64InitialUserImageOwner& operator=(const Ia64InitialUserImageOwner&) = delete;
    Ia64InitialUserImageOwner(Ia64InitialUserImageOwner&&) = delete;
    Ia64InitialUserImageOwner& operator=(Ia64InitialUserImageOwner&&) = delete;

    /// Sources and decodes the boot-file image, allocates one packed physical
    /// arena, initializes every byte, verifies the result, and publishes all
    /// executable normalized slices. Does not map, register, admit, or launch.
    [[nodiscard]] Foundation::Result<Ia64InitialUserImageStageSummary, Ia64InitialUserImageOwnerFailure>
    Stage(Kernel::Memory::BootstrapPageAllocator& allocator, Foundation::UInt8 physicalAddressBits,
          Foundation::UInt8 implementedVirtualAddressMostSignificantBit,
          ::Asteria::Executable::Elf::Elf64ByteView source, const Boot::BootFileRegion& region) noexcept;

    [[nodiscard]] constexpr Ia64InitialUserImageOwnerState GetState() const noexcept
    {
        return m_state;
    }

    [[nodiscard]] Foundation::Result<Ia64InitialUserImageStageSummary, Ia64InitialUserImageOwnerAccessError>
    GetStageSummary() const noexcept;

    [[nodiscard]] Foundation::Result<Ia64InitialUserImageSegmentPlan, Ia64InitialUserImageOwnerAccessError>
    GetSegmentPlan(Foundation::Size index) const noexcept;

    [[nodiscard]] Foundation::Result<Ia64InitialUserImageOwnerFailure, Ia64InitialUserImageOwnerAccessError>
    GetFailure() const noexcept;

private:
    friend class Ia64InitialUserImageMemoryOwner;

    using StageResult = Foundation::Result<Ia64InitialUserImageStageSummary, Ia64InitialUserImageOwnerFailure>;

    /// Externally serialized one-shot claim used only at the materializer's
    /// commit cut. Failure has no effect.
    [[nodiscard]] bool ClaimForMemoryOwner() noexcept;

    void RecordFailure(Ia64InitialUserImageOwnerError error,
                       Foundation::Size segmentIndex = Ia64InitialUserImageNoSegment,
                       Ia64InitialUserImageSourceError source = Ia64InitialUserImageSourceError::None,
                       Ia64P64ImageFailure p64 = {},
                       Ia64P64LoadPlanAccessError loadPlan = Ia64P64LoadPlanAccessError::None,
                       Kernel::Memory::BootstrapPageAllocatorError allocator =
                           Kernel::Memory::BootstrapPageAllocatorError::None) noexcept;

    [[nodiscard]] StageResult Fail(Ia64InitialUserImageOwnerError error,
                                   Foundation::Size segmentIndex = Ia64InitialUserImageNoSegment,
                                   Ia64InitialUserImageSourceError source = Ia64InitialUserImageSourceError::None,
                                   Ia64P64ImageFailure p64 = {},
                                   Ia64P64LoadPlanAccessError loadPlan = Ia64P64LoadPlanAccessError::None,
                                   Kernel::Memory::BootstrapPageAllocatorError allocator =
                                       Kernel::Memory::BootstrapPageAllocatorError::None) noexcept;

    [[nodiscard]] bool ValidateSourceExtentAndAliases(::Asteria::Executable::Elf::Elf64ByteView source,
                                                      const Kernel::Memory::BootstrapPageAllocator& allocator) noexcept;

    [[nodiscard]] bool ValidateAndRetainSegments(Foundation::Size segmentCount,
                                                 Foundation::UInt64& aggregateLoadPageCount,
                                                 Foundation::UInt64& totalBssSizeBytes) noexcept;

    [[nodiscard]] bool ValidateArenaAllocation(const Kernel::Memory::BootstrapPageAllocator& allocator,
                                               ::Asteria::Executable::Elf::Elf64ByteView source,
                                               Foundation::UInt8 physicalAddressBits, Foundation::UInt64 arenaPageCount,
                                               Foundation::UInt8*& writableArena, Foundation::Size& writableSizeBytes,
                                               Foundation::UInt64& arenaPhysicalEnd) noexcept;

    [[nodiscard]] bool PackArenaSlices(Foundation::Size segmentCount, Foundation::UInt64 arenaPhysicalEnd,
                                       Foundation::UInt64& memoryStackSizeBytes,
                                       Foundation::UInt64& rseBackingStoreSizeBytes) noexcept;

    [[nodiscard]] bool ValidateSegmentOffsets(Foundation::Size segmentCount, Foundation::Size sourceSizeBytes) noexcept;

    [[nodiscard]] bool InitializeAndVerifyArena(::Asteria::Executable::Elf::Elf64ByteView source,
                                                Foundation::UInt8* writableArena, Foundation::Size writableSizeBytes,
                                                Foundation::UInt64 memoryStackSizeBytes,
                                                Foundation::UInt64 rseBackingStoreSizeBytes) noexcept;

    Ia64P64LoadPlan m_loadPlan{};
    Ia64InitialUserImageSegmentPlan m_segmentPlans[Ia64InitialUserImageMaximumSegmentCount]{};
    Ia64InitialUserImageStageSummary m_summary{};
    Kernel::Memory::PhysicalPageRange m_arenaAllocation{};
    Ia64InitialUserImageOwnerFailure m_failure{};
    Ia64InitialUserImageOwnerState m_state{Ia64InitialUserImageOwnerState::Fresh};
};

static_assert(sizeof(Ia64InitialUserImageOwnerState) == 1U);
static_assert(sizeof(Ia64InitialUserImageOwnerError) == 4U);
static_assert(sizeof(Ia64InitialUserImageOwnerAccessError) == 4U);
static_assert(sizeof(Ia64InitialUserImageOwnerFailure) == 32U);
static_assert(alignof(Ia64InitialUserImageOwnerFailure) == 8U);
static_assert(sizeof(Ia64InitialUserImageSegmentPlan) == 112U);
static_assert(alignof(Ia64InitialUserImageSegmentPlan) == 8U);
static_assert(sizeof(Ia64InitialUserImageStageSummary) == 152U);
static_assert(alignof(Ia64InitialUserImageStageSummary) == 8U);
static_assert(alignof(Ia64InitialUserImageOwner) == 16U);
} // namespace Asteria::Platform::Ia64::Executable

#endif // ASTERIA_PLATFORM_IA64_EXECUTABLE_IA64_INITIAL_USER_IMAGE_OWNER_HPP_INCLUDED
