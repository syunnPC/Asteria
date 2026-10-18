// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64RuntimeProcessorCapabilities.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt8 ValidProcessorMask{
    static_cast<Foundation::UInt8>((Foundation::UInt8{1U} << Kernel::Scheduler::MaximumProcessorCount) - 1U)};
constexpr Foundation::UInt8 MinimumPhysicalAddressBits{32U};
constexpr Foundation::UInt8 MaximumPhysicalAddressBits{63U};
constexpr Foundation::UInt8 MinimumVirtualAddressMostSignificantBit{50U};
constexpr Foundation::UInt8 MaximumVirtualAddressMostSignificantBit{60U};
constexpr Foundation::UInt8 MinimumRegionIdentifierBits{18U};
constexpr Foundation::UInt8 MaximumRegionIdentifierBits{24U};
constexpr Foundation::UInt64 FourKibibytePageSizeBit{Foundation::UInt64{1U} << 12U};
constexpr Foundation::UInt64 BelowFourKibibytesMask{FourKibibytePageSizeBit - 1U};
constexpr Foundation::UInt8 WriteBackMemoryAttributeBit{Foundation::UInt8{1U} << 0U};
constexpr Foundation::UInt8 UncachedMemoryAttributeBit{Foundation::UInt8{1U} << 4U};
constexpr Foundation::UInt8 RequiredMemoryAttributeMask{WriteBackMemoryAttributeBit | UncachedMemoryAttributeBit};
constexpr Foundation::UInt8 ArchitectedMemoryAttributeMask{RequiredMemoryAttributeMask | (Foundation::UInt8{1U} << 5U) |
                                                           (Foundation::UInt8{1U} << 6U) |
                                                           (Foundation::UInt8{1U} << 7U)};

[[nodiscard]] constexpr Foundation::UInt8 ProcessorBit(const Foundation::UInt8 processor) noexcept
{
    return static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << processor);
}

[[nodiscard]] constexpr Foundation::UInt8 Minimum(const Foundation::UInt8 first,
                                                  const Foundation::UInt8 second) noexcept
{
    return first < second ? first : second;
}

[[nodiscard]] Ia64RuntimeProcessorCapabilitiesError
ValidateCapabilities(const PalVirtualMemoryCapabilities& capabilities) noexcept
{
    const TranslationCapabilities& translation = capabilities.Translation;
    if ((translation.Reserved16 != 0U) || (translation.Reserved64 != 0U) || (capabilities.Reserved8 != 0U) ||
        (capabilities.Reserved32 != 0U))
    {
        return Ia64RuntimeProcessorCapabilitiesError::InvalidReservedFields;
    }
    if ((translation.PhysicalAddressBits < MinimumPhysicalAddressBits) ||
        (translation.PhysicalAddressBits > MaximumPhysicalAddressBits) ||
        (translation.ImplementedVirtualAddressMostSignificantBit < MinimumVirtualAddressMostSignificantBit) ||
        (translation.ImplementedVirtualAddressMostSignificantBit > MaximumVirtualAddressMostSignificantBit) ||
        (translation.RegionIdentifierBits < MinimumRegionIdentifierBits) ||
        (translation.RegionIdentifierBits > MaximumRegionIdentifierBits))
    {
        return Ia64RuntimeProcessorCapabilitiesError::InvalidAddressCapabilities;
    }
    if ((((translation.InsertablePageSizeMask | capabilities.PurgePageSizeMask) & BelowFourKibibytesMask) != 0U) ||
        ((translation.InsertablePageSizeMask & ~capabilities.PurgePageSizeMask) != 0U))
    {
        return Ia64RuntimeProcessorCapabilitiesError::InvalidPageSizeCapabilities;
    }
    if ((translation.MemoryAttributeMask & static_cast<Foundation::UInt8>(~ArchitectedMemoryAttributeMask)) != 0U)
    {
        return Ia64RuntimeProcessorCapabilitiesError::InvalidMemoryAttributeCapabilities;
    }
    return Ia64RuntimeProcessorCapabilitiesError::None;
}

[[nodiscard]] constexpr Ia64CommonRuntimeProcessorCapabilities
FirstCommonCapabilities(const PalVirtualMemoryCapabilities& capabilities) noexcept
{
    return Ia64CommonRuntimeProcessorCapabilities{
        .InsertablePageSizeMask = capabilities.Translation.InsertablePageSizeMask,
        .PurgePageSizeMask = capabilities.PurgePageSizeMask,
        .PhysicalAddressBits = capabilities.Translation.PhysicalAddressBits,
        .ImplementedVirtualAddressMostSignificantBit =
            capabilities.Translation.ImplementedVirtualAddressMostSignificantBit,
        .RegionIdentifierBits = capabilities.Translation.RegionIdentifierBits,
        .MemoryAttributeMask = capabilities.Translation.MemoryAttributeMask,
        .HardwareWalkerImplemented = capabilities.Translation.HardwareWalkerImplemented,
        .Reserved = {},
    };
}

