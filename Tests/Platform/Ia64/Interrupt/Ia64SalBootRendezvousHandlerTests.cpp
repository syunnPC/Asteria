// SPDX-License-Identifier: Apache-2.0
#if defined(__ia64__)
#include "Platform/Ia64/Interrupt/Ia64SalBootRendezvousHandler.hpp"

extern "C" Asteria::Platform::Ia64::Interrupt::Ia64SalBootRendezvousAction
AsteriaIa64DispatchSalBootRendezvous(Asteria::Foundation::UInt64 crLid,
                                     Asteria::Foundation::UInt64 takeoverState) noexcept;

extern "C" [[gnu::used, gnu::noinline]] Asteria::Foundation::UInt64
AsteriaIa64SalBootRendezvousHandlerAbiFixture() noexcept
{
    using Asteria::Platform::Ia64::Interrupt::Ia64SalBootRendezvousHandler;
    return Ia64SalBootRendezvousHandler::GetPhysicalEntryAddress() ^
           Ia64SalBootRendezvousHandler::GetPhysicalGlobalPointerAddress() ^
           Ia64SalBootRendezvousHandler::ReadCurrentCrLid();
}

extern "C" [[gnu::used, gnu::noinline]] Asteria::Foundation::UInt64
AsteriaIa64SalBootRendezvousReturnAbiFixture(const Asteria::Foundation::UInt64 crLid) noexcept
{
    const auto result = AsteriaIa64DispatchSalBootRendezvous(
        crLid, static_cast<Asteria::Foundation::UInt64>(
                   Asteria::Platform::Ia64::Interrupt::Ia64SalBootRendezvousTakeoverState::Valid));
    return result.TranslationRegisterVirtualAddress ^ result.TranslationRegisterEncodedPageSize ^ result.Disposition ^
           result.TakeoverRecordPhysicalAddress;
}
#else
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Boot/BootInfo.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Platform/Ia64/Interrupt/Ia64ApplicationProcessorTakeoverCoordinator.hpp"
#include "Platform/Ia64/Interrupt/Ia64SalBootRendezvousHandler.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
constinit Foundation::UInt64 g_currentCrLid{0U};
constinit Foundation::UInt64 g_handlerEntryAddress{0U};
constinit Foundation::UInt64 g_handlerGlobalPointerAddress{0U};
} // namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64 AsteriaIa64ReadCurrentCrLid() noexcept
{
    return Asteria::Test::g_currentCrLid;
}

extern "C" Asteria::Foundation::UInt64 AsteriaIa64GetSalBootRendezvousEntryAddress() noexcept
{
    return Asteria::Test::g_handlerEntryAddress;
}

extern "C" Asteria::Foundation::UInt64 AsteriaIa64GetSalBootRendezvousGlobalPointerAddress() noexcept
{
    return Asteria::Test::g_handlerGlobalPointerAddress;
}

