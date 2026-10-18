// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Interrupt/Ia64LocalSapicIpiController.hpp"

#include "Boot/Acpi/I8042Evidence.hpp"

extern "C" void AsteriaIa64WriteProcessorInterruptBlock(Asteria::Foundation::UInt64 address,
                                                        Asteria::Foundation::UInt64 value) noexcept;

// The target routine is in Ia64LocalSapicIpiController.S.

namespace Asteria::Platform::Ia64::Interrupt
{
namespace
{
constexpr Foundation::UInt32 ProcessorEnabledBit{1U};
constexpr Foundation::UInt32 ProcessorOnlineCapableBit{1U << 1U};
constexpr Foundation::UInt64 ProcessorInterruptBlockSizeBytes{2U * 1024U * 1024U};
constexpr Foundation::UInt64 ProcessorInterruptBlockAlignmentMask{ProcessorInterruptBlockSizeBytes - 1U};
constexpr Foundation::UInt64 InterruptWriteSizeBytes{8U};
constexpr Foundation::UInt64 InterruptWriteAlignmentMask{InterruptWriteSizeBytes - 1U};
constexpr Foundation::UInt64 PhysicalAddressUncachedBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 MaximumPhysicalAddress{PhysicalAddressUncachedBit - 1U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt32 MaximumHardwareIdentity{0xFFU};
constexpr Foundation::UInt64 MinimumExternalInterruptVector{16U};
constexpr Foundation::UInt64 MaximumExternalInterruptVector{255U};
constexpr Foundation::Size NoProcessorIndex{Boot::Acpi::MaximumAcpiProcessors};

[[nodiscard]] constexpr Foundation::UInt8 ProcessorBit(const Foundation::UInt8 processor) noexcept
{
    return static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << processor);
}

[[nodiscard]] constexpr Foundation::UInt8 GetLidHardwareId(const Foundation::UInt64 crLid) noexcept
{
    return static_cast<Foundation::UInt8>((crLid >> 24U) & 0xFFU);
}

[[nodiscard]] constexpr Foundation::UInt8 GetLidHardwareEid(const Foundation::UInt64 crLid) noexcept
{
    return static_cast<Foundation::UInt8>((crLid >> 16U) & 0xFFU);
}

[[nodiscard]] constexpr bool IsPhysicalPibBaseValid(const Foundation::UInt64 baseAddress) noexcept
{
    return (baseAddress != 0U) && ((baseAddress & ProcessorInterruptBlockAlignmentMask) == 0U) &&
           (baseAddress <= (MaximumPhysicalAddress - ProcessorInterruptBlockAlignmentMask));
}

[[nodiscard]] constexpr bool IsAccessPibBaseValid(const Foundation::UInt64 baseAddress) noexcept
{
    return (baseAddress != 0U) && ((baseAddress & ProcessorInterruptBlockAlignmentMask) == 0U) &&
           (baseAddress <= (MaximumUInt64 - ProcessorInterruptBlockAlignmentMask));
}

[[nodiscard]] constexpr bool TryEncodeInterruptAddress(const Foundation::UInt64 baseAddress,
                                                       const Foundation::UInt8 hardwareId,
                                                       const Foundation::UInt8 hardwareEid,
                                                       Foundation::UInt64& output) noexcept
{
    if (!IsAccessPibBaseValid(baseAddress))
    {
        return false;
    }

    const Foundation::UInt64 idOffset = static_cast<Foundation::UInt64>(hardwareId) << 12U;
    const Foundation::UInt64 eidOffset = static_cast<Foundation::UInt64>(hardwareEid) << 4U;
    if ((baseAddress > (MaximumUInt64 - idOffset)) || ((baseAddress + idOffset) > (MaximumUInt64 - eidOffset)))
    {
        return false;
    }

    const Foundation::UInt64 address = baseAddress + idOffset + eidOffset;
    const Foundation::UInt64 blockEnd = baseAddress + ProcessorInterruptBlockAlignmentMask;
    if ((address < baseAddress) || (address > (blockEnd - (InterruptWriteSizeBytes - 1U))) ||
        ((address & InterruptWriteAlignmentMask) != 0U))
    {
        return false;
    }

    output = address;
    return true;
}

[[nodiscard]] constexpr Ia64LocalSapicLogicalProcessor MakeProcessor(const Boot::Acpi::AcpiProcessorTopology& processor,
                                                                     const Foundation::UInt64 interruptAddress) noexcept
{
    return Ia64LocalSapicLogicalProcessor{
        .InterruptAddress = interruptAddress,
        .AcpiUid = processor.AcpiUid,
        .HardwareId = static_cast<Foundation::UInt8>(processor.HardwareId),
        .HardwareEid = static_cast<Foundation::UInt8>(processor.HardwareEid),
        .Reserved = {0U, 0U},
    };
}
} // Namespace

Ia64LocalSapicIpiControllerInitializationError
Ia64LocalSapicIpiController::Initialize(const Boot::Acpi::AcpiTopology& topology,
                                        const Foundation::UInt64 currentBspCrLid,
                                        const Ia64ProcessorInterruptBlockAccess& access) noexcept
{
    if (m_state != Ia64LocalSapicIpiControllerState::Uninitialized)
    {
        return Ia64LocalSapicIpiControllerInitializationError::AlreadyInitialized;
    }
    // The Reserved word carries DSDT-bound PS/2 evidence; any other
    // value is invalid.
    if ((topology.Reserved != 0U) && !Boot::Acpi::I8042Evidence::Matches(topology, topology.Dsdt.PhysicalAddress,
                                                                         topology.Dsdt.Length, topology.FadtReserved))
    {
        return Ia64LocalSapicIpiControllerInitializationError::InvalidTopology;
    }
    if ((topology.ProcessorCount == 0U) ||
        (topology.ProcessorCount > static_cast<Foundation::UInt32>(Boot::Acpi::MaximumAcpiProcessors)))
    {
        return Ia64LocalSapicIpiControllerInitializationError::InvalidProcessorCount;
    }
    if (!IsPhysicalPibBaseValid(topology.LocalControllerPhysicalAddress))
    {
        return Ia64LocalSapicIpiControllerInitializationError::InvalidPhysicalBase;
    }
    if ((access.Kind != Ia64ProcessorInterruptBlockBaseKind::BootstrapPhysical) &&
        (access.Kind != Ia64ProcessorInterruptBlockBaseKind::PinnedUncachedVirtual))
    {
        return Ia64LocalSapicIpiControllerInitializationError::InvalidBaseKind;
    }
    for (const Foundation::UInt8 reserved : access.Reserved)
    {
        if (reserved != 0U)
        {
            return Ia64LocalSapicIpiControllerInitializationError::InvalidAccessBase;
        }
    }
    if (!IsAccessPibBaseValid(access.BaseAddress))
    {
        return Ia64LocalSapicIpiControllerInitializationError::InvalidAccessBase;
    }
    if ((access.Kind == Ia64ProcessorInterruptBlockBaseKind::BootstrapPhysical) &&
        (access.BaseAddress != (topology.LocalControllerPhysicalAddress | PhysicalAddressUncachedBit)))
    {
        return Ia64LocalSapicIpiControllerInitializationError::AccessBaseMismatch;
    }

    Foundation::UInt8 enabledProcessorCount{0U};
    Foundation::Size bspTopologyIndex{NoProcessorIndex};
    const Foundation::UInt8 bspHardwareId = GetLidHardwareId(currentBspCrLid);
    const Foundation::UInt8 bspHardwareEid = GetLidHardwareEid(currentBspCrLid);

    for (Foundation::Size index{0U}; index < static_cast<Foundation::Size>(topology.ProcessorCount); ++index)
    {
        const Boot::Acpi::AcpiProcessorTopology& processor = topology.Processors[index];
        if (processor.Reserved != 0U)
        {
            return Ia64LocalSapicIpiControllerInitializationError::InvalidTopology;
        }
        if (processor.ControllerKind != Boot::Acpi::AcpiProcessorControllerKind::LocalSapic)
        {
            return Ia64LocalSapicIpiControllerInitializationError::UnsupportedProcessorController;
        }
        if ((processor.Flags != ProcessorEnabledBit) && (processor.Flags != ProcessorOnlineCapableBit))
        {
            return Ia64LocalSapicIpiControllerInitializationError::InvalidProcessorFlags;
        }
        if ((processor.HardwareId > MaximumHardwareIdentity) || (processor.HardwareEid > MaximumHardwareIdentity))
        {
            return Ia64LocalSapicIpiControllerInitializationError::InvalidProcessorIdentity;
        }

        for (Foundation::Size previous{0U}; previous < index; ++previous)
        {
            const Boot::Acpi::AcpiProcessorTopology& prior = topology.Processors[previous];
            if ((processor.AcpiUid == prior.AcpiUid) ||
                ((processor.HardwareId == prior.HardwareId) && (processor.HardwareEid == prior.HardwareEid)))
            {
                return Ia64LocalSapicIpiControllerInitializationError::DuplicateProcessorIdentity;
            }
        }

        if (processor.Flags == ProcessorEnabledBit)
        {
            ++enabledProcessorCount;
            if ((processor.HardwareId == bspHardwareId) && (processor.HardwareEid == bspHardwareEid))
            {
                bspTopologyIndex = index;
            }
        }
    }

    if (enabledProcessorCount == 0U)
    {
        return Ia64LocalSapicIpiControllerInitializationError::NoEnabledProcessor;
    }
    if (bspTopologyIndex == NoProcessorIndex)
    {
        return Ia64LocalSapicIpiControllerInitializationError::BspNotEnabled;
    }

    Ia64LocalSapicLogicalProcessor processors[Kernel::Scheduler::MaximumProcessorCount]{};
    Foundation::UInt64 interruptAddress{0U};
    const Boot::Acpi::AcpiProcessorTopology& bspProcessor = topology.Processors[bspTopologyIndex];
    if (!TryEncodeInterruptAddress(access.BaseAddress, static_cast<Foundation::UInt8>(bspProcessor.HardwareId),
                                   static_cast<Foundation::UInt8>(bspProcessor.HardwareEid), interruptAddress))
    {
        return Ia64LocalSapicIpiControllerInitializationError::InvalidInterruptAddress;
    }
    processors[0U] = MakeProcessor(bspProcessor, interruptAddress);

    Foundation::UInt8 logicalProcessor{1U};
    for (Foundation::Size index{0U}; index < static_cast<Foundation::Size>(topology.ProcessorCount); ++index)
    {
        const Boot::Acpi::AcpiProcessorTopology& processor = topology.Processors[index];
        if ((index == bspTopologyIndex) || (processor.Flags != ProcessorEnabledBit))
        {
            continue;
        }
        if (!TryEncodeInterruptAddress(access.BaseAddress, static_cast<Foundation::UInt8>(processor.HardwareId),
                                       static_cast<Foundation::UInt8>(processor.HardwareEid), interruptAddress))
        {
            return Ia64LocalSapicIpiControllerInitializationError::InvalidInterruptAddress;
        }
        processors[logicalProcessor] = MakeProcessor(processor, interruptAddress);
        ++logicalProcessor;
    }

    if (logicalProcessor != enabledProcessorCount)
    {
        return Ia64LocalSapicIpiControllerInitializationError::InvalidTopology;
    }

    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        m_processors[processor] = processors[processor];
    }
    m_physicalBaseAddress = topology.LocalControllerPhysicalAddress;
    m_accessBaseAddress = access.BaseAddress;
    m_processorCount = enabledProcessorCount;
    m_enabledProcessorMask = static_cast<Foundation::UInt8>(ProcessorBit(enabledProcessorCount) - 1U);
    m_baseKind = access.Kind;
    m_state = Ia64LocalSapicIpiControllerState::Ready;
    return Ia64LocalSapicIpiControllerInitializationError::None;
}

Ia64LocalSapicIpiSendError
Ia64LocalSapicIpiController::SendExternalInterrupt(const Foundation::UInt64 vector,
                                                   const Foundation::UInt8 targetProcessorMask) const noexcept
{
    if (m_state != Ia64LocalSapicIpiControllerState::Ready)
    {
        return Ia64LocalSapicIpiSendError::NotReady;
    }
    if ((vector < MinimumExternalInterruptVector) || (vector > MaximumExternalInterruptVector))
    {
        return Ia64LocalSapicIpiSendError::InvalidVector;
    }
    if ((targetProcessorMask == 0U) ||
        ((targetProcessorMask & static_cast<Foundation::UInt8>(~m_enabledProcessorMask)) != 0U))
    {
        return Ia64LocalSapicIpiSendError::InvalidTargetMask;
    }

    for (Foundation::UInt8 processor{0U}; processor < m_processorCount; ++processor)
    {
        if ((targetProcessorMask & ProcessorBit(processor)) == 0U)
        {
            continue;
        }
        AsteriaIa64WriteProcessorInterruptBlock(m_processors[processor].InterruptAddress, vector);
    }
    return Ia64LocalSapicIpiSendError::None;
}
} // Namespace Asteria::Platform::Ia64::Interrupt
