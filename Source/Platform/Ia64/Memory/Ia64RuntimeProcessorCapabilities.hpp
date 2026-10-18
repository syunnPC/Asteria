// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_PROCESSOR_CAPABILITIES_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_PROCESSOR_CAPABILITIES_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Ia64/Memory/PalVirtualMemoryDecoder.hpp"

namespace Asteria::Platform::Ia64::Memory
{
enum class Ia64RuntimeProcessorCapabilitiesError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    NotInitialized,
    InvalidProcessorMask,
    InvalidAddressCapabilities,
    InvalidPageSizeCapabilities,
    InvalidMemoryAttributeCapabilities,
    InvalidReservedFields,
    IncompatibleCommonCapabilities,
    InvalidProcessor,
    ProcessorNotAdmitted,
};

/// Contains only PAL fields for which one value can safely govern every
/// admitted processor. Masks are intersections and widths are minima.
struct Ia64CommonRuntimeProcessorCapabilities
{
    Foundation::UInt64 InsertablePageSizeMask;
    Foundation::UInt64 PurgePageSizeMask;
    Foundation::UInt8 PhysicalAddressBits;
    Foundation::UInt8 ImplementedVirtualAddressMostSignificantBit;
    Foundation::UInt8 RegionIdentifierBits;
    Foundation::UInt8 MemoryAttributeMask;
    bool HardwareWalkerImplemented;
    Foundation::UInt8 Reserved[3];
};

/// Owns one immutable fixed-capacity virtual-memory capability profile. Initialize consumes
/// already-decoded PAL records. It retains the exact record for each admitted processor and ignores
/// every unadmitted array element. No PAL procedure is invoked here. Thread safety and publication:
/// - The caller serializes the sole successful Initialize call. - The object must not be published
/// or read concurrently until Initialize succeeds and the caller publishes it with the platform's
/// release/acquire mechanism. - After publication, all operations are read-only and require no
/// lock. - Failed initialization leaves the object unpublished and unchanged, so a serialized
/// caller may correct its input and retry. Allocation: - Does not allocate. Every loop is bounded
/// by MaximumProcessorCount.
class Ia64RuntimeProcessorCapabilities final
{
public:
    constexpr Ia64RuntimeProcessorCapabilities() noexcept = default;

    Ia64RuntimeProcessorCapabilities(const Ia64RuntimeProcessorCapabilities&) = delete;
    Ia64RuntimeProcessorCapabilities& operator=(const Ia64RuntimeProcessorCapabilities&) = delete;
    Ia64RuntimeProcessorCapabilities(Ia64RuntimeProcessorCapabilities&&) = delete;
    Ia64RuntimeProcessorCapabilities& operator=(Ia64RuntimeProcessorCapabilities&&) = delete;

    [[nodiscard]] Ia64RuntimeProcessorCapabilitiesError Initialize(
        Foundation::UInt8 admittedProcessorMask,
        const PalVirtualMemoryCapabilities (&processorCapabilities)[Kernel::Scheduler::MaximumProcessorCount]) noexcept;

    [[nodiscard]] Foundation::Result<Foundation::UInt8, Ia64RuntimeProcessorCapabilitiesError>
    GetAdmittedProcessorMask() const noexcept;

    [[nodiscard]] Foundation::Result<PalVirtualMemoryCapabilities, Ia64RuntimeProcessorCapabilitiesError>
    GetProcessorCapabilities(Kernel::Scheduler::CpuId processor) const noexcept;

    [[nodiscard]] Foundation::Result<Ia64CommonRuntimeProcessorCapabilities, Ia64RuntimeProcessorCapabilitiesError>
    GetCommonCapabilities() const noexcept;

private:
    PalVirtualMemoryCapabilities m_processorCapabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    Ia64CommonRuntimeProcessorCapabilities m_commonCapabilities{};
    Foundation::UInt8 m_admittedProcessorMask{0U};
    bool m_initialized{false};
};

static_assert(Kernel::Scheduler::MaximumProcessorCount == 4U);
static_assert(sizeof(Ia64RuntimeProcessorCapabilitiesError) == 1U);
static_assert(sizeof(Ia64CommonRuntimeProcessorCapabilities) == 24U);
static_assert(alignof(Ia64CommonRuntimeProcessorCapabilities) == 8U);
static_assert(sizeof(Ia64RuntimeProcessorCapabilities) == 224U);
static_assert(alignof(Ia64RuntimeProcessorCapabilities) == 8U);
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_PROCESSOR_CAPABILITIES_HPP_INCLUDED
