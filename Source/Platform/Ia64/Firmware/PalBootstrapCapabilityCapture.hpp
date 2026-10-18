// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_FIRMWARE_PAL_BOOTSTRAP_CAPABILITY_CAPTURE_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_FIRMWARE_PAL_BOOTSTRAP_CAPABILITY_CAPTURE_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Firmware/PalStaticCallAdapter.hpp"
#include "Platform/Ia64/Memory/PalTranslationCachePurgeDecoder.hpp"
#include "Platform/Ia64/Memory/PalVirtualMemoryDecoder.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
/// The only physical stacked-register count this kernel admits: PAL_RSE_INFO
/// must report exactly this many on every processor. Every IA-64
/// implementation has 96, the architectural maximum frame, so one frame of
/// this size spans the complete physical stacked-register file. The PL3 entry
/// paths rely on that to leave no kernel value in a stacked register
/// (Ia64UserModeLaunch.S, Ia64SystemCallEntry.S).
constexpr Foundation::UInt64 Ia64PhysicalStackedRegisterCount{96U};

/// PAL_RSE_INFO.hints: the eager-load and eager-store bits. The other bits
/// and the fourth result register are reserved and must be zero.
constexpr Foundation::UInt64 PalRseInformationHintMask{0x3U};

enum class PalBootstrapCapabilityCapturePhase : Foundation::UInt32
{
    None,
    VirtualMemorySummaryInvocation,
    VirtualMemoryPageSizeInvocation,
    MemoryAttributeInvocation,
    VirtualMemoryDecoding,
    TranslationCachePurgeInvocation,
    TranslationCachePurgeDecoding,
    RseInformationInvocation,
    /// PAL_RSE_INFO returned a failure status, a physical stacked-register
    /// count other than Ia64PhysicalStackedRegisterCount, or a nonzero
    /// reserved bit.
    RseInformationDecoding,
};

struct PalBootstrapCapabilityCaptureError
{
    PalBootstrapCapabilityCapturePhase Phase;
    PalStaticCallAdapterError AdapterError;
    Memory::PalVirtualMemoryDecodeError VirtualMemoryError;
    Memory::PalTranslationCachePurgeDecodeError TranslationCachePurgeError;
};

/// Owns the validated PAL capabilities needed by the initial IA-64 memory
/// bootstrap. Every field is copied from buffer-free static PAL results.
struct PalBootstrapCapabilities
{
    Memory::PalVirtualMemoryCapabilities VirtualMemory;
    Memory::PalTranslationCachePurgePlan TranslationCachePurge;
};

/// Executes the five allowed physical static-PAL queries in a fixed order and
/// publishes a value only after both independent decoders accept their results
/// and PAL_RSE_INFO reports exactly Ia64PhysicalStackedRegisterCount physical
/// stacked registers with no reserved bit set.
///
/// This call inherits every execution-state, stack, stability, serialization,
/// and physical-handoff precondition of PalStaticCallAdapter. The processor may
/// not change during the sequence, and the returned purge plan belongs only to
/// that processor's local translation cache. It is fail-fast: no later PAL
/// procedure is invoked after an adapter, PAL-status, or decoder failure.
class PalBootstrapCapabilityCapture final
{
public:
    [[nodiscard]] static Foundation::Result<PalBootstrapCapabilities, PalBootstrapCapabilityCaptureError>
    Capture(const SalEntrypointBinding& binding) noexcept;

private:
    PalBootstrapCapabilityCapture() = delete;
};

static_assert(sizeof(PalBootstrapCapabilityCapturePhase) == 4U);
static_assert(sizeof(PalBootstrapCapabilityCaptureError) == 16U);
static_assert(alignof(PalBootstrapCapabilityCaptureError) == 4U);
static_assert(__builtin_offsetof(PalBootstrapCapabilityCaptureError, Phase) == 0U);
static_assert(__builtin_offsetof(PalBootstrapCapabilityCaptureError, AdapterError) == 4U);
static_assert(__builtin_offsetof(PalBootstrapCapabilityCaptureError, VirtualMemoryError) == 8U);
static_assert(__builtin_offsetof(PalBootstrapCapabilityCaptureError, TranslationCachePurgeError) == 12U);
static_assert(sizeof(PalBootstrapCapabilities) == 88U);
static_assert(alignof(PalBootstrapCapabilities) == 8U);
static_assert(__builtin_offsetof(PalBootstrapCapabilities, VirtualMemory) == 0U);
static_assert(__builtin_offsetof(PalBootstrapCapabilities, TranslationCachePurge) == 48U);
static_assert(sizeof(Foundation::Result<PalBootstrapCapabilities, PalBootstrapCapabilityCaptureError>) == 112U);
static_assert(alignof(Foundation::Result<PalBootstrapCapabilities, PalBootstrapCapabilityCaptureError>) == 8U);
} // namespace Asteria::Platform::Ia64::Firmware

#endif // ASTERIA_PLATFORM_IA64_FIRMWARE_PAL_BOOTSTRAP_CAPABILITY_CAPTURE_HPP_INCLUDED
