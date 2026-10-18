// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Interrupt/Ia64SalBootRendezvousHandler.hpp"

#include "Platform/Ia64/Interrupt/Ia64ApplicationProcessorTakeoverCoordinator.hpp"

extern "C" Asteria::Foundation::UInt64 AsteriaIa64ReadCurrentCrLid() noexcept;

extern "C" Asteria::Foundation::UInt64 AsteriaIa64GetSalBootRendezvousEntryAddress() noexcept;

extern "C" Asteria::Foundation::UInt64 AsteriaIa64GetSalBootRendezvousGlobalPointerAddress() noexcept;

extern "C" Asteria::Platform::Ia64::Interrupt::Ia64SalBootRendezvousAction
AsteriaIa64DispatchSalBootRendezvous(Asteria::Foundation::UInt64 crLid,
                                     Asteria::Foundation::UInt64 takeoverState) noexcept;

extern "C" [[noreturn]] void
AsteriaIa64RunApplicationProcessorOwnedExecutionContext(Asteria::Foundation::UInt64 processorValue) noexcept;

// The target routines are in Ia64SalBootRendezvousHandler.S.

namespace Asteria::Platform::Ia64::Interrupt
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt8 MinimumPageShift{12U};
constexpr Foundation::UInt64 VirtualRegionOffsetMask{(Foundation::UInt64{1U} << 61U) - 1U};
constexpr Foundation::UInt64 PhysicalAddressTopBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 TakeoverRecordAlignmentMask{7U};

constinit Ia64SalBootRendezvousHandler g_nativeHandler;

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

[[nodiscard]] constexpr bool AreProcessorReservedBytesZero(const Ia64LocalSapicLogicalProcessor& processor) noexcept
{
    return (processor.Reserved[0U] == 0U) && (processor.Reserved[1U] == 0U);
}

[[nodiscard]] constexpr bool IsUnusedProcessorZero(const Ia64LocalSapicLogicalProcessor& processor) noexcept
{
    return (processor.InterruptAddress == 0U) && (processor.AcpiUid == 0U) && (processor.HardwareId == 0U) &&
           (processor.HardwareEid == 0U) && AreProcessorReservedBytesZero(processor);
}

[[nodiscard]] constexpr bool DoesInterruptAddressMatch(const Foundation::UInt64 accessBaseAddress,
                                                       const Ia64LocalSapicLogicalProcessor& processor) noexcept
{
    const Foundation::UInt64 idOffset = static_cast<Foundation::UInt64>(processor.HardwareId) << 12U;
    const Foundation::UInt64 eidOffset = static_cast<Foundation::UInt64>(processor.HardwareEid) << 4U;
    if ((accessBaseAddress == 0U) || (accessBaseAddress > (MaximumUInt64 - idOffset)) ||
        ((accessBaseAddress + idOffset) > (MaximumUInt64 - eidOffset)))
    {
        return false;
    }
    return processor.InterruptAddress == accessBaseAddress + idOffset + eidOffset;
}

[[nodiscard]] constexpr bool IsSalReturnContextValid(const Foundation::UInt64 virtualAddress,
                                                     const Foundation::UInt64 encodedPageSize) noexcept
{
    if ((encodedPageSize & ~Foundation::UInt64{0xFCU}) != 0U)
    {
        return false;
    }
    const Foundation::UInt8 pageShift = static_cast<Foundation::UInt8>((encodedPageSize >> 2U) & 0x3FU);
    if (pageShift < MinimumPageShift)
    {
        return false;
    }
    const Foundation::UInt64 pageOffsetMask =
        pageShift >= 61U ? VirtualRegionOffsetMask : (Foundation::UInt64{1U} << pageShift) - 1U;
    return (virtualAddress & pageOffsetMask) == 0U;
}