namespace Asteria::Test
{
namespace
{
using Kernel::Scheduler::CpuId;
using Kernel::Memory::BootstrapPageAllocator;
using Kernel::Memory::BootstrapPageAllocatorError;
using Platform::Ia64::Firmware::SalSystemTableInformation;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverCoordinator;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverCoordinatorState;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverInitializationError;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverLifecycle;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverRecord;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiControllerSnapshot;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiControllerState;
using Platform::Ia64::Interrupt::Ia64LocalSapicLogicalProcessor;
using Platform::Ia64::Interrupt::Ia64ProcessorInterruptBlockBaseKind;
using Platform::Ia64::Interrupt::Ia64SalBootRendezvousArrivalResult;
using Platform::Ia64::Interrupt::Ia64SalBootRendezvousArrivalStatus;
using Platform::Ia64::Interrupt::Ia64SalBootRendezvousAction;
using Platform::Ia64::Interrupt::Ia64SalBootRendezvousArmError;
using Platform::Ia64::Interrupt::Ia64SalBootRendezvousDisposition;
using Platform::Ia64::Interrupt::Ia64SalBootRendezvousHandler;
using Platform::Ia64::Interrupt::Ia64SalBootRendezvousTakeoverState;
using Platform::Ia64::Interrupt::Ia64SalBootRendezvousInitializationError;
using Platform::Ia64::Interrupt::Ia64SalBootRendezvousProcessorContext;
using Platform::Ia64::Interrupt::Ia64SalBootRendezvousReturnContext;
using Platform::Ia64::Interrupt::Ia64SalBootRendezvousSnapshot;
using Platform::Ia64::Interrupt::Ia64SalBootRendezvousState;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageOwner;

constexpr Foundation::UInt32 FirmwareDescriptorSize{40U};
constexpr Foundation::UInt32 ConventionalMemoryType{7U};
constexpr Foundation::UInt64 WriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 DefaultPhysicalBase{0x0000'0000'FEE0'0000U};
constexpr Foundation::UInt64 DefaultAccessBase{0x8000'0000'FEE0'0000U};
constexpr Foundation::Size TakeoverStoragePageCount{64U};

alignas(32U *
        1024U) Foundation::UInt8 g_takeoverStorage[TakeoverStoragePageCount * Kernel::Memory::PhysicalPageSizeBytes];
Boot::BootInfo g_takeoverBootInfo{};

[[nodiscard]] constexpr Foundation::UInt64 MakeCrLid(const Foundation::UInt8 hardwareId,
                                                     const Foundation::UInt8 hardwareEid) noexcept
{
    return (static_cast<Foundation::UInt64>(hardwareId) << 24U) | (static_cast<Foundation::UInt64>(hardwareEid) << 16U);
}

[[nodiscard]] constexpr Foundation::UInt64 InterruptAddress(const Foundation::UInt8 hardwareId,
                                                            const Foundation::UInt8 hardwareEid) noexcept
{
    return DefaultAccessBase + (static_cast<Foundation::UInt64>(hardwareId) << 12U) +
           (static_cast<Foundation::UInt64>(hardwareEid) << 4U);
}

[[nodiscard]] constexpr Ia64LocalSapicLogicalProcessor MakeProcessor(const Foundation::UInt32 acpiUid,
                                                                     const Foundation::UInt8 hardwareId,
                                                                     const Foundation::UInt8 hardwareEid) noexcept
{
    return Ia64LocalSapicLogicalProcessor{
        .InterruptAddress = InterruptAddress(hardwareId, hardwareEid),
        .AcpiUid = acpiUid,
        .HardwareId = hardwareId,
        .HardwareEid = hardwareEid,
        .Reserved = {},
    };
}

[[nodiscard]] Ia64LocalSapicIpiControllerSnapshot MakeSnapshot(const Foundation::UInt8 processorCount = 4U) noexcept
{
    Ia64LocalSapicIpiControllerSnapshot snapshot{};
    snapshot.PhysicalBaseAddress = DefaultPhysicalBase;
    snapshot.AccessBaseAddress = DefaultAccessBase;
    snapshot.ProcessorCount = processorCount;
    snapshot.EnabledProcessorMask = static_cast<Foundation::UInt8>((Foundation::UInt32{1U} << processorCount) - 1U);
    snapshot.BaseKind = Ia64ProcessorInterruptBlockBaseKind::BootstrapPhysical;
    snapshot.State = Ia64LocalSapicIpiControllerState::Ready;
    if (processorCount >= 1U)
    {
        snapshot.Processors[0U] = MakeProcessor(100U, 0x30U, 3U);
    }
    if (processorCount >= 2U)
    {
        snapshot.Processors[1U] = MakeProcessor(101U, 0x10U, 1U);
    }
    if (processorCount >= 3U)
    {
        snapshot.Processors[2U] = MakeProcessor(102U, 0x20U, 2U);
    }
    if (processorCount >= 4U)
    {
        snapshot.Processors[3U] = MakeProcessor(103U, 0x40U, 4U);
    }
    return snapshot;
}

[[nodiscard]] Foundation::UInt64 TakeoverStoragePhysicalStart() noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_takeoverStorage));
}

[[nodiscard]] bool InitializeTakeoverCoordinator(Ia64ApplicationProcessorTakeoverCoordinator& coordinator,
                                                 ApplicationProcessorExecutionStorageOwner& owner,
                                                 BootstrapPageAllocator& allocator,
                                                 const Foundation::UInt8 processorCount = 4U) noexcept
{
    g_takeoverBootInfo = Boot::BootInfo{};
    g_takeoverBootInfo.Signature = Boot::BootInfoSignature;
    g_takeoverBootInfo.Version = Boot::BootInfoVersion;
    g_takeoverBootInfo.SizeBytes = static_cast<Foundation::UInt32>(sizeof(Boot::BootInfo));
    g_takeoverBootInfo.MemoryRegionCount = 1U;
    g_takeoverBootInfo.FirmwareDescriptorVersion = 1U;
    g_takeoverBootInfo.FirmwareDescriptorSize = FirmwareDescriptorSize;
    g_takeoverBootInfo.FirmwareMemoryMapSizeBytes = FirmwareDescriptorSize;
    g_takeoverBootInfo.MemoryRegions[0U] = Boot::PhysicalMemoryRegion{
        .PhysicalStart = TakeoverStoragePhysicalStart() + Kernel::Memory::PhysicalPageSizeBytes,
        .VirtualStart = 0U,
        .PageCount = TakeoverStoragePageCount - 1U,
        .Attributes = WriteBackAttribute,
        .FirmwareType = ConventionalMemoryType,
        .Kind = Boot::PhysicalMemoryKind::Conventional,
    };
    if (allocator.Initialize(g_takeoverBootInfo) != BootstrapPageAllocatorError::None)
    {
        return false;
    }
    if (!owner.Prepare(allocator, MakeSnapshot(processorCount)).HasValue())
    {
        return false;
    }
    return coordinator.Initialize(owner) == Ia64ApplicationProcessorTakeoverInitializationError::None;
}

[[nodiscard]] constexpr SalSystemTableInformation MakeSalInformation() noexcept
{
    SalSystemTableInformation information{};
    information.TranslationRegisterVirtualAddress = 0U;
    information.TranslationRegisterEncodedPageSize = 22U << 2U;
    information.HasTranslationRegister = true;
    return information;
}

[[nodiscard]] bool ProcessorIs(const Ia64SalBootRendezvousProcessorContext& processor,
                               const Foundation::UInt8 logicalProcessor, const Foundation::UInt32 acpiUid,
                               const Foundation::UInt8 hardwareId, const Foundation::UInt8 hardwareEid) noexcept
{
    return (processor.Processor.GetValue() == logicalProcessor) && (processor.AcpiUid == acpiUid) &&
           (processor.HardwareId == hardwareId) && (processor.HardwareEid == hardwareEid) && (processor.Reserved == 0U);
}

[[nodiscard]] bool UnusedProcessorIsInitial(const Ia64SalBootRendezvousProcessorContext& processor) noexcept
{
    return !processor.Processor.IsValid() && (processor.AcpiUid == 0U) && (processor.HardwareId == 0U) &&
           (processor.HardwareEid == 0U) && (processor.Reserved == 0U);
}

[[nodiscard]] bool IsInitialSnapshot(const Ia64SalBootRendezvousSnapshot& snapshot) noexcept
{
    bool processorsInitial{true};
    for (const Ia64SalBootRendezvousProcessorContext& processor : snapshot.Processors)
    {
        processorsInitial = processorsInitial && UnusedProcessorIsInitial(processor);
    }
    return processorsInitial && (snapshot.TranslationRegisterVirtualAddress == 0U) &&
           (snapshot.TranslationRegisterEncodedPageSize == 0U) && (snapshot.ProcessorCount == 0U) &&
           (snapshot.ExpectedApMask == 0U) && (snapshot.ArrivedApMask == 0U) &&
           (snapshot.State == Ia64SalBootRendezvousState::Uninitialized) && (snapshot.Reserved[0U] == 0U) &&
           (snapshot.Reserved[1U] == 0U) && (snapshot.Reserved[2U] == 0U) && (snapshot.Reserved[3U] == 0U);
}

[[nodiscard]] bool ArrivalIs(const Ia64SalBootRendezvousArrivalResult& result,
                             const Ia64SalBootRendezvousArrivalStatus status,
                             const Foundation::UInt8 processor = 0xFFU) noexcept
{
    bool reservedZero{true};
    for (const Foundation::UInt8 reserved : result.Reserved)
    {
        reservedZero = reservedZero && (reserved == 0U);
    }
    return (result.Status == status) && (result.Processor.GetValue() == processor) && reservedZero;
}

[[nodiscard]] constexpr bool ActionIs(const Ia64SalBootRendezvousAction& action,
                                      const Ia64SalBootRendezvousDisposition disposition,
                                      const Foundation::UInt64 recordAddress = 0U) noexcept
{
    return (action.TranslationRegisterVirtualAddress == 0U) &&
           (action.TranslationRegisterEncodedPageSize == (22U << 2U)) &&
           (action.Disposition == static_cast<Foundation::UInt64>(disposition)) &&
           (action.TakeoverRecordPhysicalAddress == recordAddress);
}

[[nodiscard]] bool InitializationFailsAtomically(Ia64SalBootRendezvousHandler& handler,
                                                 const Ia64LocalSapicIpiControllerSnapshot& snapshot,
                                                 const Foundation::UInt64 crLid,
                                                 const Ia64SalBootRendezvousInitializationError expected) noexcept
{
    return (handler.Initialize(snapshot, crLid, MakeSalInformation()) == expected) &&
           IsInitialSnapshot(handler.GetSnapshot());
}

[[nodiscard]] bool SalInitializationFailsAtomically(Ia64SalBootRendezvousHandler& handler,
                                                    const Ia64LocalSapicIpiControllerSnapshot& snapshot,
                                                    const SalSystemTableInformation& salInformation) noexcept
{
    return (handler.Initialize(snapshot, MakeCrLid(0x30U, 3U), salInformation) ==
            Ia64SalBootRendezvousInitializationError::InvalidSalInformation) &&
           IsInitialSnapshot(handler.GetSnapshot());
}

void TestNativeValueAdapters(TestContext& runner) noexcept
{
    g_currentCrLid = 0x1234'5678'9ABC'DEF0U;
    g_handlerEntryAddress = 0x0010'0000U;
    g_handlerGlobalPointerAddress = 0x0020'0000U;
    ASTERIA_EXPECT(runner, "the typed adapters preserve raw cr.lid, entry, and GP values",
                   Ia64SalBootRendezvousHandler::ReadCurrentCrLid() == g_currentCrLid,
                   Ia64SalBootRendezvousHandler::GetPhysicalEntryAddress() == g_handlerEntryAddress,
                   Ia64SalBootRendezvousHandler::GetPhysicalGlobalPointerAddress() == g_handlerGlobalPointerAddress);
}

void TestOneProcessorBoundary(TestContext& runner) noexcept
{
    Ia64SalBootRendezvousHandler handler;
    const Ia64SalBootRendezvousReturnContext initialReturnContext = handler.GetReturnContext();
    ASTERIA_EXPECT(
        runner, "an uninitialized handler is inert", IsInitialSnapshot(handler.GetSnapshot()),
        initialReturnContext.TranslationRegisterVirtualAddress == 0U,
        initialReturnContext.TranslationRegisterEncodedPageSize == 0U,
        handler.Arm() == Ia64SalBootRendezvousArmError::NotConfigured,
        ArrivalIs(handler.RecordArrival(MakeCrLid(0x30U, 3U)), Ia64SalBootRendezvousArrivalStatus::NotArmed));

    Ia64LocalSapicIpiControllerSnapshot snapshot = MakeSnapshot(1U);
    ASTERIA_EXPECT(runner, "a BSP-only logical map configures",
                   handler.Initialize(snapshot, MakeCrLid(0x30U, 3U), MakeSalInformation()) ==
                       Ia64SalBootRendezvousInitializationError::None);
    snapshot.Processors[0U] = MakeProcessor(999U, 0x77U, 7U);
    const Ia64SalBootRendezvousSnapshot configured = handler.GetSnapshot();
    const Ia64SalBootRendezvousReturnContext returnContext = handler.GetReturnContext();
    ASTERIA_EXPECT(runner, "configuration retains an immutable BSP-first identity map",
                   configured.State == Ia64SalBootRendezvousState::Configured,
                   configured.TranslationRegisterVirtualAddress == 0U,
                   configured.TranslationRegisterEncodedPageSize == (22U << 2U),
                   returnContext.TranslationRegisterVirtualAddress == 0U,
                   returnContext.TranslationRegisterEncodedPageSize == (22U << 2U), configured.ProcessorCount == 1U,
                   configured.ExpectedApMask == 0U, configured.ArrivedApMask == 0U,
                   ProcessorIs(configured.Processors[0U], 0U, 100U, 0x30U, 3U),
                   UnusedProcessorIsInitial(configured.Processors[1U]));

    ASTERIA_EXPECT(
        runner, "CPU0 and an unknown CPU can never become AP arrivals",
        handler.Arm() == Ia64SalBootRendezvousArmError::None,
        handler.Arm() == Ia64SalBootRendezvousArmError::AlreadyArmed,
        ArrivalIs(handler.RecordArrival(MakeCrLid(0x30U, 3U)), Ia64SalBootRendezvousArrivalStatus::BootstrapProcessor,
                  0U),
        ArrivalIs(handler.RecordArrival(MakeCrLid(0x31U, 3U)), Ia64SalBootRendezvousArrivalStatus::UnknownProcessor),
        handler.GetSnapshot().ArrivedApMask == 0U);
}

void TestFourProcessorArrivals(TestContext& runner) noexcept
{
    Ia64SalBootRendezvousHandler handler;
    const Ia64LocalSapicIpiControllerSnapshot snapshot = MakeSnapshot();
    ASTERIA_EXPECT(runner, "arrivals are rejected until the BSP release-arms the map",
                   handler.Initialize(snapshot, MakeCrLid(0x30U, 3U), MakeSalInformation()) ==
                       Ia64SalBootRendezvousInitializationError::None,
                   ArrivalIs(handler.RecordArrival(MakeCrLid(0x10U, 1U)), Ia64SalBootRendezvousArrivalStatus::NotArmed),
                   handler.GetSnapshot().ArrivedApMask == 0U, handler.Arm() == Ia64SalBootRendezvousArmError::None);

    Ia64SalBootRendezvousArrivalResult results[3U]{};
    std::thread arrivals[3U]{
        std::thread{[&handler, &results]() noexcept
                    {
                        results[0U] = handler.RecordArrival(MakeCrLid(0x10U, 1U));
                    }},
        std::thread{[&handler, &results]() noexcept
                    {
                        results[1U] = handler.RecordArrival(MakeCrLid(0x20U, 2U));
                    }},
        std::thread{[&handler, &results]() noexcept
                    {
                        results[2U] = handler.RecordArrival(MakeCrLid(0x40U, 4U));
                    }},
    };
    for (std::thread& arrival : arrivals)
    {
        arrival.join();
    }

    const Ia64SalBootRendezvousSnapshot complete = handler.GetSnapshot();
    ASTERIA_EXPECT(runner, "distinct APs independently release-publish the complete mask",
                   ArrivalIs(results[0U], Ia64SalBootRendezvousArrivalStatus::Arrived, 1U),
                   ArrivalIs(results[1U], Ia64SalBootRendezvousArrivalStatus::Arrived, 2U),
                   ArrivalIs(results[2U], Ia64SalBootRendezvousArrivalStatus::Arrived, 3U),
                   complete.State == Ia64SalBootRendezvousState::Armed, complete.ProcessorCount == 4U,
                   complete.ExpectedApMask == 0x0EU, complete.ArrivedApMask == complete.ExpectedApMask);

    ASTERIA_EXPECT(
        runner, "duplicates and exact ID/EID mismatches cannot corrupt arrivals",
        ArrivalIs(handler.RecordArrival(MakeCrLid(0x20U, 2U)), Ia64SalBootRendezvousArrivalStatus::Duplicate, 2U),
        ArrivalIs(handler.RecordArrival(MakeCrLid(0x20U, 0x03U)), Ia64SalBootRendezvousArrivalStatus::UnknownProcessor),
        ArrivalIs(handler.RecordArrival(MakeCrLid(0x20U, 2U) | 0xFFFFU), Ia64SalBootRendezvousArrivalStatus::Duplicate,
                  2U),
        handler.GetSnapshot().ArrivedApMask == 0x0EU);
}

void TestDispatchActions(TestContext& runner) noexcept
{
    {
        Ia64SalBootRendezvousHandler handler;
        Ia64ApplicationProcessorTakeoverCoordinator coordinator;
        const Ia64SalBootRendezvousAction malformed =
            handler.Dispatch(MakeCrLid(0x10U, 1U), coordinator, Ia64SalBootRendezvousTakeoverState::Valid);
        ASTERIA_EXPECT(
            runner, "a malformed unconfigured purge state retains zero operands and fail-stops",
            malformed.TranslationRegisterVirtualAddress == 0U, malformed.TranslationRegisterEncodedPageSize == 0U,
            malformed.Disposition == static_cast<Foundation::UInt64>(Ia64SalBootRendezvousDisposition::FailStop),
            malformed.TakeoverRecordPhysicalAddress == 0U);
    }

    {
        Ia64SalBootRendezvousHandler handler;
        Ia64ApplicationProcessorTakeoverCoordinator coordinator;
        const Ia64LocalSapicIpiControllerSnapshot snapshot = MakeSnapshot();
        const bool configured = handler.Initialize(snapshot, MakeCrLid(0x30U, 3U), MakeSalInformation()) ==
                                Ia64SalBootRendezvousInitializationError::None;
        const Ia64SalBootRendezvousAction notArmed =
            handler.Dispatch(MakeCrLid(0x10U, 1U), coordinator, Ia64SalBootRendezvousTakeoverState::Valid);
        const bool armed = handler.Arm() == Ia64SalBootRendezvousArmError::None;
        const Ia64SalBootRendezvousAction bsp =
            handler.Dispatch(MakeCrLid(0x30U, 3U), coordinator, Ia64SalBootRendezvousTakeoverState::Valid);
        const Ia64SalBootRendezvousAction unknown =
            handler.Dispatch(MakeCrLid(0x55U, 5U), coordinator, Ia64SalBootRendezvousTakeoverState::Valid);
        const Ia64SalBootRendezvousAction arrivalOnly =
            handler.Dispatch(MakeCrLid(0x10U, 1U), coordinator, Ia64SalBootRendezvousTakeoverState::Invalid);
        const Ia64SalBootRendezvousAction duplicate =
            handler.Dispatch(MakeCrLid(0x10U, 1U), coordinator, Ia64SalBootRendezvousTakeoverState::Valid);
        ASTERIA_EXPECT(runner, "ordinary rejections and an uninitialized coordinator retain arrival-only SAL returns",
                       configured, armed, ActionIs(notArmed, Ia64SalBootRendezvousDisposition::ReturnToSal),
                       ActionIs(bsp, Ia64SalBootRendezvousDisposition::ReturnToSal),
                       ActionIs(unknown, Ia64SalBootRendezvousDisposition::ReturnToSal),
                       ActionIs(arrivalOnly, Ia64SalBootRendezvousDisposition::ReturnToSal),
                       ActionIs(duplicate, Ia64SalBootRendezvousDisposition::ReturnToSal),
                       handler.GetSnapshot().ArrivedApMask == 0x02U,
                       coordinator.GetProgressSnapshot().State ==
                           Ia64ApplicationProcessorTakeoverCoordinatorState::Uninitialized);
    }

    {
        BootstrapPageAllocator allocator;
        ApplicationProcessorExecutionStorageOwner owner;
        Ia64ApplicationProcessorTakeoverCoordinator coordinator;
        Ia64SalBootRendezvousHandler handler;
        const bool initialized = InitializeTakeoverCoordinator(coordinator, owner, allocator) &&
                                 (handler.Initialize(MakeSnapshot(), MakeCrLid(0x30U, 3U), MakeSalInformation()) ==
                                  Ia64SalBootRendezvousInitializationError::None) &&
                                 (handler.Arm() == Ia64SalBootRendezvousArmError::None);
        const Foundation::UInt64 apLids[3U]{
            MakeCrLid(0x10U, 1U),
            MakeCrLid(0x20U, 2U),
            MakeCrLid(0x40U, 4U),
        };
        bool takeoversExact{initialized};
        for (Foundation::UInt8 processorValue{1U}; processorValue < Kernel::Scheduler::MaximumProcessorCount;
             ++processorValue)
        {
            const Ia64SalBootRendezvousAction action =
                handler.Dispatch(apLids[processorValue - 1U], coordinator, Ia64SalBootRendezvousTakeoverState::Valid);
            const Foundation::UInt64 recordAddress = action.TakeoverRecordPhysicalAddress;
            const auto* const record = reinterpret_cast<const Ia64ApplicationProcessorTakeoverRecord*>(
                static_cast<Foundation::UIntPointer>(recordAddress));
            const auto snapshot = coordinator.GetProcessorSnapshot(CpuId{processorValue});
            takeoversExact =
                takeoversExact && (recordAddress != 0U) && ((recordAddress & 7U) == 0U) &&
                (recordAddress < (Foundation::UInt64{1U} << 63U)) &&
                ActionIs(action, Ia64SalBootRendezvousDisposition::TakeOver, recordAddress) && (record != nullptr) &&
                (record->ProcessorValue == processorValue) && ((record->InitialStackPointer & 15U) == 0U) &&
                ((record->RseBackingStorePhysicalStart & 0x7FFFU) == 0U) &&
                (record->InitialStackPointer + 16U == record->RseBackingStorePhysicalStart) && snapshot.HasValue() &&
                (snapshot.GetValue().Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Claimed) &&
                (snapshot.GetValue().Takeover.ProcessorValue == processorValue);
        }
        const Ia64SalBootRendezvousAction duplicate =
            handler.Dispatch(MakeCrLid(0x20U, 2U), coordinator, Ia64SalBootRendezvousTakeoverState::Valid);
        ASTERIA_EXPECT(runner, "each first ready AP claim returns its exact immutable takeover record", takeoversExact,
                       ActionIs(duplicate, Ia64SalBootRendezvousDisposition::ReturnToSal),
                       handler.GetSnapshot().ArrivedApMask == 0x0EU);
    }

    {
        BootstrapPageAllocator allocator;
        ApplicationProcessorExecutionStorageOwner owner;
        Ia64ApplicationProcessorTakeoverCoordinator coordinator;
        Ia64SalBootRendezvousHandler handler;
        const bool initialized = InitializeTakeoverCoordinator(coordinator, owner, allocator, 2U) &&
                                 (handler.Initialize(MakeSnapshot(2U), MakeCrLid(0x30U, 3U), MakeSalInformation()) ==
                                  Ia64SalBootRendezvousInitializationError::None) &&
                                 (handler.Arm() == Ia64SalBootRendezvousArmError::None);
        const Ia64SalBootRendezvousAction action =
            handler.Dispatch(MakeCrLid(0x10U, 1U), coordinator, Ia64SalBootRendezvousTakeoverState::Invalid);
        const auto processor = coordinator.GetProcessorSnapshot(CpuId{1U});
        ASTERIA_EXPECT(runner, "invalid inherited takeover state fail-stops before the coordinator claim", initialized,
                       ActionIs(action, Ia64SalBootRendezvousDisposition::FailStop), processor.HasValue(),
                       processor.GetValue().Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Prepared,
                       handler.GetSnapshot().ArrivedApMask == 0x02U);
    }

    {
        ApplicationProcessorExecutionStorageOwner freshOwner;
        Ia64ApplicationProcessorTakeoverCoordinator failedCoordinator;
        Ia64SalBootRendezvousHandler handler;
        const bool failed = failedCoordinator.Initialize(freshOwner) ==
                            Ia64ApplicationProcessorTakeoverInitializationError::OwnerNotReady;
        const bool configured = handler.Initialize(MakeSnapshot(), MakeCrLid(0x30U, 3U), MakeSalInformation()) ==
                                Ia64SalBootRendezvousInitializationError::None;
        const Ia64SalBootRendezvousAction notArmed =
            handler.Dispatch(MakeCrLid(0x10U, 1U), failedCoordinator, Ia64SalBootRendezvousTakeoverState::Valid);
        const bool armed = handler.Arm() == Ia64SalBootRendezvousArmError::None;
        const Ia64SalBootRendezvousAction action =
            handler.Dispatch(MakeCrLid(0x10U, 1U), failedCoordinator, Ia64SalBootRendezvousTakeoverState::Valid);
        ASTERIA_EXPECT(runner, "a first AP arrival fail-stops after terminal coordinator initialization failure",
                       failed, configured, armed, ActionIs(notArmed, Ia64SalBootRendezvousDisposition::ReturnToSal),
                       ActionIs(action, Ia64SalBootRendezvousDisposition::FailStop));
    }

    {
        BootstrapPageAllocator allocator;
        ApplicationProcessorExecutionStorageOwner owner;
        Ia64ApplicationProcessorTakeoverCoordinator coordinator;
        Ia64SalBootRendezvousHandler handler;
        const bool initialized = InitializeTakeoverCoordinator(coordinator, owner, allocator, 2U) &&
                                 coordinator.Claim(CpuId{1U}).HasValue() &&
                                 (handler.Initialize(MakeSnapshot(2U), MakeCrLid(0x30U, 3U), MakeSalInformation()) ==
                                  Ia64SalBootRendezvousInitializationError::None) &&
                                 (handler.Arm() == Ia64SalBootRendezvousArmError::None);
        const Ia64SalBootRendezvousAction action =
            handler.Dispatch(MakeCrLid(0x10U, 1U), coordinator, Ia64SalBootRendezvousTakeoverState::Valid);
        ASTERIA_EXPECT(runner,
                       "an arrival whose coordinator slot was already claimed is an impossible fail-stop mismatch",
                       initialized, ActionIs(action, Ia64SalBootRendezvousDisposition::FailStop));
    }
}

void TestIntermediateProcessorCountsAndIdentities(TestContext& runner) noexcept
{
    Ia64SalBootRendezvousHandler twoProcessorHandler;
    const Ia64LocalSapicIpiControllerSnapshot twoProcessorSnapshot = MakeSnapshot(2U);
    const bool twoProcessorsArrive =
        (twoProcessorHandler.Initialize(twoProcessorSnapshot, MakeCrLid(0x30U, 3U), MakeSalInformation()) ==
         Ia64SalBootRendezvousInitializationError::None) &&
        (twoProcessorHandler.Arm() == Ia64SalBootRendezvousArmError::None) &&
        ArrivalIs(twoProcessorHandler.RecordArrival(MakeCrLid(0x10U, 1U)), Ia64SalBootRendezvousArrivalStatus::Arrived,
                  1U) &&
        (twoProcessorHandler.GetSnapshot().ExpectedApMask == 0x02U) &&
        (twoProcessorHandler.GetSnapshot().ArrivedApMask == 0x02U);

    Ia64SalBootRendezvousHandler threeProcessorHandler;
    const Ia64LocalSapicIpiControllerSnapshot threeProcessorSnapshot = MakeSnapshot(3U);
    const bool threeProcessorsArrive =
        (threeProcessorHandler.Initialize(threeProcessorSnapshot, MakeCrLid(0x30U, 3U), MakeSalInformation()) ==
         Ia64SalBootRendezvousInitializationError::None) &&
        (threeProcessorHandler.Arm() == Ia64SalBootRendezvousArmError::None) &&
        ArrivalIs(threeProcessorHandler.RecordArrival(MakeCrLid(0x10U, 1U)),
                  Ia64SalBootRendezvousArrivalStatus::Arrived, 1U) &&
        ArrivalIs(threeProcessorHandler.RecordArrival(MakeCrLid(0x20U, 2U)),
                  Ia64SalBootRendezvousArrivalStatus::Arrived, 2U) &&
        (threeProcessorHandler.GetSnapshot().ExpectedApMask == 0x06U) &&
        (threeProcessorHandler.GetSnapshot().ArrivedApMask == 0x06U);
    ASTERIA_EXPECT(runner, "two- and three-processor maps publish their exact AP masks", twoProcessorsArrive,
                   threeProcessorsArrive);

    Ia64LocalSapicIpiControllerSnapshot sameIdSnapshot = MakeSnapshot(3U);
    sameIdSnapshot.Processors[2U] = MakeProcessor(102U, 0x10U, 2U);
    Ia64SalBootRendezvousHandler sameIdHandler;
    Ia64LocalSapicIpiControllerSnapshot sameEidSnapshot = MakeSnapshot(3U);
    sameEidSnapshot.Processors[2U] = MakeProcessor(102U, 0x20U, 1U);
    Ia64SalBootRendezvousHandler sameEidHandler;
    ASTERIA_EXPECT(runner, "hardware identities remain distinct when only ID or EID differs",
                   sameIdHandler.Initialize(sameIdSnapshot, MakeCrLid(0x30U, 3U), MakeSalInformation()) ==
                       Ia64SalBootRendezvousInitializationError::None,
                   sameEidHandler.Initialize(sameEidSnapshot, MakeCrLid(0x30U, 3U), MakeSalInformation()) ==
                       Ia64SalBootRendezvousInitializationError::None);
}

void TestInitializationValidation(TestContext& runner) noexcept
{
    Ia64SalBootRendezvousHandler handler;
    Ia64LocalSapicIpiControllerSnapshot snapshot = MakeSnapshot();
    snapshot.State = Ia64LocalSapicIpiControllerState::Uninitialized;
    const bool notReady = InitializationFailsAtomically(
        handler, snapshot, MakeCrLid(0x30U, 3U), Ia64SalBootRendezvousInitializationError::IpiControllerNotReady);

    snapshot = MakeSnapshot();
    snapshot.ProcessorCount = 0U;
    snapshot.EnabledProcessorMask = 0U;
    const bool zeroCount = InitializationFailsAtomically(
        handler, snapshot, MakeCrLid(0x30U, 3U), Ia64SalBootRendezvousInitializationError::InvalidProcessorCount);

    snapshot = MakeSnapshot();
    snapshot.ProcessorCount = 5U;
    const bool excessiveCount = InitializationFailsAtomically(
        handler, snapshot, MakeCrLid(0x30U, 3U), Ia64SalBootRendezvousInitializationError::InvalidProcessorCount);

    snapshot = MakeSnapshot();
    snapshot.EnabledProcessorMask = 0x0DU;
    const bool invalidMask = InitializationFailsAtomically(
        handler, snapshot, MakeCrLid(0x30U, 3U), Ia64SalBootRendezvousInitializationError::InvalidProcessorMask);

    snapshot = MakeSnapshot();
    snapshot.Reserved[3U] = 1U;
    const bool snapshotReserved = InitializationFailsAtomically(
        handler, snapshot, MakeCrLid(0x30U, 3U), Ia64SalBootRendezvousInitializationError::InvalidProcessorRecord);

    snapshot = MakeSnapshot();
    snapshot.Processors[2U].Reserved[1U] = 1U;
    const bool processorReserved = InitializationFailsAtomically(
        handler, snapshot, MakeCrLid(0x30U, 3U), Ia64SalBootRendezvousInitializationError::InvalidProcessorRecord);

    snapshot = MakeSnapshot(2U);
    snapshot.Processors[2U] = MakeProcessor(200U, 0x50U, 5U);
    const bool nonzeroUnused = InitializationFailsAtomically(
        handler, snapshot, MakeCrLid(0x30U, 3U), Ia64SalBootRendezvousInitializationError::InvalidProcessorRecord);

    snapshot = MakeSnapshot();
    ++snapshot.Processors[1U].InterruptAddress;
    const bool addressMismatch = InitializationFailsAtomically(
        handler, snapshot, MakeCrLid(0x30U, 3U), Ia64SalBootRendezvousInitializationError::InvalidProcessorRecord);

    snapshot = MakeSnapshot();
    snapshot.Processors[2U].AcpiUid = snapshot.Processors[1U].AcpiUid;
    const bool duplicateUid = InitializationFailsAtomically(
        handler, snapshot, MakeCrLid(0x30U, 3U), Ia64SalBootRendezvousInitializationError::DuplicateProcessorIdentity);

    snapshot = MakeSnapshot();
    snapshot.Processors[2U] = MakeProcessor(102U, 0x10U, 1U);
    const bool duplicateHardware = InitializationFailsAtomically(
        handler, snapshot, MakeCrLid(0x30U, 3U), Ia64SalBootRendezvousInitializationError::DuplicateProcessorIdentity);

    snapshot = MakeSnapshot();
    const bool bspMismatch = InitializationFailsAtomically(
        handler, snapshot, MakeCrLid(0x31U, 3U), Ia64SalBootRendezvousInitializationError::InvalidBspIdentity);
    const bool bspEidMismatch = InitializationFailsAtomically(
        handler, snapshot, MakeCrLid(0x30U, 4U), Ia64SalBootRendezvousInitializationError::InvalidBspIdentity);

    SalSystemTableInformation salInformation = MakeSalInformation();
    salInformation.HasTranslationRegister = false;
    const bool missingTranslation = SalInitializationFailsAtomically(handler, snapshot, salInformation);
    salInformation = MakeSalInformation();
    salInformation.Reserved8 = 1U;
    const bool reservedSal = SalInitializationFailsAtomically(handler, snapshot, salInformation);
    salInformation = MakeSalInformation();
    salInformation.TranslationRegisterEncodedPageSize |= 1U;
    const bool malformedSize = SalInitializationFailsAtomically(handler, snapshot, salInformation);
    salInformation = MakeSalInformation();
    salInformation.TranslationRegisterEncodedPageSize = 11U << 2U;
    const bool undersizedTranslation = SalInitializationFailsAtomically(handler, snapshot, salInformation);
    salInformation = MakeSalInformation();
    salInformation.TranslationRegisterEncodedPageSize = 63U << 2U;
    salInformation.TranslationRegisterVirtualAddress = Foundation::UInt64{7U} << 61U;
    Ia64SalBootRendezvousHandler fullRegionHandler;
    const bool fullRegionTranslationAccepted =
        fullRegionHandler.Initialize(snapshot, MakeCrLid(0x30U, 3U), salInformation) ==
        Ia64SalBootRendezvousInitializationError::None;

    Ia64SalBootRendezvousHandler unalignedFullRegionHandler;
    salInformation.TranslationRegisterVirtualAddress |= 0x1000U;
    const bool unalignedFullRegionTranslation =
        SalInitializationFailsAtomically(unalignedFullRegionHandler, snapshot, salInformation);

    Ia64SalBootRendezvousHandler unalignedTranslationHandler;
    salInformation = MakeSalInformation();
    salInformation.TranslationRegisterVirtualAddress = 0x1000U;
    const bool unalignedTranslation =
        SalInitializationFailsAtomically(unalignedTranslationHandler, snapshot, salInformation);

    ASTERIA_EXPECT(runner, "invalid controller snapshots fail atomically and remain retryable", notReady, zeroCount,
                   excessiveCount, invalidMask, snapshotReserved, processorReserved, nonzeroUnused, addressMismatch,
                   duplicateUid, duplicateHardware, bspMismatch, bspEidMismatch);
    ASTERIA_EXPECT(runner, "only the exact structural SAL ITR0 purge operands are retained", missingTranslation,
                   reservedSal, malformedSize, undersizedTranslation, fullRegionTranslationAccepted,
                   unalignedFullRegionTranslation, unalignedTranslation);

    snapshot = MakeSnapshot();
    ASTERIA_EXPECT(runner, "successful configuration is immutable and single-use",
                   handler.Initialize(snapshot, MakeCrLid(0x30U, 3U), MakeSalInformation()) ==
                       Ia64SalBootRendezvousInitializationError::None,
                   handler.Initialize(snapshot, MakeCrLid(0x30U, 3U), MakeSalInformation()) ==
                       Ia64SalBootRendezvousInitializationError::AlreadyInitialized);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64SalBootRendezvousHandlerTests"};
    Asteria::Test::TestNativeValueAdapters(runner);
    Asteria::Test::TestOneProcessorBoundary(runner);
    Asteria::Test::TestFourProcessorArrivals(runner);
    Asteria::Test::TestDispatchActions(runner);
    Asteria::Test::TestIntermediateProcessorCountsAndIdentities(runner);
    Asteria::Test::TestInitializationValidation(runner);
    return runner.Finish();
}
#endif
