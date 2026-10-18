// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_BOOT_AMD64_RUNTIME_POOL_LAYOUT_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_BOOT_AMD64_RUNTIME_POOL_LAYOUT_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Abi/ObjectCreationLimits.hpp"

namespace Asteria::Platform::Amd64::Bootstrap
{
/// Layout of the AMD64 runtime pool: the firmware-reserved, identity-mapped
/// LoaderData range below 4 GiB that the boot runtime owns for the lifetime
/// of the boot. Every value is a page index or a page count (4 KiB pages).
/// The boot runtime places its page-table arenas, kernel stacks, diagnostic
/// pages and the staged user image by these names only, and the VM observers
/// read the same names from this header, so the two cannot drift.
inline constexpr Foundation::Size RuntimePoolPages{3529U};
inline constexpr Foundation::UInt64 RuntimePoolPageBytes{4096U};

/// Page-table arenas: root `n` owns pages [n * RootArenaPages, (n + 1) *
/// RootArenaPages). Root 0 is the shared kernel root every processor runs
/// on, root 1 the private root of the initial user image, root 2 the private
/// root of the boot pager, root 3 the private root of the test firmware's
/// spinner. Two large arenas of LargeRootArenaPages pages are reserved at the
/// pool's end (LargeRootArenaFirstPage).
inline constexpr Foundation::Size RootArenaPages{64U};
inline constexpr Foundation::Size SharedRootIndex{0U};
inline constexpr Foundation::Size UserRootIndex{1U};
inline constexpr Foundation::Size PagerRootIndex{2U};
inline constexpr Foundation::Size SpinnerRootIndex{3U};
inline constexpr Foundation::Size RootArenaCount{4U};

/// Per-processor kernel stack extents: processor `p` owns pages
/// [ProcessorStackFirstPage + p * ProcessorStackExtentPages,... +
/// ProcessorStackExtentPages). Inside an extent, five stacks start at the
/// page offsets below; every other page of the extent stays unmapped as a
/// guard. The idle, worker and system-call stacks have KernelStackPages
/// pages, the double-fault and machine-check stacks InterruptStackPages.
inline constexpr Foundation::Size ProcessorStackFirstPage{256U};
inline constexpr Foundation::Size ProcessorStackExtentPages{24U};
inline constexpr Foundation::Size ProcessorStackSlots{4U};
inline constexpr Foundation::Size IdleStackPage{1U};
inline constexpr Foundation::Size WorkerStackPage{7U};
inline constexpr Foundation::Size SystemCallStackPage{13U};
inline constexpr Foundation::Size DoubleFaultStackPage{19U};
inline constexpr Foundation::Size MachineCheckStackPage{22U};
inline constexpr Foundation::Size KernelStackPages{4U};
inline constexpr Foundation::Size InterruptStackPages{2U};

/// Diagnostic and device pages: the shared-root shootdown probe page and the
/// three permanent AHCI DMA pages (IDENTIFY data, READ DMA EXT command, READ
/// DMA EXT data).
inline constexpr Foundation::Size SharedProbePage{352U};
inline constexpr Foundation::Size AhciIdentifyPage{353U};
inline constexpr Foundation::Size AhciCommandPage{354U};
inline constexpr Foundation::Size AhciReadDataPage{355U};
inline constexpr Foundation::Size AhciPages{3U};

/// The kernel stacks of the boot pager and of the test firmware's spinner
/// (KernelStackPages pages each, between unmapped guard pages): each thread's
/// interrupt and system-call stack.
inline constexpr Foundation::Size PagerKernelStackPage{357U};
inline constexpr Foundation::Size SpinnerKernelStackPage{362U};

/// The Frame of the lazily backed client region the boot pager serves. These
/// pages are no supervisor mapping; only the pager's resolutions map them,
/// into the initial image's root.
inline constexpr Foundation::Size LazyRegionFirstPage{367U};
inline constexpr Foundation::Size LazyRegionPages{2U};

/// The frame-pool scrub slots: processor p's slot page is ScrubSlotFirstPage + p, seed-mapped
/// supervisor RW/NX at ScrubSlotVirtualAddress + p pages in every root. The scrubber repoints the
/// active root's leaf at a dirty page while it zeroes it and then restores it.
inline constexpr Foundation::Size ScrubSlotFirstPage{369U};
inline constexpr Foundation::UInt64 ScrubSlotVirtualAddress{0x7000'1000U};

/// The staged user PT_LOAD pages and the initial user stack pages of the
/// initial image, of the boot pager and of the spinner
/// (InitialUserStoragePages each).
inline constexpr Foundation::Size UserStagingFirstPage{384U};
inline constexpr Foundation::Size PagerStagingFirstPage{512U};
inline constexpr Foundation::Size SpinnerStagingFirstPage{640U};
inline constexpr Foundation::Size UserStagingPages{128U};

/// Page-table arenas reserved for AddressSpaces created at run time: identity-mapped as page tables
/// in every root like the boot arenas, one per AddressSpace pool slot.
inline constexpr Foundation::Size ReservedArenaFirstPage{768U};
inline constexpr Foundation::Size ReservedArenaCount{5U};

/// The kernel stacks of the threads user code creates: ThreadStackSlots stacks of KernelStackPages
/// pages, the first at ThreadStackFirstPage and each next one ThreadStackStridePages further, with
/// an unmapped guard page before, between and after them. Each is identity seed-mapped supervisor
/// RW/NX in every root, so a created thread's traps enter on its own stack whichever root is
/// active. They follow the page-table arenas.
inline constexpr Foundation::Size ThreadStackSlots{5U};
inline constexpr Foundation::Size ThreadStackFirstPage{1089U};
inline constexpr Foundation::Size ThreadStackStridePages{KernelStackPages + 1U};

/// Zero-page compatibility pool; remapping storage begins at FramePoolFirstPage.
inline constexpr Foundation::Size FramePoolFirstPage{1120U};
inline constexpr Foundation::Size FramePoolPages{0U};

/// The VT-d tables, identity-mapped writable in every root after the frame pool: the root table and
/// four context tables per unit, four user domains per unit with eight pages each, and eight
/// persistent firmware domains (64 pages each). Unit zero keeps the original root/context/user
/// table offsets.
inline constexpr Foundation::Size IommuRootTablePage{1120U};
inline constexpr Foundation::Size IommuContextTableFirstPage{1121U};
inline constexpr Foundation::Size IommuContextTablePages{4U};
inline constexpr Foundation::Size IommuDomainTableFirstPage{1125U};
inline constexpr Foundation::Size IommuDomainSlots{4U};
inline constexpr Foundation::Size IommuDomainTablePages{8U};
static_assert(IommuRootTablePage == (FramePoolFirstPage + FramePoolPages));
static_assert(IommuContextTableFirstPage == (IommuRootTablePage + 1U));
static_assert(IommuDomainTableFirstPage == (IommuContextTableFirstPage + IommuContextTablePages));
inline constexpr Foundation::Size IommuUnits{4U};
inline constexpr Foundation::Size IommuUnitPages{37U};
inline constexpr Foundation::Size IommuReservedDomains{8U};
inline constexpr Foundation::Size IommuReservedTablePages{64U};
inline constexpr Foundation::Size IommuReservedTableFirstPage{IommuRootTablePage + (IommuUnits * IommuUnitPages)};
static_assert(IommuUnitPages == 1U + IommuContextTablePages + (IommuDomainSlots * IommuDomainTablePages));

inline constexpr Foundation::Size StopTableFirstPage{1780U};
inline constexpr Foundation::Size StopTablePages{514U};
static_assert(StopTableFirstPage == IommuReservedTableFirstPage + (IommuReservedDomains * IommuReservedTablePages));
/// Dedicated NMI stacks: two mapped pages each, separated by unmapped guards.
inline constexpr Foundation::Size NmiStackFirstPage{2296U};
inline constexpr Foundation::Size NmiStackStridePages{3U};
[[nodiscard]] constexpr Foundation::Size NmiStackPage(const Foundation::Size processor) noexcept
{
    return NmiStackFirstPage + processor * NmiStackStridePages;
}
static_assert(StopTableFirstPage + StopTablePages < NmiStackFirstPage);
/// AMD-Vi storage appended without moving any existing stack/table offsets.
/// Reserve the full 64K-RID table; DevTblSize covers the captured requesters and every IVRS range/alias.
inline constexpr Foundation::Size AmdViDeviceFirstPage{2308U};
inline constexpr Foundation::Size AmdViCommandFirstPage{2820U};
inline constexpr Foundation::Size AmdViEventFirstPage{2822U};
inline constexpr Foundation::Size AmdViCompletionPage{2824U};
static_assert(AmdViDeviceFirstPage == NmiStackFirstPage + ProcessorStackSlots * NmiStackStridePages);
static_assert(AmdViCommandFirstPage == AmdViDeviceFirstPage + 512U);
/// Two large page-table arenas after the AMD-Vi storage,
/// for roots whose mappings need more table pages than a RootArenaPages arena holds: the runtime
/// mapper emits only 4 KiB leaves and never frees intermediate tables, so the session compositor's
/// root needs about 80 at 2560x1440. ObjectCreate selects one for each
/// flagged AddressSpace; the seed maps the reservation supervisor RW/NX in every root.
inline constexpr Foundation::Size LargeRootArenaFirstPage{2825U};
inline constexpr Foundation::Size LargeRootArenaPages{256U};
inline constexpr Foundation::Size LargeRootArenaCount{Abi::LargePageTableArenaMaximumCount};
static_assert(LargeRootArenaFirstPage == AmdViCompletionPage + 1U);
static_assert(LargeRootArenaPages == 4U * RootArenaPages);
// Three AP idle arenas after the large page-table arenas.
inline constexpr Foundation::Size ApIdleRootFirstPage{LargeRootArenaFirstPage +
                                                      LargeRootArenaCount * LargeRootArenaPages};
inline constexpr Foundation::Size ApIdleRootCount{ProcessorStackSlots - 1U};
inline constexpr Foundation::Size BootRootCount{RootArenaCount + ApIdleRootCount};
[[nodiscard]] constexpr Foundation::Size IdleRootIndex(const Foundation::Size processor) noexcept
{
    return processor == 0U ? SharedRootIndex : RootArenaCount + processor - 1U;
}
[[nodiscard]] constexpr Foundation::Size BootRootArenaPage(const Foundation::Size root) noexcept
{
    return root < RootArenaCount ? root * RootArenaPages
                                 : ApIdleRootFirstPage + (root - RootArenaCount) * RootArenaPages;
}
static_assert(ApIdleRootFirstPage == 3337U);
static_assert(RuntimePoolPages == ApIdleRootFirstPage + ApIdleRootCount * RootArenaPages);

/// The metadata arena: 2 MiB-aligned LoaderData that the loader allocates below the fixed user
/// ranges (Abi::Amd64FixedUserRangesStart), of the pages KernelQuery::MetadataArenaPages answers,
/// and that every root identity-maps supervisor RW/NX. Every value is a page index of the arena. It
/// holds, in this order: the page-table arenas of the AddressSpace pool slots beyond
/// ReservedArenaCount; after a guard page, the kernel stacks of the thread slots beyond
/// ThreadStackSlots, ThreadStackStridePages apart, each followed by an unmapped guard page; the
/// slabs from MetadataArenaSlabFirstPage (Amd64MetadataArena.hpp); and the allocator bitmaps to its
/// end.
inline constexpr Foundation::Size MetadataArenaAlignmentPages{512U};
inline constexpr Foundation::Size MetadataArenaRootArenaFirstPage{0U};
inline constexpr Foundation::Size MetadataArenaRootArenaCount{11U};
inline constexpr Foundation::Size MetadataArenaThreadStackFirstPage{705U};
inline constexpr Foundation::Size MetadataArenaThreadStackSlots{27U};
inline constexpr Foundation::Size MetadataArenaSlabFirstPage{840U};
/// The page-table pages of each root's arena that the arena's 4 KiB seed
/// leaves may take: the arena lies below 1 GiB, whose page directory the
/// startup pages already need, so each 2 MiB of it takes one page table and
/// the arena is capped at this many 2 MiB blocks. What the seed's other
/// mappings and user mappings leave of each root's arena covers it:
/// Amd64MetadataArenaTests proves that the arena's leaves take one table page
/// per 2 MiB beyond the seed's others wherever the loader puts it, and the
/// runner checks each boot's seed table count (the AMD64 evidence's
/// SeedTablePages) against the 64-page arena with the arena at the cap.
inline constexpr Foundation::Size MetadataArenaTablePages{8U};
inline constexpr Foundation::Size MetadataArenaMaximumPages{MetadataArenaTablePages * MetadataArenaAlignmentPages};
static_assert(MetadataArenaRootArenaFirstPage + (MetadataArenaRootArenaCount * RootArenaPages) + 1U ==
              MetadataArenaThreadStackFirstPage);
static_assert(MetadataArenaThreadStackFirstPage + (MetadataArenaThreadStackSlots * ThreadStackStridePages) ==
              MetadataArenaSlabFirstPage);

/// The first page of created-thread stack slot `slot`: the runtime pool's
/// below ThreadStackSlots, the metadata arena's (an arena page index, from
/// MetadataArenaThreadStackPage) above.
[[nodiscard]] constexpr Foundation::Size ThreadStackPage(const Foundation::Size slot) noexcept
{
    return ThreadStackFirstPage + (slot * ThreadStackStridePages);
}

/// The arena page of the kernel stack of created-thread slot `slot`, at or
/// above ThreadStackSlots.
[[nodiscard]] constexpr Foundation::Size MetadataArenaThreadStackPage(const Foundation::Size slot) noexcept
{
    return MetadataArenaThreadStackFirstPage + ((slot - ThreadStackSlots) * ThreadStackStridePages);
}

/// The arena page of the page-table arena of AddressSpace slot `slot`, at or
/// above ReservedArenaCount.
[[nodiscard]] constexpr Foundation::Size MetadataArenaRootArenaPage(const Foundation::Size slot) noexcept
{
    return MetadataArenaRootArenaFirstPage + ((slot - ReservedArenaCount) * RootArenaPages);
}

/// The first page of processor `processor`'s stack extent.
[[nodiscard]] constexpr Foundation::Size ProcessorStackExtentPage(const Foundation::Size processor) noexcept
{
    return ProcessorStackFirstPage + (processor * ProcessorStackExtentPages);
}

/// The physical address of pool page `page`. The pool lies below 4 GiB and
/// `page` is below RuntimePoolPages, so the sum cannot overflow.
[[nodiscard]] constexpr Foundation::UInt64 RuntimePoolPageAddress(const Foundation::UInt64 poolPhysicalStart,
                                                                  const Foundation::Size page) noexcept
{
    return poolPhysicalStart + (static_cast<Foundation::UInt64>(page) * RuntimePoolPageBytes);
}

static_assert(RootArenaCount * RootArenaPages <= ProcessorStackFirstPage);
static_assert(IdleStackPage + KernelStackPages < WorkerStackPage);
static_assert(WorkerStackPage + KernelStackPages < SystemCallStackPage);
static_assert(SystemCallStackPage + KernelStackPages < DoubleFaultStackPage);
static_assert(DoubleFaultStackPage + InterruptStackPages < MachineCheckStackPage);
static_assert(MachineCheckStackPage + InterruptStackPages == ProcessorStackExtentPages);
static_assert(ProcessorStackExtentPage(ProcessorStackSlots) <= SharedProbePage);
static_assert((SharedProbePage < AhciIdentifyPage) && (AhciIdentifyPage + 1U == AhciCommandPage) &&
              (AhciCommandPage + 1U == AhciReadDataPage) && (AhciIdentifyPage + AhciPages == AhciReadDataPage + 1U));
static_assert(AhciReadDataPage + 1U < PagerKernelStackPage);
static_assert(PagerKernelStackPage + KernelStackPages < SpinnerKernelStackPage);
static_assert(SpinnerKernelStackPage + KernelStackPages < LazyRegionFirstPage);
static_assert(LazyRegionFirstPage + LazyRegionPages <= ScrubSlotFirstPage);
static_assert(ScrubSlotFirstPage + ProcessorStackSlots < UserStagingFirstPage);
static_assert(UserStagingFirstPage + UserStagingPages == PagerStagingFirstPage);
static_assert(PagerStagingFirstPage + UserStagingPages == SpinnerStagingFirstPage);
static_assert(SpinnerStagingFirstPage + UserStagingPages == ReservedArenaFirstPage);
// One unmapped guard page before the thread stacks and at least one after.
static_assert(ReservedArenaFirstPage + ReservedArenaCount * RootArenaPages + 1U == ThreadStackFirstPage);
static_assert(ThreadStackPage(ThreadStackSlots - 1U) + KernelStackPages < FramePoolFirstPage);
static_assert(FramePoolFirstPage + FramePoolPages == IommuRootTablePage);
} // Namespace Asteria::Platform::Amd64::Bootstrap

#endif // ASTERIA_PLATFORM_AMD64_BOOT_AMD64_RUNTIME_POOL_LAYOUT_HPP_INCLUDED
