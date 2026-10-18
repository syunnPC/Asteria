// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_MEMORY_INITIAL_PAGING_CAPABILITY_CAPTURE_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_MEMORY_INITIAL_PAGING_CAPABILITY_CAPTURE_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Platform/Amd64/Memory/InitialPageTableBuilder.hpp"

namespace Asteria::Platform::Amd64::Memory
{
enum class InitialPagingCapabilityCaptureError : Foundation::UInt32
{
    None,
    InvalidReport,
    RequiredBasicLeafUnavailable,
    RequiredExtendedLeafUnavailable,
    ModelSpecificRegistersUnavailable,
    PageAttributeTableUnavailable,
    GlobalPagesUnavailable,
    LongModeUnavailable,
    ExecuteDisableUnavailable,
    InvalidPhysicalAddressWidth,
};

struct InitialPagingCapabilityReport
{
    Foundation::UInt32 MaximumBasicLeaf;
    Foundation::UInt32 BasicFeaturesEdx;
    Foundation::UInt32 MaximumExtendedLeaf;
    Foundation::UInt32 ExtendedFeaturesEdx;
    Foundation::UInt32 AddressSizesEax;
    Foundation::UInt32 Reserved;
};

/// Captures the processor features required by the fixed initial AMD64 paging
/// policy. The published booleans describe state that the later activation
/// boundary must establish before loading the generated root; Capture itself
/// executes no privileged state transition. Output remains unchanged on error.
class InitialPagingCapabilityCapture final
{
public:
    [[nodiscard]] static InitialPagingCapabilityCaptureError Decode(const InitialPagingCapabilityReport& report,
                                                                    PagingCapabilities& output) noexcept;

    [[nodiscard]] static InitialPagingCapabilityCaptureError Capture(PagingCapabilities& output) noexcept;

private:
    InitialPagingCapabilityCapture() = delete;
};

using InitialPagingCapabilityDecodeFunction =
    InitialPagingCapabilityCaptureError (*)(const InitialPagingCapabilityReport&, PagingCapabilities&) noexcept;
using InitialPagingCapabilityCaptureFunction = InitialPagingCapabilityCaptureError (*)(PagingCapabilities&) noexcept;

static_assert(__is_same(decltype(&InitialPagingCapabilityCapture::Decode), InitialPagingCapabilityDecodeFunction));
static_assert(__is_same(decltype(&InitialPagingCapabilityCapture::Capture), InitialPagingCapabilityCaptureFunction));
static_assert(sizeof(InitialPagingCapabilityCaptureError) == 4U);
static_assert(sizeof(InitialPagingCapabilityReport) == 24U);
static_assert(alignof(InitialPagingCapabilityReport) == 4U);
static_assert(__is_aggregate(InitialPagingCapabilityReport));
static_assert(__is_standard_layout(InitialPagingCapabilityReport));
static_assert(__is_trivially_copyable(InitialPagingCapabilityReport));
static_assert(__builtin_offsetof(InitialPagingCapabilityReport, MaximumBasicLeaf) == 0U);
static_assert(__builtin_offsetof(InitialPagingCapabilityReport, BasicFeaturesEdx) == 4U);
static_assert(__builtin_offsetof(InitialPagingCapabilityReport, MaximumExtendedLeaf) == 8U);
static_assert(__builtin_offsetof(InitialPagingCapabilityReport, ExtendedFeaturesEdx) == 12U);
static_assert(__builtin_offsetof(InitialPagingCapabilityReport, AddressSizesEax) == 16U);
static_assert(__builtin_offsetof(InitialPagingCapabilityReport, Reserved) == 20U);
static_assert(__builtin_offsetof(InitialPagingCapabilityReport, Reserved) +
                  sizeof(InitialPagingCapabilityReport::Reserved) ==
              sizeof(InitialPagingCapabilityReport));
} // namespace Asteria::Platform::Amd64::Memory

#endif // ASTERIA_PLATFORM_AMD64_MEMORY_INITIAL_PAGING_CAPABILITY_CAPTURE_HPP_INCLUDED
