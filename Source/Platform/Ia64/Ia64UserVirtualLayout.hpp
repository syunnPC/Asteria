// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_IA64_USER_VIRTUAL_LAYOUT_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_IA64_USER_VIRTUAL_LAYOUT_HPP_INCLUDED

#include "Abi/PagerBootstrap.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Platform::Ia64
{
inline constexpr Foundation::UInt64 Ia64UserPageSizeBytes{4096U};
inline constexpr Foundation::UInt8 Ia64InitialUserVirtualRegion{1U};
inline constexpr Foundation::UInt64 Ia64UserGuardPageCount{1U};

inline constexpr Foundation::UInt64 Ia64UserExecutionProbeCodePageCount{1U};
inline constexpr Foundation::UInt64 Ia64UserExecutionProbeMemoryStackPageCount{4U};
inline constexpr Foundation::UInt64 Ia64UserExecutionProbeRseBackingStorePageCount{4U};
inline constexpr Foundation::UInt64 Ia64UserExecutionProbeCodeSizeBytes{Ia64UserExecutionProbeCodePageCount *
                                                                        Ia64UserPageSizeBytes};
inline constexpr Foundation::UInt64 Ia64UserExecutionProbeMemoryStackSizeBytes{
    Ia64UserExecutionProbeMemoryStackPageCount * Ia64UserPageSizeBytes};
inline constexpr Foundation::UInt64 Ia64UserExecutionProbeRseBackingStoreSizeBytes{
    Ia64UserExecutionProbeRseBackingStorePageCount * Ia64UserPageSizeBytes};

inline constexpr Foundation::UInt64 Ia64UserExecutionProbeCodeVirtualStart{0x2000'0000'0100'1000ULL};
inline constexpr Foundation::UInt64 Ia64UserExecutionProbeMemoryStackVirtualStart{0x2000'0000'0200'1000ULL};
inline constexpr Foundation::UInt64 Ia64UserExecutionProbeRseBackingStoreVirtualStart{0x2000'0000'0300'1000ULL};

inline constexpr Foundation::UInt64 Ia64UserExecutionProbeCodeGuardedVirtualStart{
    Ia64UserExecutionProbeCodeVirtualStart - (Ia64UserGuardPageCount * Ia64UserPageSizeBytes)};
inline constexpr Foundation::UInt64 Ia64UserExecutionProbeCodeGuardedVirtualSizeBytes{
    Ia64UserExecutionProbeCodeSizeBytes + (2U * Ia64UserGuardPageCount * Ia64UserPageSizeBytes)};
inline constexpr Foundation::UInt64 Ia64UserExecutionProbeMemoryStackGuardedVirtualStart{
    Ia64UserExecutionProbeMemoryStackVirtualStart - (Ia64UserGuardPageCount * Ia64UserPageSizeBytes)};
inline constexpr Foundation::UInt64 Ia64UserExecutionProbeMemoryStackGuardedVirtualSizeBytes{
    Ia64UserExecutionProbeMemoryStackSizeBytes + (2U * Ia64UserGuardPageCount * Ia64UserPageSizeBytes)};
inline constexpr Foundation::UInt64 Ia64UserExecutionProbeRseBackingStoreGuardedVirtualStart{
    Ia64UserExecutionProbeRseBackingStoreVirtualStart - (Ia64UserGuardPageCount * Ia64UserPageSizeBytes)};
inline constexpr Foundation::UInt64 Ia64UserExecutionProbeRseBackingStoreGuardedVirtualSizeBytes{
    Ia64UserExecutionProbeRseBackingStoreSizeBytes + (2U * Ia64UserGuardPageCount * Ia64UserPageSizeBytes)};

inline constexpr Foundation::UInt64 Ia64InitialUserMemoryStackPageCount{4U};
inline constexpr Foundation::UInt64 Ia64InitialUserRseBackingStorePageCount{4U};
inline constexpr Foundation::UInt64 Ia64InitialUserMemoryStackSizeBytes{Ia64InitialUserMemoryStackPageCount *
                                                                        Ia64UserPageSizeBytes};
inline constexpr Foundation::UInt64 Ia64InitialUserRseBackingStoreSizeBytes{Ia64InitialUserRseBackingStorePageCount *
                                                                            Ia64UserPageSizeBytes};
inline constexpr Foundation::UInt64 Ia64InitialUserMemoryStackVirtualStart{0x2000'0000'0500'1000ULL};
inline constexpr Foundation::UInt64 Ia64InitialUserRseBackingStoreVirtualStart{0x2000'0000'0600'1000ULL};
inline constexpr Foundation::UInt64 Ia64InitialUserMemoryStackGuardedVirtualStart{
    Ia64InitialUserMemoryStackVirtualStart - (Ia64UserGuardPageCount * Ia64UserPageSizeBytes)};
inline constexpr Foundation::UInt64 Ia64InitialUserMemoryStackGuardedVirtualSizeBytes{
    Ia64InitialUserMemoryStackSizeBytes + (2U * Ia64UserGuardPageCount * Ia64UserPageSizeBytes)};
inline constexpr Foundation::UInt64 Ia64InitialUserRseBackingStoreGuardedVirtualStart{
    Ia64InitialUserRseBackingStoreVirtualStart - (Ia64UserGuardPageCount * Ia64UserPageSizeBytes)};
inline constexpr Foundation::UInt64 Ia64InitialUserRseBackingStoreGuardedVirtualSizeBytes{
    Ia64InitialUserRseBackingStoreSizeBytes + (2U * Ia64UserGuardPageCount * Ia64UserPageSizeBytes)};

/// The initial image's lazily backed region: nothing maps it until the
/// Image touches a page and the boot pager resolves the fault. Each page starts with Abi:
/// LazyPagePattern and its page index. It lies between the image window and the memory stack's
/// guard.
inline constexpr Foundation::UInt64 Ia64InitialUserImageLimit{0x2000'0000'0480'0000ULL};
inline constexpr Foundation::UInt64 Ia64InitialUserLazyVirtualStart{0x2000'0000'0480'0000ULL};
inline constexpr Foundation::UInt64 Ia64InitialUserLazyPageCount{2U};

static_assert((Ia64InitialUserLazyVirtualStart >> 61U) == Ia64InitialUserVirtualRegion);
static_assert(Ia64InitialUserLazyVirtualStart + (Ia64InitialUserLazyPageCount * Ia64UserPageSizeBytes) <=
              Ia64InitialUserMemoryStackGuardedVirtualStart);
/// The window of the boot pager's own space that nothing maps at boot, where it maps the memory it
/// creates, as on AMD64: past the lazy region and before the memory stack's guard.
inline constexpr Foundation::UInt64 Ia64InitialUserMappingWindowStart{0x2000'0000'0490'0000ULL};
/// As many pages as a component segment may have (Ia64ComponentSegmentPages):
/// the root service stages each segment through it.
inline constexpr Foundation::UInt64 Ia64InitialUserMappingWindowPages{128U};
static_assert(Ia64InitialUserMappingWindowStart >=
              Ia64InitialUserLazyVirtualStart + (Ia64InitialUserLazyPageCount * Ia64UserPageSizeBytes));
static_assert(Ia64InitialUserMappingWindowStart + (Ia64InitialUserMappingWindowPages * Ia64UserPageSizeBytes) <=
              Ia64InitialUserMemoryStackGuardedVirtualStart);
static_assert((Ia64UserExecutionProbeCodeVirtualStart >> 61U) == Ia64InitialUserVirtualRegion);
static_assert((Ia64UserExecutionProbeMemoryStackVirtualStart >> 61U) == Ia64InitialUserVirtualRegion);
static_assert((Ia64UserExecutionProbeRseBackingStoreVirtualStart >> 61U) == Ia64InitialUserVirtualRegion);
static_assert((Ia64InitialUserMemoryStackVirtualStart >> 61U) == Ia64InitialUserVirtualRegion);
static_assert((Ia64InitialUserRseBackingStoreVirtualStart >> 61U) == Ia64InitialUserVirtualRegion);
static_assert(Abi::Ia64PagerModuleBase >= Ia64InitialUserImageLimit);
static_assert(Abi::Ia64PagerModuleBase >=
              Ia64InitialUserMemoryStackGuardedVirtualStart + Ia64InitialUserMemoryStackGuardedVirtualSizeBytes);
static_assert(Abi::Ia64PagerModuleBase >= Ia64InitialUserRseBackingStoreGuardedVirtualStart +
                                              Ia64InitialUserRseBackingStoreGuardedVirtualSizeBytes);
static_assert(Abi::Ia64PagerModuleBase >=
              Ia64InitialUserMappingWindowStart + Ia64InitialUserMappingWindowPages * Ia64UserPageSizeBytes);
static_assert(Abi::Ia64PagerModuleBase >=
              Ia64InitialUserLazyVirtualStart + Ia64InitialUserLazyPageCount * Ia64UserPageSizeBytes);
static_assert(Abi::Ia64PagerModuleBase + Abi::PagerModuleMaximumBytes < 0x2000'0000'1000'0000ULL);
} // Namespace Asteria::Platform::Ia64

#endif // ASTERIA_PLATFORM_IA64_IA64_USER_VIRTUAL_LAYOUT_HPP_INCLUDED