constexpr void IntersectCapabilities(const PalVirtualMemoryCapabilities& capabilities,
                                     Ia64CommonRuntimeProcessorCapabilities& common) noexcept
{
    common.InsertablePageSizeMask &= capabilities.Translation.InsertablePageSizeMask;
    common.PurgePageSizeMask &= capabilities.PurgePageSizeMask;
    common.PhysicalAddressBits = Minimum(common.PhysicalAddressBits, capabilities.Translation.PhysicalAddressBits);
    common.ImplementedVirtualAddressMostSignificantBit =
        Minimum(common.ImplementedVirtualAddressMostSignificantBit,
                capabilities.Translation.ImplementedVirtualAddressMostSignificantBit);
    common.RegionIdentifierBits = Minimum(common.RegionIdentifierBits, capabilities.Translation.RegionIdentifierBits);
    common.MemoryAttributeMask =
        static_cast<Foundation::UInt8>(common.MemoryAttributeMask & capabilities.Translation.MemoryAttributeMask);
    common.HardwareWalkerImplemented =
        common.HardwareWalkerImplemented && capabilities.Translation.HardwareWalkerImplemented;
}

[[nodiscard]] constexpr bool IsCommonProfileCompatible(const Ia64CommonRuntimeProcessorCapabilities& common) noexcept
{
    return ((common.InsertablePageSizeMask & FourKibibytePageSizeBit) != 0U) &&
           ((common.PurgePageSizeMask & FourKibibytePageSizeBit) != 0U) &&
           ((common.MemoryAttributeMask & RequiredMemoryAttributeMask) == RequiredMemoryAttributeMask);
}
} // namespace

Ia64RuntimeProcessorCapabilitiesError Ia64RuntimeProcessorCapabilities::Initialize(
    const Foundation::UInt8 admittedProcessorMask,
    const PalVirtualMemoryCapabilities (&processorCapabilities)[Kernel::Scheduler::MaximumProcessorCount]) noexcept
{
    if (m_initialized)
    {
        return Ia64RuntimeProcessorCapabilitiesError::AlreadyInitialized;
    }
    if ((admittedProcessorMask == 0U) ||
        ((admittedProcessorMask & static_cast<Foundation::UInt8>(~ValidProcessorMask)) != 0U))
    {
        return Ia64RuntimeProcessorCapabilitiesError::InvalidProcessorMask;
    }

    Ia64CommonRuntimeProcessorCapabilities common{};
    bool hasFirstProcessor{false};
    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        if ((admittedProcessorMask & ProcessorBit(processor)) == 0U)
        {
            continue;
        }

        const PalVirtualMemoryCapabilities& capabilities = processorCapabilities[processor];
        const Ia64RuntimeProcessorCapabilitiesError validationError = ValidateCapabilities(capabilities);
        if (validationError != Ia64RuntimeProcessorCapabilitiesError::None)
        {
            return validationError;
        }
        if (!hasFirstProcessor)
        {
            common = FirstCommonCapabilities(capabilities);
            hasFirstProcessor = true;
        }
        else
        {
            IntersectCapabilities(capabilities, common);
        }
    }

    if (!IsCommonProfileCompatible(common))
    {
        return Ia64RuntimeProcessorCapabilitiesError::IncompatibleCommonCapabilities;
    }

    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        if ((admittedProcessorMask & ProcessorBit(processor)) != 0U)
        {
            m_processorCapabilities[processor] = processorCapabilities[processor];
        }
    }
    m_commonCapabilities = common;
    m_admittedProcessorMask = admittedProcessorMask;
    m_initialized = true;
    return Ia64RuntimeProcessorCapabilitiesError::None;
}

Foundation::Result<Foundation::UInt8, Ia64RuntimeProcessorCapabilitiesError>
Ia64RuntimeProcessorCapabilities::GetAdmittedProcessorMask() const noexcept
{
    using ResultType = Foundation::Result<Foundation::UInt8, Ia64RuntimeProcessorCapabilitiesError>;

    if (!m_initialized)
    {
        return ResultType::Failure(Ia64RuntimeProcessorCapabilitiesError::NotInitialized);
    }
    return ResultType::Success(m_admittedProcessorMask);
}

Foundation::Result<PalVirtualMemoryCapabilities, Ia64RuntimeProcessorCapabilitiesError>
Ia64RuntimeProcessorCapabilities::GetProcessorCapabilities(const Kernel::Scheduler::CpuId processor) const noexcept
{
    using ResultType = Foundation::Result<PalVirtualMemoryCapabilities, Ia64RuntimeProcessorCapabilitiesError>;

    if (!m_initialized)
    {
        return ResultType::Failure(Ia64RuntimeProcessorCapabilitiesError::NotInitialized);
    }
    if (!processor.IsValid())
    {
        return ResultType::Failure(Ia64RuntimeProcessorCapabilitiesError::InvalidProcessor);
    }
    const Foundation::UInt8 processorValue = processor.GetValue();
    if ((m_admittedProcessorMask & ProcessorBit(processorValue)) == 0U)
    {
        return ResultType::Failure(Ia64RuntimeProcessorCapabilitiesError::ProcessorNotAdmitted);
    }
    return ResultType::Success(m_processorCapabilities[processorValue]);
}

Foundation::Result<Ia64CommonRuntimeProcessorCapabilities, Ia64RuntimeProcessorCapabilitiesError>
Ia64RuntimeProcessorCapabilities::GetCommonCapabilities() const noexcept
{
    using ResultType =
        Foundation::Result<Ia64CommonRuntimeProcessorCapabilities, Ia64RuntimeProcessorCapabilitiesError>;

    if (!m_initialized)
    {
        return ResultType::Failure(Ia64RuntimeProcessorCapabilitiesError::NotInitialized);
    }
    return ResultType::Success(m_commonCapabilities);
}
} // namespace Asteria::Platform::Ia64::Memory