[[nodiscard]] constexpr bool IsSalReturnContextValid(const Firmware::SalSystemTableInformation& salInformation) noexcept
{
    return salInformation.HasTranslationRegister && (salInformation.Reserved8 == 0U) &&
           IsSalReturnContextValid(salInformation.TranslationRegisterVirtualAddress,
                                   salInformation.TranslationRegisterEncodedPageSize);
}

[[nodiscard]] constexpr bool IsSalReturnContextValid(const Ia64SalBootRendezvousReturnContext& context) noexcept
{
    return IsSalReturnContextValid(context.TranslationRegisterVirtualAddress,
                                   context.TranslationRegisterEncodedPageSize);
}

[[nodiscard]] constexpr Ia64SalBootRendezvousProcessorContext
MakeProcessor(const Ia64LocalSapicLogicalProcessor& processor, const Foundation::UInt8 logicalProcessor) noexcept
{
    return Ia64SalBootRendezvousProcessorContext{
        .AcpiUid = processor.AcpiUid,
        .Processor = Kernel::Scheduler::CpuId{logicalProcessor},
        .HardwareId = processor.HardwareId,
        .HardwareEid = processor.HardwareEid,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr Ia64SalBootRendezvousArrivalResult
MakeArrivalResult(const Ia64SalBootRendezvousArrivalStatus status,
                  const Kernel::Scheduler::CpuId processor = Kernel::Scheduler::CpuId{}) noexcept
{
    return Ia64SalBootRendezvousArrivalResult{
        .Processor = processor,
        .Status = status,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr Ia64SalBootRendezvousAction
MakeAction(const Ia64SalBootRendezvousReturnContext& context, const Ia64SalBootRendezvousDisposition disposition,
           const Foundation::UInt64 takeoverRecordPhysicalAddress = 0U) noexcept
{
    return Ia64SalBootRendezvousAction{
        .TranslationRegisterVirtualAddress = context.TranslationRegisterVirtualAddress,
        .TranslationRegisterEncodedPageSize = context.TranslationRegisterEncodedPageSize,
        .Disposition = static_cast<Foundation::UInt64>(disposition),
        .TakeoverRecordPhysicalAddress = takeoverRecordPhysicalAddress,
    };
}
} // namespace

Ia64SalBootRendezvousInitializationError
Ia64SalBootRendezvousHandler::Initialize(const Ia64LocalSapicIpiControllerSnapshot& ipiController,
                                         const Foundation::UInt64 currentBspCrLid,
                                         const Firmware::SalSystemTableInformation& salInformation) noexcept
{
    if (static_cast<Ia64SalBootRendezvousState>(m_state.LoadAcquire()) != Ia64SalBootRendezvousState::Uninitialized)
    {
        return Ia64SalBootRendezvousInitializationError::AlreadyInitialized;
    }
    if (ipiController.State != Ia64LocalSapicIpiControllerState::Ready)
    {
        return Ia64SalBootRendezvousInitializationError::IpiControllerNotReady;
    }
    if ((ipiController.ProcessorCount == 0U) ||
        (ipiController.ProcessorCount > Kernel::Scheduler::MaximumProcessorCount))
    {
        return Ia64SalBootRendezvousInitializationError::InvalidProcessorCount;
    }
    const Foundation::UInt8 enabledProcessorMask =
        static_cast<Foundation::UInt8>(ProcessorBit(ipiController.ProcessorCount) - 1U);
    if (ipiController.EnabledProcessorMask != enabledProcessorMask)
    {
        return Ia64SalBootRendezvousInitializationError::InvalidProcessorMask;
    }
    for (const Foundation::UInt8 reserved : ipiController.Reserved)
    {
        if (reserved != 0U)
        {
            return Ia64SalBootRendezvousInitializationError::InvalidProcessorRecord;
        }
    }

    Ia64SalBootRendezvousProcessorContext processors[Kernel::Scheduler::MaximumProcessorCount]{};
    for (Foundation::UInt8 index{0U}; index < Kernel::Scheduler::MaximumProcessorCount; ++index)
    {
        const Ia64LocalSapicLogicalProcessor& processor = ipiController.Processors[index];
        if (index >= ipiController.ProcessorCount)
        {
            if (!IsUnusedProcessorZero(processor))
            {
                return Ia64SalBootRendezvousInitializationError::InvalidProcessorRecord;
            }
            continue;
        }
        if (!AreProcessorReservedBytesZero(processor) ||
            !DoesInterruptAddressMatch(ipiController.AccessBaseAddress, processor))
        {
            return Ia64SalBootRendezvousInitializationError::InvalidProcessorRecord;
        }
        for (Foundation::UInt8 previous{0U}; previous < index; ++previous)
        {
            const Ia64LocalSapicLogicalProcessor& prior = ipiController.Processors[previous];
            if ((processor.AcpiUid == prior.AcpiUid) ||
                ((processor.HardwareId == prior.HardwareId) && (processor.HardwareEid == prior.HardwareEid)))
            {
                return Ia64SalBootRendezvousInitializationError::DuplicateProcessorIdentity;
            }
        }
        processors[index] = MakeProcessor(processor, index);
    }

    const Ia64LocalSapicLogicalProcessor& bsp = ipiController.Processors[0U];
    if ((bsp.HardwareId != GetLidHardwareId(currentBspCrLid)) ||
        (bsp.HardwareEid != GetLidHardwareEid(currentBspCrLid)))
    {
        return Ia64SalBootRendezvousInitializationError::InvalidBspIdentity;
    }
    if (!IsSalReturnContextValid(salInformation))
    {
        return Ia64SalBootRendezvousInitializationError::InvalidSalInformation;
    }

    for (Foundation::UInt8 index{0U}; index < Kernel::Scheduler::MaximumProcessorCount; ++index)
    {
        m_processors[index] = processors[index];
    }
    m_processorCount = ipiController.ProcessorCount;
    m_expectedApMask =
        static_cast<Foundation::UInt8>(enabledProcessorMask & static_cast<Foundation::UInt8>(~Foundation::UInt8{1U}));
    m_translationRegisterVirtualAddress = salInformation.TranslationRegisterVirtualAddress;
    m_translationRegisterEncodedPageSize = salInformation.TranslationRegisterEncodedPageSize;
    m_state.StoreRelease(static_cast<Foundation::UInt64>(Ia64SalBootRendezvousState::Configured));
    return Ia64SalBootRendezvousInitializationError::None;
}

Ia64SalBootRendezvousArmError Ia64SalBootRendezvousHandler::Arm() noexcept
{
    const Ia64SalBootRendezvousState state = static_cast<Ia64SalBootRendezvousState>(m_state.LoadAcquire());
    if (state == Ia64SalBootRendezvousState::Armed)
    {
        return Ia64SalBootRendezvousArmError::AlreadyArmed;
    }
    if (state != Ia64SalBootRendezvousState::Configured)
    {
        return Ia64SalBootRendezvousArmError::NotConfigured;
    }
    m_state.StoreRelease(static_cast<Foundation::UInt64>(Ia64SalBootRendezvousState::Armed));
    return Ia64SalBootRendezvousArmError::None;
}

Ia64SalBootRendezvousArrivalResult Ia64SalBootRendezvousHandler::RecordArrival(const Foundation::UInt64 crLid) noexcept
{
    if (static_cast<Ia64SalBootRendezvousState>(m_state.LoadAcquire()) != Ia64SalBootRendezvousState::Armed)
    {
        return MakeArrivalResult(Ia64SalBootRendezvousArrivalStatus::NotArmed);
    }

    const Foundation::UInt8 hardwareId = GetLidHardwareId(crLid);
    const Foundation::UInt8 hardwareEid = GetLidHardwareEid(crLid);
    Foundation::UInt8 logicalProcessor{m_processorCount};
    for (Foundation::UInt8 index{0U}; index < m_processorCount; ++index)
    {
        if ((m_processors[index].HardwareId == hardwareId) && (m_processors[index].HardwareEid == hardwareEid))
        {
            logicalProcessor = index;
            break;
        }
    }
    if (logicalProcessor >= m_processorCount)
    {
        return MakeArrivalResult(Ia64SalBootRendezvousArrivalStatus::UnknownProcessor);
    }

    const Kernel::Scheduler::CpuId processor{logicalProcessor};
    if (logicalProcessor == 0U)
    {
        return MakeArrivalResult(Ia64SalBootRendezvousArrivalStatus::BootstrapProcessor, processor);
    }
    if (m_arrivals[logicalProcessor].LoadAcquire() != 0U)
    {
        return MakeArrivalResult(Ia64SalBootRendezvousArrivalStatus::Duplicate, processor);
    }

    // SAL serializes invocations on this AP. Distinct APs write distinct
    // words, so a release store publishes arrival without a shared RMW path.
    m_arrivals[logicalProcessor].StoreRelease(1U);
    return MakeArrivalResult(Ia64SalBootRendezvousArrivalStatus::Arrived, processor);
}

Ia64SalBootRendezvousAction
Ia64SalBootRendezvousHandler::Dispatch(const Foundation::UInt64 crLid,
                                       Ia64ApplicationProcessorTakeoverCoordinator& coordinator,
                                       const Ia64SalBootRendezvousTakeoverState takeoverState) noexcept
{
    const Ia64SalBootRendezvousArrivalResult arrival = RecordArrival(crLid);
    const Ia64SalBootRendezvousReturnContext returnContext = GetReturnContext();
    if (!IsSalReturnContextValid(returnContext))
    {
        return MakeAction(returnContext, Ia64SalBootRendezvousDisposition::FailStop);
    }

    switch (arrival.Status)
    {
    case Ia64SalBootRendezvousArrivalStatus::NotArmed:
    case Ia64SalBootRendezvousArrivalStatus::UnknownProcessor:
    case Ia64SalBootRendezvousArrivalStatus::BootstrapProcessor:
    case Ia64SalBootRendezvousArrivalStatus::Duplicate:
        return MakeAction(returnContext, Ia64SalBootRendezvousDisposition::ReturnToSal);

    case Ia64SalBootRendezvousArrivalStatus::Arrived:
        break;
    }

    const Ia64ApplicationProcessorTakeoverProgressSnapshot progress = coordinator.GetProgressSnapshot();
    switch (progress.State)
    {
    case Ia64ApplicationProcessorTakeoverCoordinatorState::Uninitialized:
        return MakeAction(returnContext, Ia64SalBootRendezvousDisposition::ReturnToSal);

    case Ia64ApplicationProcessorTakeoverCoordinatorState::InitializationFailed:
        return MakeAction(returnContext, Ia64SalBootRendezvousDisposition::FailStop);

    case Ia64ApplicationProcessorTakeoverCoordinatorState::Ready:
        break;
    }

    if (takeoverState != Ia64SalBootRendezvousTakeoverState::Valid)
    {
        return MakeAction(returnContext, Ia64SalBootRendezvousDisposition::FailStop);
    }

    const auto claim = coordinator.Claim(arrival.Processor);
    if (!claim.HasValue())
    {
        return MakeAction(returnContext, Ia64SalBootRendezvousDisposition::FailStop);
    }

    const Ia64ApplicationProcessorTakeoverRecord* const record = claim.GetValue();
    if ((record == nullptr) || !arrival.Processor.IsValid() || (record->ProcessorValue != arrival.Processor.GetValue()))
    {
        return MakeAction(returnContext, Ia64SalBootRendezvousDisposition::FailStop);
    }

    const Foundation::UInt64 recordAddress =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(record));
    if ((recordAddress == 0U) || ((recordAddress & TakeoverRecordAlignmentMask) != 0U) ||
        (recordAddress >= PhysicalAddressTopBit))
    {
        return MakeAction(returnContext, Ia64SalBootRendezvousDisposition::FailStop);
    }

    return MakeAction(returnContext, Ia64SalBootRendezvousDisposition::TakeOver, recordAddress);
}

Foundation::UInt64 Ia64SalBootRendezvousHandler::ReadCurrentCrLid() noexcept
{
    return AsteriaIa64ReadCurrentCrLid();
}

Foundation::UInt64 Ia64SalBootRendezvousHandler::GetPhysicalEntryAddress() noexcept
{
    return AsteriaIa64GetSalBootRendezvousEntryAddress();
}

Foundation::UInt64 Ia64SalBootRendezvousHandler::GetPhysicalGlobalPointerAddress() noexcept
{
    return AsteriaIa64GetSalBootRendezvousGlobalPointerAddress();
}

Ia64SalBootRendezvousReturnContext Ia64SalBootRendezvousHandler::GetReturnContext() const noexcept
{
    if (static_cast<Ia64SalBootRendezvousState>(m_state.LoadAcquire()) == Ia64SalBootRendezvousState::Uninitialized)
    {
        return Ia64SalBootRendezvousReturnContext{};
    }
    return Ia64SalBootRendezvousReturnContext{
        .TranslationRegisterVirtualAddress = m_translationRegisterVirtualAddress,
        .TranslationRegisterEncodedPageSize = m_translationRegisterEncodedPageSize,
    };
}

Ia64SalBootRendezvousSnapshot Ia64SalBootRendezvousHandler::GetSnapshot() const noexcept
{
    const Ia64SalBootRendezvousState state = static_cast<Ia64SalBootRendezvousState>(m_state.LoadAcquire());
    Foundation::UInt8 arrivedApMask{0U};
    for (Foundation::UInt8 processor{1U}; processor < m_processorCount; ++processor)
    {
        if (m_arrivals[processor].LoadAcquire() != 0U)
        {
            arrivedApMask = static_cast<Foundation::UInt8>(arrivedApMask | ProcessorBit(processor));
        }
    }
    return Ia64SalBootRendezvousSnapshot{
        .Processors =
            {
                m_processors[0U],
                m_processors[1U],
                m_processors[2U],
                m_processors[3U],
            },
        .TranslationRegisterVirtualAddress = m_translationRegisterVirtualAddress,
        .TranslationRegisterEncodedPageSize = m_translationRegisterEncodedPageSize,
        .ProcessorCount = m_processorCount,
        .ExpectedApMask = m_expectedApMask,
        .ArrivedApMask = arrivedApMask,
        .State = state,
        .Reserved = {},
    };
}

Ia64SalBootRendezvousHandler& GetNativeIa64SalBootRendezvousHandler() noexcept
{
    return g_nativeHandler;
}
} // namespace Asteria::Platform::Ia64::Interrupt

extern "C" Asteria::Platform::Ia64::Interrupt::Ia64SalBootRendezvousAction
AsteriaIa64DispatchSalBootRendezvous(const Asteria::Foundation::UInt64 crLid,
                                     const Asteria::Foundation::UInt64 takeoverState) noexcept
{
    const Asteria::Platform::Ia64::Interrupt::Ia64SalBootRendezvousTakeoverState decodedTakeoverState =
        takeoverState == static_cast<Asteria::Foundation::UInt64>(
                             Asteria::Platform::Ia64::Interrupt::Ia64SalBootRendezvousTakeoverState::Valid)
            ? Asteria::Platform::Ia64::Interrupt::Ia64SalBootRendezvousTakeoverState::Valid
            : Asteria::Platform::Ia64::Interrupt::Ia64SalBootRendezvousTakeoverState::Invalid;
    return Asteria::Platform::Ia64::Interrupt::GetNativeIa64SalBootRendezvousHandler().Dispatch(
        crLid, Asteria::Platform::Ia64::Interrupt::GetNativeIa64ApplicationProcessorTakeoverCoordinator(),
        decodedTakeoverState);
}
